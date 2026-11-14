// A sendspin node's audio code, cycle by cycle, with a real feed and real clocks (docs/node-types.md#clock).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <span>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/cut_ring.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp"
#include "feed.hpp"
#include "sendspin_code.hpp"
#include "worker.hpp"

using namespace ear::nodes::sendspin;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t rate = 48000;
constexpr std::uint32_t block = 256;
constexpr std::uint32_t capacity = 2048;

std::int64_t nowNs(clockid_t clock) {
	timespec now{};
	clock_gettime(clock, &now);
	return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

/// The graph's driver as the engine tells a block (docs/writing-a-node-type.md#lost-cycles): cycles numbered and cuts
/// recorded in a CutRing, each cycle's facts worked out by the one rule, CycleAccount, so the code is handed the facts
/// the engine would hand it, whatever a test makes the graph lose.
class Driver {
public:
	/// The next cycle the block runs, of `frames`, starting at `startRawNs`.
	ear::CycleFacts next(std::uint32_t frames, std::int64_t startRawNs) {
		cuts_.start();
		const ear::CycleAccount::Step step = account_.next(ear::DriverTick{.driverId = driverId_,
		                                                                   .graphCycle = cuts_.started(),
		                                                                   .position = position_,
		                                                                   .frames = frames,
		                                                                   .startRawNs = startRawNs},
		                                                   frames);
		position_ += frames;
		return step.facts;
	}
	/// `count` cycles of `frames` start without the block running them, each cut short by the driver: a stall the
	/// graph caught up (catchUp); with `cut` false, a pause (absent).
	void lose(std::uint32_t count, std::uint32_t frames, bool cut = true) {
		for (std::uint32_t i = 0; i < count; ++i) {
			cuts_.start();
			if (cut) cuts_.incomplete();
			position_ += frames;
		}
	}
	/// The next cycle runs on another driver (a card made driver): driverChange.
	void changeDriver() { ++driverId_; }
	/// The next cycle's position moves by `frames` with no cycle missed: reposition.
	void reposition(std::int64_t frames) { position_ += static_cast<std::uint64_t>(frames); }
	std::uint64_t position() const noexcept { return position_; }

private:
	ear::CutRing cuts_;
	ear::CycleAccount account_{ear::FrameClock(ear::SampleRate::fixed<rate>()), &cuts_};
	std::uint32_t driverId_ = 1;
	std::uint64_t position_ = 0;
};

struct Rig {
	Rig() : feed(2, capacity), code(feed, progress, bell, ear::SampleRate::fixed<rate>()) {
		left.resize(block);
		right.resize(block);
	}

	/// Frames whose left channel counts on from what was written before.
	void write(std::uint32_t frames) {
		std::vector<float> samples;
		for (std::uint32_t i = 0; i < frames; ++i) {
			samples.push_back(static_cast<float>(++written));
			samples.push_back(0.0F);
		}
		REQUIRE(feed.write(samples, frames, 0ms) == frames);
	}
	/// One cycle as the framework tells it: its facts from the driver, its start on the raw clock,
	/// `downstreamFrames` downstream of both outputs (a following card's buffer), and how long the next cycle had been
	/// due as it began (`overdueNs`: above 0 in a catch-up's burst). Returns its facts.
	ear::CycleFacts cycle(std::int64_t startRawNs = 1000, std::uint32_t downstreamFrames = 0,
	                      std::int64_t overdueNs = 0) {
		const std::array<std::span<float>, 2> outputs{left, right};
		const std::array<ear::Latency, 2> downstream{ear::Latency{downstreamFrames, downstreamFrames},
		                                             ear::Latency{downstreamFrames, downstreamFrames}};
		const std::uint64_t position = driver.position();
		const ear::CycleFacts facts = driver.next(block, startRawNs);
		const std::int64_t startNs = startRawNs - 7; // another clock: the code must not read it
		code.process(outputs, block,
		             ear::GraphCycle{.startNs = startNs,
		                             .position = position,
		                             .startRawNs = startRawNs,
		                             .nextNs = startNs + ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block),
		                             .overdueNs = overdueNs,
		                             .facts = facts,
		                             .downstream = downstream});
		return facts;
	}
	/// One on-time cycle stating `facts` as given: a cause the driver here does not make (a drive, the cycle after an
	/// oversized one) or a jump too long to lose cycle by cycle.
	void scripted(ear::CycleFacts facts) {
		const std::array<std::span<float>, 2> outputs{left, right};
		code.process(outputs, block, ear::GraphCycle{.startNs = 0, .position = 0, .startRawNs = 1000, .facts = facts});
	}
	/// Whether the bell rang since the last look.
	bool rang() { return bell.drain(); }
	/// The frames the code last told the library were played (0 before it told any).
	std::uint64_t told() const { return progress.read().value_or(PlayProgress{}).totalFrames; }

	Driver driver;
	Feed feed;
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code;
	std::vector<float> left;
	std::vector<float> right;
	std::uint32_t written = 0;
};

} // namespace

