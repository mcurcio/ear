// The cycle facts' table (header-only; the target ear_cycle_table puts it on the include path): scripts of cycles, one
// row per cause and per shape of loss, each step with the facts the block must state for it
// (docs/writing-a-node-type.md#lost-cycles). ear_core_tests runs every row through
// CycleAccount and, for each row the type harness can make, through NodeHarness (src/core/tests/cycle_facts_test.cpp);
// ear_pipewire_tests runs each row an engine can make through a real block (src/pipewire/tests/port_block_test.cpp), so
// the rule, the block and the harness cannot drift apart.
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
/// The most cycles a pause an engine makes stands for: a test's pause lasts a few cycles, never minutes.
inline constexpr std::uint32_t cycleTableEnginePause = 16;

/// One cycle the block runs, what happened before it, and the facts it must state.
struct CycleStep {
	bool drive = false;                // a drive applied before it (a row's first step always has one)
	std::uint32_t lostCut = 0;         // graph cycles before it the block missed, each cut short
	std::uint32_t lostUnscheduled = 0; // and those it missed that ran whole without it (paused)
	// It runs on this driver, and so do the rest (no graph cycle numbers but on cycleTableClock).
	std::optional<std::uint32_t> driver = std::nullopt;
	std::int64_t reposition = 0; // its position moved by this on the same driver, no time gone
	std::uint32_t frames = cycleTableFrames;
	// What only a script of ticks makes (the harness cannot: a cycle over maxBlock or of no frames is refused or means
	// maxBlock there, its lost cycles have the next cycle's frames, and it has no way to cut the cycle the block ran
	// or to number cycles without moving the clock).
	std::optional<std::uint32_t> lostFrames = std::nullopt; // the lost cycles' frames (none: this cycle's)
	std::uint32_t numbersOnly = 0; // graph cycles counted before it, the position and the time not moved
	bool cutAfter = false;         // the driver cut it short after the block ran it
	CycleFacts facts;
	bool runs = true;
	/// The harness can make this step.
	bool inHarness() const noexcept {
		return frames > 0 && frames <= cycleTableFormat.maxBlock && !lostFrames && numbersOnly == 0 && !cutAfter;
	}
	/// An engine can make this step through a real block on the internal clock, at the graph's one quantum
	/// (cycleTableFrames, docs/engine.md#the-quantum-guard): a drive; one cycle cut, by a stall of the block upstream
	/// (a 1.5-cycle stall cuts exactly the cycle it stalls in, docs/engine.md#the-one-data-loop-and-invokes; a longer
	/// one leaves how many the catch-up cuts to the loop's order); a cut after the block ran its cycle, by a stall of
	/// its own with a block after it; or a pause, the block made inactive, which misses as many cycles as the engine
	/// took to make it active again (at least lostUnscheduled, its facts scaled to them). Never two of those at once,
	/// another driver, a reposition or a number without a move: the dummy always drives and moves on by its frames.
	bool onEngine() const noexcept {
		const int losses = (lostCut > 0 ? 1 : 0) + (lostUnscheduled > 0 ? 1 : 0) + (drive ? 1 : 0);
		return frames == cycleTableFrames && !lostFrames && numbersOnly == 0 && !driver && reposition == 0 &&
		       lostCut <= 1 && lostUnscheduled <= cycleTableEnginePause && losses <= 1;
	}
};

