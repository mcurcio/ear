// The type harness's record of which cycles the driver cut short (docs/writing-a-node-type.md#lost-cycles): the
// harness's fake block reads it through its CycleAccount, and a test that runs CycleAccount by hand may keep one too.
// It is told as the engine's driver tells: a cycle starts, then may be marked cut before the next starts.
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>
#include "ear/core/cycle_facts.hpp"
namespace ear::testing {

/// Keeps the counts and a ring of cutRecordCycles slots, as every CutRecord does, written and read by CutRecord's
/// contract (its stores and loads as that contract orders them), so a test may read it from another thread while
/// one thread tells it cycles.
class HarnessCuts final : public CutRecord {
public:
	HarnessCuts();
	/// A cycle starts: newestWhole moves to the one before it if that was not cut, started() moves on, and the new
	/// cycle's slot is cleared.
	void start() noexcept;
	/// The running cycle (started()) was cut short: cutsSoFar moves on, once however often it is told. Nothing before
	/// the first start.
	void incomplete() noexcept;

	std::uint64_t started() const noexcept override { return started_.load(std::memory_order_acquire); }
	std::optional<bool> cut(std::uint64_t cycle) const noexcept override;
	std::uint64_t cutsSoFar() const noexcept override { return cutsSoFar_.load(std::memory_order_acquire); }
	std::uint64_t newestWhole() const noexcept override { return newestWhole_.load(std::memory_order_acquire); }

private:
	bool bitOf(std::uint64_t cycle) const noexcept;
	std::vector<std::atomic<std::uint64_t>> bits_; // cutRecordCycles bits, cycle c at bit c % cutRecordCycles
	std::atomic<std::uint64_t> started_{0};
	std::atomic<std::uint64_t> cutsSoFar_{0};
	std::atomic<std::uint64_t> newestWhole_{0};
};

} // namespace ear::testing