TEST_CASE("A block read counts its frames, and finishes at the cycle's start on the raw clock, as the framework "
          "made it, plus the frames' duration",
          "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	const std::int64_t start = nowNs(CLOCK_MONOTONIC_RAW) - 5000000; // the code reads no clock of its own
	rig.cycle(start);

	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == block);
	const std::int64_t blockNs = std::int64_t{block} * 1000000000 / rate;
	CHECK(progress->finishRawNs == start + blockNs);
	CHECK(rig.rang());
	CHECK(rig.left[0] == 1.0F);
	CHECK(rig.left[block - 1] == static_cast<float>(block));
}

TEST_CASE("What lies downstream of the outputs is added: a frame finishes when the card that follows has played it",
          "[sendspin][code]") {
	const std::uint32_t held = 512 + 32; // a quantum ahead of the card's buffer and its headroom, as ALSA reports it
	Rig plain;
	plain.write(block);
	Rig delayed;
	delayed.write(block);
	const std::int64_t start = nowNs(CLOCK_MONOTONIC_RAW);
	plain.cycle(start);
	delayed.cycle(start, held);
	const std::int64_t difference = delayed.progress.read()->finishRawNs - plain.progress.read()->finishRawNs;
	CHECK(difference == std::int64_t{block + held} * 1000000000 / rate - std::int64_t{block} * 1000000000 / rate);
}

