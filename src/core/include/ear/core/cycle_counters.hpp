// The diagnostics counters (docs/engine.md#counters): the graph's, which say what the host did to every output at once,
// and a node's, which say what happened to that node beyond the graph's own losses. Each is a running total of a fact
// stated once elsewhere and counted where it is stated, never worked out again: the graph's from the record of the
// internal clock's cuts (ear/core/cut_ring.hpp), a node's from its block's CycleFacts (ear/core/cycle_facts.hpp) and
// the block's own timing of its work. Pure and realtime-safe: counting locks, allocates and waits on nothing.
#pragma once
#include <atomic>
#include <cstdint>
#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp"
namespace ear {

/// A node's counters, a value: running totals from the node's add (docs/engine.md#counters).
struct NodeCounts {
	std::uint64_t missedCycles = 0;  // cycles its block missed that the driver did not cut (not scheduled)
	std::uint64_t overruns = 0;      // cycles in which its block's own work took longer than the cycle
	std::uint64_t repositions = 0;   // cycles stating `reposition`
	std::uint64_t driverChanges = 0; // cycles stating `driverChange`
	/// Two spans' totals together: NodeHost adds what a block that went counted to what the node's block counts now.
	friend constexpr NodeCounts operator+(NodeCounts a, NodeCounts b) noexcept {
		return NodeCounts{.missedCycles = a.missedCycles + b.missedCycles,
		                  .overruns = a.overruns + b.overruns,
		                  .repositions = a.repositions + b.repositions,
		                  .driverChanges = a.driverChanges + b.driverChanges};
	}
	friend constexpr bool operator==(const NodeCounts&, const NodeCounts&) noexcept = default;
};

/// The graph's counters, a value: running totals from the engine's start (docs/engine.md#counters).
struct GraphCounts {
	std::uint64_t cycles = 0;    // graph cycles the internal clock started
	std::uint64_t cutCycles = 0; // of them, cut short by the driver's own account
	std::uint64_t stalls = 0;    // runs of consecutive cut cycles: one stall of the host however many cycles it took
	/// The time the graph lost, encoded from cutCycles and never kept: each cut cycle lost one block of `quantum`
	/// frames, as ear forces one quantum (docs/engine.md#the-quantum-guard).
	constexpr std::uint64_t lostMs(FrameClock clock, std::uint32_t quantum) const noexcept {
		const std::int64_t ns = clock.ns(static_cast<std::int64_t>(cutCycles) * std::int64_t{quantum});
		return static_cast<std::uint64_t>(ns / 1'000'000);
	}
	friend constexpr bool operator==(const GraphCounts&, const GraphCounts&) noexcept = default;
};

/// The one rule for an overrun, which the engine's port block and the type harness's fake block both call: the block's
/// own work, timed from its entry to its end (so a late wake, the host's, is never the block's), took longer than the
/// cycle's `frames` last at the graph's rate.
constexpr bool cycleOverran(std::int64_t workNs, FrameClock clock, std::uint32_t frames) noexcept {
	return workNs > clock.ns(std::int64_t{frames});
}

/// One block's counters as its cycles state them. One writer, the thread that runs the block's cycles, which adds each
/// cycle once its work is done; any thread reads. Each counter is an atomic its one writer loads and stores relaxed (no
/// read-modify-write is needed with one writer) and a reader loads relaxed: a node's counters are independent, so no
/// reader needs two to agree. Allocates nothing.
class CycleCounters {
public:
	/// The writer, once per cycle its block's process sees (a cycle too large to run included), after the block's work:
	/// the cycle's facts, and whether that work overran (cycleOverran).
	void add(const CycleFacts& facts, bool overran) noexcept {
		// missedCut is at most missed; where it is a lower bound (CycleFacts::lastCut none) this counts at most one more.
		if (facts.missed > facts.missedCut) bump(missedCycles_, facts.missed - facts.missedCut);
		if (overran) bump(overruns_, 1);
		if (facts.causes.has(CycleCause::reposition)) bump(repositions_, 1);
		if (facts.causes.has(CycleCause::driverChange)) bump(driverChanges_, 1);
	}
	/// Any thread.
	NodeCounts read() const noexcept {
		return NodeCounts{.missedCycles = missedCycles_.load(std::memory_order_relaxed),
		                  .overruns = overruns_.load(std::memory_order_relaxed),
		                  .repositions = repositions_.load(std::memory_order_relaxed),
		                  .driverChanges = driverChanges_.load(std::memory_order_relaxed)};
	}

private:
	static void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by) noexcept {
		counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
	}
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "a block's counters are written on the data thread");
	std::atomic<std::uint64_t> missedCycles_{0};
	std::atomic<std::uint64_t> overruns_{0};
	std::atomic<std::uint64_t> repositions_{0};
	std::atomic<std::uint64_t> driverChanges_{0};
};

} // namespace ear
