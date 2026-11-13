// CounterWatch (docs/engine.md#reporting-the-counters): what it tells and when, on a clock moved by hand and counters
// set by hand, ticked as the heartbeat ticks it. Each rule has a case here that fails when the rule is broken.
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/counter_watch.hpp"

using namespace ear;
using namespace std::chrono_literals;

namespace {

/// The paces' clock, moved by hand.
struct HandTimers final : Timers {
	std::chrono::steady_clock::time_point at{};
	std::unique_ptr<Timer> after(std::chrono::milliseconds, std::move_only_function<void()>) override { return {}; }
	std::chrono::steady_clock::time_point now() const override { return at; }
};

struct HandGraph final : GraphCounterSource {
	GraphCounts counts;
	GraphCounts graphCounts() const noexcept override { return counts; }
};

/// Each open span's counts, as NodeHost answers them.
struct HandNodes final : NodeCounterSource {
	std::map<std::uint64_t, NodeCounts> spans;
	std::optional<NodeCounts> counts(const NodeName&) const override {
		return std::nullopt;
	} // CounterWatch's is by span
	std::optional<NodeCounts> spanCounts(std::uint64_t span) const override {
		const auto it = spans.find(span);
		if (it == spans.end()) return std::nullopt;
		return it->second;
	}
};

std::string text(const NodeCounts& c) {
	return std::format("{},{},{},{}", c.missedCycles, c.overruns, c.repositions, c.driverChanges);
}

/// Every call, as a line.
struct Told final : CounterListener {
	std::vector<std::string> lines;
	void countersChanged(const GraphCounts& graph, std::uint64_t lostMs, std::span<const NamedCounts> nodes) override {
		std::string line = std::format("changed cycles={} cut={} stalls={} ms={}", graph.cycles, graph.cutCycles,
		                               graph.stalls, lostMs);
		for (const NamedCounts& node : nodes) line += std::format(" {}={}", node.name, text(node.counts));
		lines.push_back(std::move(line));
	}
	void graphLostCycles(const GraphCounts& total, const GraphCounts& since, std::uint64_t sinceMs) override {
		lines.push_back(std::format("graph cut={} stalls={} ms={} total={}", since.cutCycles, since.stalls, sinceMs,
		                            total.cutCycles));
	}
	void nodeMissedCycles(const NodeName& node, const NodeCounts& total, const NodeCounts& since) override {
		lines.push_back(std::format("node {} since={} total={}", node, text(since), text(total)));
	}
	/// What was told since the last look.
	std::vector<std::string> take() { return std::exchange(lines, {}); }
};

struct World {
	HandTimers timers;
	HandGraph graph;
	HandNodes nodes;
	CounterWatch watch{timers, graph, nodes, AudioFormat{48000, 256}};
	Told told;
	World() { watch.subscribe(told); }
	/// One heartbeat, 100 ms after the last.
	void beat() {
		timers.at += 100ms;
		graph.counts.cycles += 19; // the graph runs on: cycles alone are no change
		watch.tick();
	}
	void beats(int n) {
		for (int i = 0; i < n; ++i) beat();
	}
	void cut(std::uint64_t cycles, std::uint64_t stalls = 0) {
		graph.counts.cutCycles += cycles;
		graph.counts.stalls += stalls;
	}
	std::map<NodeName, std::uint64_t> newest; // each name's newest span
	std::uint64_t serials = 0;
	/// A node of this name added: a span begins, a node of the name before it superseded. Returns the span.
	std::uint64_t begin(const NodeName& node) {
		const std::uint64_t span = ++serials;
		nodes.spans[span] = {};
		newest[node] = span;
		watch.countsBegan(node, span);
		return span;
	}
	/// The counts of the name's newest span, set by hand as its block's cycles would move them.
	NodeCounts& of(const NodeName& node) { return nodes.spans.at(newest.at(node)); }
	/// The span gone, with these final counts.
	void end(const NodeName& node, std::uint64_t span, NodeCounts total) {
		nodes.spans.erase(span);
		watch.countsEnded(node, span, total);
	}
};

using Lines = std::vector<std::string>;

} // namespace

TEST_CASE("Nothing is told while nothing is lost, however the graph's cycles move", "[counter_watch]") {
	World w;
	w.begin("rec");
	w.beats(50);
	CHECK(w.told.take().empty());
}