TEST_CASE("The lead a player reports covers what still plays after a clear, whatever the block", "[sendspin][code]") {
	CHECK(requiredLeadMs(2048, 256, ear::SampleRate::fixed<48000>()) ==
	      48); // 42.67 + 5.33 ms; the library's own 150 ms then rules
	CHECK(requiredLeadMs(8192, 4096, ear::SampleRate::fixed<48000>()) ==
	      256); // past it: a block of 2049 to 4096 frames
	CHECK(requiredLeadMs(8193, 4096, ear::SampleRate::fixed<48000>()) == 257); // rounded up, never under
	CHECK(requiredLeadMs(2048, 256, ear::SampleRate::fixed<44100>()) == 53);   // 52.24 ms at 44.1 kHz, rounded up
	CHECK(requiredLeadMs(4'000'000'000U, 4096, ear::SampleRate::fixed<48000>()) ==
	      UINT16_MAX); // past what the field holds: its most
}

// requiredLeadMs rounds FrameClock's truncated ns up to whole ms. That equals the exact ceil(frames * 1000 / rate), its
// formula before FrameClock, for every rate below 1 MHz: a truncated ns can sit on a whole ms only when the frames last
// a whole ms (with frames * 1000 = k * rate + r and 0 < r < rate, the frames last at least 1e6 / rate > 1 ns past k ms),
// so rounding up never falls short. At 1 MHz and above it would not hold, and no graph runs there.
TEST_CASE("The lead a player reports is ceil(frames * 1000 / rate) at every rate below 1 MHz", "[sendspin][code]") {
	const auto before = [](std::uint64_t frames, std::uint64_t rate) {
		return static_cast<std::uint16_t>(std::min<std::uint64_t>((frames * 1000 + rate - 1) / rate, UINT16_MAX));
	};
	for (const std::uint32_t rate : {1U, 3U, 7U, 8000U, 11025U, 22050U, 32000U, 44100U, 48000U, 88200U, 96000U, 176400U,
	                                 192000U, 352800U, 384000U, 705600U, 768000U, 999'999U}) {
		INFO("at " << rate << " Hz");
		for (const std::uint32_t maxBlock : {1U, 64U, 256U, 1024U, 4096U, 8192U}) {
			std::vector<std::uint32_t> feeds{0, 1, rate / 1000, rate, rate - 1, rate + 1, 4'000'000'000U};
			for (std::uint32_t feed = 0; feed <= 200'000; feed += 97) feeds.push_back(feed);
			for (const std::uint32_t feed : feeds) {
				INFO("feed " << feed << ", block " << maxBlock);
				REQUIRE(requiredLeadMs(feed, maxBlock, *ear::SampleRate::of(rate)) ==
				        before(std::uint64_t{feed} + maxBlock, rate));
			}
		}
	}
}

// docs/node-types.md#clock: on the cycle that states a catch-up, what the feed holds of the lost time is skipped and
// counted played; a catch-up on time is told at once.
TEST_CASE("A cycle the graph lost and caught up is skipped and counted played, and told at once when on time",
          "[sendspin][code]") {
	Rig rig;
	rig.write(4 * block);
	rig.cycle();
	REQUIRE(rig.rang());
	rig.driver.lose(1, block);
	const std::int64_t start = 1'000'000;
	REQUIRE(rig.cycle(start).causes == ear::CycleCause::catchUp);
	CHECK(rig.code.lastHeldAtStart() == 3 * block);
	CHECK(rig.code.lastSkipped() == block);
	CHECK(rig.left[0] == static_cast<float>(2 * block + 1)); // that block's frames are gone
	CHECK(rig.rang());
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 3 * block); // the first block, the skip and this cycle's read
	CHECK(progress->finishRawNs == ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(start, block, {}));
}

// docs/node-types.md#clock: the node keeps no debt. What the feed did not hold of a jump is the library's to correct,
// which it sees in the next finish it is told; nothing more is skipped from what it writes next.
TEST_CASE("A catch-up past what the feed holds skips what it holds and no more, and owes nothing after",
          "[sendspin][code]") {
	SECTION("past what it held") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		rig.driver.lose(4, block); // 1024 frames lost; the feed holds 512
		REQUIRE(rig.cycle().jumpFrames == 4 * block);
		CHECK(rig.code.lastSkipped() == 2 * block);
		CHECK(rig.left[0] == 0.0F); // nothing left to read
		CHECK(rig.told() == 3 * block);
		rig.write(block);
		rig.cycle();
		CHECK(rig.code.lastSkipped() == 0);
		CHECK(rig.left[0] == static_cast<float>(3 * block + 1)); // what came next plays from its first frame
		CHECK(rig.told() == 4 * block);
	}
	SECTION("past 2^32 frames: what it holds, never a wrapped few") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		constexpr std::int64_t jump = (std::int64_t{1} << 32) + block;
		rig.scripted({.missed = static_cast<std::uint32_t>(jump / block),
		              .missedCut = static_cast<std::uint32_t>(jump / block),
		              .jumpFrames = jump,
		              .causes = ear::CycleCause::catchUp});
		CHECK(rig.code.lastSkipped() == 2 * block);
		CHECK(rig.left[0] == 0.0F);
		rig.write(block);
		rig.scripted({});
		CHECK(rig.left[0] == static_cast<float>(3 * block + 1));
		CHECK(rig.told() == 4 * block);
	}
}

// A pause (cycles missed and not cut) moved the graph on as a catch-up does: the feed's audio of that time is stale, so
// it is skipped rather than played late, and the library re-anchors from the finish it is told.
TEST_CASE("A pause skips what the feed holds of it, as a catch-up does", "[sendspin][code]") {
	Rig rig;
	rig.write(4 * block);
	rig.cycle();
	rig.driver.lose(2, block, false);
	REQUIRE(rig.cycle().causes == ear::CycleCause::absent);
	CHECK(rig.code.lastSkipped() == 2 * block);
	CHECK(rig.left[0] == static_cast<float>(3 * block + 1)); // two blocks skipped, the third read
	CHECK(rig.told() == 4 * block);
	rig.write(block);
	rig.cycle();
	CHECK(rig.told() == 5 * block);
}

TEST_CASE("Any other cause is a new clock: a driver change, a reposition, the first cycle, a drive and the cycle after "
          "an oversized one skip nothing",
          "[sendspin][code]") {
	SECTION("a driver change") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		rig.driver.changeDriver();
		REQUIRE(rig.cycle().causes == ear::CycleCause::driverChange);
		CHECK(rig.code.lastSkipped() == 0);
		CHECK(rig.told() == 2 * block);
		CHECK(rig.left[0] == static_cast<float>(block + 1));
	}
	SECTION("a reposition, ahead or back") {
		for (const std::int64_t by : {std::int64_t{block} + 1, -std::int64_t{block}}) {
			Rig rig;
			rig.write(3 * block);
			rig.cycle();
			rig.cycle();
			rig.driver.reposition(by);
			const ear::CycleFacts facts = rig.cycle();
			REQUIRE(facts.causes == ear::CycleCause::reposition);
			REQUIRE(facts.jumpFrames != 0);
			CHECK(rig.code.lastSkipped() == 0);
			CHECK(rig.told() == 3 * block);
			CHECK(rig.left[0] == static_cast<float>(2 * block + 1));
		}
	}
	SECTION("the first cycle knows no gap") {
		Rig rig;
		rig.write(3 * block);
		rig.driver.reposition(block);
		REQUIRE(rig.cycle().causes == ear::CycleCause::start);
		CHECK(rig.code.lastSkipped() == 0);
		CHECK(rig.told() == block);
	}
	SECTION("a drive, and the cycle after an oversized one, each stating a jump") {
		for (const ear::CycleCause cause : {ear::CycleCause::drive, ear::CycleCause::oversized}) {
			Rig rig;
			rig.write(3 * block);
			rig.scripted({});
			rig.scripted({.jumpFrames = block, .causes = cause});
			CHECK(rig.code.lastSkipped() == 0);
			CHECK(rig.left[0] == static_cast<float>(block + 1));
			CHECK(rig.told() == 2 * block);
		}
	}
}

// docs/node-types.md#clock: a clear ends the stream, so a catch-up on the cycle that applies it skips nothing of the
// stream after it.
TEST_CASE("A catch-up on the cycle that applies a clear skips nothing of the next stream", "[sendspin][code]") {
	Rig rig;
	rig.write(2 * block);
	rig.cycle();
	rig.feed.clear();
	rig.write(2 * block); // the next stream, written after the clear
	rig.driver.lose(1, block);
	REQUIRE(rig.cycle().causes == ear::CycleCause::catchUp);
	CHECK(rig.code.lastHeldAtStart() == 2 * block);
	CHECK(rig.code.lastSkipped() == 0);
	CHECK(rig.left[0] == static_cast<float>(2 * block + 1));
	CHECK(rig.told() == 2 * block); // the first stream's block and the next's first
}

