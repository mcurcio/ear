#pragma once
// Private to ear_pipewire: the one place PipeWire's latency triples and ear's frames meet
// (docs/engine.md#latency-per-bank-pair). A peer's {quantum, rate, ns} is converted to frames at the port, once, before
// ear's model combines it; what a block tells its peers goes back in rate units only. The graph has one quantum and
// one rate, fixed at boot, so the conversion is exact.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include <spa/param/latency-utils.h>

#include "ear/core/latency.hpp"

namespace ear::pipewire {

/// A peer's latency in frames: a quantum is `quantum` frames, ns are at `rate`; each bound rounded to the nearest
/// frame, a negative one to zero. None for an empty combination (spa_latency_info_combine_start's, not finished): no
/// path.
inline std::optional<Latency> fromSpa(const spa_latency_info& info, std::uint32_t quantum,
                                      std::uint32_t rate) noexcept {
	if (info.min_quantum == std::numeric_limits<float>::max() &&
	    info.min_rate == std::numeric_limits<std::int32_t>::max() &&
	    info.min_ns == std::numeric_limits<std::int64_t>::max())
		return std::nullopt;
	const auto frames = [&](float quanta, std::int32_t rateFrames, std::int64_t ns) {
		const double total = static_cast<double>(quanta) * quantum + rateFrames + static_cast<double>(ns) * rate / 1e9;
		if (total <= 0.0) return std::uint32_t{0};
		if (total >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
			return std::numeric_limits<std::uint32_t>::max();
		return static_cast<std::uint32_t>(std::lround(total));
	};
	return Latency{frames(info.min_quantum, info.min_rate, info.min_ns),
	               frames(info.max_quantum, info.max_rate, info.max_ns)};
}

/// What a block tells a peer: `latency` in rate units only (frames at the graph's rate), on the side `direction`
/// names (SPA_DIRECTION_INPUT: playback, told upstream; SPA_DIRECTION_OUTPUT: capture, told downstream).
inline spa_latency_info toSpa(Latency latency, spa_direction direction) noexcept {
	const auto rateUnits = [](std::uint32_t frames) {
		return static_cast<std::int32_t>(std::min<std::uint32_t>(frames, std::numeric_limits<std::int32_t>::max()));
	};
	spa_latency_info info{};
	info.direction = direction;
	info.min_rate = rateUnits(latency.minFrames);
	info.max_rate = rateUnits(latency.maxFrames);
	return info;
}

} // namespace ear::pipewire