TEST_CASE("lostMs is one block per cut cycle at the graph's rate", "[counter_watch]") {
	World w;
	CHECK(w.watch.lostMs(39) == 208); // 39 * 256 / 48000 s
	CHECK(w.watch.lostMs(0) == 0);
}

TEST_CASE("counters.changed: at the next beat a loss moved, the graph whole and only the nodes that moved, at most "
          "once a second",
          "[counter_watch][wire]") {
	World w;
	w.begin("rec");
	w.begin("noise");
	w.beat();
	w.cut(3, 1);
	w.beat();
	// The graph's record waits for the cuts to hold still, so this beat tells the wire alone.
	CHECK(w.told.take() == Lines{"changed cycles=38 cut=3 stalls=1 ms=16"});
	w.of("rec").overruns = 2;
	w.beat(); // within the second: held
	CHECK(w.told.take() == Lines{"graph cut=3 stalls=1 ms=16 total=3"});
	w.beats(8);
	CHECK(w.told.take() == Lines{"node rec since=0,2,0,0 total=0,2,0,0"});
	w.beat(); // a second since the last: the node that moved, and the graph whole
	CHECK(w.told.take() == Lines{"changed cycles=228 cut=3 stalls=1 ms=16 rec=0,2,0,0"});
	w.of("noise").repositions = 1; // a node counter the records do not report still goes on the wire
	w.beats(10);
	CHECK(w.told.take() == Lines{"changed cycles=418 cut=3 stalls=1 ms=16 noise=0,0,1,0"});
	w.beats(30);
	CHECK(w.told.take().empty());
}

TEST_CASE("A node's counts begin from zero: one begun again after a replace is told from zero",
          "[counter_watch][wire]") {
	World w;
	w.begin("x");
	w.of("x").missedCycles = 4;
	w.beats(2);
	CHECK(w.told.take() ==
	      Lines{"changed cycles=19 cut=0 stalls=0 ms=0 x=4,0,0,0", "node x since=4,0,0,0 total=4,0,0,0"});
	// The same name added again: a newer node, whose counts NodeHost answers from zero. It reaches 4 by the next beat,
	// the very counts the client last held for the name, yet its node.added said zero, so the wire names it.
	w.begin("x");
	w.of("x").missedCycles = 4;
	w.beats(10);
	const Lines told = w.told.take();
	REQUIRE(told.size() == 2);
	CHECK(told[0] == "node x since=4,0,0,0 total=4,0,0,0"); // its own record, from its own zero
	CHECK(told[1] == "changed cycles=209 cut=0 stalls=0 ms=0 x=4,0,0,0");
}

TEST_CASE("ear.graph-lost-cycles: a stall's burst is one record once the cuts hold still, whatever beats it spans",
          "[counter_watch][records]") {
	World w;
	w.beats(3);
	// A long stall: nothing is cut while the data thread is held, then the catch-up's burst cuts across three beats.
	w.cut(80, 1);
	w.beat();
	w.cut(70);
	w.beat();
	w.cut(37);
	w.beat();
	CHECK(w.told.take() == Lines{"changed cycles=76 cut=80 stalls=1 ms=426"}); // the wire's sample; no record yet
	w.beat();                                                                  // held still: the stall's one record
	CHECK(w.told.take() == Lines{"graph cut=187 stalls=1 ms=997 total=187"});
	w.beats(7);
	CHECK(w.told.take() == Lines{"changed cycles=266 cut=187 stalls=1 ms=997"}); // a second after its last sample
	w.beats(20);
	CHECK(w.told.take().empty());
}

TEST_CASE("ear.graph-lost-cycles: at most one a second, and one at least every 10 s while cuts never hold still",
          "[counter_watch][records]") {
	World w;
	w.cut(1, 1);
	w.beats(2);
	CHECK(w.told.take() == Lines{"changed cycles=19 cut=1 stalls=1 ms=5", "graph cut=1 stalls=1 ms=5 total=1"});
	w.cut(2, 1);
	w.beats(2); // held still, but within the second
	CHECK(w.told.take().empty());
	w.beats(7);
	CHECK(w.told.take() == Lines{"changed cycles=209 cut=3 stalls=2 ms=16"});
	w.beat(); // a second since the last record
	// ms is the difference of the totals' times, so the records' ms add up to the total lost.
	CHECK(w.told.take() == Lines{"graph cut=2 stalls=1 ms=11 total=3"});
	// A host that cuts every beat: held until the cuts have kept moving for 10 s, then one record for all of them.
	int records = 0;
	for (int i = 0; i < 100; ++i) {
		w.cut(1);
		w.beat();
		for (const std::string& line : w.told.take()) records += line.starts_with("graph ") ? 1 : 0;
	}
	CHECK(records == 0);
	w.cut(1);
	w.beat();
	CHECK(w.told.take() == Lines{"graph cut=101 stalls=0 ms=538 total=104"});
}