// docs/node-types.md#clock: a cycle that began while the next was already due is one of a catch-up's burst, behind now
// until the burst ends, so it tells nothing, whether the block runs every timeout (whole, overdue) or misses some (a
// catch-up, overdue), and it still skips what its jump states. The first cycle on time tells every frame of the burst,
// dated by its own nominal start.
TEST_CASE("A burst of overdue cycles tells nothing, and the first on time tells every frame of it, dated nominally",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	rig.write(6 * block);
	rig.cycle(0);
	REQUIRE(rig.told() == block);
	REQUIRE(rig.rang());
	// Four cycles of the burst, each overdue (the last by less): two whole, then a catch-up of a block, then whole.
	for (int i = 0; i < 4; ++i) {
		if (i == 2) rig.driver.lose(1, block);
		const ear::CycleFacts facts = rig.cycle((i + 1) * blockNs, 0, (40 - 10 * i) * blockNs);
		REQUIRE(facts.causes == (i == 2 ? ear::CycleCauses(ear::CycleCause::catchUp) : ear::CycleCauses{}));
		CHECK(rig.code.lastSkipped() == (i == 2 ? block : 0));
		CHECK(rig.told() == block);
		CHECK_FALSE(rig.rang());
	}
	const std::int64_t start = 6 * blockNs;
	REQUIRE(rig.cycle(start).whole());
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 6 * block); // the burst's four reads and its skip, and this cycle's block
	CHECK(progress->finishRawNs == ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(start, block, {}));
	CHECK(rig.rang());
	CHECK(rig.code.clamped() == 0);
}

TEST_CASE("One overdue cycle under load tells nothing, and the next on time tells it", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	rig.write(3 * block);
	rig.cycle(0);
	REQUIRE(rig.rang());
	REQUIRE(rig.cycle(blockNs, 0, 1000).whole()); // the next due a microsecond before it began
	CHECK(rig.told() == block);
	CHECK_FALSE(rig.rang());
	REQUIRE(rig.cycle(2 * blockNs).whole());
	CHECK(rig.told() == 3 * block);
	CHECK(rig.progress.read()->finishRawNs ==
	      ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(2 * blockNs, block, {}));
}

// docs/node-types.md#clock: with no debt there is nothing for a lossy cycle to wait for. A host whose every cycle loses
// frames, each on time, tells the library every cycle.
TEST_CASE("A cycle that lost frames, on time, tells", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	std::int64_t at = 0;
	rig.cycle(at);
	std::uint64_t written = 0;
	for (int i = 0; i < 8; ++i) {
		at += 2 * blockNs;
		rig.write(2 * block);
		written += 2 * block;
		rig.driver.lose(1, block);
		REQUIRE(rig.cycle(at).causes == ear::CycleCause::catchUp);
		CHECK(rig.code.lastSkipped() == block);
		CHECK(rig.told() == written);
		CHECK(rig.progress.read()->finishRawNs ==
		      ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(at, block, {}));
	}
}

// docs/node-types.md#clock: untold for untoldBound of the graph's nominal time, the library is told from the next cycle
// even if it is overdue, dated by when that cycle really ran (GraphCycle::ranRawNs); the on-time report after it is
// clamped to it, never stepping back, and the one after that is dated nominally again.
TEST_CASE("Overdue for untoldBound, the library is told from a cycle of the burst, dated by when it ran, and the next "
          "report is clamped to it",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	const std::int64_t overdue = 3 * untoldBound.count();
	rig.write(capacity);
	rig.cycle(0);
	REQUIRE(rig.told() == block);
	std::int64_t at = blockNs;
	const std::int64_t untoldSince = at;
	std::uint64_t deferred = 0;
	for (; at - untoldSince < untoldBound.count(); at += blockNs, ++deferred) {
		rig.write(block);
		REQUIRE(rig.cycle(at, 0, overdue).whole());
		CHECK(rig.told() == block);
	}
	REQUIRE(deferred > 1);
	rig.write(block);
	REQUIRE(rig.cycle(at, 0, overdue).whole());
	const std::uint64_t forcedTotal = block + (deferred + 1) * block;
	CHECK(rig.told() == forcedTotal);                // told at the bound, though still overdue
	const std::int64_t ran = at + blockNs + overdue; // the next cycle's due time plus how long it was overdue
	const std::int64_t forced = ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(ran, block, {});
	CHECK(rig.progress.read()->finishRawNs == forced);
	CHECK(rig.code.clamped() == 0);
	// The first cycle on time after it began within a cycle before the forced one ran: dated nominally it would step
	// back, so it is clamped to the forced finish.
	rig.write(block);
	const std::int64_t onTime = ran - blockNs / 2;
	REQUIRE(rig.cycle(onTime).whole());
	CHECK(rig.told() == forcedTotal + block);
	CHECK(rig.progress.read()->finishRawNs == forced);
	CHECK(rig.code.clamped() == 1);
	// Then nominal again.
	rig.write(block);
	REQUIRE(rig.cycle(onTime + blockNs).whole());
	CHECK(rig.progress.read()->finishRawNs ==
	      ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(onTime + blockNs, block, {}));
	CHECK(rig.code.clamped() == 1);
}

