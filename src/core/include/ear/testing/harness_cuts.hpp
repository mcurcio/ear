// The type harness's record of which cycles the driver cut short (docs/writing-a-node-type.md#lost-cycles): the
// harness's fake block reads it through its CycleAccount, and a test that runs CycleAccount by hand may keep one too.
// It is told as the engine's driver tells: a cycle starts, then may be marked cut before the next starts.
#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "ear/core/cycle_facts.hpp"
namespace ear::testing {

/// Keeps the newest cutRecordCycles cycles, as every CutRecord does, so the harness states what the engine would for a
/// stall longer than that too. One thread: the test's.
class HarnessCuts final : public CutRecord {
public:
	HarnessCuts();
	/// A cycle starts: started() moves on to it, and it is not cut.
	void start() noexcept;
	/// The running cycle (started()) was cut short. Nothing before the first start.
	void incomplete() noexcept;

	std::uint64_t started() const noexcept override { return started_; }
	std::optional<bool> cut(std::uint64_t cycle) const noexcept override;
	Count count(std::uint64_t from, std::uint64_t to) const noexcept override;

private:
	std::vector<std::uint64_t> bits_; // cutRecordCycles bits, cycle c at bit c % cutRecordCycles
	std::uint64_t started_ = 0;
};

} // namespace ear::testing
