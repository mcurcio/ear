// Where PipeWire's latency triples meet ear's frames (latency_spa.hpp, docs/engine.md#latency-per-bank-pair): the
// conversion rows, and ear's parallel rule (Paths) checked against PipeWire's own spa_latency_info_combine and
// spa_latency_info_combine_finish on the same peers, one unit at a time, so a PipeWire that changes its rule fails
// here. No engine: the functions are pure.
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <spa/param/latency-utils.h>

#include "../latency_spa.hpp"
#include "catch2/catch.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"

using namespace ear;
using ear::pipewire::fromSpa;
using ear::pipewire::toSpa;

namespace {

constexpr std::uint32_t kQuantum = 256;
constexpr std::uint32_t kRate = 48000;

enum class Unit { quantum, rate, ns };

/// A peer's report {least, most} in one unit only, as a playback (input-side) latency.
spa_latency_info inUnit(Unit unit, std::int64_t least, std::int64_t most) {
	spa_latency_info info{};
	info.direction = SPA_DIRECTION_INPUT;
	switch (unit) {
	case Unit::quantum:
		info.min_quantum = static_cast<float>(least) / 4.0F; // quarter quanta: exact in float
		info.max_quantum = static_cast<float>(most) / 4.0F;
		break;
	case Unit::rate:
		info.min_rate = static_cast<std::int32_t>(least);
		info.max_rate = static_cast<std::int32_t>(most);
		break;
	case Unit::ns:
		info.min_ns = least * 1000;
		info.max_ns = most * 1000;
		break;
	}
	return info;
}

/// PipeWire's own parallel combination of `peers`, unfinished (as spa leaves it with no peer) or finished.
spa_latency_info spaCombined(const std::vector<spa_latency_info>& peers, bool finish) {
	spa_latency_info info{};
	spa_latency_info_combine_start(&info, SPA_DIRECTION_INPUT);
	for (const spa_latency_info& peer : peers) spa_latency_info_combine(&info, &peer);
	if (finish) spa_latency_info_combine_finish(&info);
	return info;
}

/// ear's: each peer converted at the port, then Paths.
Paths earCombined(const std::vector<spa_latency_info>& peers) {
	Paths paths;
	for (const spa_latency_info& peer : peers)
		paths.add(fromSpa(peer, kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())));
	return paths;
}

/// A fixed pseudo-random sequence, so every run checks the same peers.
struct Sequence {
	std::uint64_t state = 0x9E3779B97F4A7C15ULL;
	std::int64_t next(std::int64_t bound) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return static_cast<std::int64_t>((state >> 33U) % static_cast<std::uint64_t>(bound));
	}
};

} // namespace

TEST_CASE("A peer's triple comes to frames at the graph's quantum and rate", "[latency]") {
	struct Row {
		const char* what;
		spa_latency_info info;
		Latency frames;
	};
	const auto info = [](float minQ, float maxQ, std::int32_t minR, std::int32_t maxR, std::int64_t minNs,
	                     std::int64_t maxNs) {
		spa_latency_info made{};
		made.direction = SPA_DIRECTION_INPUT;
		made.min_quantum = minQ;
		made.max_quantum = maxQ;
		made.min_rate = minR;
		made.max_rate = maxR;
		made.min_ns = minNs;
		made.max_ns = maxNs;
		return made;
	};
	const std::vector<Row> rows{
		{"zero", info(0, 0, 0, 0, 0, 0), {0, 0}},
		{"one quantum, as ALSA's playback node reports its period", info(1, 1, 0, 0, 0, 0), {256, 256}},
		{"rate units are frames", info(0, 0, 480, 960, 0, 0), {480, 960}},
		// Regression (rate conversion): ns at the graph's rate, rounded to the nearest frame.
		{"10 ms at 48 kHz", info(0, 0, 0, 0, 10'000'000, 10'000'000), {480, 480}},
		{"a frame and a bit rounds down", info(0, 0, 0, 0, 25'000, 25'000), {1, 1}},
		{"a frame and most of one rounds up", info(0, 0, 0, 0, 40'000, 40'000), {2, 2}},
		{"all three units add", info(0.5F, 1, 10, 20, 1'000'000, 2'000'000), {128 + 10 + 48, 256 + 20 + 96}},
		{"a negative total is zero", info(0, 0, -10, -5, 0, 0), {0, 0}},
		{"a sum past what frames hold saturates", info(0, 1e9F, 0, 0, 0, 0), {0, UINT32_MAX}},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		CHECK(fromSpa(row.info, kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) == row.frames);
	}
	// The same ns at another rate.
	CHECK(fromSpa(info(0, 0, 0, 0, 10'000'000, 10'000'000), kQuantum, FrameClock(ear::SampleRate::fixed<44100>())) ==
	      Latency{441, 441});
	// An unfinished empty combination is no path, not zero.
	CHECK(fromSpa(spaCombined({}, false), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) == std::nullopt);
	CHECK(fromSpa(spaCombined({}, true), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) == Latency{});
}

