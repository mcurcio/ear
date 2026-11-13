// The one test judge of lost cycles (docs/testing.md#the-cycle-judge): the only place a test may excuse a cycle its
// judged node missed. It is fed the facts of the node whose output the test judges (the last of a chain), one cycle
// at a time in the order they ran, and gives each a verdict: judged (nothing lost before it), excused (cycles the
// driver cut, exactly as many as were missed, the jump their frames and no further than the raw clock went), expected
// (a cause the test declared, up to the count it declared) or a failure. There is no window: a disturbance excuses
// its own cycles and not one more. It holds the stalls the test makes (1.5-cycle stalls by default), so every run
// proves its excuse path, and it is done only once the planned boundaries were judged and every planned stall was
// made and excused. Header-only (the target ear_cycle_judge); the test's thread only, reading ledgers the data thread
// filled.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "cycle_table.hpp" // describe(CycleFacts), for a failure's message
#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp"
#include "inducers.hpp"

namespace ear::test {

/// What a test plans: the boundaries it judges, the cuts it makes itself, and the causes it makes on purpose.
struct JudgePlan {
	/// Cycle boundaries to judge (verdict judge); 0 is refused.
	std::size_t judged = 0;
	/// The stalls the test's upstream code makes through CycleJudge::stalls(), each cutting its cycle short straight
	/// after it, so the judged node misses it; StallPlan{} to make none (a test that must not stall says why).
	StallPlan induce = StallPlan::every(50, 7, 4, StallLength::cycles(1.5));
	/// Causes the test makes itself, each up to its count: the judged node's first cycle, each drive, each card made a
	/// driver, each pause, each cycle too large for the block.
	struct Expected {
		std::uint32_t starts = 1, drives = 0, driverChanges = 0, absences = 0, oversized = 0;
	} expected{};
};

enum class Verdict : std::uint8_t {
	judge,    // nothing was lost before this cycle: the test judges the boundary into it
	excused,  // it follows cycles the driver cut, each counted
	expected, // a cause the test declared (with any cut cycles it follows excused as well)
	fail,     // anything else
};

/// What a judge saw, for a test's INFO and its one REQUIRE.
struct JudgeReport {
	std::size_t judged = 0, judgedPlanned = 0;
	std::size_t excusedCycles = 0; // cycles missed and excused as cut, each once
	std::size_t excusedEvents = 0; // verdicts that excused some: a stall that cut three cycles is one
	std::size_t expected = 0, failed = 0;
	std::size_t stallsMade = 0, stallsPlanned = 0;
	std::string firstFailure; // the cycle, its facts and the rule it broke
	/// Every planned boundary judged and every planned stall made and excused, with no failure.
	bool done() const noexcept {
		return judged >= judgedPlanned && stallsMade >= stallsPlanned && excusedCycles >= stallsPlanned;
	}
	bool ok() const noexcept { return failed == 0 && done(); }
	friend std::ostream& operator<<(std::ostream& out, const JudgeReport& report) {
		out << "judged " << report.judged << " of " << report.judgedPlanned << " boundaries; excused "
			<< report.excusedCycles << " cut cycles in " << report.excusedEvents << " verdicts; " << report.expected
			<< " expected; stalls made " << report.stallsMade << " of " << report.stallsPlanned << "; " << report.failed
			<< " failed";
		if (!report.firstFailure.empty()) out << " (the first: " << report.firstFailure << ")";
		return out;
	}
};

class CycleJudge {
public:
	/// Refuses (std::invalid_argument) a plan that judges nothing: nothing passes on nothing. `clock` is the graph's,
	/// for the raw-clock check; `cuts` (outliving the judge) answers blockMissing, and may be null for a judge fed
	/// facts only.
	CycleJudge(JudgePlan plan, FrameClock clock, const CutRecord* cuts)
		: plan_(checked(std::move(plan))), clock_(clock), cuts_(cuts), stalls_(plan_.induce) {}
	CycleJudge(const CycleJudge&) = delete;
	CycleJudge& operator=(const CycleJudge&) = delete;

	/// The stalls the test's upstream code calls in its process() (its index: the code's own count of its cycles).
	Stalls& stalls() noexcept { return stalls_; }

