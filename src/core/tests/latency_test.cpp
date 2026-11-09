// ear's one latency model (ear/core/latency.hpp, docs/engine.md#latency-per-bank-pair), by table: the algebra (series
// apart from parallel, no path apart from zero), then one row per bank shape and per past regression through
// workOut, then FrameClock's units and its two dating rules. What must not compile is in compile_fail/latency_bad.cpp.
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"

using namespace ear;
using latency_model::LatencyPlan;
using latency_model::workOut;
using Peers = std::vector<std::optional<Latency>>;

// A delay is never a path, and a path never a delay, by type.
static_assert(!std::is_convertible_v<Delay, Latency>);
static_assert(!std::is_convertible_v<Latency, Delay>);
static_assert(!std::is_convertible_v<Delay, std::optional<Latency>>);

// The algebra holds at compile time.
static_assert(Latency{1, 2} + Latency{10, 20} == Latency{11, 22});
static_assert(Paths{}.combined() == std::nullopt);
static_assert(Paths{}.orZero() == Latency{});
static_assert(FrameClock(48000).ns(48000) == 1'000'000'000);

namespace {

constexpr std::uint32_t kMax = UINT32_MAX;

Paths pathsOf(std::initializer_list<std::optional<Latency>> paths) {
	Paths combined;
	for (const auto& path : paths) combined.add(path);
	return combined;
}

} // namespace

TEST_CASE("Series adds least to least and most to most, associatively, never wrapping", "[latency]") {
	const Latency a{1, 5}, b{20, 40}, c{300, 700};
	CHECK((a + b) + c == a + (b + c));
	CHECK(a + b == b + a);
	CHECK(a + Latency{} == a);
	CHECK(Latency{kMax - 1, 10} + Latency{5, 5} == Latency{kMax, 15});
	CHECK(Latency{0, kMax} + Latency{0, kMax} == Latency{0, kMax});
	// A delay adds in series on either side, and to another delay.
	CHECK(a + Delay{b} == a + b);
	CHECK(Delay{b} + a == a + b);
	CHECK(Delay{a} + Delay{b} == Delay{a + b});
}

TEST_CASE("Paths: the least of the leasts and the most of the mosts, and empty is no path, not zero", "[latency]") {
	struct Row {
		const char* what;
		std::vector<std::optional<Latency>> paths;
		std::optional<Latency> combined;
	};
	const std::vector<Row> rows{
		{"no path at all", {}, std::nullopt},
		{"only no-path peers", {std::nullopt, std::nullopt}, std::nullopt},
		{"one path is that one", {Latency{3, 9}}, Latency{3, 9}},
		{"a zero path is a path", {Latency{0, 0}}, Latency{0, 0}},
		// Regression (parallel peers' min/max): each bound from whichever path holds it, not one path's pair.
		{"parallel peers", {Latency{64, 64}, Latency{256, 256}, Latency{128, 512}}, Latency{64, 512}},
		{"a no-path peer leaves the rest unchanged", {Latency{10, 20}, std::nullopt, Latency{5, 15}}, Latency{5, 20}},
		// Regression (no path vs zero): a peer at zero lowers the least; a port with no peer does not.
		{"a zero peer beside a buffer", {Latency{0, 0}, Latency{300, 300}}, Latency{0, 300}},
		{"no peer beside a buffer", {std::nullopt, Latency{300, 300}}, Latency{300, 300}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		Paths paths;
		for (const auto& path : row.paths) paths.add(path);
		CHECK(paths.combined() == row.combined);
		CHECK(paths.orZero() == row.combined.value_or(Latency{}));
		// Order does not matter.
		Paths reversed;
		for (auto it = row.paths.rbegin(); it != row.paths.rend(); ++it) reversed.add(*it);
		CHECK(reversed.combined() == row.combined);
	}
	CHECK(pathsOf({Latency{1, 1}}).combined() == Latency{1, 1});
}

TEST_CASE("workOut: what each bank tells and each output's downstream, one row per bank shape", "[latency]") {
	using R = PortBank::Reach;
	struct Row {
		const char* what;
		std::vector<PortBank> banks;
		LatencyPlan plan;
		Peers inputPeers;  // one per input port, inputs then insideInputs
		Peers outputPeers; // one per output port, outputs then insideOutputs
		std::vector<Latency> told;
		std::vector<Latency> downstream;
	};
	const std::optional<Latency> none;
	const std::vector<Row> rows{
		{"an input bank in no pair tells zero, whatever its peers",
	     {{Direction::input, "in", numbered(2)}},
	     {},
	     {Latency{10, 20}, Latency{30, 40}},
	     {},
	     {{0, 0}, {0, 0}},
	     {}},
		{"an output bank in no pair tells zero; its downstream is its peers side by side",
	     {{Direction::output, "out", numbered(2)}},
	     {},
	     {},
	     {Latency{5, 7}, Latency{3, 9}},
	     {{0, 0}, {0, 0}},
	     {{3, 9}, {3, 9}}},
		{"an output bank with no peer has zero downstream",
	     {{Direction::output, "out", numbered(1)}},
	     {},
	     {},
	     {none},
	     {{0, 0}},
	     {{0, 0}}},
		{"a pair carries each side's beyond to the other",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {Latency{10, 20}, Latency{12, 18}},
	     {Latency{100, 200}, none},
	     {{100, 200}, {100, 200}, {10, 20}, {10, 20}},
	     {{100, 200}, {100, 200}}},
		// Regression (no path vs zero): the output port with no peer is no path, so the input tells the buffer whole.
		{"a port with no peer is no path",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {none, none},
	     {none, Latency{300, 300}},
	     {{300, 300}, {300, 300}, {0, 0}, {0, 0}},
	     {{300, 300}, {300, 300}}},
		{"a port whose peer reports zero is a zero path",
	     {{Direction::input, "in", numbered(2)}, {Direction::output, "out", numbered(2)}},
	     {.pairs = {{"in", "out"}}},
	     {none, none},
	     {Latency{0, 0}, Latency{300, 300}},
	     {{0, 300}, {0, 300}, {0, 0}, {0, 0}},
	     {{0, 300}, {0, 300}}},
		// Regression (parallel peers' min/max), on a bank.
		{"parallel peers on one bank",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(3)}},
	     {.pairs = {{"in", "out"}}},
	     {none},
	     {Latency{64, 64}, Latency{256, 256}, Latency{128, 512}},
	     {{64, 512}, {0, 0}, {0, 0}, {0, 0}},
	     {{64, 512}, {64, 512}, {64, 512}}},
		// Two pairs into one output are two paths side by side; a paired bank with no peer is a zero path (PipeWire
		// finishes an empty combination at zero, and the pair still carries it).
		{"two pairs into one output",
	     {{Direction::input, "a", numbered(1)},
	      {Direction::input, "b", numbered(1)},
	      {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"a", "out"}, {"b", "out"}}},
	     {Latency{100, 200}, none},
	     {none},
	     {{0, 0}, {0, 0}, {0, 200}},
	     {{0, 0}}},
		// Regression (3b.2, 6c1299b556): a code's delay declared on its pair reaches the input bank's told playback
		// latency, so a source on it dates Played late enough; and the output's capture likewise.
		{"a pair's delay counts both ways on its path",
	     {{Direction::output, "out", numbered(1)}, {Direction::input, "src", numbered(1), R::inside}},
	     {.pairs = {{"src", "out"}},
	      .onPair = {{{"src", "out"}, Delay{{0, 64}}}},
	      .beyond = {{"src", Delay{{0, 1024}}}}},
	     {none},
	     {Latency{256, 256}},
	     {{256, 320}, {0, 1088}},
	     {{256, 256}}},
		{"a pair's delay is on its path only, not on another pair's into the same output",
	     {{Direction::input, "a", numbered(1)},
	      {Direction::input, "b", numbered(1)},
	      {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"a", "out"}, {"b", "out"}}, .onPair = {{{"a", "out"}, Delay{{50, 50}}}}},
	     {Latency{10, 10}, Latency{20, 20}},
	     {Latency{5, 5}},
	     {{55, 55}, {5, 5}, {20, 60}},
	     {{5, 5}}},
		// sendspin's feed, declared on its unpaired output `from`.
		{"a bank's own delay is on what it tells, not on its downstream",
	     {{Direction::input, "to", numbered(2)}, {Direction::output, "from", numbered(2)}},
	     {.onBank = {{"from", Delay{{0, 9600}}}}},
	     {none, none},
	     {Latency{256, 256}, Latency{256, 256}},
	     {{0, 0}, {0, 0}, {0, 9600}, {0, 9600}},
	     {{256, 256}, {256, 256}}},
		{"an input's own delay is on what it tells only",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"in", "out"}}, .onBank = {{"in", Delay{{7, 7}}}}},
	     {Latency{10, 10}},
	     {Latency{100, 100}},
	     {{107, 107}, {10, 10}},
	     {{100, 100}}},
		// Regression (3b.3, 40be60af04): rtp-source's ports report zero once linked; the receiver's buffer lies behind
		// them, in series, so `stream` tells {B, B}, never {0, B}.
		{"a companion's beyond in series with peers reporting zero",
	     {{Direction::output, "stream", numbered(2)}, {Direction::input, "from_rtp", numbered(2), R::inside}},
	     {.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}},
	     {Latency{0, 0}, Latency{0, 0}},
	     {none, none},
	     {{0, 0}, {0, 0}, {480, 480}, {480, 480}},
	     {{0, 0}, {0, 0}}},
		{"a companion's beyond with no peer at all",
	     {{Direction::output, "stream", numbered(1)}, {Direction::input, "from_rtp", numbered(1), R::inside}},
	     {.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}},
	     {none},
	     {none},
	     {{0, 0}, {480, 480}},
	     {{0, 0}}},
		{"a beyond on an output bank lies in series with its downstream",
	     {{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}},
	     {.pairs = {{"in", "out"}}, .beyond = {{"out", Delay{{16, 16}}}}},
	     {none},
	     {Latency{100, 200}},
	     {{116, 216}, {0, 0}},
	     {{116, 216}}},
		// A source's ring declared beyond its bank: [0, capacity].
		{"a source's ring",
	     {{Direction::output, "out", numbered(2)}, {Direction::input, "ring", numbered(2), R::inside}},
	     {.pairs = {{"ring", "out"}}, .beyond = {{"ring", Delay{{0, 1024}}}}},
	     {none, none},
	     {none, none},
	     {{0, 0}, {0, 0}, {0, 1024}, {0, 1024}},
	     {{0, 0}, {0, 0}}},
		// A card block: playback from `play` to the card and capture from the card to `capture`, never mixed.
		{"a card block's two directions kept apart",
	     {{Direction::input, "play", numbered(1)},
	      {Direction::output, "capture", numbered(1)},
	      {Direction::output, "to_card", numbered(1), R::inside},
	      {Direction::input, "from_card", numbered(1), R::inside}},
	     {.pairs = {{"play", "to_card"}, {"from_card", "capture"}}},
	     {Latency{10, 10}, Latency{128, 128}},
	     {Latency{20, 20}, Latency{256, 256}},
	     {{256, 256}, {20, 20}, {128, 128}, {10, 10}},
	     {{20, 20}, {256, 256}}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		const Ports ports(row.banks);
		const latency_model::LatencyFacts facts = workOut(ports, row.plan, row.inputPeers, row.outputPeers);
		CHECK(facts.told == row.told);
		CHECK(facts.downstream == row.downstream);
	}
}

