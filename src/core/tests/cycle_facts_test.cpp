// Cycle facts (docs/writing-a-node-type.md#lost-cycles): the cycle table (test/support/cycle_table.hpp) through the one
// rule, CycleAccount, over the harness's cut record; through CycleContinuity, the real block's discontinuity, which must
// put every discontinuity where the table's facts do; and through the type harness, whose loseCycles, changeDriver and
// reposition make each row it can. Then the pieces: the causes as a set, the cut record and an account with no record.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "cycle_table.hpp"
#include "ear/core/block_rules.hpp"
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
		for (std::uint32_t i = 0; i < step.lostCut + step.lostUnscheduled; ++i) {
			cuts_.start();
			if (i < step.lostCut) cuts_.incomplete();
			position_ += step.frames;
			time_ += step.frames;
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
			states = states || row.steps[i].facts.discontinuity() || row.steps[i].facts.lastCut;
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
	}
}

TEST_CASE("CycleContinuity, the real block's rule, puts each discontinuity the cycle table states without cycle "
          "numbers where its facts do",
          "[cycle_facts]") {
	for (const test::CycleRow& row : test::cycleRows()) {
		INFO(row.what);
		testing::HarnessCuts cuts;
		Script script(cuts);
		CycleContinuity continuity;
		for (std::size_t i = 0; i < row.steps.size(); ++i) {
			const test::CycleStep& step = row.steps[i];
			INFO("step " << i << ", expected " << test::describe(step.facts));
			const DriverTick tick = script.before(step);
			if (step.drive) continuity.driven();
			const CycleContinuity::Step got =
				continuity.next(tick.driverId, tick.position, tick.frames, test::cycleTableFormat.maxBlock);
			// Cycles missed with no move are known only by their numbers, which the real block is not given yet: it
			// sees a whole cycle there, and the table's rule a reposition.
			CHECK(got.discontinuity == (step.numbersOnly == 0 && step.facts.discontinuity()));
			CHECK(got.runs == step.runs);
			script.after(step);
		}
	}
}

TEST_CASE("The type harness states the facts of every cycle table row it can make, and hands its code the same",
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
			CHECK(seen->discontinuity == step.facts.discontinuity());
			CHECK(harness->cuts().started() >= step.facts.graphCycle);
		}
		++made;
	}
	CHECK(made >= 10); // the rows only a script of ticks makes are three
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

TEST_CASE("The harness's cut record answers for the cycles it keeps and none other", "[cycle_facts]") {
	testing::HarnessCuts cuts;
	CHECK(cuts.started() == 0);
	cuts.incomplete(); // before any cycle: nothing
	CHECK_FALSE(cuts.cut(0));
	cuts.start();
	cuts.incomplete();
	cuts.start();
	CHECK(cuts.started() == 2);
	CHECK(cuts.cut(1) == std::optional<bool>(true));
	CHECK_FALSE(cuts.cut(2)); // still running
	CHECK_FALSE(cuts.cut(3)); // not started
	cuts.start();
	CHECK(cuts.cut(2) == std::optional<bool>(false));
	CHECK(cuts.count(1, 3).cut == 1);
	CHECK(cuts.count(1, 3).known == 2);
	CHECK(cuts.count(1, 9).known == 2); // the running cycle and those not started have no answer
	// Cycle 1's slot is used again by cycle 1 + cutRecordCycles: that one is not cut, and cycle 1 has no answer.
	while (cuts.started() < 2 + cutRecordCycles) cuts.start();
	CHECK_FALSE(cuts.cut(1));
	CHECK_FALSE(cuts.cut(2));
	CHECK(cuts.cut(3) == std::optional<bool>(false)); // the oldest it keeps
	CHECK(cuts.cut(1 + cutRecordCycles) == std::optional<bool>(false));
	CHECK(cuts.count(0, cuts.started()).known == cutRecordCycles - 1);
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
	CHECK_FALSE(facts.lastCut);
	CHECK(facts.causes == CycleCauses(CycleCause::absent));
	CHECK(account.xruns() == 1);
}
