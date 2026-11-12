// Cycle facts (docs/writing-a-node-type.md#lost-cycles): the cycle table (test/support/cycle_table.hpp) through the one
// rule, CycleAccount, over a cut record; and through the type harness, whose loseCycles, changeDriver and reposition
// make each row it can (ear_pipewire_tests runs the rows an engine can make through a real block). Then the pieces: the
// causes as a set, the cut record (the engine's and the harness's, CutRing), and an account with no record.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "cycle_table.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/testing/harness_cuts.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace cycle_facts_test {

using namespace ear;

constexpr auto out = bank::output<"out">;
constexpr TypeSpec spec{.settings = SettingsTable<>{"cycles"}, .banks = Banks<out>{}};

/// The cycle the code was last handed.
std::optional<GraphCycle> seen;

/// Writes silence and keeps the cycle it is handed.
struct Keeper {
	void process(Io<spec> io, const GraphCycle& cycle) noexcept {
		seen = cycle;
		for (std::size_t member = 0; member < io[out].size(); ++member)
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame) io[out][member][frame] = 0.0F;
	}
};

struct Cycles {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
		cx.use(Keeper{});
		return Configured::applied();
	}
};

/// A row's steps as the ticks a driver gives, over a cut record told as the driver tells it: what both table runners
/// in core feed their rule.
class Script {
public:
	explicit Script(testing::HarnessCuts& cuts) : cuts_(cuts) {}
	DriverTick before(const test::CycleStep& step) {
		if (step.driver) driver_ = *step.driver;
		const std::uint32_t lostFrames = step.lostFrames.value_or(step.frames);
		for (std::uint32_t i = 0; i < step.lostCut + step.lostUnscheduled; ++i) {
			cuts_.start();
			if (i < step.lostCut) cuts_.incomplete();
			position_ += lostFrames;
			time_ += lostFrames;
		}
		for (std::uint32_t i = 0; i < step.numbersOnly; ++i) cuts_.start();
		position_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(position_) + step.reposition);
		cuts_.start();
		return DriverTick{.driverId = driver_,
		                  .graphCycle = driver_ == test::cycleTableClock ? cuts_.started() : 0,
		                  .position = position_,
		                  .frames = step.frames,
		                  .startRawNs = FrameClock(test::cycleTableFormat.rate).ns(static_cast<std::int64_t>(time_))};
	}
	void after(const test::CycleStep& step) {
		position_ += step.frames;
		time_ += step.frames;
		if (step.cutAfter) cuts_.incomplete();
	}

private:
	testing::HarnessCuts& cuts_;
	std::uint32_t driver_ = test::cycleTableClock;
	std::uint64_t position_ = 0;
	std::uint64_t time_ = 0; // frames of raw time
};

} // namespace cycle_facts_test
using namespace cycle_facts_test;

TEST_CASE("Every row of the cycle table states its facts through CycleAccount", "[cycle_facts]") {
	const std::vector<test::CycleRow> rows = test::cycleRows();
	REQUIRE_FALSE(rows.empty());
	for (const test::CycleRow& row : rows) {
		INFO(row.what);
		// A row the block passes by stating nothing proves nothing: each expects some fact after its first cycle.
		bool states = false;
		for (std::size_t i = 1; i < row.steps.size(); ++i)
			states = states || row.steps[i].facts.discontinuity() || row.steps[i].facts.lastCut.value_or(false);
		REQUIRE(states);
		testing::HarnessCuts cuts;
		Script script(cuts);
		CycleAccount account(FrameClock(test::cycleTableFormat.rate), &cuts);
		for (std::size_t i = 0; i < row.steps.size(); ++i) {
			const test::CycleStep& step = row.steps[i];
			INFO("step " << i << ", expected " << test::describe(step.facts));
			const DriverTick tick = script.before(step);
			if (step.drive) account.driven();
			const CycleAccount::Step got = account.next(tick, test::cycleTableFormat.maxBlock);
			INFO("stated " << test::describe(got.facts));
			CHECK(got.facts == step.facts);
			CHECK(got.runs == step.runs);
			CHECK(account.nextPosition() == tick.position + tick.frames);
			script.after(step);
		}
		CHECK(account.xruns() == row.xruns);
	}
}

