// The record of the driver's cuts (ear/core/cut_ring.hpp), by CutRecord's contract (ear/core/cycle_facts.hpp).
#include "ear/core/cut_ring.hpp"

#include <cstddef>

namespace ear {
namespace {
constexpr std::uint64_t wordBits = 64;
constexpr std::uint64_t bit(std::uint64_t cycle) noexcept { return std::uint64_t{1} << (cycle % wordBits); }
constexpr std::size_t word(std::uint64_t cycle) noexcept {
	return static_cast<std::size_t>(cycle % cutRecordCycles / wordBits);
}
} // namespace

CutRing::CutRing() : bits_(cutRecordCycles / wordBits) {}

bool CutRing::bitOf(std::uint64_t cycle) const noexcept {
	return (bits_[word(cycle)].load(std::memory_order_relaxed) & bit(cycle)) != 0;
}

void CutRing::start() noexcept {
	const std::uint64_t running = started_.load(std::memory_order_relaxed);
	if (running > 0 && !bitOf(running)) newestWhole_.store(running, std::memory_order_release);
	started_.store(running + 1, std::memory_order_release);
	// The slot is cleared after started() moved on, so a reader that sees it cleared sees the new started() too.
	std::atomic_thread_fence(std::memory_order_release);
	bits_[word(running + 1)].fetch_and(~bit(running + 1), std::memory_order_relaxed);
}

void CutRing::incomplete() noexcept {
	const std::uint64_t running = started_.load(std::memory_order_relaxed);
	if (running == 0) return;
	const std::uint64_t was = bits_[word(running)].fetch_or(bit(running), std::memory_order_relaxed);
	if ((was & bit(running)) != 0) return;
	cutsSoFar_.store(cutsSoFar_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	// A cut while the graph is still behind from an earlier one (no cycle since started on time) is the same stall of
	// the host, whatever the burst's cycles between were cut or ran whole (docs/engine.md#counters).
	if (stalling_) return;
	stalling_ = true;
	stalls_.store(stalls_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

GraphCounts CutRing::counts() const noexcept {
	const std::uint64_t stalls = stalls_.load(std::memory_order_acquire);
	const std::uint64_t cuts = cutsSoFar_.load(std::memory_order_acquire);
	return GraphCounts{.cycles = started_.load(std::memory_order_acquire), .cutCycles = cuts, .stalls = stalls};
}

std::optional<bool> CutRing::cut(std::uint64_t cycle) const noexcept {
	if (!cutRecordKeeps(cycle, started_.load(std::memory_order_acquire))) return std::nullopt;
	const bool was = bitOf(cycle);
	std::atomic_thread_fence(std::memory_order_acquire);
	if (!cutRecordKeeps(cycle, started_.load(std::memory_order_relaxed))) return std::nullopt;
	return was;
}

} // namespace ear
