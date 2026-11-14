// What must not compile about a rate (ear/core/sample_rate.hpp, docs/engine.md#dating; src/core/CMakeLists.txt
// registers one CTest test per case, each with the reason it must be refused). A rate is never a raw number and never
// an operand: frames and time convert only through FrameClock. With no case defined this is the control, and it must
// compile: what the cases break is only what they name.
#include <chrono>
#include <cstdint>

#include "ear/core/latency.hpp"
#include "ear/core/sample_rate.hpp"

namespace sample_rate_bad {

using namespace ear;

void takesRate(SampleRate) {}

void control() {
	const SampleRate rate = SampleRate::fixed<48000>();
	const SampleRate parsed = SampleRate::of(44100).value_or(rate);
	const FrameClock clock(rate);
	const std::int64_t frames = clock.frames(std::chrono::milliseconds{5});
	const std::int64_t ns = clock.ns(frames);
	const bool same = rate == parsed || rate < parsed;
	const std::uint32_t edge = rate.hz(); // a library's or the wire's edge
	takesRate(rate);
	static_cast<void>(ns);
	static_cast<void>(same);
	static_cast<void>(edge);
}

#ifdef EAR_CASE_raw_integer_constructed
// A number becomes a rate only through of() (which refuses 0) or fixed<>(): the constructor is private.
void bad() {
	const SampleRate rate{48000};
	static_cast<void>(rate);
}
#endif

#ifdef EAR_CASE_raw_integer_converted
void bad() {
	const SampleRate rate = 48000;
	static_cast<void>(rate);
}
#endif

#ifdef EAR_CASE_raw_integer_passed
// Where an interface takes a rate, a number is not one.
void bad() { takesRate(48000); }
#endif

#ifdef EAR_CASE_fixed_zero
// No rate is 0, known at compile time either.
void bad() {
	const SampleRate rate = SampleRate::fixed<0>();
	static_cast<void>(rate);
}
#endif

#ifdef EAR_CASE_rate_as_integer
// A rate never passes for its number: hz() is the one way out, and the files that call it are listed.
void bad() {
	const std::uint32_t hz = SampleRate::fixed<48000>();
	static_cast<void>(hz);
}
#endif

#ifdef EAR_CASE_rate_cast
void bad() {
	const double hz = static_cast<double>(SampleRate::fixed<48000>());
	static_cast<void>(hz);
}
#endif

#ifdef EAR_CASE_rate_times_frames
// Frames from time by hand, the frame guard's first case: the rate has no *.
void bad() {
	const std::int64_t frames = SampleRate::fixed<48000>() * 5 / 1000;
	static_cast<void>(frames);
}
#endif

#ifdef EAR_CASE_frames_over_rate
// Time from frames by hand.
void bad() {
	const std::int64_t frames = 256;
	const std::int64_t ns = frames * 1'000'000'000 / SampleRate::fixed<48000>();
	static_cast<void>(ns);
}
#endif

#ifdef EAR_CASE_frames_remainder_rate
void bad() {
	const std::int64_t frames = 256;
	const std::int64_t rest = frames % SampleRate::fixed<48000>();
	static_cast<void>(rest);
}
#endif

#ifdef EAR_CASE_rate_plus_frames
void bad() {
	const auto sum = SampleRate::fixed<48000>() + std::int64_t{256};
	static_cast<void>(sum);
}
#endif

#ifdef EAR_CASE_rate_times_duration
// A rate with ns: FrameClock::frames is the conversion.
void bad() {
	const auto frames = SampleRate::fixed<48000>() * std::chrono::nanoseconds{5'000'000};
	static_cast<void>(frames);
}
#endif

#ifdef EAR_CASE_duration_over_rate
void bad() {
	const auto perFrame = std::chrono::nanoseconds{1'000'000'000} / SampleRate::fixed<48000>();
	static_cast<void>(perFrame);
}
#endif

#ifdef EAR_CASE_rate_compared_with_integer
// A rate compares only with a rate.
void bad() {
	const bool fortyEight = SampleRate::fixed<48000>() == 48000;
	static_cast<void>(fortyEight);
}
#endif

#ifdef EAR_CASE_rate_as_frames
// A rate is not a count of frames.
void bad() {
	const FrameClock clock(SampleRate::fixed<48000>());
	const std::int64_t second = clock.ns(SampleRate::fixed<48000>());
	static_cast<void>(second);
}
#endif

} // namespace sample_rate_bad
