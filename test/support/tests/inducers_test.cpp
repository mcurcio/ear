// The inducers themselves (test/support/inducers.hpp), on cycles made by hand: which cycles a plan stalls and for how
// long, a stall carried out and counted, a ledger keeping what each cycle's block stated, and StallingCode doing
// both. The engine tests that use them (the clock tests in port_block_test.cpp, S5, the source's cut-short cycles)
// prove them live.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "catch2/catch.hpp"
#include "inducers.hpp"

namespace {
using namespace std::chrono_literals;
using ear::test::CycleLedger;
using ear::test::StallingCode;
using ear::test::StallLength;
using ear::test::StallPlan;
using ear::test::Stalls;

/// A block's cycle as the framework would give it: `frames` on from `position`, nothing lost before it.
ear::GraphCycle cycleAt(std::uint64_t position) {
	ear::GraphCycle cycle{};
	cycle.position = position;
	cycle.startRawNs = static_cast<std::int64_t>(position) * 1'000'000'000 / 48000;
	return cycle;
}

} // namespace

TEST_CASE("A stall length is a fixed time, or a number of the stalled cycle's durations", "[inducers]") {
	CHECK(StallLength::of(12ms).at(256, ear::FrameClock(ear::SampleRate::fixed<48000>())) == 12ms);
	CHECK(StallLength::of(12ms).at(64, ear::FrameClock(ear::SampleRate::fixed<44100>())) == 12ms);
	CHECK(StallLength::cycles(1.5).at(256, ear::FrameClock(ear::SampleRate::fixed<48000>())) == 8ms); // 1.5 x 5.333 ms
	CHECK(StallLength::cycles(1.5).at(64, ear::FrameClock(ear::SampleRate::fixed<48000>())) ==
	      2'000'000ns); // 1.5 x 1.333 ms
	CHECK(StallLength::cycles(2.0).at(480, ear::FrameClock(ear::SampleRate::fixed<48000>())) == 20ms);
}

TEST_CASE("A plan names the cycles listed, or every n-th from its phase up to its limit, and nothing else",
          "[inducers]") {
	const StallPlan none;
	CHECK_FALSE(none.planned(0));
	CHECK(none.size() == 0);

	const StallPlan listed = StallPlan::at({{200, StallLength::of(30ms)}, {600, StallLength::of(60ms)}});
	CHECK(listed.size() == 2);
	REQUIRE(listed.planned(200));
	CHECK(listed.planned(200)->at(256, ear::FrameClock(ear::SampleRate::fixed<48000>())) == 30ms);
	CHECK(listed.planned(600)->at(256, ear::FrameClock(ear::SampleRate::fixed<48000>())) == 60ms);
	CHECK_FALSE(listed.planned(199));
	CHECK_FALSE(listed.planned(201));

	// S5's: every 16th from the 4th, 3 of them.
	const StallPlan every = StallPlan::every(16, 4, 3, StallLength::cycles(1.5));
	CHECK(every.size() == 3);
	std::vector<std::size_t> stalled;
	for (std::size_t n = 0; n < 100; ++n)
		if (every.planned(n)) stalled.push_back(n);
	CHECK(stalled == std::vector<std::size_t>{4, 20, 36});

	const StallPlan unbounded = StallPlan::every(50, 49, std::numeric_limits<std::size_t>::max(), StallLength::of(1ms));
	CHECK(unbounded.planned(49));
	CHECK(unbounded.planned(1'000'049));
	CHECK_FALSE(unbounded.planned(50));
}

TEST_CASE("Stalls sleep the planned length at a planned cycle, return at once at any other, and count what they made",
          "[inducers]") {
	Stalls stalls(StallPlan::at({{1, StallLength::of(20ms)}}));
	const auto before = std::chrono::steady_clock::now();
	CHECK_FALSE(stalls(0, 256, ear::SampleRate::fixed<48000>()));
	CHECK(stalls.made() == 0);
	CHECK(stalls(1, 256, ear::SampleRate::fixed<48000>()));
	CHECK(std::chrono::steady_clock::now() - before >= 20ms); // a sleep is at least its length
	CHECK(stalls.made() == 1);
	CHECK_FALSE(stalls(2, 256, ear::SampleRate::fixed<48000>()));
	CHECK(stalls.made() == 1);
}

TEST_CASE("A ledger keeps each cycle as its block stated it, working nothing out, until it is full", "[inducers]") {
	CycleLedger ledger(3);
	ear::GraphCycle lost = cycleAt(1768);
	lost.facts = {.graphCycle = 9, .missed = 1, .missedCut = 1, .jumpFrames = 256, .causes = ear::CycleCause::catchUp};
	REQUIRE(ledger.record(cycleAt(1000), 256) == 0U);
	REQUIRE(ledger.record(cycleAt(1256), 256) == 1U);
	REQUIRE(ledger.record(lost, 256) == 2U);
	CHECK(ledger[0].whole()); // a GraphCycle with empty facts: the ledger never judges the positions itself
	CHECK(ledger[1].whole());
	CHECK_FALSE(ledger[2].whole());
	CHECK(ledger[2].facts == lost.facts);
	CHECK(ledger[2].position == 1768);
	CHECK(ledger[2].frames == 256);
	CHECK(ledger[2].startRawNs == lost.startRawNs);
	CHECK(ledger.full());
	CHECK_FALSE(ledger.overflowed());
	CHECK_FALSE(ledger.record(cycleAt(2024), 256));
	CHECK(ledger.overflowed());
	CHECK(ledger.count() == 3);
	CHECK(ledger.cycles().size() == 3);
}

TEST_CASE("StallingCode writes silence, records every cycle, stalls at its plan's cycles and wakes each cycle",
          "[inducers]") {
	std::size_t wakes = 0;
	StallingCode code(4, StallPlan::at({{1, StallLength::cycles(1.5)}}), [&] { ++wakes; });
	code.prepare(ear::AudioFormat{ear::SampleRate::fixed<48000>(), 256});
	std::vector<float> out(256, std::nanf(""));
	const std::vector<std::span<float>> outputs{out};
	for (std::uint64_t n = 0; n < 6; ++n) code.process({}, outputs, 256, cycleAt(n * 256));
	CHECK(wakes == 6);
	CHECK(code.stalls() == 1);
	CHECK(std::ranges::all_of(out, [](float sample) { return sample == 0.0F; }));
	const CycleLedger& ledger = code.ledger();
	REQUIRE(ledger.count() == 4); // full: the two cycles after it are not recorded
	CHECK(ledger.overflowed());
	CHECK(ledger[3].position == 3 * 256);
}