TEST_CASE("The type harness states the facts of every cycle table row it can make, and hands its code the same in "
          "GraphCycle",
          "[cycle_facts]") {
	std::size_t made = 0;
	for (const test::CycleRow& row : test::cycleRows()) {
		if (!row.inHarness()) continue;
		INFO(row.what);
		testing::HarnessOptions options;
		options.format = test::cycleTableFormat;
		testing::TypeHarness<spec, Cycles> harness(options);
		REQUIRE_FALSE(harness.parse({}));
		harness.add();
		harness.blockReady(); // the row's first drive
		REQUIRE(harness.phase() == "running");
		for (std::size_t i = 0; i < row.steps.size(); ++i) {
			const test::CycleStep& step = row.steps[i];
			INFO("step " << i << ", expected " << test::describe(step.facts));
			if (i == 0) REQUIRE(step.drive); // made by blockReady
			else if (step.drive) {
				REQUIRE(harness.stop());
				REQUIRE(harness.start());
			}
			if (step.lostCut > 0) harness->loseCycles(step.lostCut, testing::Lost::cut);
			if (step.lostUnscheduled > 0) harness->loseCycles(step.lostUnscheduled, testing::Lost::notScheduled);
			if (step.driver) harness->changeDriver(*step.driver);
			if (step.reposition != 0) harness->reposition(step.reposition);
			seen.reset();
			harness.process(step.frames);
			INFO("stated " << test::describe(harness->facts()));
			CHECK(harness->facts() == step.facts);
			REQUIRE(seen);
			CHECK(seen->facts == step.facts);
			CHECK(seen->discontinuity == step.facts.discontinuity());
			CHECK(harness->cuts().started() >= step.facts.graphCycle);
		}
		REQUIRE(seen);
		CHECK(seen->xruns == row.xruns);
		++made;
	}
	CHECK(made == 14); // the rows only a script of ticks makes are seven
}

TEST_CASE("Causes are a set", "[cycle_facts]") {
	const CycleCauses none;
	CHECK(none.empty());
	const CycleCauses both = CycleCause::drive | CycleCause::catchUp;
	CHECK(both.has(CycleCause::drive));
	CHECK(both.has(CycleCause::catchUp));
	CHECK_FALSE(both.has(CycleCause::absent));
	CHECK(both.only(CycleCause::drive | CycleCause::catchUp | CycleCause::start));
	CHECK_FALSE(both.only(CycleCause::drive));
	CHECK(none.only(CycleCause::drive));
	CHECK((both | CycleCause::drive) == both);
	const CycleFacts facts{.missed = 2, .missedCut = 1, .causes = both};
	CHECK(facts.discontinuity());
	CHECK_FALSE(facts.whole());
	CHECK_FALSE(facts.confirmed());
	CHECK(CycleFacts{}.whole());
	CHECK(CycleFacts{}.confirmed());
}

TEST_CASE("A cut record (CutRing, the engine's and the harness's) answers for the cycles its ring keeps and counts "
          "every cut once",
          "[cycle_facts]") {
	testing::HarnessCuts cuts;
	CHECK(cuts.started() == 0);
	cuts.incomplete(); // before any cycle: nothing
	CHECK(cuts.cutsSoFar() == 0);
	CHECK_FALSE(cuts.cut(0));
	cuts.start();
	cuts.incomplete();
	cuts.incomplete(); // told twice, counted once
	CHECK(cuts.cutsSoFar() == 1);
	CHECK(cuts.newestWhole() == 0);
	cuts.start();
	CHECK(cuts.newestWhole() == 0); // cycle 1 was cut
	CHECK(cuts.started() == 2);
	CHECK(cuts.cut(1) == std::optional<bool>(true));
	CHECK_FALSE(cuts.cut(2)); // still running
	CHECK_FALSE(cuts.cut(3)); // not started
	cuts.start();
	CHECK(cuts.newestWhole() == 2);
	CHECK(cuts.cut(2) == std::optional<bool>(false));
	// Cycle 1's slot is used again by cycle 1 + cutRecordCycles: that one is not cut, and cycle 1 has no answer.
	while (cuts.started() < 2 + cutRecordCycles) cuts.start();
	CHECK_FALSE(cuts.cut(1));
	CHECK_FALSE(cuts.cut(2));
	CHECK(cuts.cut(3) == std::optional<bool>(false)); // the oldest it keeps
	CHECK(cuts.cut(1 + cutRecordCycles) == std::optional<bool>(false));
	CHECK(cuts.cutsSoFar() == 1); // the count has no reach
	CHECK(cutRecordKeeps(3, 2 + cutRecordCycles));
	CHECK_FALSE(cutRecordKeeps(2, 2 + cutRecordCycles));
	CHECK_FALSE(cutRecordKeeps(0, 5));
	CHECK_FALSE(cutRecordKeeps(5, 5));
}

