// Programs (programs.hpp, docs/writing-a-node-type.md#programs): the thread `ear-programs`, which spawns every program
// a node asks for, tells its lines, its feed and its end, stops it, and reaps every child of ear's, a program's or an
// orphan re-parented to ear (docs/architecture.md#boot-and-stop). It waits in epoll and nowhere else: a program's pidfd
// is its end, its pipes its lines, its feed its reports, an eventfd the main thread's asks, a signalfd SIGCHLD, an
// inotify descriptor the shared-memory names programs need coming and going in /dev/shm, and one timerfd the SIGKILL a
// stop owes past programTermBound.
#include "ear/runtime/programs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
// glibc 2.41's sys/pidfd.h declares its functions without C linkage (no __BEGIN_DECLS), so C++ would look for mangled
// names that libc does not have.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace ear::runtime {
namespace {

/// What every program runs under: setpriv sets PR_SET_PDEATHSIG and executes the program, so the program dies when the
/// thread that spawned it does (this one, so ear), even outside a container whose PID 1 would end it.
constexpr const char* kShim = "/usr/bin/setpriv";
/// A program's whole environment.
constexpr const char* kPath = "PATH=/usr/local/bin:/usr/bin:/bin";
/// Where Linux keeps POSIX shared memory: shm_open's "/<name>" is the file /dev/shm/<name>.
constexpr const char* kShm = "/dev/shm";

/// The file behind a shared-memory name ("/x" is /dev/shm/x).
std::string shmPath(const std::string& name) { return std::string(kShm) + name; }

bool exists(const std::string& path) {
	struct stat seen{};
	return ::stat(path.c_str(), &seen) == 0;
}

/// A descriptor closed when it goes.
class Fd {
public:
	Fd() = default;
	explicit Fd(int fd) noexcept : fd_(fd) {}
	~Fd() { reset(); }
	Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
	Fd& operator=(Fd&& other) noexcept {
		if (this != &other) {
			reset();
			fd_ = std::exchange(other.fd_, -1);
		}
		return *this;
	}
	int get() const noexcept { return fd_; }
	explicit operator bool() const noexcept { return fd_ >= 0; }
	void reset() noexcept {
		if (fd_ >= 0) ::close(fd_);
		fd_ = -1;
	}

private:
	int fd_ = -1;
};

Fd required(int fd, const char* what) {
	if (fd < 0) throw std::system_error(errno, std::generic_category(), what);
	return Fd(fd);
}

/// One of a program's two output pipes: the line it is in, cut at programLineBytes.
struct Stream {
	const char* topic; // "stdout" | "stderr"
	LogLevel level;
	Fd fd;
	std::uint64_t token = 0;
	std::string line;
	bool cut = false;
};

timespec monotonicNow() {
	timespec now{};
	::clock_gettime(CLOCK_MONOTONIC, &now);
	return now;
}

std::chrono::nanoseconds sinceEpoch(const timespec& at) {
	return std::chrono::seconds(at.tv_sec) + std::chrono::nanoseconds(at.tv_nsec);
}

} // namespace

/// One launched program, from its asking to its end. The main thread fills `launch` and `events` and never touches
/// the rest; everything else is the thread's.
struct ProgramRecord {
	ProgramRecord(ProgramLaunch launched, ProgramEvents& told) : launch(std::move(launched)), events(told) {}
	ProgramLaunch launch;
	ProgramEvents& events;
	std::vector<std::string> made; // the files and the feed written so far, removed at its end
	Fd feed;
	std::uint64_t feedToken = 0;
	Stream out{"stdout", LogLevel::info};
	Stream err{"stderr", LogLevel::warn};
	Fd pidfd;
	std::uint64_t pidToken = 0;
	pid_t pid = 0;
	bool running = false;                           // spawned, not yet reaped
	bool stopped = false;                           // stop() was heard once it ran
	std::optional<std::chrono::nanoseconds> killAt; // CLOCK_MONOTONIC: its group's SIGKILL is due
	std::string lastLine;                           // its last line on standard error
	std::vector<bool> present;                      // by launch.needs' index: seen in /dev/shm
	bool readyTold = false;
	std::string removed; // the need whose removal made the thread stop it; empty: none
	bool allPresent() const {
		return std::ranges::all_of(present, [](bool seen) { return seen; });
	}
};