TEST_CASE("ear.node-missed-cycles: once the misses hold still, then at most once per 10 s per node",
          "[counter_watch][records]") {
	World w;
	w.begin("rec");
	w.begin("ma");
	w.of("rec").missedCycles = 33;
	w.beat();
	w.told.take();
	w.beat();
	CHECK(w.told.take() == Lines{"node rec since=33,0,0,0 total=33,0,0,0"});
	w.of("rec").overruns = 1;
	w.of("ma").missedCycles = 2; // another node is paced on its own
	w.beats(11);
	CHECK(w.told.take() == Lines{"node ma since=2,0,0,0 total=2,0,0,0",
	                             "changed cycles=209 cut=0 stalls=0 ms=0 ma=2,0,0,0 rec=33,1,0,0"});
	w.beats(88);
	CHECK(w.told.take().empty());
	w.beat(); // 10 s since rec's record
	CHECK(w.told.take() == Lines{"node rec since=0,1,0,0 total=33,1,0,0"});
	// repositions and driverChanges are the wire's alone.
	w.of("ma").driverChanges = 1;
	w.beats(200);
	CHECK(w.told.take() == Lines{"changed cycles=1957 cut=0 stalls=0 ms=0 ma=2,0,0,1"});
}

TEST_CASE("A node's end writes what it held back at once, with its final counts, a superseded node's too",
          "[counter_watch][records]") {
	World w;
	const std::uint64_t rec = w.begin("rec");
	w.of("rec").missedCycles = 1;
	w.beats(2);
	w.told.take();
	w.of("rec").missedCycles = 5; // held back: within 10 s of its record
	w.beats(3);
	CHECK(w.told.take().empty());                     // within the wire's second and the record's 10 s
	w.end("rec", rec, NodeCounts{.missedCycles = 6}); // its last cycles, after the last beat read it
	CHECK(w.told.take() == Lines{"node rec since=5,0,0,0 total=6,0,0,0"});
	w.beats(200);
	CHECK(w.told.take().empty()); // its span is closed

	// A newer node of the name (another type): the older node's span stays open while it closes, read and recorded on
	// its own, and off the wire, which names the newer node the client holds.
	const std::uint64_t older = w.begin("x");
	w.of("x").overruns = 1;
	w.beats(2);
	w.of("x").overruns = 3;
	w.beat();
	w.told.take();
	const std::uint64_t newer = w.begin("x");
	CHECK(w.told.take().empty());
	w.nodes.spans.at(older).overruns = 4; // its block still runs until its barrier
	w.of("x").missedCycles = 2;
	w.beats(12);
	CHECK(w.told.take() ==
	      Lines{"node x since=2,0,0,0 total=2,0,0,0", "changed cycles=4104 cut=0 stalls=0 ms=0 x=2,0,0,0"});
	w.end("x", older, NodeCounts{.overruns = 5}); // gone: every overrun since its record, the last read or not
	CHECK(w.told.take() == Lines{"node x since=0,4,0,0 total=0,5,0,0"});
	w.end("x", newer, NodeCounts{.missedCycles = 2});
	CHECK(w.told.take().empty());
	w.watch.countsEnded("nobody", 99, NodeCounts{.missedCycles = 1}); // a span never begun tells nothing
	CHECK(w.told.take().empty());
}

TEST_CASE("flush writes every record held back, pace and hold waived, so the records' sums are the totals",
          "[counter_watch][records]") {
	World w;
	w.begin("rec");
	w.cut(4, 1);
	w.of("rec").missedCycles = 2;
	w.beat(); // moving: nothing recorded yet
	w.told.take();
	w.of("rec").missedCycles = 3;
	w.cut(1);
	w.watch.flush();
	CHECK(w.told.take() == Lines{"graph cut=5 stalls=1 ms=26 total=5", "node rec since=3,0,0,0 total=3,0,0,0"});
	w.watch.flush();
	CHECK(w.told.take().empty());
}