TEST_CASE("A cut record read from another thread while cycles wrap its ring never answers wrongly", "[cycle_facts]") {
	// The writer tells cycles whose cuts follow a pattern, wrapping the ring three times; the reader asks about cycles
	// behind the writer the whole while. Every answer must be the pattern's or none (CutRecord's seqlock contract),
	// whatever the interleaving. The reader stops when the writer has finished, never on time.
	const auto isCut = [](std::uint64_t cycle) { return cycle % 3 == 0 || cycle % 7 == 0; };
	constexpr std::uint64_t cycles = 3 * cutRecordCycles;
	testing::HarnessCuts cuts;
	std::atomic<bool> done{false};
	std::thread writer([&] {
		for (std::uint64_t cycle = 1; cycle <= cycles; ++cycle) {
			cuts.start();
			if (isCut(cycle)) cuts.incomplete();
		}
		done.store(true, std::memory_order_release);
	});
	std::uint64_t answered = 0;
	std::uint64_t wrong = 0;
	std::uint64_t asked = 0;
	do {
		const std::uint64_t started = cuts.started();
		// Behind the writer by one to the ring's reach and a little past it, so slots are reused under the reader.
		const std::uint64_t behind = 1 + asked % (cutRecordCycles + 64);
		const std::uint64_t cycle = started > behind ? started - behind : 1;
		if (const std::optional<bool> was = cuts.cut(cycle)) {
			++answered;
			if (*was != isCut(cycle)) ++wrong;
		}
		++asked;
	} while (!done.load(std::memory_order_acquire));
	writer.join();
	INFO("asked " << asked << ", answered " << answered);
	CHECK(wrong == 0);
	REQUIRE(answered > 0);
	CHECK(cuts.cutsSoFar() == [&] {
		std::uint64_t n = 0;
		for (std::uint64_t cycle = 1; cycle <= cycles; ++cycle) n += isCut(cycle) ? 1 : 0;
		return n;
	}());
}

TEST_CASE("An account with no cut record confirms nothing and knows no last cut", "[cycle_facts]") {
	CycleAccount account(FrameClock(test::cycleTableFormat.rate), nullptr);
	const auto tick = [](std::uint64_t graphCycle, std::uint64_t position) {
		return DriverTick{.graphCycle = graphCycle, .position = position, .frames = 240};
	};
	CHECK(account.next(tick(1, 0), 256).facts.causes == CycleCauses(CycleCause::start));
	const CycleFacts facts = account.next(tick(3, 480), 256).facts;
	CHECK(facts.missed == 1);
	CHECK(facts.missedCut == 0);
	CHECK_FALSE(facts.lastCut.has_value());
	CHECK(facts.causes == CycleCauses(CycleCause::absent));
	CHECK(account.xruns() == 1);
}

// The rule off the numbered clock with no record: the discontinuities and xruns the real block counted before it was
// given cycle numbers and the driver's cuts, which every such cycle still states (docs/engine.md#graphcycle).
TEST_CASE("An account with no record and no cycle numbers: a discontinuity on the first cycle, a drive's first, a new "
          "driver's, a position jump and after a cycle too large to run, and xruns only for a jump ahead",
          "[cycle_facts]") {
	CycleAccount account(FrameClock(48000), nullptr);
	struct Row {
		const char* what;
		bool drive; // a drive installed a code before the cycle
		std::uint32_t driver;
		std::uint64_t position;
		std::uint32_t frames;
		bool discontinuity;
		bool runs;
		std::uint64_t xruns;
	};
	const std::vector<Row> rows{
		{"the first cycle", false, 3, 0, 256, true, true, 0},
		{"the next, contiguous", false, 3, 256, 256, false, true, 0},
		{"a drive's first", true, 3, 512, 256, true, true, 0},
		{"then contiguous", false, 3, 768, 256, false, true, 0},
		{"a jump ahead: an xrun", false, 3, 2048, 256, true, true, 1},
		{"a new driver: no xrun", false, 4, 9000, 256, true, true, 1},
		{"too large to run", false, 4, 9256, 1024, false, false, 1},
		{"the cycle after it", false, 4, 10280, 256, true, true, 1},
		{"a jump back on the same driver: no xrun", false, 4, 100, 256, true, true, 1},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		if (row.drive) account.driven();
		const CycleAccount::Step step =
			account.next(DriverTick{.driverId = row.driver, .position = row.position, .frames = row.frames}, 256);
		CHECK(step.facts.discontinuity() == row.discontinuity);
		CHECK(step.runs == row.runs);
		CHECK(account.xruns() == row.xruns);
		CHECK(account.nextPosition() == row.position + row.frames);
	}
}