// docs/node-types.md#clock: the clamp acts only on the report after one the bound forced. Through bursts below the
// bound, catch-ups, dry cycles, a pause and a following card's latency changing, every report is the cycle's own
// nominal finish, exactly: even one behind the last, when what lies downstream shrank, is told as it is.
TEST_CASE("Below the bound the clamp never acts: every report is its cycle's nominal finish", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	std::int64_t at = 0;
	std::uint32_t downstream = 4 * block + 32;
	std::size_t reports = 0;
	std::size_t backwards = 0;
	std::int64_t lastFinish = 0;
	std::uint64_t lastTold = 0;
	const auto check = [&](const ear::CycleFacts& facts, std::int64_t overdueNs) {
		INFO("the cycle at " << at << " ns, causes " << int{facts.causes.bits()} << ", overdue " << overdueNs);
		if (rig.told() == lastTold) return;
		++reports;
		CHECK(overdueNs == 0);
		const std::int64_t finish = rig.progress.read()->finishRawNs;
		CHECK(finish ==
		      ear::FrameClock(ear::SampleRate::fixed<rate>()).heardRawNs(at, block, {downstream, downstream}));
		if (finish < lastFinish) ++backwards;
		lastFinish = finish;
		lastTold = rig.told();
	};
	for (int round = 0; round < 40; ++round) {
		rig.write(8 * block);
		// On time, a short burst (overdue, then cut once), a catch-up on time, a cycle the feed leaves short, a pause.
		const std::array<std::int64_t, 6> overdue{0, 9 * blockNs, 4 * blockNs, 0, 0, 0};
		for (std::size_t i = 0; i < overdue.size(); ++i) {
			if (i == 2 || i == 3) rig.driver.lose(1, block);
			if (i == 5) rig.driver.lose(1, block, false);
			at += blockNs;
			if (i == 4) rig.feed.skip(rig.feed.held() - block / 2); // half a block left: this cycle reads short
			check(rig.cycle(at, downstream, overdue[i]), overdue[i]);
		}
		if (round == 20) downstream = 32; // the card that followed is gone: what lies downstream shrinks
	}
	CHECK(reports == 3 * 40);
	CHECK(backwards == 1); // at the shrink, told as it is
	CHECK(rig.code.clamped() == 0);
}

TEST_CASE("Nothing read rings nothing, and plays zeros", "[sendspin][code]") {
	Rig rig;
	rig.left[0] = 9.0F;
	rig.right[0] = 9.0F;
	rig.cycle();
	CHECK_FALSE(rig.rang());
	CHECK_FALSE(rig.progress.read().has_value());
	CHECK(rig.left[0] == 0.0F);
	CHECK(rig.right[0] == 0.0F);
}

TEST_CASE("Cleared frames are never reported", "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	rig.cycle();
	REQUIRE(rig.progress.read()->totalFrames == block);
	REQUIRE(rig.rang());

	rig.write(2 * block);
	rig.feed.clear();
	rig.cycle();
	CHECK(rig.progress.read()->totalFrames == block);
	CHECK_FALSE(rig.rang());
	CHECK(rig.left[0] == 0.0F);

	// A catch-up before the next stream plays has nothing to skip.
	rig.driver.lose(1, block);
	rig.cycle();
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == static_cast<float>(3 * block + 1));
	CHECK(rig.progress.read()->totalFrames == 2 * block);

	// One while the feed holds frames skips them; nothing is owed, so the stream after a clear plays from its first
	// frame.
	rig.write(block);
	rig.driver.lose(2, block);
	rig.cycle(); // the block held skipped
	CHECK(rig.code.lastSkipped() == block);
	CHECK(rig.progress.read()->totalFrames == 3 * block);
	rig.write(block);
	rig.feed.clear();
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == static_cast<float>(6 * block + 1));
	CHECK(rig.progress.read()->totalFrames == 4 * block);
}

TEST_CASE("The mailbox holds the newest value, and nothing before the first", "[sendspin][code]") {
	Mailbox<PlayProgress> mailbox;
	CHECK_FALSE(mailbox.read().has_value());
	mailbox.publish({1, 2});
	mailbox.publish({3, 4});
	const auto value = mailbox.read();
	REQUIRE(value.has_value());
	CHECK(value->totalFrames == 3);
	CHECK(value->finishRawNs == 4);
}

TEST_CASE("The mailbox's fields, read while it is written from another thread, always belong together",
          "[sendspin][code]") {
	Mailbox<PlayProgress> mailbox;
	std::atomic<bool> done{false};
	std::thread writer([&] {
		for (std::uint64_t value = 1; !done.load(std::memory_order_relaxed); ++value) {
			mailbox.publish({value, static_cast<std::int64_t>(value) * 3});
		}
	});
	std::uint64_t mixed = 0;
	std::uint64_t reads = 0;
	std::uint64_t newest = 0;
	std::uint64_t backwards = 0;
	const auto until = std::chrono::steady_clock::now() + 500ms;
	while (std::chrono::steady_clock::now() < until) {
		const auto value = mailbox.read();
		if (!value) continue;
		++reads;
		if (value->finishRawNs != static_cast<std::int64_t>(value->totalFrames) * 3) ++mixed;
		if (value->totalFrames < newest) ++backwards;
		newest = value->totalFrames;
	}
	done = true;
	writer.join();
	INFO(reads << " reads");
	CHECK(reads > 0);
	CHECK(mixed == 0);
	CHECK(backwards == 0);
}

