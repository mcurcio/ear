// The type harness's cut record (ear/testing/harness_cuts.hpp).
#include "ear/testing/harness_cuts.hpp"

#include <algorithm>
#include <cstddef>

namespace ear::testing {
namespace {
constexpr std::uint64_t wordBits = 64;
constexpr std::uint64_t bit(std::uint64_t cycle) noexcept { return std::uint64_t{1} << (cycle % wordBits); }
constexpr std::size_t word(std::uint64_t cycle) noexcept {
	return static_cast<std::size_t>(cycle % cutRecordCycles / wordBits);
}
} // namespace

HarnessCuts::HarnessCuts() : bits_(cutRecordCycles / wordBits, 0) {}

void HarnessCuts::start() noexcept {
	++started_;
	bits_[word(started_)] &= ~bit(started_);
}

void HarnessCuts::incomplete() noexcept {
	if (started_ > 0) bits_[word(started_)] |= bit(started_);
}

std::optional<bool> HarnessCuts::cut(std::uint64_t cycle) const noexcept {
	// The running cycle's slot is the oldest's too, so the record answers for the cutRecordCycles - 1 before it.
	if (cycle == 0 || cycle >= started_ || started_ - cycle >= cutRecordCycles) return std::nullopt;
	return (bits_[word(cycle)] & bit(cycle)) != 0;
}

CutRecord::Count HarnessCuts::count(std::uint64_t from, std::uint64_t to) const noexcept {
	Count counted;
	// Only the cycles the record keeps can answer, so a span of a long stall walks the record at most once.
	if (started_ >= cutRecordCycles) from = std::max(from, started_ - cutRecordCycles + 1);
	to = std::min(to, started_);
	for (std::uint64_t cycle = from; cycle < to; ++cycle)
		if (const std::optional<bool> was = cut(cycle)) {
			++counted.known;
			if (*was) ++counted.cut;
		}
	return counted;
}

} // namespace ear::testing
