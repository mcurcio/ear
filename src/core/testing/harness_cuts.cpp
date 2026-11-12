// The type harness's cut record (ear/testing/harness_cuts.hpp), by CutRecord's contract (ear/core/cycle_facts.hpp).
#include "ear/testing/harness_cuts.hpp"

#include <cstddef>

namespace ear::testing {
namespace {
constexpr std::uint64_t wordBits = 64;
constexpr std::uint64_t bit(std::uint64_t cycle) noexcept { return std::uint64_t{1} << (cycle % wordBits); }
constexpr std::size_t word(std::uint64_t cycle) noexcept {
	return static_cast<std::size_t>(cycle % cutRecordCycles / wordBits);
}
} // namespace

HarnessCuts::HarnessCuts() : bits_(cutRecordCycles / wordBits) {}

bool HarnessCuts::bitOf(std::uint64_t cycle) const noexcept {
	return (bits_[word(cycle)].load(std::memory_order_relaxed) & bit(cycle)) != 0;
}

void HarnessCuts::start() noexcept {
	const std::uint64_t running = started_.load(std::memory_order_relaxed);
	if (running > 0 && !bitOf(running)) newestWhole_.store(running, std::memory_order_release);
	started_.store(running + 1, std::memory_order_release);
	// The slot is cleared after started() moved on, so a reader that sees it cleared sees the new started() too.
	std::atomic_thread_fence(std::memory_order_release);
	bits_[word(running + 1)].fetch_and(~bit(running + 1), std::memory_order_relaxed);
}

void HarnessCuts::incomplete() noexcept {
	const std::uint64_t running = started_.load(std::memory_order_relaxed);
	if (running == 0) return;
	const std::uint64_t was = bits_[word(running)].fetch_or(bit(running), std::memory_order_relaxed);
	if ((was & bit(running)) == 0)
		cutsSoFar_.store(cutsSoFar_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

std::optional<bool> HarnessCuts::cut(std::uint64_t cycle) const noexcept {
	if (!cutRecordKeeps(cycle, started_.load(std::memory_order_acquire))) return std::nullopt;
	const bool was = bitOf(cycle);
	std::atomic_thread_fence(std::memory_order_acquire);
	if (!cutRecordKeeps(cycle, started_.load(std::memory_order_relaxed))) return std::nullopt;
	return was;
}

} // namespace ear::testing