struct CycleRow {
	const char* what;
	std::vector<CycleStep> steps;
	bool inHarness() const noexcept {
		for (const CycleStep& step : steps)
			if (!step.inHarness()) return false;
		return true;
	}
	bool onEngine() const noexcept {
		for (const CycleStep& step : steps)
			if (!step.onEngine()) return false;
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
	       std::to_string(facts.missedCut) + ", lastCut " +
	       (facts.lastCut ? (*facts.lastCut ? "yes" : "no") : "unknown") + ", jump " +
	       std::to_string(facts.jumpFrames) + ", lostRawNs " + std::to_string(facts.lostRawNs) + ", causes " +
	       (causes.empty() ? "none" : causes);
}

inline std::vector<CycleRow> cycleRows() {
	using C = CycleCause;
	constexpr std::int64_t f = cycleTableFrames;
	constexpr std::int64_t ns = 5'000'000; // one cycle's time
	constexpr std::optional<bool> unknown;
	const auto first = [] {
		return CycleStep{.drive = true, .facts = {.graphCycle = 1, .lastCut = false, .causes = C::start | C::drive}};
	};
	const auto whole = [](std::uint64_t graphCycle) {
		return CycleStep{.facts = {.graphCycle = graphCycle, .lastCut = false}};
	};
	return {
		{"the first cycle is start, the next whole, and a drive's first is drive",
	     {first(),
	      whole(2),
	      {.drive = true, .facts = {.graphCycle = 3, .lastCut = false, .causes = C::drive}},
	      whole(4)}},
		{"one cycle cut: catchUp, confirmed, then whole",
	     {first(),
	      whole(2),
	      {.lostCut = 1,
	       .facts = {.graphCycle = 4,
	                 .missed = 1,
	                 .missedCut = 1,
	                 .lastCut = false,
	                 .jumpFrames = f,
	                 .lostRawNs = ns,
	                 .causes = C::catchUp}},
	      whole(5)}},
		{"three cycles cut: one catchUp of three",
	     {first(),
	      whole(2),
	      {.lostCut = 3,
	       .facts = {.graphCycle = 6,
	                 .missed = 3,
	                 .missedCut = 3,
	                 .lastCut = false,
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
	                 .lastCut = false,
	                 .jumpFrames = 2 * f,
	                 .lostRawNs = 2 * ns,
	                 .causes = C::drive | C::catchUp}}}},
		// The counts have no reach: a stall past the ring is confirmed as any other.
		{"70,000 cycles cut, past the ring's reach: catchUp, every one counted",
	     {first(),
	      whole(2),
	      {.lostCut = 70'000,
	       .facts = {.graphCycle = 70'003,
	                 .missed = 70'000,
	                 .missedCut = 70'000,
	                 .lastCut = false,
	                 .jumpFrames = 70'000 * f,
	                 .lostRawNs = 70'000 * ns,
	                 .causes = C::catchUp}}}},
		{"a pause: cycles missed and none cut is absent",
	     {first(),
	      whole(2),
	      {.lostUnscheduled = 2,
	       .facts = {.graphCycle = 5,
	                 .missed = 2,
	                 .lastCut = false,
	                 .jumpFrames = 2 * f,
	                 .lostRawNs = 2 * ns,
	                 .causes = C::absent}},
	      whole(6)}},
		{"a pause past the ring's reach: absent, nothing cut, the last cycle known whole",
	     {first(),
	      whole(2),
	      {.lostUnscheduled = 70'000,
	       .facts = {.graphCycle = 70'003,
	                 .missed = 70'000,
	                 .lastCut = false,
	                 .jumpFrames = 70'000 * f,
	                 .lostRawNs = 70'000 * ns,
	                 .causes = C::absent}}}},
		// The one undecided case: the ring no longer keeps the last cycle, and the cuts since it could be its or not.
		{"past the ring's reach with one cycle not cut: absent, lastCut unknown, missedCut at least one fewer",
	     {first(),
	      whole(2),
	      {.lostCut = 69'999,
	       .lostUnscheduled = 1,
	       .facts = {.graphCycle = 70'003,
	                 .missed = 70'000,
	                 .missedCut = 69'998,
	                 .lastCut = unknown,
	                 .jumpFrames = 70'000 * f,
	                 .lostRawNs = 70'000 * ns,
	                 .causes = C::absent}}}},
		{"cycles missed, some cut and one not: absent",
	     {first(),
	      whole(2),
	      {.lostCut = 2,
	       .lostUnscheduled = 1,
	       .facts = {.graphCycle = 6,
	                 .missed = 3,
	                 .missedCut = 2,
	                 .lastCut = false,
	                 .jumpFrames = 3 * f,
	                 .lostRawNs = 3 * ns,
	                 .causes = C::absent}}}},
		{"another driver is driverChange and unnumbered: its losses absent, its repositions read from the position",
	     {first(),
	      whole(2),
	      {.driver = 5, .facts = {.lastCut = false, .causes = C::driverChange}},
	      {.facts = {}},
	      {.lostCut = 2, .facts = {.missed = 2, .jumpFrames = 2 * f, .lostRawNs = 2 * ns, .causes = C::absent}},
	      {.reposition = 2 * f, .facts = {.missed = 2, .jumpFrames = 2 * f, .causes = C::absent}},
	      {.reposition = 100, .facts = {.jumpFrames = 100, .causes = C::reposition}},
	      {.driver = cycleTableClock, .facts = {.graphCycle = 10, .causes = C::driverChange}},
	      whole(11)}},
		{"a driver change in the same step as a loss is driverChange alone",
	     {first(), whole(2), {.lostCut = 2, .driver = 5, .facts = {.lastCut = false, .causes = C::driverChange}}}},
		{"a cycle too large to run is written as zeros, and the next is oversized",
	     {first(),
	      whole(2),
	      {.frames = 512, .facts = {.graphCycle = 3, .lastCut = false}, .runs = false},
	      {.facts = {.graphCycle = 4, .lastCut = false, .causes = C::oversized}},
	      whole(5)}},
		{"a drive applied before a cycle too large to run: drive there, oversized after",
	     {first(),
	      whole(2),
	      {.drive = true,
	       .frames = 512,
	       .facts = {.graphCycle = 3, .lastCut = false, .causes = C::drive},
	       .runs = false},
	      {.facts = {.graphCycle = 4, .lastCut = false, .causes = C::oversized}}}},
		{"the position stepping back is reposition",
	     {first(),
	      whole(2),
	      {.reposition = -100,
	       .facts = {.graphCycle = 3, .lastCut = false, .jumpFrames = -100, .causes = C::reposition}}}},
		{"the position moving by a fraction of a cycle is reposition",
	     {first(),
	      whole(2),
	      {.reposition = 100,
	       .facts = {.graphCycle = 3, .lastCut = false, .jumpFrames = 100, .causes = C::reposition}}}},
		{"the position moving by whole cycles with none missed is reposition",
	     {first(),
	      whole(2),
	      {.reposition = 2 * f,
	       .facts = {.graphCycle = 3, .lastCut = false, .jumpFrames = 2 * f, .causes = C::reposition}}}},
		{"cycles missed with the position not moved is reposition",
	     {first(),
	      whole(2),
	      {.numbersOnly = 2, .facts = {.graphCycle = 5, .missed = 2, .lastCut = false, .causes = C::reposition}}}},
		{"a cycle cut after the block ran it: the next is whole and lastCut",
	     {first(),
	      {.cutAfter = true, .facts = {.graphCycle = 2, .lastCut = false}},
	      {.facts = {.graphCycle = 3, .lastCut = true}},
	      whole(4)}},
		{"a cut after the block ran its cycle, then a stall: catchUp and lastCut",
	     {first(),
	      {.cutAfter = true, .facts = {.graphCycle = 2, .lastCut = false}},
	      {.lostCut = 2,
	       .facts = {.graphCycle = 5,
	                 .missed = 2,
	                 .missedCut = 2,
	                 .lastCut = true,
	                 .jumpFrames = 2 * f,
	                 .lostRawNs = 2 * ns,
	                 .causes = C::catchUp}}}},
		{"a cycle of no frames after a cut: no move, so reposition",
	     {first(),
	      whole(2),
	      {.lostCut = 1,
	       .frames = 0,
	       .facts = {.graphCycle = 4, .missed = 1, .missedCut = 1, .lastCut = false, .causes = C::reposition}},
	      whole(5)}},
		// ear forces one quantum, so this is never made on the engine; the rule must fail toward reposition.
		{"the quantum changed across a loss: not whole cycles of the new frames, so reposition",
	     {first(),
	      whole(2),
	      {.lostCut = 1,
	       .frames = 256,
	       .lostFrames = cycleTableFrames,
	       .facts = {.graphCycle = 4,
	                 .missed = 1,
	                 .missedCut = 1,
	                 .lastCut = false,
	                 .jumpFrames = f,
	                 .lostRawNs = ns,
	                 .causes = C::reposition}}}},
	};
}

} // namespace ear::test