namespace {
std::int64_t fakeRaw = 0;
std::int64_t fakeRawNow() noexcept { return fakeRaw; }
} // namespace

TEST_CASE(
	"After the feed was left undrained past its bound, what it held is skipped stale, and that and what it dropped "
	"are the feed's to say, not the code's",
	"[sendspin][code]") {
	constexpr std::int64_t msNs = 1'000'000;
	fakeRaw = 50'000 * msNs;
	Feed feed(2, capacity, FeedDrain{.rate = ear::SampleRate::fixed<rate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow});
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code(feed, progress, bell, ear::SampleRate::fixed<rate>());
	std::vector<float> left(block);
	std::vector<float> right(block);
	const std::array<std::span<float>, 2> outputs{left, right};
	const auto cycle = [&](std::int64_t startRawNs, ear::CycleFacts facts) {
		code.process(outputs, block,
		             ear::GraphCycle{.startNs = 0, .position = 0, .startRawNs = startRawNs, .facts = facts});
	};
	std::uint32_t written = 0;
	const auto write = [&](std::uint32_t frames) {
		std::vector<float> samples;
		for (std::uint32_t i = 1; i <= frames; ++i) {
			samples.push_back(static_cast<float>(written + i));
			samples.push_back(0.0F);
		}
		const std::uint32_t accepted = feed.write(samples, frames, 0ms);
		written += accepted;
		return accepted;
	};

	cycle(fakeRaw, {}); // drained now
	REQUIRE(write(capacity) == capacity);
	cycle(fakeRaw, {});
	CHECK(progress.read()->totalFrames == block);
	REQUIRE(write(block) == block); // full again: 2048 held

	fakeRaw += 2000 * msNs; // stopped for 2 s: the writer drops what it is given rather than wait, up to now
	REQUIRE(write(1000) == 0);
	fakeRaw += 21 * msNs;
	REQUIRE(write(1000) == 1000);
	CHECK(feed.dropped().total == capacity + 1000); // what the feed held, and what was dropped: the notifier's to tell

	ear::CycleFacts drive;
	drive.causes = ear::CycleCause::drive;
	cycle(fakeRaw, drive); // the start: what the feed held is stale
	CHECK(code.lastStaleSkipped() == capacity);
	CHECK(left[0] == 0.0F);                       // nothing fresh to read yet
	CHECK(progress.read()->totalFrames == block); // nothing new of the code's: the stale frames are the feed's to tell
	REQUIRE(write(block) == block);               // the reader is back: the feed takes it to be read
	cycle(fakeRaw + 5 * msNs, {});
	CHECK(code.lastStaleSkipped() == 0);
	// The frames written since were due from the last drop on: 5 ms of them late by this cycle, skipped.
	CHECK(code.lastResumeSkipped() == 248);
	CHECK(left[0] == static_cast<float>(capacity + block + 1000 + 248 + 1));
	CHECK(progress.read()->totalFrames == block + block);
}

TEST_CASE("The finish told never steps back: across a stop and a start, and across a stall past the feed's bound",
          "[sendspin][code]") {
	constexpr std::int64_t msNs = 1'000'000;
	const std::int64_t blockNs = ear::FrameClock(ear::SampleRate::fixed<rate>()).ns(block);
	fakeRaw = 90'000 * msNs;
	Feed feed(2, capacity, FeedDrain{.rate = ear::SampleRate::fixed<rate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow});
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code(feed, progress, bell, ear::SampleRate::fixed<rate>());
	PlayedTeller teller;
	std::vector<float> left(block);
	std::vector<float> right(block);
	const std::array<std::span<float>, 2> outputs{left, right};
	std::vector<std::int64_t> finishes;
	// The notifier's turn: what the library is told now, if anything.
	const auto notify = [&] {
		const auto read = progress.read();
		const auto dropped = feed.dropped();
		if (const auto report = teller.next(read, dropped)) {
			finishes.push_back(report->finishRawNs);
			teller.told(read, dropped, *report);
		}
	};
	const std::vector<float> samples(std::size_t{block} * 2, 0.5F);
	// The library's sync thread: as much as the feed takes now.
	const auto write = [&] {
		while (feed.write(samples, block, 0ms) > 0) {
		}
	};
	// `cycles` on-time cycles from now, the clock moving a block each, the library writing and the notifier telling
	// between them; the first stating `facts`.
	const auto run = [&](int cycles, ear::CycleFacts facts) {
		for (int i = 0; i < cycles; ++i) {
			write();
			code.process(
				outputs, block,
				ear::GraphCycle{
					.startNs = 0, .position = 0, .startRawNs = fakeRaw, .facts = i == 0 ? facts : ear::CycleFacts{}});
			notify();
			fakeRaw += blockNs;
		}
	};
	// The node not running for `ms`: the library writing every 5 ms, the notifier telling what was dropped.
	const auto away = [&](std::int64_t ms) {
		for (std::int64_t at = 0; at < ms; at += 5) {
			fakeRaw += 5 * msNs;
			write();
			notify();
		}
	};

	run(50, {});
	away(3000); // a stop: dropped from 1.5 s on
	ear::CycleFacts drive;
	drive.causes = ear::CycleCause::drive;
	run(50, drive); // the start
	away(2500);     // a stall past the bound
	ear::CycleFacts caughtUp;
	caughtUp.causes = ear::CycleCause::catchUp;
	caughtUp.jumpFrames = static_cast<std::int64_t>(ear::FrameClock(ear::SampleRate::fixed<rate>()).frames(2500ms));
	run(50, caughtUp);

	REQUIRE(feed.dropped().total > 0);
	REQUIRE(finishes.size() > 100);
	for (std::size_t i = 1; i < finishes.size(); ++i) {
		INFO("report " << i << " of " << finishes.size());
		CHECK(finishes[i] >= finishes[i - 1]);
	}
}

