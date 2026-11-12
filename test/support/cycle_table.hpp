// The cycle facts' table (header-only; the target ear_cycle_table puts it on the include path): scripts of cycles, one
// row per cause and per shape of loss, each step with the facts the block must state for it
// (docs/writing-a-node-type.md#lost-cycles). ear_core_tests runs every row through CycleAccount, through
// CycleContinuity (the real block's discontinuity today, which must agree with every row's) and, for each row the type
// harness can make, through NodeHarness (src/core/tests/cycle_facts_test.cpp), so the rule, the block and the harness
// cannot drift apart.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"

namespace ear::test {

/// Every row runs at this format, with cycles of cycleTableFrames: 5 ms exactly, so each row's lostRawNs is exact.
inline constexpr AudioFormat cycleTableFormat{.rate = 48000, .maxBlock = 256};
inline constexpr std::uint32_t cycleTableFrames = 240;
/// The driver the rows start on, the harness's own clock (numbered cycles).
inline constexpr std::uint32_t cycleTableClock = 0;

/// One cycle the block runs, what happened before it, and the facts it must state.
struct CycleStep {
	bool drive = false;                // a drive applied before it (a row's first step always has one)
	std::uint32_t lostCut = 0;         // graph cycles before it the block missed, each cut short
	std::uint32_t lostUnscheduled = 0; // and those it missed that ran whole without it (paused)
	std::optional<std::uint32_t> driver =
		std::nullopt;            // it runs on this driver, and so do the rest (no graph cycle numbers but on
								 // cycleTableClock)
	std::int64_t reposition = 0; // its position moved by this on the same driver, no time gone
	std::uint32_t frames = cycleTableFrames;
	// What only a script of ticks makes (the harness cannot: a cycle over maxBlock is refused there, and it has no
	// way to cut the cycle the block ran or to number cycles without moving the clock).
	std::uint32_t numbersOnly = 0; // graph cycles counted before it, the position and the time not moved
	bool cutAfter = false;         // the driver cut it short after the block ran it
	CycleFacts facts;
	bool runs = true;
	/// The harness can make this step.
	bool inHarness() const noexcept { return frames <= cycleTableFormat.maxBlock && numbersOnly == 0 && !cutAfter; }
};

struct CycleRow {
	const char* what;
	std::vector<CycleStep> steps;
	bool inHarness() const noexcept {
		for (const CycleStep& step : steps)
			if (!step.inHarness()) return false;
		return true;
	}
};

/// The facts, for a failing check's message.
inline std::string describe(const CycleFacts& facts) {
	std::string causes;
	const std::pair<CycleCause, const char*> names[] = {{CycleCause::start, "start"},
	                                                    {CycleCause::drive, "drive"},
	                                                    {CycleCause::driverChange, "driverChange"},
	                                                    {CycleCause::catchUp, "catchUp"},
	                                                    {CycleCause::absent, "absent"},
	                                                    {CycleCause::oversized, "oversized"},
	                                                    {CycleCause::reposition, "reposition"}};
	for (const auto& [cause, name] : names)
		if (facts.causes.has(cause)) causes += (causes.empty() ? "" : "|") + std::string(name);
	return "graphCycle " + std::to_string(facts.graphCycle) + ", missed " + std::to_string(facts.missed) + ", cut " +
	       std::to_string(facts.missedCut) + ", lastCut " + (facts.lastCut ? "yes" : "no") + ", jump " +
	       std::to_string(facts.jumpFrames) + ", lostRawNs " + std::to_string(facts.lostRawNs) + ", causes " +
	       (causes.empty() ? "none" : causes);
}

inline std::vector<CycleRow> cycleRows() {
	using C = CycleCause;
	constexpr std::int64_t f = cycleTableFrames;
	constexpr std::int64_t ns = 5'000'000; // one cycle's time
	const auto first = [] {
		return CycleStep{.drive = true, .facts = {.graphCycle = 1, .causes = C::start | C::drive}};
	};
	const auto whole = [](std::uint64_t graphCycle) { return CycleStep{.facts = {.graphCycle = graphCycle}}; };
	return {
		{"the first cycle is start, the next whole, and a drive's first is drive",
	     {first(), whole(2), {.drive = true, .facts = {.graphCycle = 3, .causes = C::drive}}, whole(4)}},
		{"one cycle cut: catchUp, confirmed, then whole",
	     {first(),
	      whole(2),
	      {.lostCut = 1,
	       .facts =
	           {.graphCycle = 4, .missed = 1, .missedCut = 1, .jumpFrames = f, .lostRawNs = ns, .causes = C::catchUp}},
	      whole(5)}},
		{"three cycles cut: one catchUp of three",
	     {first(),
	      whole(2),
	      {.lostCut = 3,
	       .facts = {.graphCycle = 6,
	                 .missed = 3,
	                 .missedCut = 3,
	                 .jumpFrames = 3 * f,
	                 .lostRawNs = 3 * ns,
	                 .causes = C::catchUp}}}},
		{"a drive across a stall is both",
	     {first(),
	      whole(2),
	      {.drive = true,
	       .lostCut = 2,
	       .facts = {.graphCycle = 5,
	                 .missed = 2,
	                 .missedCut = 2,
	                 .jumpFrames = 2 * f,
	                 .lostRawNs = 2 * ns,
	                 .causes = C::drive | C::catchUp}}}},
		// The record answers for the cutRecordCycles - 1 cycles before the running one, so the oldest go unconfirmed.
		{"70,000 cycles cut, past what the record keeps: absent, the known ones counted",
	     {first(),
	      whole(2),
	      {.lostCut = 70'000,
	       .facts = {.graphCycle = 70'003,
	                 .missed = 70'000,
	                 .missedCut = static_cast<std::uint32_t>(cutRecordCycles - 1),
	                 .jumpFrames = 70'000 * f,
	                 .lostRawNs = 70'000 * ns,
	                 .causes = C::absent}}}},
		{"a pause: cycles missed and none cut is absent",
	     {first(),
	      whole(2),
	      {.lostUnscheduled = 2,
	       .facts = {.graphCycle = 5, .missed = 2, .jumpFrames = 2 * f, .lostRawNs = 2 * ns, .causes = C::absent}},
	      whole(6)}},
		{"cycles missed, some cut and one not: absent",
	     {first(),
	      whole(2),
	      {.lostCut = 2,
	       .lostUnscheduled = 1,
	       .facts = {.graphCycle = 6,
	                 .missed = 3,
	                 .missedCut = 2,
	                 .jumpFrames = 3 * f,
	                 .lostRawNs = 3 * ns,
	                 .causes = C::absent}}}},
		{"another driver is driverChange, unnumbered, its losses never confirmed, and back again",
	     {first(),
	      whole(2),
	      {.driver = 5, .facts = {.causes = C::driverChange}},
	      {.facts = {}},
	      {.lostCut = 2, .facts = {.missed = 2, .jumpFrames = 2 * f, .lostRawNs = 2 * ns, .causes = C::absent}},
	      {.driver = cycleTableClock, .facts = {.graphCycle = 8, .causes = C::driverChange}},
	      whole(9)}},
		{"a cycle too large to run is written as zeros, and the next is oversized",
	     {first(),
	      whole(2),
	      {.frames = 512, .facts = {.graphCycle = 3}, .runs = false},
	      {.facts = {.graphCycle = 4, .causes = C::oversized}},
	      whole(5)}},
		{"the position stepping back is reposition",
	     {first(),
	      whole(2),
	      {.reposition = -100,
	       .facts = {.graphCycle = 3, .jumpFrames = -100, .lostRawNs = 0, .causes = C::reposition}}}},
		{"the position moving by a fraction of a cycle is reposition",
	     {first(),
	      whole(2),
	      {.reposition = 100, .facts = {.graphCycle = 3, .jumpFrames = 100, .causes = C::reposition}}}},
		{"the position moving by whole cycles with none missed is reposition",
	     {first(),
	      whole(2),
	      {.reposition = 2 * f, .facts = {.graphCycle = 3, .jumpFrames = 2 * f, .causes = C::reposition}}}},
		{"cycles missed with the position not moved is reposition",
	     {first(), whole(2), {.numbersOnly = 2, .facts = {.graphCycle = 5, .missed = 2, .causes = C::reposition}}}},
		{"a cycle cut after the block ran it: the next is whole and lastCut",
	     {first(),
	      {.cutAfter = true, .facts = {.graphCycle = 2}},
	      {.facts = {.graphCycle = 3, .lastCut = true}},
	      whole(4)}},
	};
}

} // namespace ear::test
