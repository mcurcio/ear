// ear's one latency model (ear/core/latency.hpp, docs/engine.md#latency-per-bank-pair), by table: the algebra (series
// apart from parallel, no path apart from zero), then one row per bank shape and per past regression through workOut
// (the table, test/support/latency_table.hpp, which ear_pipewire_tests runs through a real block too), then
// FrameClock's units, its two roundings (framesCeil, framesNearest) and its two dating rules, and SampleRate, the
// rate it takes. What must not compile is in compile_fail/latency_bad.cpp and compile_fail/sample_rate_bad.cpp.
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/sample_rate.hpp"
#include "latency_table.hpp"

using namespace ear;
using Peers = std::vector<std::optional<Latency>>;

// A delay is never a path, and a path never a delay, by type.
static_assert(!std::is_convertible_v<Delay, Latency>);
static_assert(!std::is_convertible_v<Latency, Delay>);
static_assert(!std::is_convertible_v<Delay, std::optional<Latency>>);

// The algebra holds at compile time.
static_assert(Latency{1, 2} + Latency{10, 20} == Latency{11, 22});
static_assert(Paths{}.combined() == std::nullopt);
static_assert(Paths{}.orZero() == Latency{});
static_assert(FrameClock(ear::SampleRate::fixed<48000>()).ns(48000) == 1'000'000'000);

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
	for (const ear::test::LatencyRow& row : ear::test::latencyRows()) {
		INFO(row.what);
		const Ports ports(row.banks);
		const LatencyFacts facts = workOut(ports, row.plan, row.inputPeers, row.outputPeers);
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
		// Large positions: frames times 1e9 leaves int64 past about 9.2e9 frames (two days at 48 kHz); the whole
		// seconds and the rest convert apart, so the harness's position and any long count stay exact.
		{"a week at 96 kHz (the harness's position): frames times 1e9 overflows", 96000, 58'060'800'000,
	     604'800'000'000'000},
		{"2^40 frames at 48 kHz", 48000, std::int64_t{1} << 40, 22'906'492'245'333'333},
		{"2^40 frames before, at 44.1 kHz, truncated toward zero", 44100, -(std::int64_t{1} << 40),
	     -24'932'236'457'505'668},
		// Each site's case.
		{"a wrapped sink chunk's second call: the first's 100 frames at 48 kHz", 48000, 100, 2'083'333},
		{"sendspin's lead: its feed and a block, 2304 frames at 48 kHz", 48000, 2304, 48'000'000},
		{"sendspin's lead at 44.1 kHz, rounded up to ms by its caller", 44100, 2304, 52'244'897},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(FrameClock(*SampleRate::of(row.rate)).ns(row.frames) == row.ns);
	}
}

TEST_CASE("FrameClock: a duration as whole frames at the graph's rate", "[latency]") {
	using std::chrono::milliseconds;
	using std::chrono::nanoseconds;
	struct Row {
		const char* what;
		std::uint32_t rate;
		nanoseconds duration;
		std::int64_t frames;
	};
	const std::vector<Row> rows{
		// Each site's case.
		{"a fade (block, source, harness): 5 ms at 48 kHz", 48000, milliseconds{5}, 240},
		{"a fade at 44.1 kHz: 220.5 truncated", 44100, milliseconds{5}, 220},
		{"a fade at a rate of 1: none, which its callers raise to one frame", 1, milliseconds{5}, 0},
		{"sendspin's feed floor: 40 ms at 44.1 kHz", 44100, milliseconds{40}, 1764},
		{"sendspin's pieces to the server: 100 ms at 44.1 kHz", 44100, milliseconds{100}, 4410},
		{"sendspin's handoff: 500 ms at 44.1 kHz", 44100, milliseconds{500}, 22050},
		{"file's sink ring: 2 s at 48 kHz", 48000, std::chrono::seconds{2}, 96000},
		{"an RTP receiver's buffer: 3 ms at 44.1 kHz, 132.3 truncated", 44100, milliseconds{3}, 132},
		{"an RTP receiver's buffer: its most, 1000 ms at 96 kHz", 96000, milliseconds{1000}, 96000},
		// Rounding and range.
		{"a nanosecond is no frame", 48000, nanoseconds{1}, 0},
		{"a frame's length, truncated, is no frame", 48000, nanoseconds{20'833}, 0},
		{"before, truncated toward zero", 48000, -milliseconds{5}, -240},
		{"a week at 96 kHz: ns times the rate overflows", 96000, std::chrono::hours{24 * 7}, 58'060'800'000},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(FrameClock(*SampleRate::of(row.rate)).frames(row.duration) == row.frames);
	}
}

// R3 moved every site onto FrameClock and changed no date: where the one division each site made did not overflow,
// FrameClock gives the same number, at every rate a graph may run.
TEST_CASE("FrameClock gives what each site's one division gave", "[latency]") {
	for (const std::uint32_t rate : {1U, 8000U, 22050U, 44100U, 48000U, 96000U, 192000U, 384000U, 768000U}) {
		INFO("at " << rate << " Hz");
		const FrameClock clock(*ear::SampleRate::of(rate));
		for (std::int64_t frames = -2048; frames <= 100'000; frames += 7)
			REQUIRE(clock.ns(frames) == frames * 1'000'000'000 / std::int64_t{rate});
		for (std::int64_t ms = 0; ms <= 2000; ++ms)
			REQUIRE(clock.frames(std::chrono::milliseconds{ms}) == ms * rate / 1000);
	}
}

namespace {

using Wide = __int128;
constexpr Wide nsPerSecond = 1'000'000'000;
constexpr Wide int64Max = std::numeric_limits<std::int64_t>::max();
constexpr Wide int64Min = std::numeric_limits<std::int64_t>::min();

// Every rate a graph may run at, the real ones and the awkward small ones (1 Hz: whole seconds; 3 and 7 Hz: thirds and
// sevenths of a second, which no ns count holds exactly).
constexpr std::uint32_t oracleRates[] = {1,     2,     3,     7,      8000,   11025,  16000,  22050,  32000, 44100,
                                         48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};

bool fits(Wide value) { return value >= int64Min && value <= int64Max; }

// The counts where a conversion can go wrong: zero, one frame either way, a whole number of seconds and one frame
// either side of it, where frames * 1e9 leaves int64 (about 9.2e9), and the largest count whose result still fits.
std::vector<Wide> edgeCounts(std::uint32_t rate, Wide perSecond, Wide largest) {
	std::vector<Wide> counts{0, 1, -1, largest, largest - 1, -largest, -largest + 1};
	for (const Wide k : {Wide{1}, Wide{2}, Wide{1000}, Wide{86'400}, Wide{86'400} * 365})
		for (const Wide d : {Wide{-1}, Wide{0}, Wide{1}}) {
			counts.push_back(perSecond * k + d);
			counts.push_back(-(perSecond * k + d));
		}
	for (const Wide around :
	     {int64Max / nsPerSecond, int64Max / nsPerSecond + 1, int64Max / Wide{rate}, int64Max / Wide{rate} + 1})
		for (const Wide d : {Wide{-1}, Wide{0}, Wide{1}}) {
			counts.push_back(around + d);
			counts.push_back(-(around + d));
		}
	return counts;
}

} // namespace

// The exact quotient, truncated toward zero, worked out in 128 bits (where no count here overflows) against FrameClock
// in 64: ns(frames) for every frame count whose ns fit int64, and frames(duration) for every duration that is an
// int64, including those past int64_max / rate where ns * rate would overflow.
TEST_CASE("FrameClock agrees with the exact quotient at every rate and every edge count", "[latency]") {
	for (const std::uint32_t rate : oracleRates) {
		INFO("at " << rate << " Hz");
		const FrameClock clock(*ear::SampleRate::of(rate));
		// ns(frames): the largest frame count whose time fits is int64_max * rate / 1e9.
		for (const Wide frames : edgeCounts(rate, rate, int64Max * Wide{rate} / nsPerSecond)) {
			const Wide exact = frames * nsPerSecond / Wide{rate};
			if (!fits(frames) || !fits(exact)) continue;
			INFO("frames " << static_cast<std::int64_t>(frames));
			REQUIRE(clock.ns(static_cast<std::int64_t>(frames)) == static_cast<std::int64_t>(exact));
		}
		// frames(duration): every int64 duration fits once converted, so int64_max itself is an edge.
		for (const Wide ns : edgeCounts(rate, nsPerSecond, int64Max)) {
			if (!fits(ns)) continue;
			const Wide exact = ns * Wide{rate} / nsPerSecond;
			INFO("ns " << static_cast<std::int64_t>(ns));
			REQUIRE(clock.frames(std::chrono::nanoseconds{static_cast<std::int64_t>(ns)}) ==
			        static_cast<std::int64_t>(exact));
		}
		// Negative and sub-millisecond durations: a frame's length either side, and a millisecond's.
		const Wide frameNs = nsPerSecond / Wide{rate};
		for (const Wide ns :
		     {Wide{1}, Wide{999'999}, Wide{1'000'000}, frameNs - 1, frameNs, frameNs + 1, 2 * frameNs + 1})
			for (const Wide sign : {Wide{1}, Wide{-1}}) {
				const Wide signedNs = sign * ns;
				INFO("ns " << static_cast<std::int64_t>(signedNs));
				REQUIRE(clock.frames(std::chrono::nanoseconds{static_cast<std::int64_t>(signedNs)}) ==
				        static_cast<std::int64_t>(signedNs * Wide{rate} / nsPerSecond));
			}
	}
}

namespace {

// The exact quotient worked out in 128 bits, rounded up, and rounded to the nearest with halves away from zero.
Wide ceilQuotient(Wide numerator, Wide denominator) {
	return numerator / denominator + (numerator % denominator > 0 ? 1 : 0);
}
Wide nearestQuotient(Wide numerator, Wide denominator) {
	const Wide left = numerator % denominator;
	return numerator / denominator + (2 * left >= denominator ? 1 : (2 * left <= -denominator ? -1 : 0));
}

} // namespace

// A rate is made only from a number that is one, and never converts back but through hz().
static_assert(SampleRate::of(0) == std::nullopt);
static_assert(SampleRate::of(48000)->hz() == 48000);
static_assert(SampleRate::of(UINT32_MAX)->hz() == UINT32_MAX);
static_assert(SampleRate::fixed<44100>() == *SampleRate::of(44100));
static_assert(SampleRate::fixed<44100>() < SampleRate::fixed<48000>());
static_assert(!std::is_convertible_v<std::uint32_t, SampleRate> && !std::is_constructible_v<SampleRate, std::uint32_t>);
static_assert(!std::is_convertible_v<SampleRate, std::uint32_t> && !std::is_convertible_v<SampleRate, double>);
static_assert(FrameClock(SampleRate::fixed<48000>()).ns(48000) == 1'000'000'000);
static_assert(FrameClock(SampleRate::fixed<48000>())
                  .framesCeil(std::chrono::duration<std::int64_t, std::ratio<1, 30>>{1}) == 1600);
// FrameClock is made only from a rate, and gives back the rate it was made from.
static_assert(!std::is_constructible_v<FrameClock, std::uint32_t> && !std::is_constructible_v<FrameClock, int>);
static_assert(FrameClock(SampleRate::fixed<44100>()).rate() == SampleRate::fixed<44100>());

// framesCeil: exact for any period, so the level window's thirtieth of a second is ceil(rate / 30), the rule
// levelWindowFrames wrote by hand ((rate + 29) / 30).
TEST_CASE("FrameClock::framesCeil rounds up, exactly, for any period", "[latency]") {
	using Thirtieth = std::chrono::duration<std::int64_t, std::ratio<1, 30>>;
	using Seventh = std::chrono::duration<std::int64_t, std::ratio<1, 7>>;
	using std::chrono::milliseconds;
	for (const std::uint32_t rate : oracleRates) {
		INFO("at " << rate << " Hz");
		const FrameClock clock(*SampleRate::of(rate));
		REQUIRE(clock.framesCeil(Thirtieth{1}) == (std::int64_t{rate} + 29) / 30);
		for (const std::int64_t count : {-61, -30, -29, -1, 0, 1, 2, 29, 30, 31, 3600, 2'592'000}) {
			INFO(count << " thirtieths");
			REQUIRE(clock.framesCeil(Thirtieth{count}) ==
			        static_cast<std::int64_t>(ceilQuotient(Wide{count} * Wide{rate}, Wide{30})));
		}
		for (const std::int64_t count : {-8, -7, -1, 0, 1, 6, 7, 8, 7 * 86'400}) {
			INFO(count << " sevenths");
			REQUIRE(clock.framesCeil(Seventh{count}) ==
			        static_cast<std::int64_t>(ceilQuotient(Wide{count} * Wide{rate}, Wide{7})));
		}
		for (const std::int64_t ms : {-1001, -1, 0, 1, 5, 999, 1000, 1001}) {
			INFO(ms << " ms");
			REQUIRE(clock.framesCeil(milliseconds{ms}) ==
			        static_cast<std::int64_t>(ceilQuotient(Wide{ms} * Wide{rate}, Wide{1000})));
		}
		REQUIRE(clock.framesCeil(std::chrono::hours{24}) == std::int64_t{rate} * 86'400);
	}
	struct Row {
		const char* what;
		std::uint32_t rate;
		std::int64_t thirtieths;
		std::int64_t frames;
	};
	const std::vector<Row> rows{
		{"the level window at 48 kHz", 48000, 1, 1600},
		{"the level window at 44.1 kHz", 44100, 1, 1470},
		{"the level window at 22.05 kHz: 735 exactly", 22050, 1, 735},
		{"the level window at 11.025 kHz: 367.5 rounded up", 11025, 1, 368},
		{"the level window at 1 Hz: a thirtieth of a frame, rounded up to one", 1, 1, 1},
		{"a thirtieth before, rounded up toward zero", 44100, -1, -1470},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(FrameClock(*SampleRate::of(row.rate)).framesCeil(Thirtieth{row.thirtieths}) == row.frames);
	}
}

// framesNearest: a peer's latency in ns at the graph's rate, to the nearest frame, halves away from zero (as lround
// rounds), worked out in integers so no count loses a frame to a double's rounding.
TEST_CASE("FrameClock::framesNearest rounds to the nearest frame, exactly", "[latency]") {
	using std::chrono::nanoseconds;
	for (const std::uint32_t rate : oracleRates) {
		INFO("at " << rate << " Hz");
		const FrameClock clock(*SampleRate::of(rate));
		for (const Wide ns : edgeCounts(rate, nsPerSecond, int64Max)) {
			if (!fits(ns)) continue;
			INFO("ns " << static_cast<std::int64_t>(ns));
			REQUIRE(clock.framesNearest(nanoseconds{static_cast<std::int64_t>(ns)}) ==
			        static_cast<std::int64_t>(nearestQuotient(ns * Wide{rate}, nsPerSecond)));
		}
		const Wide frameNs = nsPerSecond / Wide{rate};
		for (const Wide ns : {Wide{1}, frameNs / 2 - 1, frameNs / 2, frameNs / 2 + 1, frameNs, frameNs + frameNs / 2})
			for (const Wide sign : {Wide{1}, Wide{-1}}) {
				INFO("ns " << static_cast<std::int64_t>(sign * ns));
				REQUIRE(clock.framesNearest(nanoseconds{static_cast<std::int64_t>(sign * ns)}) ==
				        static_cast<std::int64_t>(nearestQuotient(sign * ns * Wide{rate}, nsPerSecond)));
			}
	}
	struct Row {
		const char* what;
		std::uint32_t rate;
		nanoseconds duration;
		std::int64_t frames;
	};
	const std::vector<Row> rows{
		{"a millisecond at 48 kHz", 48000, nanoseconds{1'000'000}, 48},
		{"half a frame at 48 kHz, rounded away from zero", 48000, nanoseconds{10'417}, 1},
		{"just under half a frame", 48000, nanoseconds{10'416}, 0},
		{"half a frame before, away from zero", 48000, nanoseconds{-10'417}, -1},
		{"a millisecond at 44.1 kHz: 44.1 rounded down", 44100, nanoseconds{1'000'000}, 44},
		{"10 ms at 44.1 kHz: 441", 44100, nanoseconds{10'000'000}, 441},
		{"0.25 s at 2 Hz: half a frame, away from zero", 2, nanoseconds{250'000'000}, 1},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(FrameClock(*SampleRate::of(row.rate)).framesNearest(row.duration) == row.frames);
	}
}

TEST_CASE("FrameClock dates a frame by its offset and the latency beside it", "[latency]") {
	const FrameClock clock(ear::SampleRate::fixed<44100>());
	const std::int64_t start = 1'000'000'000;
	// Heard once what lies downstream has played: its most, with the offset, in one division.
	CHECK(clock.heardRawNs(start, 0, {}) == start);
	CHECK(clock.heardRawNs(start, 1, Latency{0, 1}) == start + 45'351);
	CHECK(clock.heardRawNs(start, 100, Latency{256, 512}) == start + clock.ns(612));
	// Entered what lies upstream (its most) before.
	CHECK(clock.enteredRawNs(start, 0, Latency{0, 441}) == start - 10'000'000);
	CHECK(clock.enteredRawNs(start, 441, Latency{0, 441}) == start);
	CHECK(clock.enteredRawNs(start, 100, {}) == clock.heardRawNs(start, 100, {}));
	// A frame before the cycle's first (sendspin's capture, taken before its newest anchor).
	CHECK(clock.enteredRawNs(start, -441, {}) == start - 10'000'000);
	CHECK(clock.enteredRawNs(start, -1, {}) == start - 22'675);
	CHECK(clock.enteredRawNs(start, -441, Latency{0, 441}) == start - 20'000'000);
}

TEST_CASE("Each dating site's case, by FrameClock", "[latency]") {
	struct Row {
		const char* what;
		std::uint32_t rate;
		std::int64_t startRawNs;
		std::int64_t offset;
		Latency beside;
		bool heard; // heardRawNs with `beside` downstream; else enteredRawNs with it upstream
		std::int64_t ns;
	};
	constexpr std::int64_t start = 5'000'000'000'000; // well past boot, as the raw clock is
	const std::vector<Row> rows{
		{"256 frames played, no peer", 48000, start, 256, {}, true, start + 5'333'333},
		{"256 frames played behind a card's 1024", 48000, start, 256, Latency{256, 1024}, true, start + 26'666'666},
		{"sendspin's progress: one division over the frames and the downstream", 44100, start, 100, Latency{0, 512},
	     true, start + 13'877'551},
		{"sendspin's capture, 256 frames after its anchor", 48000, start, 256, {}, false, start + 5'333'333},
		{"sendspin's capture, 256 frames before its anchor", 48000, start, -256, {}, false, start - 5'333'333},
		{"a sink's chunk behind a card's capture of 1024", 48000, start, 0, Latency{0, 1024}, false,
	     start - 21'333'333},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		const FrameClock clock(*SampleRate::of(row.rate));
		if (row.heard)
			CHECK(clock.heardRawNs(row.startRawNs, static_cast<std::uint32_t>(row.offset), row.beside) == row.ns);
		else CHECK(clock.enteredRawNs(row.startRawNs, row.offset, row.beside) == row.ns);
	}
}

// Regression (e4b72cf661): a sink chunk that wraps its ring's end is two calls, and the second's start is its own
// first frame's: the cycle's start advanced by the first call's frames, never the cycle's start again.
TEST_CASE("A wrapped chunk's second call is dated by its own first frame", "[latency]") {
	const FrameClock clock(ear::SampleRate::fixed<48000>());
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
