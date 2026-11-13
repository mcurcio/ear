// The one judge of lost cycles (test/support/cycle_judge.hpp, docs/testing.md#the-cycle-judge), on facts made by hand:
// each verdict and the rule behind it, a plan that judges nothing refused, and done() needing the planned boundaries
// and every planned stall made and excused. The engine tests that judge through it prove it live.
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "catch2/catch.hpp"
#include "cycle_judge.hpp"
#include "ear/core/cut_ring.hpp"

namespace {
using namespace std::chrono_literals;
using ear::CycleCause;
using ear::CycleFacts;
using ear::FrameClock;
using ear::test::CycleJudge;
using ear::test::JudgePlan;
using ear::test::StallLength;
using ear::test::StallPlan;
using ear::test::Verdict;

constexpr std::uint32_t kFrames = 256;
const FrameClock kClock(48000);

/// A judge of `judged` boundaries that makes no stall: the verdicts alone.
JudgePlan plain(std::size_t judged = 1, JudgePlan::Expected expected = {}) {
	return JudgePlan{.judged = judged, .induce = StallPlan{}, .expected = expected};
}

/// The facts of a cycle after `missed` cycles, `cut` of them cut, the position moved by `jump` frames and the raw clock
/// by `rawNs` beyond the last cycle's end.
CycleFacts caughtUp(std::uint32_t missed, std::uint32_t cut, std::int64_t jump, std::int64_t rawNs) {
	return CycleFacts{.graphCycle = 100,
	                  .missed = missed,
	                  .missedCut = cut,
	                  .lastCut = false,
	                  .jumpFrames = jump,
	                  .lostRawNs = rawNs,
	                  .causes = CycleCause::catchUp};
}

} // namespace

TEST_CASE("A cycle judge refuses a plan that judges nothing, and is not done before its first judged cycle",
          "[cycle_judge]") {
	CHECK_THROWS_AS(CycleJudge(plain(0), kClock, nullptr), std::invalid_argument);
	CHECK_THROWS_AS(CycleJudge(JudgePlan{}, kClock, nullptr), std::invalid_argument);
	// A plan that stalls needs the record that numbers each stall's cycle.
	CHECK_THROWS_AS(CycleJudge(JudgePlan{.judged = 1}, kClock, nullptr), std::invalid_argument);
	CycleJudge judge(plain(1), kClock, nullptr);
	CHECK_FALSE(judge.done());
	CHECK(judge.at(CycleFacts{}, kFrames) == Verdict::judge);
	CHECK(judge.done());
	CHECK(judge.report().ok());
	// By default a plan makes four stalls of one and a half cycles.
	CHECK(JudgePlan{.judged = 1}.induce.size() == 4);
	CHECK(JudgePlan{.judged = 1}.induce.planned(7));
}

TEST_CASE("A cycle judge judges a whole cycle, and excuses exactly the cycles the driver cut, by their frames, within "
          "1 ms of the raw clock",
          "[cycle_judge]") {
	CycleJudge judge(plain(1), kClock, nullptr);
	CHECK(judge.at(CycleFacts{}, kFrames) == Verdict::judge);
	// Three missed, three cut, three cycles' frames, the raw clock three cycles on: excused, all three counted.
	CHECK(judge.at(caughtUp(3, 3, 3 * kFrames, kClock.ns(3 * kFrames)), kFrames) == Verdict::excused);
	// The position may lead the raw clock by the driver's 1 ms clamp, and no more: a cycle's start is the driver's
	// deadline, so a late wake leaves no room.
	const std::int64_t room = 1'000'000;
	CHECK(judge.at(caughtUp(1, 1, kFrames, kClock.ns(kFrames) - room), kFrames) == Verdict::excused);
	CHECK(judge.report().excusedCycles == 4);
	CHECK(judge.report().excusedEvents == 2);
	CHECK(judge.report().failed == 0);
	CHECK(judge.at(caughtUp(1, 1, kFrames, kClock.ns(kFrames) - room - 1), kFrames) == Verdict::fail);
	CHECK(judge.report().firstFailure.find("cycle 3 (") == 0);
	CHECK(judge.report().firstFailure.find("further than the raw clock went") != std::string::npos);
	CHECK(judge.report().excusedCycles == 4);
	CHECK(judge.report().mostOverLostNs == std::optional<std::int64_t>(room + 1)); // the evidence, excused or not
	CHECK_FALSE(judge.report().ok());
}

