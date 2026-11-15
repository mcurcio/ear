// Programs on real processes (programs.hpp, docs/writing-a-node-type.md#programs): what a program is told and tells,
// what it starts with, and how it is stopped. Each program is a script under tests/programs/ run by /bin/sh, or the
// compiled probe beside it; each test first blocks and ignores signals as ear does (asEarRuns), since Programs' thread,
// started by the first launch, inherits its launcher's mask, and what a program must not inherit has to be there to be
// inherited. Every wait is event-gated: a test waits for the event it needs, and a deadline only fails it.
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
// glibc 2.41's sys/pidfd.h declares its functions without C linkage (no __BEGIN_DECLS), so C++ would look for mangled
// names that libc does not have.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/runtime/programs.hpp"
#include "programs/recorded_program.hpp"
#include "temp_folder.hpp"

namespace {
using namespace std::chrono_literals;
using ear::ProgramExit;
using ear::ProgramLaunch;
using ear::test::RecordedProgram;

/// The signals ear blocks in every thread (SIGINT and SIGTERM: Engine::create; SIGCHLD: main.cpp), blocked here in the
/// thread that launches, and SIGPIPE ignored, so a program that inherited either would show it.
void asEarRuns() {
	sigset_t blocked;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGCHLD);
	REQUIRE(pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0);
	REQUIRE(::signal(SIGPIPE, SIG_IGN) != SIG_ERR);
}

/// /bin/sh running tests/programs/<script> with `args`, in `folder`; argv[0], and so its lines' source, is the script.
ProgramLaunch script(const ear::test::TempFolder& folder, const std::string& name, std::vector<std::string> args = {}) {
	ProgramLaunch launch{.node = "air", .executable = "/bin/sh", .argv = {name, EAR_PROGRAMS_DIR "/" + name}};
	for (std::string& arg : args) launch.argv.push_back(std::move(arg));
	launch.folder = folder.path().string();
	return launch;
}

/// Waits until the process behind `pid` (not necessarily a child) has ended, through a pidfd: false past the deadline.
bool diesWithin(pid_t pid, std::chrono::milliseconds deadline) {
	const int fd = ::pidfd_open(pid, 0);
	if (fd < 0) return errno == ESRCH; // already gone
	pollfd watched{fd, POLLIN, 0};
	const int ready = ::poll(&watched, 1, static_cast<int>(deadline.count()));
	::close(fd);
	return ready == 1;
}

/// Kills what a failed test left running, so its sleep does not outlive it.
void killIfAlive(pid_t pid) {
	const int fd = ::pidfd_open(pid, 0);
	if (fd < 0) return;
	::pidfd_send_signal(fd, SIGKILL, nullptr, 0);
	::close(fd);
}

/// A POSIX shared-memory name of this test process's own (so parallel runs never share one), unlinked when it goes.
class SharedMemory {
public:
	explicit SharedMemory(std::string_view what) : name_("/ear-programs-test-" + std::to_string(::getpid()) + "-") {
		name_ += what;
		::shm_unlink(name_.c_str()); // a crashed earlier run of this pid's
	}
	~SharedMemory() { ::shm_unlink(name_.c_str()); }
	SharedMemory(const SharedMemory&) = delete;
	SharedMemory& operator=(const SharedMemory&) = delete;
	const std::string& name() const noexcept { return name_; }
	void make() const {
		const int fd = ::shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		REQUIRE(fd >= 0);
		::close(fd);
	}
	void remove() const { REQUIRE(::shm_unlink(name_.c_str()) == 0); }

private:
	std::string name_;
};

bool saidReady(const RecordedProgram& program) { return !program.texts("stdout").empty(); }

} // namespace

TEST_CASE("A program that writes to standard error and exits 3 is told started, then exited with status 3 and its last "
          "line",
          "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(script(folder, "exit3.sh"), events);
	REQUIRE(events.ended());
	CHECK(events.isStarted);
	CHECK_FALSE(events.refusal);
	REQUIRE(events.exit);
	CHECK(*events.exit == ProgramExit{ProgramExit::How::exited, 3, "last words"});
}

TEST_CASE("A program's standard output lines are told at info and its standard error lines at warn, each in order, "
          "and a line of 5000 bytes is cut at 1024",
          "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(script(folder, "lines.sh"), events);
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->number == 0);
	CHECK(events.texts("stdout") == std::vector<std::string>{"out 1", "out 2", std::string(1024, 'x'), "out 3"});
	CHECK(events.texts("stderr") == std::vector<std::string>{"err 1", "err 2"});
	for (const ear::LogEntry& line : events.lines) {
		INFO(line.topic << ": " << line.text.substr(0, 20));
		CHECK(line.source == "lines.sh");
		CHECK(line.node == ear::NodeName("air"));
		CHECK(line.level == (line.topic == "stdout" ? ear::LogLevel::info : ear::LogLevel::warn));
		CHECK(line.cut == (line.text.size() == ear::programLineBytes));
	}
}