TEST_CASE("workOut with no peers is what the harness tells", "[latency]") {
	const Ports ports({{Direction::output, "stream", numbered(2)},
	                   {Direction::input, "from_rtp", numbered(2), PortBank::Reach::inside}});
	const LatencyPlan plan{.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}};
	const Peers none(2);
	const auto facts = workOut(ports, plan, none, none);
	CHECK(facts.told == std::vector<Latency>{{0, 0}, {0, 0}, {480, 480}, {480, 480}});
}

TEST_CASE("workOut refuses a plan that does not fit the ports, naming why", "[latency]") {
	const Ports ports({{Direction::input, "in", numbered(1)}, {Direction::output, "out", numbered(1)}});
	const Peers one(1);
	struct Row {
		const char* what;
		LatencyPlan plan;
	};
	const std::vector<Row> rows{
		{"a pair naming no bank", {.pairs = {{"nope", "out"}}}},
		{"a pair backwards", {.pairs = {{"out", "in"}}}},
		{"a pair's delay on no pair of the plan", {.onPair = {{{"in", "out"}, Delay{{1, 1}}}}}},
		{"a pair's delay twice",
	     {.pairs = {{"in", "out"}}, .onPair = {{{"in", "out"}, Delay{{1, 1}}}, {{"in", "out"}, Delay{{2, 2}}}}}},
		{"a bank's delay on no bank", {.onBank = {{"nope", Delay{{1, 1}}}}}},
		{"a bank's delay twice", {.onBank = {{"in", Delay{{1, 1}}}, {"in", Delay{{1, 1}}}}}},
		{"a beyond on no bank", {.beyond = {{"nope", Delay{{1, 1}}}}}},
		{"a beyond twice", {.beyond = {{"in", Delay{{1, 1}}}, {"in", Delay{{1, 1}}}}}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK_THROWS_AS(workOut(ports, row.plan, one, one), std::logic_error);
	}
	INFO("peers of the wrong count");
	CHECK_THROWS_AS(workOut(ports, {}, Peers(2), one), std::logic_error);
	CHECK_THROWS_AS(workOut(ports, {}, one, Peers{}), std::logic_error);
}

TEST_CASE("FrameClock: frames as time at the graph's rate, and the two dating rules", "[latency]") {
	struct Row {
		const char* what;
		std::uint32_t rate;
		std::int64_t frames;
		std::int64_t ns;
	};
	// Regression (rate conversion): the rate is the graph's, given once; one division of the whole count.
	const std::vector<Row> rows{
		{"a second at 48 kHz", 48000, 48000, 1'000'000'000},
		{"a frame at 48 kHz, truncated", 48000, 1, 20'833},
		{"a block at 48 kHz", 48000, 256, 5'333'333},
		{"a second at 44.1 kHz", 44100, 44100, 1'000'000'000},
		{"a frame at 44.1 kHz", 44100, 1, 22'675},
		{"two frames at 44.1 kHz: one division, not two", 44100, 2, 45'351},
		{"before, truncated toward zero", 48000, -1, -20'833},
		{"past 2^32 frames", 48000, std::int64_t{1} << 33, 178'956'970'666'666},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(FrameClock(row.rate).ns(row.frames) == row.ns);
	}
}

TEST_CASE("FrameClock dates a frame by its offset and the latency beside it", "[latency]") {
	const FrameClock clock(44100);
	const std::int64_t start = 1'000'000'000;
	// Heard once what lies downstream has played: its most, with the offset, in one division.
	CHECK(clock.heardRawNs(start, 0, {}) == start);
	CHECK(clock.heardRawNs(start, 1, Latency{0, 1}) == start + 45'351);
	CHECK(clock.heardRawNs(start, 100, Latency{256, 512}) == start + clock.ns(612));
	// Entered what lies upstream (its most) before.
	CHECK(clock.enteredRawNs(start, 0, Latency{0, 441}) == start - 10'000'000);
	CHECK(clock.enteredRawNs(start, 441, Latency{0, 441}) == start);
	CHECK(clock.enteredRawNs(start, 100, {}) == clock.heardRawNs(start, 100, {}));
}

// Regression (3b.1, e4b72cf661): a sink chunk that wraps its ring's end is two calls, and the second's start is its own
// first frame's: the cycle's start advanced by the first call's frames, never the cycle's start again.
TEST_CASE("A wrapped chunk's second call is dated by its own first frame", "[latency]") {
	const FrameClock clock(48000);
	const std::int64_t startRawNs = 7'000'000'000;
	const std::uint32_t frames = 256;
	for (const std::uint32_t first : {1U, 100U, 255U}) {
		INFO("the ring wraps after " << first << " frames");
		const std::int64_t secondStart = startRawNs + clock.ns(first);
		CHECK(secondStart == clock.heardRawNs(startRawNs, first, {}));
		CHECK(secondStart > startRawNs);
		// Its last frame is dated as the unsplit chunk's last frame.
		CHECK(secondStart + clock.ns(frames - first - 1) <= clock.heardRawNs(startRawNs, frames - 1, {}));
		CHECK(clock.heardRawNs(startRawNs, frames - 1, {}) - (secondStart + clock.ns(frames - first - 1)) <= 1);
	}
}
