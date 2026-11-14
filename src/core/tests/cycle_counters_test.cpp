// The diagnostics counters (docs/engine.md#counters): what each counts, by table; the graph's from the cut record
// (CutRing, the engine's and the harness's), stalls exactly the runs of cut cycles and any reading ordered; and each
// counter moved by its own cause through the type harness, an induced cut moving the graph's and no node's.
// ear_pipewire_tests makes the same causes on a real engine; node_host_test.cpp carries a node's across its blocks.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/cycle_counters.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/testing/harness_cuts.hpp"
#include "ear/testing/type_harness.hpp"
#include "inducers.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace cycle_counters_test {

using namespace ear;

constexpr auto out = bank::output<"out">;
constexpr TypeSpec spec{.settings = SettingsTable<>{"counted"}, .banks = Banks<out>{}};
constexpr AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 256};

/// The stalls the code makes, by its own cycle count; none when null.
test::Stalls* stalls = nullptr;

/// Writes silence and, where the test's plan says, stalls past its cycle on the thread that runs it.
struct Staller {
	std::size_t cycle = 0;
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::size_t member = 0; member < io[out].size(); ++member)
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame) io[out][member][frame] = 0.0F;
		if (stalls != nullptr) (*stalls)(cycle, io.frames(), format.rate);
		++cycle;
	}
};

struct Counted {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
		cx.use(Staller{});
		return Configured::applied();
	}
};

/// A running harness of the counted type at `format`, its first cycle run.
struct Running {
	Running() : harness(options()) {
		REQUIRE_FALSE(harness.parse({}));
		harness.add();
		harness.blockReady();
		REQUIRE(harness.phase() == "running");
		harness.process();
	}
	static testing::HarnessOptions options() {
		testing::HarnessOptions made;
		made.format = format;
		return made;
	}
	testing::TypeHarness<spec, Counted> harness;
};

} // namespace cycle_counters_test
using namespace cycle_counters_test;

namespace Catch {
template <> struct StringMaker<ear::GraphCounts> {
	static std::string convert(const ear::GraphCounts& c) {
		return "{cycles " + std::to_string(c.cycles) + ", cut " + std::to_string(c.cutCycles) + ", stalls " +
		       std::to_string(c.stalls) + "}";
	}
};
template <> struct StringMaker<ear::NodeCounts> {
	static std::string convert(const ear::NodeCounts& c) {
		return "{missed " + std::to_string(c.missedCycles) + ", overruns " + std::to_string(c.overruns) +
		       ", repositions " + std::to_string(c.repositions) + ", driver changes " +
		       std::to_string(c.driverChanges) + "}";
	}
};
} // namespace Catch