TEST_CASE("The feed's undrained bound is the ring's room behind a full advertised buffer at the PCM byte rate, less "
          "the library's wait, and never under two feeds",
          "[sendspin][code]") {
	using std::chrono::nanoseconds;
	// A third of 1 MB at 24-bit stereo: 333333 B / 288000 B/s = 1.1574 s, less 100 ms.
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<48000>(), 2048) == nanoseconds{1'157'406'250 - 100'000'000});
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<96000>(), 4096) == nanoseconds{578'703'125 - 100'000'000});
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<192000>(), 8192) == nanoseconds{289'351'562 - 100'000'000});
	// A feed so long that two of it outlast the room: the floor.
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<192000>(), 65536) ==
	      nanoseconds{ear::FrameClock(ear::SampleRate::fixed<192000>()).ns(2 * 65536)});
}

// docs/node-types.md#clock: the code's finishes add what lies downstream (a following card's buffer), so the drops the
// feed's writer tells are dated with it too, and the frames the code skips as late on its return are counted on that
// one timeline: the first report back is the drop's last finish plus the frames it tells, with nothing to correct.
TEST_CASE("Behind a following card, the first report after a drop is the drop's finish plus the frames it tells",
          "[sendspin][code]") {
	constexpr std::int64_t msNs = 1'000'000;
	constexpr std::uint32_t held = 512 + 32; // the card's buffer, as the framework carries it back
	const ear::FrameClock clock(ear::SampleRate::fixed<rate>());
	fakeRaw = 120'000 * msNs;
	Feed feed(2, capacity, FeedDrain{.rate = ear::SampleRate::fixed<rate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow});
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code(feed, progress, bell, ear::SampleRate::fixed<rate>());
	PlayedTeller teller;
	std::vector<float> left(block);
	std::vector<float> right(block);
	const std::array<std::span<float>, 2> outputs{left, right};
	const std::array<ear::Latency, 2> downstream{ear::Latency{held, held}, ear::Latency{held, held}};
	const auto cycle = [&](ear::CycleFacts facts) {
		code.process(outputs, block,
		             ear::GraphCycle{
						 .startNs = 0, .position = 0, .startRawNs = fakeRaw, .facts = facts, .downstream = downstream});
	};
	std::vector<PlayedTeller::Report> told;
	const auto notify = [&] {
		const auto read = progress.read();
		const auto dropped = feed.dropped();
		if (const auto report = teller.next(read, dropped)) {
			told.push_back(*report);
			teller.told(read, dropped, *report);
		}
	};
	const std::vector<float> samples(std::size_t{block} * 2, 0.5F);
	const auto write = [&] {
		while (feed.write(samples, block, 0ms) > 0) {
		}
	};

	for (int i = 0; i < 20; ++i) {
		write();
		cycle({});
		notify();
		fakeRaw += clock.ns(block);
	}
	for (int at = 0; at < 2000; at += 5) { // stopped for 2 s: dropped from 1.5 s on, each drop told
		fakeRaw += 5 * msNs;
		write();
		notify();
	}
	REQUIRE(feed.dropped().total > 0);
	const PlayedTeller::Report lastDrop = told.back();
	fakeRaw += 5 * msNs; // the start comes 5 ms after the last drop
	ear::CycleFacts drive;
	drive.causes = ear::CycleCause::drive;
	cycle(drive); // takes the feed back: the stale skipped, nothing fresh yet
	notify();
	write();
	fakeRaw += clock.ns(block);
	cycle({});
	CHECK(code.lastResumeSkipped() > 0);
	const std::size_t before = told.size();
	notify();
	REQUIRE(told.size() == before + 1);
	const PlayedTeller::Report back = told.back();
	INFO("the drop's last finish " << lastDrop.finishRawNs << ", the report back " << back.finishRawNs << " for "
	                               << back.frames << " frames");
	// Within the rounding of frames to ns, once for the late skip and once for the block.
	CHECK(std::abs(back.finishRawNs - (lastDrop.finishRawNs + clock.ns(back.frames))) <= 2);
	CHECK(teller.clamped() == 0);
}

