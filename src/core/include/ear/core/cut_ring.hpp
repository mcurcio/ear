// The one record of which graph cycles the driver cut short (docs/writing-a-node-type.md#lost-cycles): the engine's,
// which ear_pipewire tells from the internal clock's own events (docs/engine.md#graphcycle), and the type harness's
// (ear/testing/harness_cuts.hpp) are this class, so the harness answers as the engine does by construction. Told as
// the driver tells: a cycle starts, then the running one may be marked cut before the next starts.
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>
#include "ear/core/cycle_counters.hpp"
#include "ear/core/cycle_facts.hpp"
namespace ear {

/// Keeps the counts and a ring of cutRecordCycles slots, written and read by CutRecord's contract (its stores and loads
/// as that contract orders them), so any thread may read it while one thread tells it cycles. Made (its ring
/// allocated) before the first cycle; start(), incomplete() and onTime() allocate nothing, lock nothing and wait on nothing, so
/// the data thread tells them.
class CutRing final : public CutRecord {
public:
	CutRing();
	CutRing(const CutRing&) = delete;
	CutRing& operator=(const CutRing&) = delete;
	~CutRing() = default;
	/// A cycle starts: newestWhole moves to the one before it if that was not cut, started() moves on, and the new
	/// cycle's slot is cleared.
	void start() noexcept;
	/// The running cycle (started()) was cut short: cutsSoFar moves on, once however often it is told, and the count of
	/// stalls too unless a stall is in progress (none has ended since the last cut). Nothing before the first start.
	void incomplete() noexcept;
	/// The running cycle started on time: as it began, the next cycle was not yet due (GraphCycle::overdueNs 0), so the
	/// graph is no longer behind and a stall in progress has ended; the next cut is a new stall. Told inside the cycle,
	/// on the thread that tells the rest, by whoever states the cycle's overdue time (a port block, the harness), any
	/// number of times. Until it is told, the cuts of a catch-up's burst are one stall, whether its cycles between ran
	/// whole or not.
	void onTime() noexcept { stalling_ = false; }

	std::uint64_t started() const noexcept override { return started_.load(std::memory_order_acquire); }
	std::optional<bool> cut(std::uint64_t cycle) const noexcept override;
	std::uint64_t cutsSoFar() const noexcept override { return cutsSoFar_.load(std::memory_order_acquire); }
	std::uint64_t newestWhole() const noexcept override { return newestWhole_.load(std::memory_order_acquire); }
	/// Any thread: the graph's counters (docs/engine.md#counters), each the record's own count. Read in the reverse of
	/// the order the writer stores them (stalls, then cuts, then cycles, each acquire against its release store), so any
	/// reading has stalls <= cutCycles <= cycles.
	GraphCounts counts() const noexcept;

private:
	bool bitOf(std::uint64_t cycle) const noexcept;
	std::vector<std::atomic<std::uint64_t>> bits_; // cutRecordCycles bits, cycle c at bit c % cutRecordCycles
	std::atomic<std::uint64_t> started_{0};
	std::atomic<std::uint64_t> cutsSoFar_{0};
	std::atomic<std::uint64_t> newestWhole_{0};
	std::atomic<std::uint64_t> stalls_{0}; // stalls of the host: cuts made while none was in progress
	bool stalling_ = false;                // a stall is in progress: no cycle started on time since its first cut
};

} // namespace ear
