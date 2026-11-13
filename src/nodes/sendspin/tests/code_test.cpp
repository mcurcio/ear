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

using namespace ear::sendspin;
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

/// The handoff's realtime end: keeps the frames it accepts, up to a limit.
class FakeWriter final : public ear::RtWriter {
public:
	std::uint32_t write(std::span<const std::span<const float>> channels, std::uint32_t frames) noexcept override {
		const std::uint32_t accepted = std::min(frames, room);
		room -= accepted;
		for (std::uint32_t frame = 0; frame < accepted; ++frame) {
			fromLeft.push_back(channels[0][frame]);
			fromRight.push_back(channels[1][frame]);
		}
		return accepted;
	}
	std::uint32_t room = 1000000;
	std::vector<float> fromLeft;
	std::vector<float> fromRight;
};

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
	ear::CycleAccount account_{ear::FrameClock(rate), &cuts_};
	std::uint32_t driverId_ = 1;
	std::uint64_t position_ = 0;
};

struct Rig {
	Rig()
		: feed(2, capacity), code(feed, progress, bell, rate, toServer, capture), inLeft(block, 0.25F),
		  inRight(block, -0.5F) {
		code.prepare({rate, block});
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
	/// One cycle as the framework tells it: its facts from the driver, its start on the raw clock, and
	/// `downstreamFrames` downstream of both outputs (a following card's buffer). Returns its facts.
	ear::CycleFacts cycle(std::int64_t startRawNs = 1000, std::uint32_t downstreamFrames = 0) {
		const std::array<std::span<float>, 2> outputs{left, right};
		const std::array<std::span<const float>, 2> inputs{inLeft, inRight};
		const std::array<ear::Latency, 2> downstream{ear::Latency{downstreamFrames, downstreamFrames},
		                                             ear::Latency{downstreamFrames, downstreamFrames}};
		const std::uint64_t position = driver.position();
		const ear::CycleFacts facts = driver.next(block, startRawNs);
		code.process(inputs, outputs, block,
		             ear::GraphCycle{.startNs = startRawNs - 7, // another clock: the code must not read it
		                             .position = position,
		                             .startRawNs = startRawNs,
		                             .facts = facts,
		                             .downstream = downstream});
		return facts;
	}
	/// Whether the bell rang since the last look.
	bool rang() { return bell.drain(); }
	/// The frames the code last told the library were played (0 before it told any).
	std::uint64_t told() const { return progress.read().value_or(PlayProgress{}).totalFrames; }

	Driver driver;
	Feed feed;
	Mailbox<PlayProgress> progress;
	Mailbox<CaptureAnchor> capture;
	FakeWriter toServer;
	EventFd bell;
	SendspinCode code;
	std::vector<float> inLeft;
	std::vector<float> inRight;
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
	CHECK(requiredLeadMs(2048, 256, 48000) == 48);   // 42.67 + 5.33 ms; the library's own 150 ms then rules
	CHECK(requiredLeadMs(8192, 4096, 48000) == 256); // past it: a block of 2049 to 4096 frames
	CHECK(requiredLeadMs(8193, 4096, 48000) == 257); // rounded up, never under
	CHECK(requiredLeadMs(2048, 256, 44100) == 53);   // 52.24 ms at 44.1 kHz, rounded up
	CHECK(requiredLeadMs(4'000'000'000U, 4096, 48000) == UINT16_MAX); // past what the field holds: its most
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
				REQUIRE(requiredLeadMs(feed, maxBlock, rate) == before(std::uint64_t{feed} + maxBlock, rate));
			}
		}
	}
}

TEST_CASE("A cycle the graph lost and caught up is skipped and counted played", "[sendspin][code]") {
	Rig rig;
	rig.write(4 * block);
	rig.cycle();
	rig.driver.lose(1, block);
	REQUIRE(rig.cycle().causes == ear::CycleCause::catchUp);
	CHECK(rig.left[0] == static_cast<float>(2 * block + 1)); // that block's frames are gone
	rig.cycle();
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 4 * block);
}

// docs/node-types.md#clock: a cycle that caught lost time up is dated by the driver's own clock, behind raw time until
// the catch-up's burst ends, so what it skipped and read is told with the next cycle that lost nothing, at that cycle's
// finish.
TEST_CASE("What a catch-up cycle skipped and read is told with the next whole cycle, at its finish",
          "[sendspin][code]") {
	Rig rig;
	rig.write(6 * block);
	rig.cycle(1000);
	REQUIRE(rig.rang());
	for (int burst = 0; burst < 2; ++burst) {
		rig.driver.lose(1, block);
		REQUIRE(rig.cycle(2000).causes == ear::CycleCause::catchUp);
		CHECK(rig.told() == block); // nothing told from the burst
		CHECK_FALSE(rig.rang());
	}
	const std::int64_t start = 1'000'000;
	REQUIRE(rig.cycle(start).whole());
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 6 * block); // the two skips, the two reads and this cycle's block
	CHECK(progress->finishRawNs == ear::FrameClock(rate).heardRawNs(start, block, {}));
	CHECK(rig.rang());
}