TEST_CASE("A cycle judge fails a catch-up over a cycle the driver did not cut, one whose jump is not its cycles' "
          "frames, and every reposition",
          "[cycle_judge]") {
	const auto first = [](CycleFacts facts) {
		CycleJudge judge(plain(), kClock, nullptr);
		CHECK(judge.at(facts, kFrames) == Verdict::fail);
		CHECK(judge.report().failed == 1);
		CHECK(judge.report().excusedCycles == 0);
		return judge.report().firstFailure;
	};
	CHECK_THAT(first(caughtUp(3, 2, 3 * kFrames, kClock.ns(3 * kFrames))),
	           Catch::Contains("the driver did not all cut"));
	CHECK_THAT(first(caughtUp(3, 3, 2 * kFrames, kClock.ns(3 * kFrames))),
	           Catch::Contains("not its missed cycles' frames"));
	CHECK_THAT(first(caughtUp(0, 0, 0, 0)), Catch::Contains("not its missed cycles' frames"));
	CycleFacts back{.graphCycle = 7, .jumpFrames = -64, .causes = CycleCause::reposition};
	CHECK_THAT(first(back), Catch::Contains("a reposition"));
	CycleFacts missedNoMove{.graphCycle = 7, .missed = 2, .missedCut = 2, .causes = CycleCause::reposition};
	CHECK_THAT(first(missedNoMove), Catch::Contains("a reposition"));
	// A cause the judge has no rule for (one added to CycleCause later) fails: deny by default.
	CycleFacts unknown{.graphCycle = 7, .causes = static_cast<CycleCause>(1U << 7U)};
	CHECK_THAT(first(unknown), Catch::Contains("a cause the judge does not know"));
}

TEST_CASE("A cycle judge expects only the causes the test declared, each up to its count", "[cycle_judge]") {
	CycleJudge judge(plain(1, {.starts = 1, .drives = 2, .driverChanges = 1, .absences = 1, .oversized = 1}), kClock,
	                 nullptr);
	CHECK(judge.at(CycleFacts{.causes = CycleCause::start | CycleCause::drive}, kFrames) == Verdict::expected);
	CHECK(judge.at(CycleFacts{.causes = CycleCause::driverChange}, kFrames) == Verdict::expected);
	CHECK(judge.at(CycleFacts{.missed = 4, .jumpFrames = 4 * kFrames, .causes = CycleCause::absent}, kFrames) ==
	      Verdict::expected);
	CHECK(judge.at(CycleFacts{.causes = CycleCause::oversized}, kFrames) == Verdict::expected);
	// A drive across a stall: expected, and its cut cycles excused as well.
	CycleFacts redrive = caughtUp(2, 2, 2 * kFrames, kClock.ns(2 * kFrames));
	redrive.causes = redrive.causes | CycleCause::drive;
	CHECK(judge.at(redrive, kFrames) == Verdict::expected);
	CHECK(judge.report().expected == 5);
	CHECK(judge.report().excusedCycles == 2);
	CHECK(judge.report().failed == 0);
	// One more of any is not declared.
	CHECK(judge.at(CycleFacts{.causes = CycleCause::start}, kFrames) == Verdict::fail);
	CHECK_THAT(judge.report().firstFailure, Catch::Contains("more starts than the test declared"));
	CHECK(judge.at(CycleFacts{.causes = CycleCause::drive}, kFrames) == Verdict::fail);
	CHECK(judge.at(CycleFacts{.causes = CycleCause::driverChange}, kFrames) == Verdict::fail);
	CHECK(judge.at(CycleFacts{.missed = 1, .jumpFrames = kFrames, .causes = CycleCause::absent}, kFrames) ==
	      Verdict::fail);
	CHECK(judge.at(CycleFacts{.causes = CycleCause::oversized}, kFrames) == Verdict::fail);
	CHECK(judge.report().failed == 5);
	// A failed verdict uses nothing up: the default plan expects one start and nothing else.
	CycleJudge fresh(plain(), kClock, nullptr);
	CHECK(fresh.at(CycleFacts{.causes = CycleCause::start | CycleCause::drive}, kFrames) == Verdict::fail);
	CHECK(fresh.at(CycleFacts{.causes = CycleCause::start}, kFrames) == Verdict::expected);
}

TEST_CASE("A cycle judge excuses a block missing from a card's file only where the driver cut its cycle",
          "[cycle_judge]") {
	ear::CutRing ring;
	ring.start(); // cycle 1, cut
	ring.incomplete();
	ring.start(); // cycle 2, whole
	ring.start(); // cycle 3, running
	CycleJudge judge(plain(1), kClock, &ring);
	CHECK(judge.blockMissing(1) == Verdict::excused);
	CHECK(judge.blockMissing(2) == Verdict::fail);
	CHECK_THAT(judge.report().firstFailure, Catch::Contains("graph cycle 2") && Catch::Contains("did not cut"));
	CHECK(judge.blockMissing(3) == Verdict::fail); // still running: nothing answers yet
	CHECK(judge.blockMissing(0) == Verdict::fail);
	CycleJudge noRecord(plain(1), kClock, nullptr);
	CHECK(noRecord.blockMissing(1) == Verdict::fail);
	CHECK_THAT(noRecord.report().firstFailure, Catch::Contains("no record answers"));
	CHECK(judge.blockHeard() == Verdict::judge);
	CHECK(judge.report().judged == 1);
	CHECK(judge.report().excusedCycles == 1);
	CHECK(judge.report().failed == 3);
}

