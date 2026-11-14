// What must not compile in a test (test/CMakeLists.txt registers one CTest test per case, each with the reason it must
// be refused; docs/testing.md#sleeps-by-role). Compiled as every test target's sources are, with no_raw_sleeps.hpp
// forced in first. With no case defined this is the control, and it must compile: each role call, waiting on
// descriptors, Catch's checks that fail, and the standard library's waits that are not sleeps. Syntax only: nothing
// here runs.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include <poll.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "catch2/catch.hpp" // already read by the forced header: nothing new, so nothing poisoned
#include "sleeps.hpp"

namespace no_raw_sleeps_bad {

using namespace std::chrono_literals;

[[maybe_unused]] void waits(int fd, std::condition_variable& changed, std::mutex& mutex, bool& done) {
	// The control: each role, as a test sleeps.
	ear::sleeps::induce(50ms, "a stall made on purpose");
	ear::sleeps::negative(std::chrono::milliseconds(100), "nothing may come after stop");
	ear::sleeps::poll(1ms, "between looks at the pipe, which decide");
	ear::sleeps::pace(std::chrono::microseconds(200), "a sender's rate");
	ear::sleeps::pace(std::chrono::steady_clock::now() + 1ms, "a packet sent when it is due");
	// Waiting on descriptors, and the standard library's waits on an event, which are not sleeps.
	pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
	[[maybe_unused]] const int polled = ::poll(&ready, 1, 100);
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(fd, &readable);
	timeval timeout{.tv_sec = 1, .tv_usec = 0};
	[[maybe_unused]] const int selected = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
	std::unique_lock lock(mutex);
	changed.wait_for(lock, 10ms, [&] { return done; });
	std::this_thread::yield();
	[[maybe_unused]] const auto self = std::this_thread::get_id();
	[[maybe_unused]] const char* text = "std::this_thread::sleep_for(1s) and WARN(\"x\") in a string";
}

[[maybe_unused]] void checks(int value) {
	// The control: Catch's checks, which fail a test naming what it waited for.
	INFO("value " << value);
	CHECK(value == 1);
	REQUIRE(value >= 0);
	CHECK_FALSE(value < 0);
}

[[maybe_unused]] void raw() {
#ifdef EAR_CASE_sleep_for
	// The commonest sleep, which says neither its role nor why.
	std::this_thread::sleep_for(10ms);
#endif
#ifdef EAR_CASE_sleep_until
	std::this_thread::sleep_until(std::chrono::steady_clock::now() + 10ms);
#endif
#ifdef EAR_CASE_split_call
	// The name on a line of its own: the poison is lexical, so the line break hides nothing.
	std::this_thread::sleep_for(10ms);
#endif
#ifdef EAR_CASE_using
	using std::this_thread::sleep_for;
#endif
#ifdef EAR_CASE_pasted
	// The name made by token pasting, in a macro defined after the poison.
#define EAR_NAP(how) std::this_thread::sleep_##how
	EAR_NAP(for)(10ms);
#endif
#ifdef EAR_CASE_macro_after
	// A macro defined after the poison whose body names a raw sleep.
#define EAR_REST() usleep(10)
	EAR_REST();
#endif
#ifdef EAR_CASE_usleep
	usleep(1000);
#endif
#ifdef EAR_CASE_nanosleep
	const timespec pause{0, 1000};
	nanosleep(&pause, nullptr);
#endif
#ifdef EAR_CASE_clock_nanosleep
	const timespec pause{0, 1000};
	clock_nanosleep(CLOCK_MONOTONIC, 0, &pause, nullptr);
#endif
#ifdef EAR_CASE_posix_sleep
	::sleep(1);
#endif
#ifdef EAR_CASE_bare_sleep
	sleep(1);
#endif
#ifdef EAR_CASE_poll_nothing
	// A poll on no descriptors is a sleep.
	::poll(nullptr, 0, 10);
#endif
#ifdef EAR_CASE_ppoll_nothing
	const timespec pause{0, 1000};
	ppoll(nullptr, 0, &pause, nullptr);
#endif
#ifdef EAR_CASE_select_nothing
	// A select on no sets is a sleep.
	timeval pause{.tv_sec = 0, .tv_usec = 1000};
	::select(0, nullptr, nullptr, nullptr, &pause);
#endif
#ifdef EAR_CASE_pselect_nothing
	const timespec pause{0, 1000};
	pselect(0, nullptr, nullptr, nullptr, &pause, nullptr);
#endif
#ifdef EAR_CASE_empty_why
	// A role call that says no why.
	ear::sleeps::poll(1ms, "");
#endif
#ifdef EAR_CASE_why_not_a_literal
	// Why is a string literal, written at the call, never a value made elsewhere.
	const char* why = "made elsewhere";
	ear::sleeps::induce(1ms, why);
#endif
#ifdef EAR_CASE_no_why
	ear::sleeps::negative(1ms);
#endif
}

[[maybe_unused]] void goesOn(int value) {
	static_cast<void>(value);
#ifdef EAR_CASE_warn
	// Tells and goes on, where the test should fail naming the event it waited for.
	WARN("the graph did not settle");
#endif
#ifdef EAR_CASE_catch_warn
	CATCH_WARN("the graph did not settle");
#endif
#ifdef EAR_CASE_check_nofail
	CHECK_NOFAIL(value == 1);
#endif
#ifdef EAR_CASE_catch_check_nofail
	CATCH_CHECK_NOFAIL(value == 1);
#endif
#ifdef EAR_CASE_undef_warn
	// Catch's macro taken back by hand, to define one's own: the poison refuses the name in a directive too.
#undef WARN
#endif
}

} // namespace no_raw_sleeps_bad

#ifdef EAR_CASE_later_header
// A header the forced one did not read first, naming a raw sleep: it fails, so the forced header must read it first.
#include "naps.hpp"
#endif