struct Programs::Shared {
	enum class Ask { launch, stop };
	struct Command {
		Ask ask;
		std::shared_ptr<ProgramRecord> program;
	};
	enum class Watch { pidfd, out, err, feed };
	struct Watched {
		std::shared_ptr<ProgramRecord> program;
		Watch what;
	};
	static constexpr std::uint64_t kWake = 0;
	static constexpr std::uint64_t kSignals = 1;
	static constexpr std::uint64_t kTimer = 2;
	static constexpr std::uint64_t kShmEvents = 3;

	Shared()
		: epoll(required(::epoll_create1(EPOLL_CLOEXEC), "epoll_create1")),
		  wake(required(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), "eventfd")),
		  signals(required(childSignal(), "signalfd")),
		  timer(required(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK), "timerfd_create")),
		  shmEvents(required(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK), "inotify_init1")) {
		add(wake.get(), kWake);
		add(signals.get(), kSignals);
		add(timer.get(), kTimer);
		add(shmEvents.get(), kShmEvents);
	}

	/// A signalfd for SIGCHLD. It hears the signal only while every thread blocks it, as main.cpp does before the first
	/// starts; unblocked (a test program), each program is still reaped through its pidfd.
	static int childSignal() {
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGCHLD);
		return ::signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
	}

	void add(int fd, std::uint64_t token) const {
		epoll_event event{};
		event.events = EPOLLIN;
		event.data.u64 = token;
		if (::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, fd, &event) != 0)
			throw std::system_error(errno, std::generic_category(), "epoll_ctl");
	}

	/// Main thread: one ask for the thread, which starts with the first.
	void ask(Ask what, std::shared_ptr<ProgramRecord> program) {
		const std::lock_guard lock(mutex);
		commands.push_back(Command{what, std::move(program)});
		if (!thread.joinable()) thread = std::thread([this] { run(); });
		const std::uint64_t one = 1;
		[[maybe_unused]] const ssize_t wrote = ::write(wake.get(), &one, sizeof one);
	}

	void quit() {
		{
			const std::lock_guard lock(mutex);
			quitting = true;
			const std::uint64_t one = 1;
			[[maybe_unused]] const ssize_t wrote = ::write(wake.get(), &one, sizeof one);
		}
		if (thread.joinable()) thread.join();
	}

	// ---- The thread ----

	void run() {
		pthread_setname_np(pthread_self(), "ear-programs");
		std::array<epoll_event, 32> ready{};
		for (;;) {
			const int count = ::epoll_wait(epoll.get(), ready.data(), static_cast<int>(ready.size()), -1);
			if (count < 0) {
				if (errno == EINTR) continue;
				return; // only a broken epoll descriptor: nothing more can be heard
			}
			for (int i = 0; i < count; ++i) {
				const std::uint64_t token = ready[static_cast<std::size_t>(i)].data.u64;
				if (token == kWake) {
					if (!asked()) return;
				} else if (token == kSignals) {
					reapChildren();
				} else if (token == kTimer) {
					killOverdue();
				} else if (token == kShmEvents) {
					shmChanged();
				} else {
					heard(token);
				}
			}
		}
	}

	/// The main thread's asks, in order; false once quitting.
	bool asked() {
		std::uint64_t count = 0;
		[[maybe_unused]] const ssize_t got = ::read(wake.get(), &count, sizeof count);
		std::deque<Command> taken;
		bool quit = false;
		{
			const std::lock_guard lock(mutex);
			taken.swap(commands);
			quit = quitting;
		}
		for (Command& command : taken) {
			if (command.ask == Ask::launch) start(command.program);
			else stop(*command.program);
		}
		return !quit;
	}

	std::uint64_t watch(int fd, const std::shared_ptr<ProgramRecord>& program, Watch what) {
		const std::uint64_t token = nextToken++;
		add(fd, token);
		watched.emplace(token, Watched{program, what});
		return token;
	}

	void unwatch(int fd, std::uint64_t token) {
		if (fd >= 0) ::epoll_ctl(epoll.get(), EPOLL_CTL_DEL, fd, nullptr);
		watched.erase(token);
	}

	void heard(std::uint64_t token) {
		const auto found = watched.find(token);
		if (found == watched.end()) return; // ended earlier in this same wake
		const std::shared_ptr<ProgramRecord> program = found->second.program;
		switch (found->second.what) {
		case Watch::pidfd:
			end(*program);
			break;
		case Watch::out:
			readLines(*program, program->out);
			break;
		case Watch::err:
			readLines(*program, program->err);
			break;
		case Watch::feed:
			readFeed(*program);
			break;
		}
	}

	/// Removes what a launch made so far, in any state.
	static void removeMade(ProgramRecord& program) {
		program.feed.reset();
		for (const std::string& path : program.made) ::unlink(path.c_str());
		program.made.clear();
	}

	/// A launch: the watch on what it needs, the files, the feed, the pipes, then the spawn. Any failure removes what it
	/// made and tells refused.
	void start(const std::shared_ptr<ProgramRecord>& shared) {
		ProgramRecord& program = *shared;
		const ProgramLaunch& launch = program.launch;
		const auto refuse = [&](int error) {
			removeMade(program);
			forgetNeeds(program);
			program.events.refused(std::strerror(error));
		};
		if (!launch.needs.empty()) {
			// The watch before the look, so a name made between the two is still heard.
			if (shmWatch < 0) {
				shmWatch = ::inotify_add_watch(shmEvents.get(), kShm,
				                               IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF);
				if (shmWatch < 0) {
					const int error = errno;
					removeMade(program);
					return program.events.refused(std::string("cannot watch ") + kShm + ": " + std::strerror(error));
				}
			}
			needing.push_back(shared);
			for (const std::string& name : launch.needs) program.present.push_back(exists(shmPath(name)));
		}
		for (const auto& [path, text] : launch.files) {
			Fd file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
			if (!file) return refuse(errno);
			program.made.push_back(path);
			for (std::size_t at = 0; at < text.size();) {
				const ssize_t wrote = ::write(file.get(), text.data() + at, text.size() - at);
				if (wrote < 0 && errno == EINTR) continue;
				if (wrote < 0) return refuse(errno);
				at += static_cast<std::size_t>(wrote);
			}
		}
		if (launch.feed) {
			if (::mkfifo(launch.feed->c_str(), 0600) != 0) return refuse(errno);
			program.made.push_back(*launch.feed);
			// Read and write: its reader never sees end of file while the program closes and opens its writer again.
			program.feed = Fd(::open(launch.feed->c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
			if (!program.feed) return refuse(errno);
		}
		int out[2] = {-1, -1};
		int err[2] = {-1, -1};
		if (::pipe2(out, O_CLOEXEC) != 0) return refuse(errno);
		Fd outRead(out[0]);
		Fd outWrite(out[1]);
		if (::pipe2(err, O_CLOEXEC) != 0) return refuse(errno);
		Fd errRead(err[0]);
		Fd errWrite(err[1]);
		// setpriv's own failure to execute it would only be an exit status: a missing or unrunnable executable is told
		// as the refusal it is, before the spawn.
		if (::access(launch.executable.c_str(), X_OK) != 0) return refuse(errno);

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
		posix_spawn_file_actions_adddup2(&actions, outWrite.get(), 1);
		posix_spawn_file_actions_adddup2(&actions, errWrite.get(), 2);
		posix_spawn_file_actions_addclosefrom_np(&actions, 3);
		posix_spawn_file_actions_addchdir_np(&actions, launch.folder.c_str());
		posix_spawnattr_t attributes;
		posix_spawnattr_init(&attributes);
		// Its own process group, so a stop reaches what it starts; and an empty mask with every signal at its default,
		// since ear blocks the stop signals and SIGCHLD in every thread and a child inherits both.
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
		posix_spawnattr_setpgroup(&attributes, 0);
		sigset_t none;
		sigemptyset(&none);
		posix_spawnattr_setsigmask(&attributes, &none);
		// Every bit, not sigfillset: glibc's leaves out the two signals it keeps for itself (32 and 33), and its spawn sets
		// those to SIG_IGN in the child unless the default set names them, so the program would start with them ignored.
		sigset_t all;
		std::memset(&all, 0xff, sizeof all);
		posix_spawnattr_setsigdefault(&attributes, &all);

		std::vector<std::string> words{"setpriv", "--pdeathsig", "KILL", "--", launch.executable};
		for (std::size_t i = 1; i < launch.argv.size(); ++i) words.push_back(launch.argv[i]);
		std::vector<char*> argv;
		for (std::string& word : words) argv.push_back(word.data());
		argv.push_back(nullptr);
		std::string path(kPath);
		char* envp[] = {path.data(), nullptr};

		int pidfd = -1;
		const int spawned = ::pidfd_spawn(&pidfd, kShim, &actions, &attributes, argv.data(), envp);
		posix_spawn_file_actions_destroy(&actions);
		posix_spawnattr_destroy(&attributes);
		if (spawned != 0) return refuse(spawned);
		program.pidfd = Fd(pidfd);
		program.pid = ::pidfd_getpid(pidfd);
		program.running = true;
		outWrite.reset();
		errWrite.reset();
		::fcntl(outRead.get(), F_SETFL, O_NONBLOCK);
		::fcntl(errRead.get(), F_SETFL, O_NONBLOCK);
		program.out.fd = std::move(outRead);
		program.err.fd = std::move(errRead);
		program.pidToken = watch(program.pidfd.get(), shared, Watch::pidfd);
		program.out.token = watch(program.out.fd.get(), shared, Watch::out);
		program.err.token = watch(program.err.fd.get(), shared, Watch::err);
		if (program.feed) program.feedToken = watch(program.feed.get(), shared, Watch::feed);
		byPid.emplace(program.pid, shared);
		program.events.started();
		tellReady(program);
	}

	/// Ready, once: started, and every name it needs seen.
	static void tellReady(ProgramRecord& program) {
		if (program.readyTold || !program.running || !program.allPresent()) return;
		program.readyTold = true;
		program.events.ready();
	}

	/// It needs nothing more: out of the watched programs, and the watch on /dev/shm gone with the last of them.
	void forgetNeeds(const ProgramRecord& program) {
		if (std::erase_if(needing, [&](const auto& other) { return other.get() == &program; }) == 0) return;
		if (needing.empty() && shmWatch >= 0) {
			::inotify_rm_watch(shmEvents.get(), shmWatch);
			shmWatch = -1;
		}
	}

	/// Name `name` (its "/" included) seen made or gone, in every program that needs it.
	void shmSeen(const std::string& name, bool made) {
		for (const std::shared_ptr<ProgramRecord>& program : needing) {
			const std::vector<std::string>& needs = program->launch.needs;
			for (std::size_t i = 0; i < needs.size(); ++i) {
				if (needs[i] != name || program->present[i] == made) continue;
				program->present[i] = made;
				if (made) {
					tellReady(*program);
				} else if (program->running && !program->stopped && program->removed.empty()) {
					// It needs what went: ear ends it, and its end carries the cause (ProgramExit::removed).
					program->removed = name;
					stop(*program);
				}
			}
		}
	}

	/// /dev/shm changed: each name made or gone, or with the queue overflowed every watched name looked at again.
	void shmChanged() {
		alignas(inotify_event) std::array<char, 4096> buffer{};
		for (;;) {
			const ssize_t got = ::read(shmEvents.get(), buffer.data(), buffer.size());
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) return; // EAGAIN: all of it for now
			for (ssize_t at = 0; at < got;) {
				const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + at);
				at += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
				if ((event->mask & IN_Q_OVERFLOW) != 0) {
					lookAgain();
				} else if (event->wd != shmWatch) {
					continue; // a watch removed earlier
				} else if ((event->mask & IN_IGNORED) != 0) {
					shmWatch = -1; // /dev/shm itself went: the kernel removed the watch
				} else if (event->len > 0) {
					const std::string name = std::string("/") + event->name; // NUL-padded: up to its first NUL
					if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) shmSeen(name, true);
					if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) shmSeen(name, false);
				}
			}
		}
	}

	/// The queue overflowed, so events were lost: every watched name is looked at in /dev/shm again.
	void lookAgain() {
		for (const std::shared_ptr<ProgramRecord>& program : needing)
			for (const std::string& name : program->launch.needs) shmSeen(name, exists(shmPath(name)));
	}

	/// SIGTERM to its group at once, SIGKILL at programTermBound unless it is reaped first. Before it ran (refused),
	/// after its end, or again: nothing.
	void stop(ProgramRecord& program) {
		if (!program.running || program.stopped) return;
		program.stopped = true;
		::killpg(program.pid, SIGTERM);
		program.killAt =
			sinceEpoch(monotonicNow()) + std::chrono::duration_cast<std::chrono::nanoseconds>(programTermBound);
		armTimer();
	}

	/// The one timer: the earliest SIGKILL owed, or none.
	void armTimer() {
		std::optional<std::chrono::nanoseconds> earliest;
		for (const auto& [pid, program] : byPid)
			if (program->killAt && (!earliest || *program->killAt < *earliest)) earliest = program->killAt;
		itimerspec when{};
		if (earliest) {
			when.it_value.tv_sec = static_cast<time_t>(earliest->count() / 1'000'000'000);
			when.it_value.tv_nsec = static_cast<long>(earliest->count() % 1'000'000'000);
			if (when.it_value.tv_sec == 0 && when.it_value.tv_nsec == 0) when.it_value.tv_nsec = 1;
		}
		::timerfd_settime(timer.get(), TFD_TIMER_ABSTIME, &when, nullptr);
	}

	void killOverdue() {
		std::uint64_t expirations = 0;
		[[maybe_unused]] const ssize_t got = ::read(timer.get(), &expirations, sizeof expirations);
		const std::chrono::nanoseconds now = sinceEpoch(monotonicNow());
		for (const auto& [pid, program] : byPid) {
			if (!program->killAt || *program->killAt > now) continue;
			program->killAt.reset();
			::killpg(pid, SIGKILL);
		}
		armTimer();
	}

	/// Every child that ended (SIGCHLD): a program's through its own end, any other (an orphan ear inherited as its
	/// subreaper) by its pid, so none stays a zombie.
	void reapChildren() {
		signalfd_siginfo info{};
		while (::read(signals.get(), &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
		}
		for (;;) {
			siginfo_t child{};
			if (::waitid(P_ALL, 0, &child, WEXITED | WNOHANG | WNOWAIT) != 0 || child.si_pid == 0) return;
			if (const auto found = byPid.find(child.si_pid); found != byPid.end()) {
				const std::shared_ptr<ProgramRecord> program = found->second;
				end(*program);
			} else {
				::waitpid(child.si_pid, nullptr, 0);
			}
		}
	}

	/// Its pidfd readable: reap it, tell what its pipes and feed still hold, remove its files and feed, then exited.
	void end(ProgramRecord& program) {
		siginfo_t info{};
		const int reaped = ::waitid(P_PIDFD, static_cast<id_t>(program.pidfd.get()), &info, WEXITED);
		ProgramExit exit;
		if (reaped == 0 && (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED)) {
			exit.how = ProgramExit::How::killed;
			exit.number = info.si_status;
		} else {
			exit.how = ProgramExit::How::exited;
			exit.number = reaped == 0 ? info.si_status : 0;
		}
		readLines(program, program.out);
		readLines(program, program.err);
		finishLine(program, program.out);
		finishLine(program, program.err);
		if (program.feed) readFeed(program);
		unwatch(program.out.fd.get(), program.out.token);
		unwatch(program.err.fd.get(), program.err.token);
		unwatch(program.feed.get(), program.feedToken);
		unwatch(program.pidfd.get(), program.pidToken);
		program.out.fd.reset();
		program.err.fd.reset();
		program.pidfd.reset();
		removeMade(program);
		program.running = false;
		program.killAt.reset();
		byPid.erase(program.pid); // its caller holds the record through the tell
		forgetNeeds(program);
		armTimer();
		exit.lastLine = program.lastLine;
		exit.removed = program.removed;
		program.events.exited(std::move(exit));
	}

	/// What a pipe holds now, line by line; at end of file, the line it was in and the pipe no more.
	void readLines(ProgramRecord& program, Stream& stream) {
		if (!stream.fd) return;
		std::array<char, 4096> chunk{};
		for (;;) {
			const ssize_t got = ::read(stream.fd.get(), chunk.data(), chunk.size());
			if (got < 0 && errno == EINTR) continue;
			if (got < 0) return; // EAGAIN: all of it for now
			if (got == 0) {
				finishLine(program, stream);
				unwatch(stream.fd.get(), stream.token);
				stream.fd.reset();
				return;
			}
			for (ssize_t i = 0; i < got; ++i) {
				const char c = chunk[static_cast<std::size_t>(i)];
				if (c == '\n') {
					say(program, stream);
				} else if (stream.line.size() < programLineBytes) {
					stream.line += c;
				} else {
					stream.cut = true;
				}
			}
		}
	}

	void finishLine(ProgramRecord& program, Stream& stream) {
		if (!stream.line.empty() || stream.cut) say(program, stream);
	}

	void say(ProgramRecord& program, Stream& stream) {
		if (stream.level == LogLevel::warn) program.lastLine = stream.line;
		program.events.said(
			LogEntry{.level = stream.level,
		             .source = program.launch.argv.empty() ? program.launch.executable : program.launch.argv.front(),
		             .text = std::exchange(stream.line, {}),
		             .topic = stream.topic,
		             .node = program.launch.node,
		             .cut = std::exchange(stream.cut, false)});
	}

	void readFeed(ProgramRecord& program) {
		std::string chunk(programFeedChunkBytes, '\0');
		for (;;) {
			const ssize_t got = ::read(program.feed.get(), chunk.data(), chunk.size());
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) return; // EAGAIN; never end of file, as it holds the writing side too
			program.events.fed(chunk.substr(0, static_cast<std::size_t>(got)));
		}
	}

	Fd epoll;
	Fd wake;
	Fd signals;
	Fd timer;
	Fd shmEvents; // inotify: /dev/shm, watched while any program needs a name
	std::mutex mutex;
	std::deque<Command> commands; // under mutex
	bool quitting = false;        // under mutex
	std::thread thread;           // made under mutex, joined by quit
	// The thread's only:
	std::map<std::uint64_t, Watched> watched;
	std::map<pid_t, std::shared_ptr<ProgramRecord>> byPid; // every program running
	std::vector<std::shared_ptr<ProgramRecord>> needing;   // every program that needs a name, from launch to end
	int shmWatch = -1;                                     // the watch on /dev/shm, or none
	std::uint64_t nextToken = 4;
};

namespace {

class Running final : public RunningProgram {
public:
	Running(std::shared_ptr<Programs::Shared> shared, std::shared_ptr<ProgramRecord> program)
		: shared_(std::move(shared)), program_(std::move(program)) {}
	void stop() override { shared_->ask(Programs::Shared::Ask::stop, program_); }

private:
	std::shared_ptr<Programs::Shared> shared_;
	std::shared_ptr<ProgramRecord> program_;
};

} // namespace

Programs::Programs() : shared_(std::make_shared<Shared>()) {}

Programs::~Programs() { shared_->quit(); }

std::unique_ptr<RunningProgram> Programs::launch(ProgramLaunch launch, ProgramEvents& events) {
	auto program = std::make_shared<ProgramRecord>(std::move(launch), events);
	shared_->ask(Shared::Ask::launch, program);
	return std::make_unique<Running>(shared_, std::move(program));
}

} // namespace ear::runtime