TEST_CASE("A stop that spans a stream's end keeps the drop's timeline for the next stream's frames",
          "[sendspin][code]") {
	constexpr std::int64_t msNs = 1'000'000;
	const ear::FrameClock clock(ear::SampleRate::fixed<rate>());
	fakeRaw = 140'000 * msNs;
	Feed feed(2, capacity, FeedDrain{.rate = ear::SampleRate::fixed<rate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow});
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code(feed, progress, bell, ear::SampleRate::fixed<rate>());
	std::vector<float> left(block);
	std::vector<float> right(block);
	const std::array<std::span<float>, 2> outputs{left, right};
	const auto cycle = [&](ear::CycleFacts facts) {
		code.process(outputs, block,
		             ear::GraphCycle{.startNs = 0, .position = 0, .startRawNs = fakeRaw, .facts = facts});
	};
	const std::vector<float> samples(std::size_t{block} * 2, 0.5F);
	const auto write = [&] {
		while (feed.write(samples, block, 0ms) > 0) {
		}
	};

	write();
	cycle({});
	for (int at = 0; at < 2000; at += 5) { // stopped: dropped from 1.5 s on
		fakeRaw += 5 * msNs;
		write();
	}
	feed.clear(); // the stream ends while stopped, and the next begins: the writer drops its frames too
	for (int at = 0; at < 100; at += 5) {
		fakeRaw += 5 * msNs;
		write();
	}
	const auto dropped = feed.dropped();
	REQUIRE(dropped.total > dropped.forgotten); // the library counts the next stream's drops
	fakeRaw += 5 * msNs;
	ear::CycleFacts drive;
	drive.causes = ear::CycleCause::drive;
	cycle(drive); // the start: the clear applied, the feed taken back
	write();
	fakeRaw += clock.ns(block);
	cycle({});
	// What the library wrote since the drop's last finish is late by this cycle's run, on the next stream's timeline.
	CHECK(code.lastResumeSkipped() == clock.framesNearest(std::chrono::nanoseconds{fakeRaw - dropped.finishRawNs}));
}

TEST_CASE("A frame read twice in the instant the writer began dropping is repaid in its own stream, never the next",
          "[sendspin][code]") {
	constexpr std::int64_t msNs = 1'000'000;
	fakeRaw = 160'000 * msNs;
	Feed feed(2, capacity, FeedDrain{.rate = ear::SampleRate::fixed<rate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow});
	Mailbox<PlayProgress> progress;
	EventFd bell;
	SendspinCode code(feed, progress, bell, ear::SampleRate::fixed<rate>());
	std::vector<float> left(block);
	std::vector<float> right(block);
	const std::array<std::span<float>, 2> outputs{left, right};
	const auto cycle = [&] {
		code.process(outputs, block, ear::GraphCycle{.startNs = 0, .position = 0, .startRawNs = fakeRaw});
	};
	const std::vector<float> samples(std::size_t{block} * 2, 0.5F);

	REQUIRE(feed.write(samples, block, 0ms) == block);
	cycle();
	REQUIRE(progress.read()->totalFrames == block);
	std::vector<float> chunk(std::size_t{capacity} * 2, 0.5F);
	REQUIRE(feed.write(chunk, capacity, 0ms) == capacity);
	fakeRaw += 1600 * msNs;
	REQUIRE(feed.write(samples, block, 0ms) == 0); // the writer begins: the 2048 held counted dropped
	// The reader's cycle in flight that instant read a block of them: the test reads it as that cycle did.
	REQUIRE(feed.read(outputs, block) == block);
	cycle(); // takes the feed back: one block read twice, owed; nothing of the code's to tell
	CHECK(code.lastStaleSkipped() == capacity - block);
	feed.clear(); // the stream ends before anything repaid it: the library forgot that stream's count
	cycle();
	REQUIRE(feed.write(samples, block, 0ms) == block); // the next stream's first frames
	cycle();
	CHECK(progress.read()->totalFrames == block + block); // counted whole: nothing of the old stream is repaid from it
}

TEST_CASE("Where the two tellers meet a finish that would step back is held at the last told, and counted",
          "[sendspin][code]") {
	PlayedTeller teller;
	const std::optional<PlayProgress> code = PlayProgress{.totalFrames = 256, .finishRawNs = 1'000'000};
	const Feed::Dropped none{.total = 0, .forgotten = 0, .finishRawNs = 0};
	const auto first = teller.next(code, none);
	REQUIRE(first.has_value());
	CHECK_FALSE(first->clamped);
	teller.told(code, none, *first);
	// A drop dated before the code's last finish (a hand-over gone wrong): told at the last finish, never before it.
	const Feed::Dropped earlier{.total = 100, .forgotten = 0, .finishRawNs = 900'000};
	const auto second = teller.next(code, earlier);
	REQUIRE(second.has_value());
	CHECK(second->frames == 100);
	CHECK(second->finishRawNs == 1'000'000);
	CHECK(second->clamped);
	teller.told(code, earlier, *second);
	CHECK(teller.clamped() == 1);
	const Feed::Dropped later{.total = 200, .forgotten = 0, .finishRawNs = 1'100'000};
	const auto third = teller.next(code, later);
	REQUIRE(third.has_value());
	CHECK(third->finishRawNs == 1'100'000);
	CHECK_FALSE(third->clamped);
}
