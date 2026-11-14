// Test support: the only sleeps a test may make, each named for its role (docs/testing.md#sleeps-by-role). A test
// waits for the event it needs; a sleep is allowed only where it does not stand in for one, and the function called
// says which kind it is. Every test source is compiled with no_raw_sleeps.hpp forced in, which poisons the raw sleeps
// after this header, so a sleep made any other way does not compile.
//
// Each takes the length (or, for pace, the moment) and why: a string literal, never empty, so a reader sees at the call
// what the sleep is for.
#pragma once
#include <chrono>
#include <cstddef>
#include <thread>
namespace ear::sleeps {
namespace detail {
// Not constexpr, so a call from Why's consteval constructor is refused naming it (an empty why).
inline void a_sleep_says_why_it_sleeps() {}
} // namespace detail

/// Why a sleep sleeps: a string literal, never empty, checked at compile time.
class Why {
public:
	template <std::size_t N> consteval Why(const char (&)[N]) {
		if (N <= 1) detail::a_sleep_says_why_it_sleeps();
	}
};

/// The sleep is the event: a stall, a hold or a slow step the test makes on purpose.
template <class Rep, class Period> void induce(std::chrono::duration<Rep, Period> length, Why) noexcept {
	std::this_thread::sleep_for(length);
}

/// A bounded look for something that must not happen, taken after the event that would have caused it.
template <class Rep, class Period> void negative(std::chrono::duration<Rep, Period> length, Why) noexcept {
	std::this_thread::sleep_for(length);
}

/// A pause between looks at the event a test waits for: the look decides, and a deadline only fails the test.
template <class Rep, class Period> void poll(std::chrono::duration<Rep, Period> length, Why) noexcept {
	std::this_thread::sleep_for(length);
}

/// The clock of a stand-in for something that keeps real time (a sender, a heartbeat, a storm's rate): for a length,
/// or until a moment on a schedule of its own.
template <class Rep, class Period> void pace(std::chrono::duration<Rep, Period> length, Why) noexcept {
	std::this_thread::sleep_for(length);
}
template <class Clock, class Duration> void pace(std::chrono::time_point<Clock, Duration> until, Why) noexcept {
	std::this_thread::sleep_until(until);
}
} // namespace ear::sleeps
