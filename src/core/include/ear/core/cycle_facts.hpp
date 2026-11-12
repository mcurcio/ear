// What each graph cycle states, and the one rule that states it (docs/writing-a-node-type.md#lost-cycles): CycleFacts,
// their causes, the record of which cycles the driver cut short (CutRecord, implemented where cuts are known) and
// CycleAccount, which works a block's facts out from its driver's ticks. The type harness's fake block runs it today,
// and the real block's discontinuity (CycleContinuity, ear/core/block_rules.hpp) is CycleAccount with no cuts, so the
// two cannot disagree.
// Pure and realtime-safe: nothing here locks, allocates or waits. A CycleAccount belongs to the one thread that runs
// its block's cycles.
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include "ear/core/latency.hpp"
namespace ear {

/// Why a cycle is a discontinuity: one bit each, as they coincide (a drive across a stall is both).
enum class CycleCause : std::uint8_t {
	start = 1U << 0U,        // the block's first cycle
	drive = 1U << 1U,        // a new code's first cycle (any drive, a drive of the same code again included)
	driverChange = 1U << 2U, // another driver than the last cycle's: another clock
	catchUp = 1U << 3U,      // missed cycles, every one cut, the position moved by exactly their frames
	absent = 1U << 4U,       // as catchUp, but some missed cycle was not cut: the block was not scheduled
	oversized = 1U << 5U,    // the cycle after one too large for the block (written as zeros, the code not run)
	reposition = 1U << 6U,   // the position moved on the same driver by anything else: back, or not whole cycles
};

/// A set of causes; empty is a cycle that follows its last with nothing lost.
class CycleCauses {
public:
	constexpr CycleCauses() noexcept = default;
	constexpr CycleCauses(CycleCause cause) noexcept : bits_(static_cast<std::uint8_t>(cause)) {} // implicit: one cause
	constexpr bool has(CycleCause cause) const noexcept { return (bits_ & static_cast<std::uint8_t>(cause)) != 0; }
	constexpr bool empty() const noexcept { return bits_ == 0; }
	constexpr CycleCauses operator|(CycleCauses other) const noexcept {
		return CycleCauses(static_cast<std::uint8_t>(bits_ | other.bits_));
	}
	/// Every cause in this set is in `allowed`.
	constexpr bool only(CycleCauses allowed) const noexcept { return (bits_ & ~allowed.bits_) == 0; }
	/// The set's bits, CycleCause's values or'd: for a test's message, never for a rule.
	constexpr std::uint8_t bits() const noexcept { return bits_; }
	friend constexpr bool operator==(CycleCauses, CycleCauses) noexcept = default;

private:
	explicit constexpr CycleCauses(std::uint8_t bits) noexcept : bits_(bits) {}
	std::uint8_t bits_ = 0;
};
constexpr CycleCauses operator|(CycleCause a, CycleCause b) noexcept { return CycleCauses(a) | b; }

/// What one cycle states, as the block that ran it accounts for it.
struct CycleFacts {
	std::uint64_t graphCycle = 0; // the internal clock's cycle number; 0: not on it, or not told (a test's own cycle)
	std::uint32_t missed = 0;     // cycles started on the same driver since the block last ran one
	std::uint32_t missedCut = 0;  // of them, cut short by the driver's own account
	bool lastCut = false;         // the cycle the block last ran was cut short after it
	std::int64_t jumpFrames = 0;  // position minus the last cycle's end, same driver (0 otherwise)
	std::int64_t lostRawNs = 0;   // raw time from the last cycle's end to this start, same driver (0 otherwise)
	CycleCauses causes{};
	/// Nothing was lost before this cycle.
	constexpr bool whole() const noexcept { return causes.empty(); }
	constexpr bool discontinuity() const noexcept { return !causes.empty(); }
	/// The driver cut every cycle the block missed.
	constexpr bool confirmed() const noexcept { return missed == missedCut; }
	friend constexpr bool operator==(const CycleFacts&, const CycleFacts&) noexcept = default;
};

/// How many of the newest cycles a CutRecord answers for, the running one's slot included: a cycle older than that has
/// no answer. One number for every record (the engine's and the harness's), so the harness states what the engine
/// would for the longest stalls too.
inline constexpr std::uint64_t cutRecordCycles = std::uint64_t{1} << 16U;

/// Which graph cycles the driver cut short. Implemented by the type harness's HarnessCuts (and, once built, the
/// engine's listener on the internal clock). Written only on the thread that runs the cycles.
class CutRecord {
public:
	/// Cycles started so far; inside a cycle, the number of the one running.
	virtual std::uint64_t started() const noexcept = 0;
	/// Whether cycle `cycle` was cut short; none for one still running, 0, one not started, or one older than the
	/// record keeps (cutRecordCycles).
	virtual std::optional<bool> cut(std::uint64_t cycle) const noexcept = 0;
	/// Cuts among cycles [from, to): how many were cut, and how many of them the record could answer for.
	struct Count {
		std::uint32_t cut = 0;
		std::uint32_t known = 0;
	};
	virtual Count count(std::uint64_t from, std::uint64_t to) const noexcept = 0;

protected:
	CutRecord() = default;
	CutRecord(const CutRecord&) = default;
	CutRecord& operator=(const CutRecord&) = default;
	~CutRecord() = default;
};

/// One cycle as the block's driver states it.
struct DriverTick {
	std::uint32_t driverId = 0;
	std::uint64_t graphCycle = 0; // CutRecord::started() at the cycle's entry; 0 when the block is not on the internal
								  // clock (or no record counts its cycles)
	std::uint64_t position = 0;
	std::uint32_t frames = 0;
	std::int64_t startRawNs = 0;
};

/// The one rule: each cycle's facts, and whether the code runs it. Its causes, in order: no last cycle, start; another
/// driver, driverChange (nothing else is worked out: another clock); else the jump against the last cycle's end and
/// the cycles missed (by the graph's cycle numbers, or, with none, by whole cycles of this one's frames in a forward
/// jump: ear forces one quantum, docs/engine.md#the-quantum-guard); missed cycles that moved the position by exactly
/// their frames, catchUp if the driver cut each one, else absent; any other jump, or cycles missed with no jump,
/// reposition; then drive and oversized as flags. The discontinuities are where CycleContinuity puts them
/// (docs/engine.md#graphcycle).
class CycleAccount {
public:
	/// `clock`: the graph's, for lostRawNs. `cuts` outlives the account; null: no cut is known (every missed cycle
	/// unconfirmed).
	CycleAccount(FrameClock clock, const CutRecord* cuts) noexcept : clock_(clock), cuts_(cuts) {}