	/// One cycle of the judged node, of `frames`, in the order they ran.
	Verdict at(const CycleFacts& facts, std::uint32_t frames) {
		const std::size_t index = seen_++;
		if (facts.whole()) {
			++judged_;
			return Verdict::judge;
		}
		std::string broken;
		std::uint32_t excusing = 0;
		if (facts.causes.has(CycleCause::catchUp)) {
			// The driver cut every missed cycle, the position moved by exactly their frames, and no further than the raw
			// clock went: within the driver's 1 ms error clamp and two cycles' wake room (docs/engine.md#graphcycle).
			const std::int64_t whole = static_cast<std::int64_t>(facts.missed) * frames;
			if (!facts.confirmed()) broken = "a catch-up over cycles the driver did not all cut";
			else if (facts.missed == 0 || facts.jumpFrames != whole)
				broken = "a catch-up whose jump is not its missed cycles' frames";
			else if (clock_.ns(facts.jumpFrames) > facts.lostRawNs + kClampNs + clock_.ns(2 * std::int64_t{frames}))
				broken = "a catch-up further than the raw clock went";
			else excusing = facts.missed;
		}
		if (facts.causes.has(CycleCause::reposition)) broken = "a reposition, which nothing excuses";
		Expected used = used_;
		bool expecting = false;
		const auto declare = [&](CycleCause cause, std::uint32_t Expected::* count, const char* name) {
			if (!facts.causes.has(cause)) return;
			if (used.*count >= plan_.expected.*count) broken = std::string("more ") + name + " than the test declared";
			++(used.*count);
			expecting = true;
		};
		declare(CycleCause::start, &Expected::starts, "starts");
		declare(CycleCause::drive, &Expected::drives, "drives");
		declare(CycleCause::driverChange, &Expected::driverChanges, "driver changes");
		declare(CycleCause::absent, &Expected::absences, "absences");
		declare(CycleCause::oversized, &Expected::oversized, "oversized cycles");
		if (!broken.empty()) return fail("cycle " + std::to_string(index) + " (" + describe(facts) + "): " + broken);
		used_ = used;
		if (excusing > 0) {
			excusedCycles_ += excusing;
			++excusedEvents_;
		}
		if (expecting) {
			++expected_;
			return Verdict::expected;
		}
		return Verdict::excused;
	}

	/// A card's block the ramp wrote in `graphCycle` is missing from the card's file (docs/testing.md#the-cycle-judge):
	/// excused only if the driver cut that cycle.
	Verdict blockMissing(std::uint64_t graphCycle) {
		const std::optional<bool> cut = cuts_ != nullptr ? cuts_->cut(graphCycle) : std::nullopt;
		if (cut.value_or(false)) {
			++excusedCycles_;
			++excusedEvents_;
			return Verdict::excused;
		}
		return fail("a block of graph cycle " + std::to_string(graphCycle) + " missing from the card's file: " +
		            (cut ? "the driver did not cut it" : "no record answers for it"));
	}

	/// A card's block boundary heard on both sides in its file, in order: judged.
	Verdict blockHeard() noexcept {
		++judged_;
		return Verdict::judge;
	}

	bool done() const noexcept { return report().done(); }
	JudgeReport report() const {
		return JudgeReport{.judged = judged_,
		                   .judgedPlanned = plan_.judged,
		                   .excusedCycles = excusedCycles_,
		                   .excusedEvents = excusedEvents_,
		                   .expected = expected_,
		                   .failed = failed_,
		                   .stallsMade = stalls_.made(),
		                   .stallsPlanned = plan_.induce.size(),
		                   .firstFailure = firstFailure_};
	}

private:
	using Expected = JudgePlan::Expected;
	static constexpr std::int64_t kClampNs = 1'000'000; // node-driver.c's error clamp

	static JudgePlan checked(JudgePlan plan) {
		if (plan.judged == 0) throw std::invalid_argument("a cycle judge must judge some boundary");
		return plan;
	}
	Verdict fail(std::string what) {
		if (failed_++ == 0) firstFailure_ = std::move(what);
		return Verdict::fail;
	}

	JudgePlan plan_;
	FrameClock clock_;
	const CutRecord* cuts_;
	Stalls stalls_;
	Expected used_{0, 0, 0, 0, 0};
	std::size_t seen_ = 0, judged_ = 0, excusedCycles_ = 0, excusedEvents_ = 0, expected_ = 0, failed_ = 0;
	std::string firstFailure_;
};

} // namespace ear::test
