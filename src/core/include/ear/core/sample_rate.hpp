#pragma once
// A rate in hertz, with no arithmetic (docs/engine.md#dating). Frames and time convert only through FrameClock
// (ear/core/latency.hpp), so a rate is never an operand: it has no operators but comparison and converts to nothing.
// Its one other exit is hz(), for a library's or the wire's edge.
#include <compare>
#include <cstdint>
#include <optional>
namespace ear {

class SampleRate {
public:
	/// None for 0: the boundary where a number becomes a rate (ear.conf, a flag, a type's setting).
	static constexpr std::optional<SampleRate> of(std::uint32_t hz) noexcept {
		if (hz == 0) return std::nullopt;
		return SampleRate(hz);
	}
	/// A rate known at compile time (tests, defaults): SampleRate::fixed<48'000>().
	template <std::uint32_t Hz>
		requires(Hz > 0)
	static consteval SampleRate fixed() noexcept {
		return SampleRate(Hz);
	}
	/// The number, for a library (PipeWire's properties, libsndfile, sendspin-cpp) or the wire. Never an operand:
	/// convert frames and time through FrameClock.
	constexpr std::uint32_t hz() const noexcept { return hz_; }
	friend constexpr bool operator==(SampleRate, SampleRate) noexcept = default;
	friend constexpr auto operator<=>(SampleRate, SampleRate) noexcept = default;

private:
	explicit constexpr SampleRate(std::uint32_t hz) noexcept : hz_(hz) {}
	std::uint32_t hz_;
};

} // namespace ear