TEST_CASE("A cycle's facts move each node counter by its own cause and no other", "[cycle_counters]") {
	struct Row {
		const char* what;
		CycleFacts facts;
		bool overran;
		NodeCounts moves;
	};
	const std::vector<Row> rows{
		{"a whole cycle", {}, false, {}},
		{"a catch-up over three cut cycles: the graph's, not the node's",
	     {.missed = 3, .missedCut = 3, .causes = CycleCause::catchUp},
	     false,
	     {}},
		{"a pause of five cycles, none cut", {.missed = 5, .causes = CycleCause::absent}, false, {.missedCycles = 5}},
		{"seven missed, two of them cut",
	     {.missed = 7, .missedCut = 2, .causes = CycleCause::absent},
	     false,
	     {.missedCycles = 5}},
		{"a reposition", {.causes = CycleCause::reposition}, false, {.repositions = 1}},
		{"a reposition over two cycles not cut",
	     {.missed = 2, .causes = CycleCause::reposition},
	     false,
	     {.missedCycles = 2, .repositions = 1}},
		{"another driver", {.causes = CycleCause::driverChange}, false, {.driverChanges = 1}},
		{"a whole cycle whose work overran", {}, true, {.overruns = 1}},
		{"a drive, a start and a cycle after one too large: none is counted",
	     {.causes = CycleCause::start | CycleCause::drive | CycleCause::oversized},
	     false,
	     {}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CycleCounters counters;
		CHECK(counters.read() == NodeCounts{});
		counters.add(row.facts, row.overran);
		CHECK(counters.read() == row.moves);
		counters.add(row.facts, row.overran); // a total: the same cycle again counts again
		CHECK(counters.read() == row.moves + row.moves);
	}
}

TEST_CASE("An overrun is work longer than the cycle, and the graph's lost time is a block per cut cycle",
          "[cycle_counters]") {
	const FrameClock clock(ear::SampleRate::fixed<48000>());
	CHECK_FALSE(cycleOverran(0, clock, 256));
	CHECK_FALSE(cycleOverran(5'333'333, clock, 256)); // 256 frames last 5,333,333 ns: the whole cycle is no overrun
	CHECK(cycleOverran(5'333'334, clock, 256));
	CHECK(cycleOverran(8'000'000, clock, 256)); // a stall of one and a half cycles
	CHECK_FALSE(cycleOverran(8'000'000, clock, 512));
	CHECK(GraphCounts{}.lostMs(clock, 256) == 0);
	CHECK(GraphCounts{.cycles = 100, .cutCycles = 3, .stalls = 1}.lostMs(clock, 256) == 16); // 16 ms of 3 blocks
	CHECK(GraphCounts{.cutCycles = 187'500}.lostMs(clock, 256) == 1'000'000);                // 1000 s
	CHECK(GraphCounts{.cutCycles = 1}.lostMs(clock, 48) == 1);
	CHECK((NodeCounts{1, 2, 3, 4} + NodeCounts{10, 20, 30, 40}) == NodeCounts{11, 22, 33, 44});
}

TEST_CASE("A cut record counts the cycles started, the cuts once each, and a stall per time the graph fell behind",
          "[cycle_counters]") {
	// One cycle as the record is told it: whether it started on time (a block stated overdueNs 0) and whether it was cut.
	struct Cycle {
		bool onTime;
		bool cut;
	};
	constexpr Cycle whole{true, false};
	constexpr Cycle stalled{true, true}; // started on time, then held past its end: a stall's first cut
	constexpr Cycle burstCut{false, true};
	constexpr Cycle burstWhole{false, false}; // a catch-up's cycle that ran whole while the graph was still behind
	constexpr Cycle lateCut{false, true};     // a cut whose cycle no block ran on time
	struct Row {
		const char* what;
		std::vector<Cycle> cycles;
		GraphCounts counts;
	};
	const std::vector<Row> rows{
		{"whole cycles", {whole, whole, whole}, {.cycles = 3}},
		{"a stall in the first cycle", {stalled, whole}, {.cycles = 2, .cutCycles = 1, .stalls = 1}},
		{"a stall whose burst cuts every cycle (the cut-every-cycle shape)",
	     {whole, stalled, burstCut, burstCut, burstCut, whole},
	     {.cycles = 6, .cutCycles = 4, .stalls = 1}},
		{"a stall whose burst alternates cut and whole cycles (the dev host's shape): one stall, not one per cut",
	     {whole, stalled, burstWhole, burstCut, burstWhole, burstCut, burstWhole, burstCut, whole},
	     {.cycles = 9, .cutCycles = 4, .stalls = 1}},
		{"a stall whose burst runs whole (the runner's shape)",
	     {whole, stalled, burstWhole, burstWhole, burstWhole, whole},
	     {.cycles = 6, .cutCycles = 1, .stalls = 1}},
		{"two stalls with a cycle on time between",
	     {stalled, burstCut, whole, stalled, burstWhole, burstCut, whole},
	     {.cycles = 7, .cutCycles = 4, .stalls = 2}},
		{"a cut no block ran on time, after the graph caught up",
	     {whole, lateCut, burstCut, whole, lateCut},
	     {.cycles = 5, .cutCycles = 3, .stalls = 2}},
		{"cuts in consecutive cycles that each started on time: a stall each, the graph caught up before each",
	     {stalled, stalled, stalled, whole},
	     {.cycles = 4, .cutCycles = 3, .stalls = 3}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		testing::HarnessCuts cuts;
		cuts.incomplete(); // before any cycle: nothing
		cuts.onTime();
		CHECK(cuts.counts() == GraphCounts{});
		for (const Cycle& cycle : row.cycles) {
			cuts.start();
			if (cycle.onTime) cuts.onTime();
			if (cycle.cut) {
				cuts.incomplete();
				cuts.incomplete(); // told twice, counted once
			}
		}
		CHECK(cuts.counts() == row.counts);
	}
}

TEST_CASE("A stall past the cut record's ring is one stall, and the next after a cycle on time is another",
          "[cycle_counters]") {
	testing::HarnessCuts cuts;
	cuts.start();
	cuts.onTime();
	for (std::uint64_t i = 0; i < cutRecordCycles + 10; ++i) {
		cuts.start();
		cuts.incomplete();
	}
	CHECK(cuts.counts() == GraphCounts{.cycles = cutRecordCycles + 11, .cutCycles = cutRecordCycles + 10, .stalls = 1});
	cuts.start();
	cuts.onTime();
	cuts.start();
	cuts.onTime();
	cuts.incomplete();
	CHECK(cuts.counts().stalls == 2);
}

TEST_CASE("The graph's counters read from another thread never show more stalls than cuts or more cuts than cycles",
          "[cycle_counters]") {
	testing::HarnessCuts cuts;
	std::atomic<bool> done{false};
	std::thread writer([&] {
		// Every third cycle cut, every fifth cut twice in a row, every other cycle on time: stalls and cuts move as fast
		// as cycles.
		for (std::uint64_t cycle = 1; cycle <= 3 * cutRecordCycles; ++cycle) {
			cuts.start();
			if (cycle % 2 == 0) cuts.onTime();
			if (cycle % 3 == 0 || cycle % 5 == 0) cuts.incomplete();
		}
		done.store(true, std::memory_order_release);
	});
	std::size_t readings = 0, wrong = 0;
	GraphCounts last;
	while (!done.load(std::memory_order_acquire)) {
		const GraphCounts now = cuts.counts();
		++readings;
		if (now.stalls > now.cutCycles || now.cutCycles > now.cycles) ++wrong;
		if (now.cycles < last.cycles || now.cutCycles < last.cutCycles || now.stalls < last.stalls) ++wrong;
		last = now;
	}
	writer.join();
	INFO(readings << " readings");
	CHECK(wrong == 0);
	CHECK(cuts.counts().cycles == 3 * cutRecordCycles);
}

TEST_CASE("Through the type harness, cycles lost to cuts move the graph's counters and no node's", "[cycle_counters]") {
	Running r;
	const GraphCounts before = r.harness->graphCounts();
	CHECK(before == GraphCounts{.cycles = 1});
	r.harness->loseCycles(3, testing::Lost::cut);
	r.harness.process();
	CHECK(r.harness->facts().causes == CycleCauses(CycleCause::catchUp));
	CHECK(r.harness->graphCounts() == GraphCounts{.cycles = 5, .cutCycles = 3, .stalls = 1});
	r.harness->loseCycles(1, testing::Lost::cut);
	r.harness.process();
	CHECK(r.harness->graphCounts() == GraphCounts{.cycles = 7, .cutCycles = 4, .stalls = 2});
	CHECK(r.harness->counts() == NodeCounts{});
}

TEST_CASE("Through the type harness, a paused block moves its missed cycles by the cycles it was not scheduled for",
          "[cycle_counters]") {
	Running r;
	r.harness->loseCycles(5, testing::Lost::notScheduled);
	r.harness.process();
	CHECK(r.harness->facts().causes == CycleCauses(CycleCause::absent));
	CHECK(r.harness->counts() == NodeCounts{.missedCycles = 5});
	CHECK(r.harness->graphCounts().cutCycles == 0);
	// Cut and not cut in one loss: only the ones not cut are the node's.
	r.harness->loseCycles(2, testing::Lost::cut);
	r.harness->loseCycles(4, testing::Lost::notScheduled);
	r.harness.process();
	CHECK(r.harness->counts() == NodeCounts{.missedCycles = 9});
	CHECK(r.harness->graphCounts().cutCycles == 2);
}

TEST_CASE("Through the type harness, a reposition and a change of driver each move their own counter",
          "[cycle_counters]") {
	Running r;
	r.harness->reposition(-100);
	r.harness.process();
	CHECK(r.harness->counts() == NodeCounts{.repositions = 1});
	r.harness->changeDriver(42);
	r.harness.process();
	CHECK(r.harness->counts() == NodeCounts{.repositions = 1, .driverChanges = 1});
	r.harness.process();
	CHECK(r.harness->counts() == NodeCounts{.repositions = 1, .driverChanges = 1});
}

TEST_CASE("Through the type harness, a code that stalls past its cycle moves its node's overruns, once a stall",
          "[cycle_counters]") {
	Running r;
	test::Stalls planned(test::StallPlan::every(4, 2, 3, test::StallLength::cycles(1.5)));
	stalls = &planned;
	for (int i = 0; i < 16; ++i) r.harness.process();
	stalls = nullptr;
	REQUIRE(planned.made() == 3);
	// Each stalled cycle's work outlasts it; the others take microseconds of a 5.3 ms cycle on any host.
	CHECK(r.harness->counts().overruns >= 3);
	CHECK(r.harness->counts().overruns <= 17);
	CHECK(r.harness->counts().missedCycles == 0);
	CHECK(r.harness->graphCounts().cutCycles == 0); // the harness's driver cuts only what loseCycles says
}

TEST_CASE("The type harness's node counters are NodeHost's: none once the node is gone", "[cycle_counters]") {
	Running r;
	r.harness->loseCycles(2, testing::Lost::notScheduled);
	r.harness.process();
	CHECK(r.harness->counts().missedCycles == 2);
	r.harness.remove();
	REQUIRE(r.harness.gone());
	CHECK_THROWS_AS(r.harness->counts(), std::logic_error);
}