// docs/node-types.md#clock: however long the stall, a catch-up owes its whole jump. What the feed holds of it is skipped
// at once and the rest from what the library writes next, before any of it is read; a cycle the feed leaves short
// while the catch-up is untold owes its zeros too. The library is told once nothing is owed, so it is never told a
// frame ear did not take from what it wrote, and needs no correction of its own.
TEST_CASE("A catch-up of any length is paid from the feed and from what is written next, and told once paid",
          "[sendspin][code]") {
	SECTION("past what it held, within its capacity") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		rig.driver.lose(4, block); // 1024 frames; the feed holds 512, and this cycle plays zeros: 768 owed
		REQUIRE(rig.cycle().jumpFrames == 4 * block);
		CHECK(rig.left[0] == 0.0F);
		CHECK(rig.told() == block);

		rig.write(4 * block);
		rig.cycle(); // 768 skipped, then a block read
		CHECK(rig.left[0] == static_cast<float>(6 * block + 1));
		CHECK(rig.told() == 7 * block);
	}
	SECTION("past its capacity") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		rig.driver.lose(10, block); // 2560 frames, past the feed's 2048: 512 skipped, 2304 owed with this cycle's zeros
		const ear::CycleFacts facts = rig.cycle();
		REQUIRE(facts.causes == ear::CycleCause::catchUp);
		REQUIRE(facts.jumpFrames == 10 * block);
		CHECK(rig.left[0] == 0.0F);

		rig.write(capacity); // all of it skipped, and this cycle's zeros owed: 512
		rig.cycle();
		CHECK(rig.left[0] == 0.0F);
		CHECK(rig.told() == block);

		rig.write(3 * block);
		rig.cycle(); // 512 skipped, then a block read
		CHECK(rig.left[0] == static_cast<float>(3 * block + capacity + 2 * block + 1));
		CHECK(rig.told() == 3 * block + capacity + 3 * block);
	}
	SECTION("past 2^32 frames: owed whole, never a wrapped few") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		const std::array<std::span<float>, 2> outputs{rig.left, rig.right};
		const std::array<std::span<const float>, 2> inputs{rig.inLeft, rig.inRight};
		constexpr std::int64_t jump = (std::int64_t{1} << 32) + block;
		rig.code.process(inputs, outputs, block,
		                 ear::GraphCycle{.startNs = 0,
		                                 .position = block + jump,
		                                 .startRawNs = 1000,
		                                 .facts = {.missed = static_cast<std::uint32_t>(jump / block),
		                                           .missedCut = static_cast<std::uint32_t>(jump / block),
		                                           .jumpFrames = jump,
		                                           .causes = ear::CycleCause::catchUp}});
		CHECK(rig.left[0] == 0.0F); // the two blocks held skipped
		for (int i = 0; i < 4; ++i) {
			rig.write(block);
			rig.code.process(inputs, outputs, block,
			                 ear::GraphCycle{.startNs = 0,
			                                 .position = (2 + static_cast<std::uint64_t>(i)) * block + jump,
			                                 .startRawNs = 2000});
			CHECK(rig.left[0] == 0.0F); // every block written is owed
		}
		CHECK(rig.told() == block);
	}
}

// docs/node-types.md#clock: ear and the library never both correct the same lost time. A library that sees itself
// starved (everything it wrote told played) realigns its next chunk to now on its own, so a catch-up then owes nothing,
// and what it writes next plays from its first frame.
TEST_CASE("A catch-up while the library sees itself starved owes nothing: the refill plays whole", "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	rig.cycle(); // read and told: the library holds nothing it was not told played
	rig.cycle(); // the feed dry: nothing consumed, nothing to tell
	REQUIRE(rig.told() == block);
	rig.driver.lose(4, block);
	REQUIRE(rig.cycle().causes == ear::CycleCause::catchUp);
	rig.cycle(); // dry again: no zeros owed either
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == static_cast<float>(block + 1)); // nothing dropped
	CHECK(rig.told() == 2 * block);
}