TEST_CASE("A program's feed bytes arrive in order across a writer that closes and opens it again", "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const std::string feed = (folder.path() / "air.1.feed").string();
	ProgramLaunch launch = script(folder, "feed.sh", {feed});
	launch.feed = feed;
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->number == 0);
	CHECK(events.feed == "onetwothree");
}

TEST_CASE("A program's files are mode 0600 with their text before it runs, and they and its feed are gone before it "
          "is told exited",
          "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const std::string first = (folder.path() / "air.1.conf").string();
	const std::string second = (folder.path() / "air.1.keys").string();
	const std::string feed = (folder.path() / "air.1.feed").string();
	ProgramLaunch launch = script(folder, "files.sh", {first, second});
	launch.files = {{first, "name = air"}, {second, "secret"}};
	launch.feed = feed;
	bool anyLeftAtExit = true;
	events.atExit = [&] {
		anyLeftAtExit =
			std::filesystem::exists(first) || std::filesystem::exists(second) || std::filesystem::exists(feed);
	};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->number == 0);
	CHECK(events.texts("stdout") == std::vector<std::string>{"mode 600", "text name = air", "mode 600", "text secret"});
	CHECK_FALSE(anyLeftAtExit);
}

TEST_CASE("A missing executable is refused in the kernel's words, with nothing left behind", "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const std::string file = (folder.path() / "air.1.conf").string();
	const std::string feed = (folder.path() / "air.1.feed").string();
	const auto running = programs.launch(ProgramLaunch{.node = "air",
	                                                   .executable = "/usr/bin/no-such-program",
	                                                   .argv = {"no-such-program"},
	                                                   .files = {{file, "x"}},
	                                                   .feed = feed,
	                                                   .folder = folder.path().string()},
	                                     events);
	REQUIRE(events.ended());
	CHECK(events.refusal == std::optional<std::string>("No such file or directory"));
	CHECK_FALSE(events.isStarted);
	CHECK_FALSE(events.exit);
	CHECK(std::filesystem::is_empty(folder.path()));
}

TEST_CASE("A program runs with PATH alone, the folder as its working folder, /dev/null as standard input and "
          "descriptors 0 to 2 only",
          "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	// A descriptor some library in ear's process left inheritable (no O_CLOEXEC): the program must not get it.
	const int inheritable = ::open("/dev/null", O_RDONLY);
	REQUIRE(inheritable > 2);
	const auto running = programs.launch(
		ProgramLaunch{
			.node = "air", .executable = EAR_PROGRAMS_PROBE, .argv = {"probe"}, .folder = folder.path().string()},
		events);
	REQUIRE(events.ended());
	::close(inheritable);
	REQUIRE(events.exit);
	CHECK(events.exit->number == 0);
	std::vector<std::string> env;
	std::set<std::string> fds;
	std::optional<std::string> cwd;
	std::optional<std::string> stdinTarget;
	for (const std::string& line : events.texts("stdout")) {
		if (line.starts_with("env ")) env.push_back(line.substr(4));
		if (line.starts_with("fd ")) fds.insert(line.substr(3));
		if (line.starts_with("cwd ")) cwd = line.substr(4);
		if (line.starts_with("stdin ")) stdinTarget = line.substr(6);
	}
	CHECK(env == std::vector<std::string>{"PATH=/usr/local/bin:/usr/bin:/bin"});
	CHECK(cwd == std::filesystem::canonical(folder.path()).string());
	CHECK(stdinTarget == std::optional<std::string>("/dev/null"));
	CHECK(fds == std::set<std::string>{"0", "1", "2"});
}

TEST_CASE("A program starts with no signal blocked or ignored, though its launcher blocks and ignores some",
          "[programs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(
		ProgramLaunch{
			.node = "air", .executable = EAR_PROGRAMS_PROBE, .argv = {"probe"}, .folder = folder.path().string()},
		events);
	REQUIRE(events.ended());
	std::vector<std::string> signals;
	for (const std::string& line : events.texts("stdout"))
		if (line.starts_with("Sig")) signals.push_back(line);
	CHECK(signals == std::vector<std::string>{"SigBlk:\t0000000000000000", "SigIgn:\t0000000000000000"});
}

TEST_CASE("stop() ends a sleeping program with SIGTERM", "[programs][stop]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(script(folder, "sleeps.sh"), events);
	REQUIRE(events.until([](const RecordedProgram& program) { return !program.texts("stdout").empty(); }));
	running->stop();
	REQUIRE(events.ended());
	CHECK(events.exit == std::optional<ProgramExit>(ProgramExit{ProgramExit::How::killed, SIGTERM, ""}));
	running->stop(); // after its end: nothing
}