	/// A drive installed a code: its first cycle has `drive`.
	void driven() noexcept { fresh_ = true; }

	struct Step {
		CycleFacts facts;
		bool runs; // at most maxBlock frames: the code runs it (a larger cycle is written as zeros)
	};
	/// One cycle, in the order cycles come.
	Step next(const DriverTick& tick, std::uint32_t maxBlock) noexcept {
		CycleFacts facts{.graphCycle = tick.graphCycle};
		if (cycles_ == 0) facts.causes = CycleCause::start;
		else if (tick.driverId != lastDriver_) facts.causes = CycleCause::driverChange;
		else sameDriver(tick, facts);
		if (cycles_ > 0 && lastGraphCycle_ != 0 && cuts_ != nullptr)
			facts.lastCut = cuts_->cut(lastGraphCycle_).value_or(false);
		if (fresh_) facts.causes = facts.causes | CycleCause::drive;
		if (skipped_) facts.causes = facts.causes | CycleCause::oversized;
		if (cycles_ > 0 && tick.driverId == lastDriver_ && facts.jumpFrames > 0) ++xruns_;
		fresh_ = false;
		skipped_ = tick.frames > maxBlock;
		++cycles_;
		lastDriver_ = tick.driverId;
		lastGraphCycle_ = tick.graphCycle;
		lastStartRawNs_ = tick.startRawNs;
		lastFrames_ = tick.frames;
		nextPosition_ = tick.position + tick.frames;
		return {facts, !skipped_};
	}
	/// Where the next cycle starts on the same driver: a drive applied now takes effect from it.
	std::uint64_t nextPosition() const noexcept { return nextPosition_; }
	/// Cycles that started ahead of where the last ended on the same driver (GraphCycle::xruns, as it counts them today:
	/// one a jump, however many cycles it missed).
	std::uint64_t xruns() const noexcept { return xruns_; }

private:
	void sameDriver(const DriverTick& tick, CycleFacts& facts) const noexcept {
		facts.jumpFrames = static_cast<std::int64_t>(tick.position - nextPosition_);
		facts.lostRawNs = tick.startRawNs - lastStartRawNs_ - clock_.ns(lastFrames_);
		if (tick.graphCycle != 0 && lastGraphCycle_ != 0) {
			if (tick.graphCycle > lastGraphCycle_) {
				const std::uint64_t missed = tick.graphCycle - lastGraphCycle_ - 1;
				facts.missed = clamped(missed);
				if (cuts_ != nullptr && missed > 0)
					facts.missedCut = cuts_->count(lastGraphCycle_ + 1, tick.graphCycle).cut;
			}
		} else if (facts.jumpFrames > 0 && tick.frames > 0 && facts.jumpFrames % tick.frames == 0) {
			const std::int64_t missed = facts.jumpFrames / tick.frames;
			facts.missed = clamped(missed);
		}
		if (facts.missed > 0 && facts.jumpFrames == std::int64_t{facts.missed} * tick.frames)
			facts.causes = facts.confirmed() ? CycleCause::catchUp : CycleCause::absent;
		else if (facts.jumpFrames != 0 || facts.missed > 0) facts.causes = CycleCause::reposition;
	}

	/// A count of cycles as a fact's width: past it (a stall of years) the fact says the most it can.
	template <class N> static constexpr std::uint32_t clamped(N count) noexcept {
		constexpr auto most = std::numeric_limits<std::uint32_t>::max();
		return count > N{most} ? most : static_cast<std::uint32_t>(count);
	}

	FrameClock clock_;
	const CutRecord* cuts_;
	std::uint64_t cycles_ = 0;
	std::uint32_t lastDriver_ = 0;
	std::uint64_t lastGraphCycle_ = 0;
	std::int64_t lastStartRawNs_ = 0;
	std::uint32_t lastFrames_ = 0;
	std::uint64_t nextPosition_ = 0;
	std::uint64_t xruns_ = 0;
	bool fresh_ = false;   // a drive installed a code since the last cycle
	bool skipped_ = false; // the last cycle was too large to run
};

} // namespace ear