TEST_CASE("What a block tells goes back in rate units only, and reads back the same", "[latency]") {
	for (const Latency latency : {Latency{}, Latency{256, 256}, Latency{0, 1088}, Latency{480, 9600}}) {
		for (const spa_direction direction : {SPA_DIRECTION_INPUT, SPA_DIRECTION_OUTPUT}) {
			const spa_latency_info told = toSpa(latency, direction);
			CHECK(told.direction == direction);
			CHECK(told.min_quantum == 0.0F);
			CHECK(told.max_quantum == 0.0F);
			CHECK(told.min_ns == 0);
			CHECK(told.max_ns == 0);
			CHECK(fromSpa(told, kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) == latency);
		}
	}
	CHECK(toSpa(Latency{0, UINT32_MAX}, SPA_DIRECTION_INPUT).max_rate == std::numeric_limits<std::int32_t>::max());
}

TEST_CASE("ear's parallel combine is PipeWire's spa_latency_info_combine, unit by unit", "[latency]") {
	Sequence sequence;
	for (const Unit unit : {Unit::quantum, Unit::rate, Unit::ns}) {
		for (int trial = 0; trial < 200; ++trial) {
			std::vector<spa_latency_info> peers;
			const auto count = sequence.next(5); // none to four peers
			for (std::int64_t i = 0; i < count; ++i) {
				const std::int64_t least = sequence.next(2000);
				peers.push_back(inUnit(unit, least, least + sequence.next(2000)));
			}
			INFO("unit " << static_cast<int>(unit) << ", trial " << trial << ", " << count << " peers");
			const Paths ours = earCombined(peers);
			// Unfinished, PipeWire's empty combination is ear's no path; finished, ear's orZero.
			CHECK(fromSpa(spaCombined(peers, false), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) ==
			      ours.combined());
			CHECK(fromSpa(spaCombined(peers, true), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>())) ==
			      ours.orZero());
		}
	}
}

TEST_CASE("Peers in mixed units: frames first is exact, inside PipeWire's per-unit bound", "[latency]") {
	// One peer a quantum (256 frames), the other 100 rate units: per unit PipeWire takes the least quantum (0) and the
	// least rate (0), and the most of each (1 and 100), so {0, 356}; the paths are 256 and 100, so {100, 256}.
	const std::vector<spa_latency_info> peers{inUnit(Unit::quantum, 4, 4), inUnit(Unit::rate, 100, 100)};
	const Latency bound = *fromSpa(spaCombined(peers, true), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>()));
	CHECK(bound == Latency{0, 356});
	CHECK(earCombined(peers).combined() == Latency{100, 256});

	Sequence sequence;
	for (int trial = 0; trial < 200; ++trial) {
		std::vector<spa_latency_info> mixed;
		for (const Unit unit : {Unit::quantum, Unit::rate, Unit::ns}) {
			const std::int64_t least = sequence.next(2000);
			mixed.push_back(inUnit(unit, least, least + sequence.next(2000)));
		}
		INFO("trial " << trial);
		const Latency outer = *fromSpa(spaCombined(mixed, true), kQuantum, FrameClock(ear::SampleRate::fixed<kRate>()));
		const Latency exact = earCombined(mixed).orZero();
		CHECK(exact.minFrames >= outer.minFrames);
		CHECK(exact.maxFrames <= outer.maxFrames);
	}
}

// A negative peer is clamped at the port, to no latency, before anything is added in series beyond the bank: a
// receiver's 480 frames behind a peer reporting -100 tell 480. Summed in PipeWire's units first (the block before
// ear's latency model), the same peer told 380 (docs/engine.md#latency-per-bank-pair).
TEST_CASE("A peer's negative latency is clamped at the port, before a companion's beyond is added in series",
          "[latency]") {
	spa_latency_info negative{};
	negative.direction = SPA_DIRECTION_OUTPUT;
	negative.min_rate = -100;
	negative.max_rate = -100;
	const std::optional<Latency> peer = fromSpa(negative, kQuantum, FrameClock(ear::SampleRate::fixed<kRate>()));
	CHECK(peer == Latency{0, 0});
	const Ports ports({{Direction::output, "stream", numbered(1)},
	                   {Direction::input, "from_rtp", numbered(1), PortBank::Reach::inside}});
	const LatencyPlan plan{.pairs = {{"from_rtp", "stream"}}, .beyond = {{"from_rtp", Delay{{480, 480}}}}};
	const std::vector<std::optional<Latency>> inputs{peer};
	const std::vector<std::optional<Latency>> outputs{std::nullopt};
	CHECK(workOut(ports, plan, inputs, outputs).told == std::vector<Latency>{{0, 0}, {480, 480}});
}