TEST_CASE("A cycle judge is done only when its planned boundaries were judged and every planned stall was made in a "
          "cycle an excused verdict covers",
          "[cycle_judge]") {
	ear::CutRing ring;
	CycleJudge judge(JudgePlan{.judged = 2, .induce = StallPlan::every(10, 3, 2, StallLength::of(0ms))}, kClock, &ring);
	CHECK(judge.at(CycleFacts{}, kFrames) == Verdict::judge);
	CHECK(judge.at(CycleFacts{}, kFrames) == Verdict::judge);
	CHECK_FALSE(judge.done()); // judged, but no stall made
	// The test's upstream code stalls through the judge, by its own count of cycles; each stall keeps the graph cycle
	// it was made in, the record's running one.
	for (std::size_t n = 0; n < 30; ++n) {
		ring.start();
		judge.stalls()(n, kFrames, 48000);
	}
	CHECK(judge.stalls().made() == 2);
	CHECK(judge.stalls().madeIn(0) == 4);
	CHECK(judge.stalls().madeIn(1) == 14);
	CHECK(judge.report().stallsMade == 2);
	CHECK_FALSE(judge.done()); // made, but not excused: the judged node never missed them
	// A catch-up over other cycles excuses those, not the stalls'.
	CycleFacts elsewhere = caughtUp(2, 2, 2 * kFrames, kClock.ns(2 * kFrames));
	elsewhere.graphCycle = 8; // missed 6 and 7
	CHECK(judge.at(elsewhere, kFrames) == Verdict::excused);
	CHECK(judge.report().stallsExcused == 0);
	CHECK_FALSE(judge.done());
	CycleFacts first = caughtUp(1, 1, kFrames, kClock.ns(kFrames));
	first.graphCycle = 5; // missed 4, the first stall's
	CHECK(judge.at(first, kFrames) == Verdict::excused);
	CHECK(judge.report().stallsExcused == 1);
	CHECK_FALSE(judge.done());
	CycleFacts second = caughtUp(3, 3, 3 * kFrames, kClock.ns(3 * kFrames));
	second.graphCycle = 16; // missed 13 to 15, the second stall's among them
	CHECK(judge.at(second, kFrames) == Verdict::excused);
	CHECK(judge.done());
	CHECK(judge.report().ok());
	std::ostringstream told;
	told << judge.report();
	CHECK_THAT(told.str(), Catch::Contains("judged 2 of 2") && Catch::Contains("stalls made 2 of 2, 2 excused"));
	// A failure leaves it done but not ok.
	CHECK(judge.at(CycleFacts{.causes = CycleCause::drive}, kFrames) == Verdict::fail);
	CHECK(judge.done());
	CHECK_FALSE(judge.report().ok());
}

TEST_CASE("A cycle judge counts a card's missing block as covering the stall made in its cycle", "[cycle_judge]") {
	ear::CutRing ring;
	CycleJudge judge(JudgePlan{.judged = 1, .induce = StallPlan::every(1, 0, 1, StallLength::of(0ms))}, kClock, &ring);
	ring.start(); // cycle 1: the stall's, cut
	judge.stalls()(0, kFrames, 48000);
	ring.incomplete();
	ring.start();
	CHECK(judge.blockHeard() == Verdict::judge);
	CHECK_FALSE(judge.done());
	CHECK(judge.blockMissing(1) == Verdict::excused);
	CHECK(judge.done());
	CHECK(judge.report().ok());
}

TEST_CASE("A cycle judge counts each library settle a test declares, by its cause, and excuses no cycle by it",
          "[cycle_judge]") {
	CycleJudge judge(plain(1), kClock, nullptr);
	judge.librarySettle(ear::test::LibrarySettle::dry, 2);
	judge.librarySettle(ear::test::LibrarySettle::logged, 2);
	judge.librarySettle(ear::test::LibrarySettle::logged, 3);
	CHECK_FALSE(judge.done()); // nothing judged: a settle is no judged boundary
	CHECK(judge.at(caughtUp(1, 0, kFrames, kClock.ns(kFrames)), kFrames) == Verdict::fail); // and no excuse
	const auto report = judge.report();
	CHECK(report.librarySettles == std::array<std::size_t, 4>{1, 0, 0, 2});
	CHECK(report.librarySettleChunks == 7);
	std::ostringstream text;
	text << report;
	CHECK(text.str().find("library settles: dry 1, starved catch-up 0, forgotten debt 0, logged 2, 7 chunks") !=
	      std::string::npos);
}
