// Programs as ear's reaper (programs.hpp, docs/architecture.md#boot-and-stop), in a test program of its own: like ear,
// it blocks SIGCHLD and makes itself its descendants' subreaper before anything else runs (below, at static
// initialisation, before Catch's runner and before any thread), so an orphan a program leaves is re-parented to it and
// SIGCHLD reaches Programs' signalfd. Every wait is event-gated (a pidfd, a told event); a deadline only fails the test.
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include <poll.h>
#include <signal.h>
// glibc 2.41's sys/pidfd.h declares its functions without C linkage (no __BEGIN_DECLS), so C++ would look for mangled
// names that libc does not have.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/prctl.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/runtime/programs.hpp"
#include "programs/recorded_program.hpp"
#include "sleeps.hpp"
#include "temp_folder.hpp"

namespace {
using namespace std::chrono_literals;
using ear::ProgramExit;
using ear::ProgramLaunch;
using ear::test::RecordedProgram;

/// What main.cpp does first, done here before main: SIGCHLD blocked, so it waits for Programs' signalfd, and this
/// process its descendants' reaper.
const bool asEar = [] {
	sigset_t child;
	sigemptyset(&child);
	sigaddset(&child, SIGCHLD);
	return pthread_sigmask(SIG_BLOCK, &child, nullptr) == 0 && ::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0;
}();

/// Waits until the process behind `fd` has ended: false past the deadline.
bool endsWithin(int fd, std::chrono::milliseconds deadline) {
	pollfd watched{fd, POLLIN, 0};
	return ::poll(&watched, 1, static_cast<int>(deadline.count())) == 1;
}

std::string firstWord(const std::string& line) { return line.substr(0, line.find(' ')); }

} // namespace

TEST_CASE("An orphan a program left behind is reaped once it ends, never left a zombie", "[programs][reaper]") {
	REQUIRE(asEar);
	ear::test::TempFolder folder("reaper");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(ProgramLaunch{.node = "air",
	                                                   .executable = "/bin/sh",
	                                                   .argv = {"double_fork.sh", EAR_PROGRAMS_DIR "/double_fork.sh"},
	                                                   .folder = folder.path().string()},
	                                     events);
	REQUIRE(events.ended());
	REQUIRE(events.exit);
	CHECK(events.exit->number == 0);
	const auto out = events.texts("stdout");
	REQUIRE(out.size() == 1);
	const pid_t orphan = std::stoi(out.front());
	const int fd = ::pidfd_open(orphan, 0);
	REQUIRE(fd >= 0);
	// Its pidfd is readable once it ended; then the zombie it was goes as soon as Programs reaps it.
	REQUIRE(endsWithin(fd, 10s));
	::close(fd);
	const std::filesystem::path entry = "/proc/" + std::to_string(orphan);
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::filesystem::exists(entry) && std::chrono::steady_clock::now() < deadline)
		ear::sleeps::poll(10ms, "a reaped zombie's /proc entry goes, and nothing tells it");
	CHECK_FALSE(std::filesystem::exists(entry));
}

TEST_CASE("A program dies with the process that launched it, through setpriv's parent-death signal",
          "[programs][reaper]") {
	REQUIRE(asEar);
	ear::test::TempFolder folder("reaper");
	ear::runtime::Programs programs;
	RecordedProgram events;
	const auto running = programs.launch(
		ProgramLaunch{
			.node = "air", .executable = EAR_PROGRAMS_LAUNCHER, .argv = {"launcher"}, .folder = folder.path().string()},
		events);
	pid_t launcher = 0;
	pid_t sleeper = 0;
	REQUIRE(events.until([&](const RecordedProgram& program) {
		for (const std::string& line : program.texts("stdout")) {
			const std::string word = firstWord(line);
			if (word == "launcher") launcher = std::stoi(line.substr(9));
			if (word == "sleep" && line != "sleep ended") sleeper = std::stoi(line.substr(6));
		}
		return launcher > 0 && sleeper > 0;
	}));
	const int sleeperFd = ::pidfd_open(sleeper, 0);
	REQUIRE(sleeperFd >= 0);
	REQUIRE(::kill(launcher, SIGKILL) == 0);
	REQUIRE(events.ended());
	CHECK(events.exit == std::optional<ProgramExit>(ProgramExit{ProgramExit::How::killed, SIGKILL, ""}));
	const bool died = endsWithin(sleeperFd, 10s);
	if (!died) ::pidfd_send_signal(sleeperFd, SIGKILL, nullptr, 0); // so a failure leaves no sleep behind
	::close(sleeperFd);
	CHECK(died);
}
