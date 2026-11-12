// The engine's record of which graph cycles the internal clock cut short (docs/engine.md#graphcycle): a realtime
// listener on the dummy driver's own events, registered through the callback boundary, telling a CutRing
// (ear/core/cut_ring.hpp) as the driver tells. Every port block names its cycle by this record's count and reads its
// cuts through CycleAccount (ear/core/cycle_facts.hpp). Private to ear_pipewire: its sources include it by this path.
#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include <pipewire/context.h>

#include "callback_boundary.hpp"
#include "ear/core/cut_ring.hpp"
#include "ear/pipewire/engine.hpp"

namespace ear::pipewire {

/// Written on the data thread (the driver's start and incomplete events, in node_ready: a cycle's cut is told before
/// the next cycle's start, so inside cycle N every cut of a cycle before N is recorded); read from any thread by
/// CutRecord's contract. Cycles are counted from the listener's first start, never read from clock.cycle: the
/// incomplete event names no cycle, and the server side's position is private to impl-node.c, so the record counts
/// them itself and the blocks name theirs by its count.
class DriverCuts final : public CutRecord {
public:
	/// Main thread, in Engine::create once the context has loaded engine.conf's objects, before any port block (so
	/// every block's first cycle has a number): finds the internal clock by node.name and listens to it. Allocates its
	/// ring here only. None found: EngineError.
	static std::expected<std::unique_ptr<DriverCuts>, EngineError> listen(pw_context* context);
	DriverCuts(const DriverCuts&) = delete;
	DriverCuts& operator=(const DriverCuts&) = delete;
	/// Main thread: the listener off the driver, waiting on the data loop. Never destroyed once that loop is wedged:
	/// the engine leaks it (release()), since the data loop would still call into it.
	~DriverCuts() = default;

	std::uint64_t started() const noexcept override { return ring_.started(); }
	std::optional<bool> cut(std::uint64_t cycle) const noexcept override { return ring_.cut(cycle); }
	std::uint64_t cutsSoFar() const noexcept override { return ring_.cutsSoFar(); }
	std::uint64_t newestWhole() const noexcept override { return ring_.newestWhole(); }
	/// The internal clock's global id, which a cycle's clock.id names when it drives.
	std::uint32_t clockId() const noexcept { return clockId_; }

private:
	DriverCuts(pw_impl_node* clock, std::uint32_t clockId);
	/// Data thread: the driver starts a cycle.
	void onStart() noexcept { ring_.start(); }
	/// Data thread: the running cycle was cut short (a follower had not finished it when the next began).
	void onIncomplete() noexcept { ring_.incomplete(); }

	CutRing ring_;
	std::uint32_t clockId_;
	RealtimeListener listener_; // last: off the driver before the ring goes
};

} // namespace ear::pipewire