TEST_CASE("A program that ignores SIGTERM is killed by SIGKILL once programTermBound has passed", "[programs][stop]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(script(folder, "ignores_term.sh"), events);
	REQUIRE(events.until([](const RecordedProgram& program) { return !program.texts("stdout").empty(); }));
	const auto stoppedAt = std::chrono::steady_clock::now();
	running->stop();
	running->stop(); // again: nothing more
	REQUIRE(events.ended(10s));
	const auto took = std::chrono::steady_clock::now() - stoppedAt;
	CHECK(events.exit == std::optional<ProgramExit>(ProgramExit{ProgramExit::How::killed, SIGKILL, ""}));
	CHECK(took >= ear::programTermBound - 50ms);
	CHECK(took < ear::programTermBound + 2s);
}

TEST_CASE("stop() reaches the program's whole process group: a child it started dies with it", "[programs][stop]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(script(folder, "grandchild.sh"), events);
	std::string said;
	REQUIRE(events.until([&](const RecordedProgram& program) {
		const auto out = program.texts("stdout");
		if (!out.empty()) said = out.front();
		return !out.empty();
	}));
	const pid_t child = std::stoi(said);
	running->stop();
	REQUIRE(events.ended());
	const bool died = diesWithin(child, 5s);
	if (!died) killIfAlive(child);
	CHECK(died);
	CHECK((events.exit && events.exit->how == ProgramExit::How::killed));
}

TEST_CASE("A program needing a name that exists before its launch is told started, then ready", "[programs][needs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	const SharedMemory shm("before");
	shm.make();
	ear::runtime::Programs programs;
	RecordedProgram events;
	ProgramLaunch launch = script(folder, "sleeps.sh");
	launch.needs = {shm.name()};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.until([](const RecordedProgram& program) { return program.isReady; }));
	CHECK(events.isStarted);
	running->stop();
	REQUIRE(events.ended());
	CHECK(events.exit == std::optional<ProgramExit>(ProgramExit{ProgramExit::How::killed, SIGTERM, ""}));
}

TEST_CASE("A program needing a name made after it started is told ready only once the name is made",
          "[programs][needs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	const SharedMemory shm("after");
	ear::runtime::Programs programs;
	RecordedProgram events;
	ProgramLaunch launch = script(folder, "sleeps.sh");
	launch.needs = {shm.name()};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.until(saidReady)); // started, and running its script
	CHECK(events.until([](const RecordedProgram& program) { return program.isStarted && !program.isReady; }));
	shm.make();
	REQUIRE(events.until([](const RecordedProgram& program) { return program.isReady; }));
	running->stop();
	REQUIRE(events.ended());
	CHECK(events.exit->removed.empty());
}

TEST_CASE("A name a running program needs, once removed, ends it, and its end says which", "[programs][needs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	const SharedMemory shm("removed");
	shm.make();
	ear::runtime::Programs programs;
	RecordedProgram events;
	ProgramLaunch launch = script(folder, "sleeps.sh");
	launch.needs = {shm.name()};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.until([](const RecordedProgram& program) { return program.isReady && saidReady(program); }));
	shm.remove();
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->removed == shm.name());
	CHECK(events.exit->how == ProgramExit::How::killed);
	CHECK(events.exit->number == SIGTERM);
}

TEST_CASE("A program needing a name never made that exits is told exited, and never ready", "[programs][needs]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	const SharedMemory shm("never");
	ear::runtime::Programs programs;
	RecordedProgram events;
	ProgramLaunch launch = script(folder, "exit3.sh");
	launch.needs = {shm.name()};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.ended());
	CHECK(events.isStarted);
	CHECK_FALSE(events.isReady);
	REQUIRE(events.exit);
	CHECK(*events.exit == ProgramExit{ProgramExit::How::exited, 3, "last words"});
}

TEST_CASE("A name removed after the program was stopped is no cause of its end", "[programs][needs][stop]") {
	asEarRuns();
	ear::test::TempFolder folder("programs");
	const SharedMemory shm("stopped");
	shm.make();
	const std::string hold = (folder.path() / "hold").string();
	REQUIRE(::mkfifo(hold.c_str(), 0600) == 0);
	ear::runtime::Programs programs;
	RecordedProgram events;
	ProgramLaunch launch = script(folder, "term_waits.sh", {hold});
	launch.needs = {shm.name()};
	const auto running = programs.launch(std::move(launch), events);
	REQUIRE(events.until([](const RecordedProgram& program) { return program.isReady && saidReady(program); }));
	running->stop();
	// Its "term" line: the stop was heard and the program holds, alive, until the test lets it go.
	REQUIRE(events.until([](const RecordedProgram& program) { return program.texts("stdout").size() == 2; }));
	shm.remove();
	const int writer = ::open(hold.c_str(), O_WRONLY | O_CLOEXEC);
	REQUIRE(writer >= 0);
	REQUIRE(::write(writer, "go\n", 3) == 3);
	::close(writer);
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->removed.empty());
	CHECK(*events.exit == ProgramExit{ProgramExit::How::exited, 0, ""});
}
