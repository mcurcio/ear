// A cycle as its driver states it, for CycleAccount (docs/engine.md#graphcycle): built in one place from the clock a
// node reads in its process, so the port block and a test's probe (spike S5's B) state their cycles alike. Private to
// ear_pipewire; the data thread only.
#pragma once

#include <cstdint>
#include <ctime>

#include <spa/node/io.h>

#include "ear/core/cycle_facts.hpp"

namespace ear::pipewire {

/// A cycle's tick and its start on CLOCK_MONOTONIC (the driver's clock.nsec).
struct StatedTick {
	DriverTick tick;
	std::int64_t startNs;
};

namespace detail {
inline std::int64_t nowNs(clockid_t clock) noexcept {
	timespec now{};
	::clock_gettime(clock, &now);
	return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}
} // namespace detail

/// Data thread, inside the cycle: the tick of the cycle `clock` describes. Its start is the driver's: clock.nsec is
/// CLOCK_MONOTONIC even while the driver tracks the raw clock (docs/engine.md#the-internal-clock), so it is mapped to
/// CLOCK_MONOTONIC_RAW by the two clocks' offset read now; the raw read sits between two monotonic ones and the offset
/// is taken from their midpoint, so a preemption between reads skews it by half at most (node-driver.c,
/// get_nsec_offset), and a late wake cancels out. The cycle is numbered by `cuts`, whose count the driver's start moved
/// on just before it triggered the followers, on this thread; a cycle of another driver than `internalClock` has no
/// number.
inline StatedTick driverTick(const spa_io_clock& clock, std::uint32_t internalClock, const CutRecord& cuts) noexcept {
	const std::int64_t monoBefore = detail::nowNs(CLOCK_MONOTONIC);
	const std::int64_t raw = detail::nowNs(CLOCK_MONOTONIC_RAW);
	const std::int64_t monoAfter = detail::nowNs(CLOCK_MONOTONIC);
	const std::int64_t mono = monoBefore + (monoAfter - monoBefore) / 2;
	const auto startNs = static_cast<std::int64_t>(clock.nsec);
	return StatedTick{.tick = DriverTick{.driverId = clock.id,
	                                     .graphCycle = clock.id == internalClock ? cuts.started() : 0,
	                                     .position = clock.position,
	                                     .frames = static_cast<std::uint32_t>(clock.duration),
	                                     .startRawNs = startNs + (raw - mono)},
	                  .startNs = startNs};
}

} // namespace ear::pipewire