// docs/node-types.md#clock: the library goes untold at most untoldBound of the graph's time. Then the debt is forgotten
// and nothing more is owed until it is told, so the library realigns itself; and it is still told only from a cycle
// that lost nothing, never from one dated behind raw time.
TEST_CASE("Untold for untoldBound, the debt is forgotten and nothing more owed, and the library is told only from a "
          "cycle that lost nothing",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = ear::FrameClock(rate).ns(block);
	std::int64_t at = 0;
	rig.write(3 * block);
	rig.cycle(at);
	REQUIRE(rig.told() == block);
	// A stall the library cannot refill for: every cycle a catch-up, past the bound.
	std::size_t lossy = 0;
	for (at += 2 * blockNs; at < untoldBound.count() + 8 * blockNs; at += 2 * blockNs, ++lossy) {
		rig.driver.lose(1, block);
		REQUIRE(rig.cycle(at).causes == ear::CycleCause::catchUp);
		CHECK(rig.told() == block); // never told from a cycle that lost frames
	}
	REQUIRE(lossy > 0);
	// The first cycle that lost nothing tells; nothing is owed, so what comes next plays from its first frame.
	rig.write(block);
	REQUIRE(rig.cycle(at).whole());
	CHECK(rig.left[0] == static_cast<float>(3 * block + 1));
	CHECK(rig.told() == 4 * block); // a block read, a block skipped and a block read at the first catch-up, and this
}

TEST_CASE("Any other cause is a new clock: a driver change, a reposition and the first cycle skip nothing",
          "[sendspin][code]") {
	SECTION("a driver change") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle();
		rig.driver.changeDriver();
		REQUIRE(rig.cycle().causes == ear::CycleCause::driverChange);
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
			REQUIRE(rig.cycle().causes == ear::CycleCause::reposition);
			CHECK(rig.told() == 3 * block);
			CHECK(rig.left[0] == static_cast<float>(2 * block + 1));
		}
	}
	SECTION("the first cycle knows no gap") {
		Rig rig;
		rig.write(3 * block);
		rig.driver.reposition(block);
		REQUIRE(rig.cycle().causes == ear::CycleCause::start);
		CHECK(rig.told() == block);
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
	CHECK(rig.left[0] == static_cast<float>(3 * block + 1)); // two blocks skipped, the third read
	rig.write(block);
	rig.cycle();
	CHECK(rig.told() == 5 * block);
}

TEST_CASE("A catch-up whose feed runs dry owes its zeros too, and is told once paid", "[sendspin][code]") {
	Rig rig;
	rig.write(2 * block);
	rig.cycle();
	REQUIRE(rig.rang());
	rig.driver.lose(1, block);
	rig.cycle(); // the block it held skipped; this cycle's zeros owed
	CHECK(rig.left[0] == 0.0F);
	rig.cycle(); // nothing written: these zeros owed too
	CHECK(rig.told() == block);
	CHECK_FALSE(rig.rang());
	rig.write(3 * block);
	rig.cycle(); // two blocks skipped, the third read
	CHECK(rig.left[0] == static_cast<float>(4 * block + 1));
	CHECK(rig.told() == 5 * block);
	CHECK(rig.rang());
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

	// A catch-up before the next stream plays owes nothing: it has no place on the clock yet.
	rig.driver.lose(1, block);
	rig.cycle();
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == static_cast<float>(3 * block + 1));
	CHECK(rig.progress.read()->totalFrames == 2 * block);

	// One while the library holds frames is owed by what comes next; a clear forgets it, and the stream after it plays
	// from its first frame.
	rig.write(block);
	rig.driver.lose(2, block);
	rig.cycle(); // the block held skipped, the rest owed
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == 0.0F); // owed
	CHECK(rig.progress.read()->totalFrames == 2 * block);
	rig.feed.clear();
	rig.write(block);
	rig.cycle();
	CHECK(rig.left[0] == static_cast<float>(6 * block + 1));
	CHECK(rig.progress.read()->totalFrames == 5 * block);
}

TEST_CASE("Inputs go to the handoff whatever the feed holds, side for side", "[sendspin][code]") {
	Rig rig;
	rig.cycle();
	REQUIRE(rig.toServer.fromLeft.size() == block);
	CHECK(rig.toServer.fromLeft[0] == 0.25F);
	CHECK(rig.toServer.fromRight[block - 1] == -0.5F);
}

TEST_CASE("The capture anchor dates each cycle's first frame from the cycle's start on the raw clock",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t start = nowNs(CLOCK_MONOTONIC_RAW) - 5000000;
	rig.cycle(start);

	const auto first = rig.capture.read();
	REQUIRE(first.has_value());
	CHECK(first->framesWritten == 0);
	CHECK(first->atRawNs == start);

	rig.cycle(start);
	CHECK(rig.capture.read()->framesWritten == block);
}

TEST_CASE("Frames the handoff refuses are not counted in the anchor, so what it took stays dated", "[sendspin][code]") {
	Rig rig;
	rig.toServer.room = block + 10;
	for (int i = 0; i < 4; ++i) rig.cycle();
	CHECK(rig.capture.read()->framesWritten == block + 10);
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
