// A sendspin node's clock, tick by tick (docs/node-types.md#clock): SendspinClock on core's own SourceRing, played by
// the one play rule the block and the harness run (SourceRing::play), with scripted facts and a scripted raw clock, the
// library's writes going through the clock as the worker's do. The same rows through the type as a whole are
// type_test.cpp's; through the real engine and the real library, clock_test.cpp's.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/cut_ring.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/source_ring.hpp"
#include "sleeps.hpp"
#include "sendspin_clock.hpp"
#include "worker.hpp"

using namespace ear::nodes::sendspin;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t rate = 48000;
constexpr std::uint32_t block = 256;
constexpr std::uint32_t capacity = 2048;
constexpr std::int64_t msNs = 1'000'000;
const ear::FrameClock frameClock(ear::SampleRate::fixed<rate>());

std::atomic<std::int64_t> fakeRaw{0};
std::int64_t fakeRawNow() noexcept { return fakeRaw.load(std::memory_order_relaxed); }

/// The graph's driver as the engine tells a block (docs/writing-a-node-type.md#lost-cycles): cycles numbered and cuts
/// recorded in a CutRing, each cycle's facts worked out by the one rule, CycleAccount.
class Driver {
public:
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
	/// `count` cycles of `frames` start without the block running them: catchUp, or with `cut` false a pause (absent).
	void lose(std::uint32_t count, std::uint32_t frames, bool cut = true) {
		for (std::uint32_t i = 0; i < count; ++i) {
			cuts_.start();
			if (cut) cuts_.incomplete();
			position_ += frames;
		}
	}
	void changeDriver() { ++driverId_; }
	void reposition(std::int64_t frames) { position_ += static_cast<std::uint64_t>(frames); }
	std::uint64_t position() const noexcept { return position_; }

private:
	ear::CutRing cuts_;
	ear::CycleAccount account_{frameClock, &cuts_};
	std::uint32_t driverId_ = 1;
	std::uint64_t position_ = 0;
};

/// The source's writer over core's SourceRing, never waiting (as the harness's), its clock wake a flag.
class RingWriter final : public ear::SourceWriter, public ear::SourceRing::Signals, public ear::Wake {
public:
	RingWriter() : rules(2, capacity, ear::SampleRate::fixed<rate>(), *this) {}
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds) override {
		return rules.offer(interleaved, frames);
	}
	void idle(std::chrono::milliseconds) override { rules.markIdle(); }
	void clear() noexcept override { rules.clear(); }
	bool closed() const noexcept override { return rules.closed(); }
	std::uint32_t capacityFrames() const noexcept override { return rules.capacityFrames(); }
	ear::Wake& clockWake() noexcept override { return *this; }
	void ring() noexcept override { rung.store(true); }
	int descriptor() const noexcept override { return -1; }
	void answer() noexcept override { rung.store(false); }
	void roomFreed() noexcept override {}
	void clockAsked() noexcept override { ring(); }
	ear::SourceRing rules;
	std::atomic<bool> rung{false};
};

/// The clock, its source's ring and the library's writes, tick by tick on the fake raw clock.
struct Rig {
	explicit Rig(std::int64_t at = 1'000'000, std::uint32_t downstream = 0)
		: madeAt((fakeRaw = at, at)),
		  clock(SendspinClock::Config{
			  .rate = ear::SampleRate::fixed<rate>(), .undrainedBound = 1500ms, .nowRawNs = &fakeRawNow}),
		  downstreamFrames(downstream), left(block), right(block) {
		clock.attach(writer);
	}
	/// Up to `frames` frames through the clock, each left sample counting on from what was taken before (the right 0);
	/// returns the frames taken.
	std::uint32_t write(std::uint32_t frames) {
		std::vector<float> samples;
		for (std::uint32_t i = 1; i <= frames; ++i) {
			samples.push_back(static_cast<float>(written + i));
			samples.push_back(0.0F);
		}
		const std::uint32_t taken = clock.write(samples, frames, 0ms);
		written += taken;
		return taken;
	}
	/// As much as it takes now, a block at a time.
	std::uint32_t fill() {
		std::uint32_t taken = 0;
		while (const std::uint32_t now = write(block)) taken += now;
		return taken;
	}
	/// One tick as the block plays it: its facts from the driver (or `facts` as given), starting at `startRawNs`, the
	/// next due one block on unless `overdueNs` says it was due that long before.
	ear::CycleFacts cycle(std::int64_t startRawNs, std::int64_t overdueNs = 0,
	                      std::optional<ear::CycleFacts> facts = std::nullopt) {
		const ear::CycleFacts stated = facts ? *facts : driver.next(block, startRawNs);
		const std::int64_t startNs = startRawNs - 7; // another clock: the clock must not read it
		const ear::GraphCycle graphCycle{.startNs = startNs,
		                                 .position = driver.position(),
		                                 .startRawNs = startRawNs,
		                                 .nextNs = startNs + frameClock.ns(block),
		                                 .overdueNs = overdueNs,
		                                 .facts = stated};
		const std::array<std::span<float>, 2> bank{left, right};
		writer.rules.play(bank,
		                  ear::SourceTick{.cycle = graphCycle,
		                                  .frames = block,
		                                  .rate = ear::SampleRate::fixed<rate>(),
		                                  .playback = ear::Latency{downstreamFrames, downstreamFrames}},
		                  &clock);
		return stated;
	}
	/// One on-time tick at the fake clock's now.
	ear::CycleFacts tick(std::optional<ear::CycleFacts> facts = std::nullopt) { return cycle(fakeRaw, 0, facts); }
	/// A stop's silence completed: the ring drops what it holds and refuses writes until the next tick.
	void stop() { writer.rules.dropForStop(); }
	/// The node not running for `ms`: the library writing every 5 ms, the notifier telling.
	void away(std::int64_t ms) {
		for (std::int64_t at = 0; at < ms; at += 5) {
			fakeRaw += 5 * msNs;
			fill();
			notify();
		}
	}
	/// The notifier's turn (worker.cpp's Notifier::notify): what the library is told now, if anything.
	struct Told {
		std::uint64_t frames;
		std::int64_t finishRawNs;
	};
	void notify() {
		const auto progress = clock.progress();
		if (!progress) return;
		const std::uint64_t frames = framesToTell(*progress, reported, clock.stream());
		if (frames == 0) return;
		told.push_back({frames, progress->finishRawNs});
		reported = progress->played;
	}
	bool rang() { return writer.rung.exchange(false); }
	std::uint64_t played() const { return clock.progress().value_or(PlayProgress{}).played; }
	std::int64_t finish() const { return clock.progress().value_or(PlayProgress{}).finishRawNs; }
	/// The value of the last frame the tick played into the left output (past the 5 ms fade-in of a stream's start).
	float last() const { return left[block - 1]; }

	std::int64_t madeAt; // the fake clock set first: the clock counts as drained when made
	Driver driver;
	RingWriter writer;
	SendspinClock clock;
	std::uint32_t downstreamFrames;
	std::vector<float> left;
	std::vector<float> right;
	std::uint32_t written = 0;
	std::uint64_t reported = 0;
	std::vector<Told> told;
};

ear::CycleFacts drive() {
	ear::CycleFacts facts;
	facts.causes = ear::CycleCause::drive;
	return facts;
}

} // namespace

TEST_CASE("A block read counts its frames, and finishes at the tick's start on the raw clock plus their duration",
          "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	const std::int64_t start = 5'000'000'000;
	rig.cycle(start);
	const auto progress = rig.clock.progress();
	REQUIRE(progress.has_value());
	CHECK(progress->played == block);
	CHECK(progress->base == 0);
	CHECK(progress->finishRawNs == start + frameClock.ns(block));
	CHECK(rig.rang());
	CHECK(rig.last() == static_cast<float>(block));
}

TEST_CASE("What lies downstream of the outputs is added: a frame finishes when the card that follows has played it",
          "[sendspin][code]") {
	const std::uint32_t held = 512 + 32; // a quantum ahead of the card's buffer and its headroom, as ALSA reports it
	Rig plain;
	plain.write(block);
	Rig delayed(1'000'000, held);
	delayed.write(block);
	plain.cycle(1'000'000);
	delayed.cycle(1'000'000);
	CHECK(delayed.finish() - plain.finish() == frameClock.ns(block + held) - frameClock.ns(block));
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
	const auto before = [](std::uint64_t frames, std::uint64_t at) {
		return static_cast<std::uint16_t>(std::min<std::uint64_t>((frames * 1000 + at - 1) / at, UINT16_MAX));
	};
	for (const std::uint32_t at : {1U, 3U, 7U, 8000U, 11025U, 22050U, 32000U, 44100U, 48000U, 88200U, 96000U, 176400U,
	                               192000U, 352800U, 384000U, 705600U, 768000U, 999'999U}) {
		INFO("at " << at << " Hz");
		for (const std::uint32_t maxBlock : {1U, 64U, 256U, 1024U, 4096U, 8192U}) {
			std::vector<std::uint32_t> rings{0, 1, at / 1000, at, at - 1, at + 1, 4'000'000'000U};
			for (std::uint32_t frames = 0; frames <= 200'000; frames += 97) rings.push_back(frames);
			for (const std::uint32_t frames : rings) {
				INFO("ring " << frames << ", block " << maxBlock);
				REQUIRE(requiredLeadMs(frames, maxBlock, *ear::SampleRate::of(at)) ==
				        before(std::uint64_t{frames} + maxBlock, at));
			}
		}
	}
}

// docs/node-types.md#clock: on the tick that states a catch-up, what the ring holds of the lost time is skipped and
// counted played; a catch-up on time is told at once.
TEST_CASE("A tick the graph lost and caught up is skipped and counted played, and told at once when on time",
          "[sendspin][code]") {
	Rig rig;
	rig.write(4 * block);
	rig.cycle(1000);
	REQUIRE(rig.rang());
	rig.driver.lose(1, block);
	const std::int64_t start = 1'000'000;
	REQUIRE(rig.cycle(start).causes == ear::CycleCause::catchUp);
	CHECK(rig.clock.lastHeld().frames == 3 * block);
	CHECK(rig.clock.lastSkipped() == block);
	CHECK(rig.last() == static_cast<float>(3 * block)); // that block's frames are gone
	CHECK(rig.rang());
	CHECK(rig.played() == 3 * block); // the first block, the skip and this tick's read
	CHECK(rig.finish() == frameClock.heardRawNs(start, block, {}));
}

// docs/node-types.md#clock: the node keeps no debt. What the ring did not hold of a jump is the library's to correct,
// which it sees in the next finish it is told; nothing more is skipped from what it writes next.
TEST_CASE("A catch-up past what the ring holds skips what it holds and no more, and owes nothing after",
          "[sendspin][code]") {
	SECTION("past what it held") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(1000);
		rig.driver.lose(4, block); // 1024 frames lost; the ring holds 512
		REQUIRE(rig.cycle(2000).jumpFrames == 4 * block);
		CHECK(rig.clock.lastSkipped() == 2 * block);
		CHECK(rig.clock.lastPlayed().read == 0); // nothing left to read
		CHECK(rig.played() == 3 * block);
		rig.write(block);
		rig.cycle(3000);
		CHECK(rig.clock.lastSkipped() == 0);
		CHECK(rig.clock.lastPlayed().read == block); // what came next plays whole
		CHECK(rig.last() == static_cast<float>(4 * block));
		CHECK(rig.played() == 4 * block);
	}
	SECTION("past 2^32 frames: what it holds, never a wrapped few") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(1000);
		constexpr std::int64_t jump = (std::int64_t{1} << 32) + block;
		rig.cycle(2000, 0,
		          ear::CycleFacts{.missed = static_cast<std::uint32_t>(jump / block),
		                          .missedCut = static_cast<std::uint32_t>(jump / block),
		                          .jumpFrames = jump,
		                          .causes = ear::CycleCause::catchUp});
		CHECK(rig.clock.lastSkipped() == 2 * block);
		rig.write(block);
		rig.cycle(3000, 0, ear::CycleFacts{});
		CHECK(rig.last() == static_cast<float>(4 * block));
		CHECK(rig.played() == 4 * block);
	}
}

// A pause (cycles missed and not cut) moved the graph on as a catch-up does: the ring's audio of that time is stale.
TEST_CASE("A pause skips what the ring holds of it, as a catch-up does", "[sendspin][code]") {
	Rig rig;
	rig.write(4 * block);
	rig.cycle(1000);
	rig.driver.lose(2, block, false);
	REQUIRE(rig.cycle(2000).causes == ear::CycleCause::absent);
	CHECK(rig.clock.lastSkipped() == 2 * block);
	CHECK(rig.last() == static_cast<float>(4 * block)); // two blocks skipped, the third read
	CHECK(rig.played() == 4 * block);
	rig.write(block);
	rig.cycle(3000);
	CHECK(rig.played() == 5 * block);
}

TEST_CASE("Any other cause is a new clock: a driver change, a reposition, the first tick, a drive and the tick after "
          "an oversized one skip nothing",
          "[sendspin][code]") {
	SECTION("a driver change") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(1000);
		rig.driver.changeDriver();
		REQUIRE(rig.cycle(2000).causes == ear::CycleCause::driverChange);
		CHECK(rig.clock.lastSkipped() == 0);
		CHECK(rig.played() == 2 * block);
		CHECK(rig.last() == static_cast<float>(2 * block));
	}
	SECTION("a reposition, ahead or back") {
		for (const std::int64_t by : {std::int64_t{block} + 1, -std::int64_t{block}}) {
			Rig rig;
			rig.write(3 * block);
			rig.cycle(1000);
			rig.cycle(2000);
			rig.driver.reposition(by);
			const ear::CycleFacts facts = rig.cycle(3000);
			REQUIRE(facts.causes == ear::CycleCause::reposition);
			REQUIRE(facts.jumpFrames != 0);
			CHECK(rig.clock.lastSkipped() == 0);
			CHECK(rig.played() == 3 * block);
		}
	}
	SECTION("the first tick knows no gap") {
		Rig rig;
		rig.write(3 * block);
		rig.driver.reposition(block);
		REQUIRE(rig.cycle(1000).causes == ear::CycleCause::start);
		CHECK(rig.clock.lastSkipped() == 0);
		CHECK(rig.played() == block);
	}
	SECTION("a drive, and the tick after an oversized one, each stating a jump") {
		for (const ear::CycleCause cause : {ear::CycleCause::drive, ear::CycleCause::oversized}) {
			Rig rig;
			rig.write(3 * block);
			rig.cycle(1000, 0, ear::CycleFacts{});
			rig.cycle(2000, 0, ear::CycleFacts{.jumpFrames = block, .causes = cause});
			CHECK(rig.clock.lastSkipped() == 0);
			CHECK(rig.last() == static_cast<float>(2 * block));
			CHECK(rig.played() == 2 * block);
		}
	}
}

// docs/node-types.md#clock: a clear ends the stream, so a catch-up on the tick that applies it skips nothing of the
// stream after it.
TEST_CASE("A catch-up on the tick that applies a clear skips nothing of the next stream", "[sendspin][code]") {
	Rig rig;
	rig.write(2 * block);
	rig.cycle(1000);
	rig.clock.clear();
	rig.write(2 * block); // the next stream, written after the clear
	rig.driver.lose(1, block);
	REQUIRE(rig.cycle(2000).causes == ear::CycleCause::catchUp);
	CHECK(rig.clock.lastHeld() == ear::SourceHeld{.frames = 2 * block, .clearing = true, .clears = 1});
	CHECK(rig.clock.lastSkipped() == 0);
	const auto progress = rig.clock.progress();
	REQUIRE(progress.has_value());
	CHECK(progress->base == block);                                          // the first stream's block, forgotten
	CHECK(progress->played - progress->base == rig.clock.lastPlayed().read); // the next stream's first frames
}

// docs/node-types.md#clock: a tick that began while the next was already due is one of a catch-up's burst, behind now
// until the burst ends, so it tells nothing, and it still skips what its jump states. The first tick on time tells
// every frame of the burst, dated by its own nominal start.
TEST_CASE("A burst of overdue ticks tells nothing, and the first on time tells every frame of it, dated nominally",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = frameClock.ns(block);
	rig.write(6 * block);
	rig.cycle(0);
	REQUIRE(rig.played() == block);
	REQUIRE(rig.rang());
	for (int i = 0; i < 4; ++i) {
		if (i == 2) rig.driver.lose(1, block);
		const ear::CycleFacts facts = rig.cycle((i + 1) * blockNs, (40 - 10 * i) * blockNs);
		REQUIRE(facts.causes == (i == 2 ? ear::CycleCauses(ear::CycleCause::catchUp) : ear::CycleCauses{}));
		CHECK(rig.clock.lastSkipped() == (i == 2 ? block : 0));
		CHECK(rig.played() == block);
		CHECK_FALSE(rig.rang());
	}
	const std::int64_t start = 6 * blockNs;
	REQUIRE(rig.cycle(start).whole());
	CHECK(rig.played() == 6 * block); // the burst's four reads and its skip, and this tick's block
	CHECK(rig.finish() == frameClock.heardRawNs(start, block, {}));
	CHECK(rig.rang());
	CHECK(rig.clock.clamped() == 0);
}

TEST_CASE("One overdue tick under load tells nothing, and the next on time tells it", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = frameClock.ns(block);
	rig.write(3 * block);
	rig.cycle(0);
	REQUIRE(rig.rang());
	REQUIRE(rig.cycle(blockNs, 1000).whole()); // the next due a microsecond before it began
	CHECK(rig.played() == block);
	CHECK_FALSE(rig.rang());
	REQUIRE(rig.cycle(2 * blockNs).whole());
	CHECK(rig.played() == 3 * block);
	CHECK(rig.finish() == frameClock.heardRawNs(2 * blockNs, block, {}));
}

// docs/node-types.md#clock: with no debt there is nothing for a lossy tick to wait for.
TEST_CASE("A tick that lost frames, on time, tells", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = frameClock.ns(block);
	std::int64_t at = 0;
	rig.cycle(at);
	std::uint64_t written = 0;
	for (int i = 0; i < 8; ++i) {
		at += 2 * blockNs;
		rig.write(2 * block);
		written += 2 * block;
		rig.driver.lose(1, block);
		REQUIRE(rig.cycle(at).causes == ear::CycleCause::catchUp);
		CHECK(rig.clock.lastSkipped() == block);
		CHECK(rig.played() == written);
		CHECK(rig.finish() == frameClock.heardRawNs(at, block, {}));
	}
}

// docs/node-types.md#clock: untold for untoldBound of the graph's nominal time, the library is told from the next tick
// even if it is overdue, dated by when that tick really ran (GraphCycle::ranRawNs); the on-time report after it is
// clamped to it, never stepping back, and the one after that is dated nominally again.
TEST_CASE("Overdue for untoldBound, the library is told from a tick of the burst, dated by when it ran, and the next "
          "report is clamped to it",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = frameClock.ns(block);
	const std::int64_t overdue = 3 * untoldBound.count();
	rig.write(capacity);
	rig.cycle(0);
	REQUIRE(rig.played() == block);
	std::int64_t at = blockNs;
	const std::int64_t untoldSince = at;
	std::uint64_t deferred = 0;
	for (; at - untoldSince < untoldBound.count(); at += blockNs, ++deferred) {
		rig.write(block);
		REQUIRE(rig.cycle(at, overdue).whole());
		CHECK(rig.played() == block);
	}
	REQUIRE(deferred > 1);
	rig.write(block);
	REQUIRE(rig.cycle(at, overdue).whole());
	const std::uint64_t forcedTotal = block + (deferred + 1) * block;
	CHECK(rig.played() == forcedTotal);              // told at the bound, though still overdue
	const std::int64_t ran = at + blockNs + overdue; // the next tick's due time plus how long it was overdue
	const std::int64_t forced = frameClock.heardRawNs(ran, block, {});
	CHECK(rig.finish() == forced);
	CHECK(rig.clock.clamped() == 0);
	rig.write(block);
	const std::int64_t onTime = ran - blockNs / 2;
	REQUIRE(rig.cycle(onTime).whole());
	CHECK(rig.played() == forcedTotal + block);
	CHECK(rig.finish() == forced);
	CHECK(rig.clock.clamped() == 1);
	rig.write(block);
	REQUIRE(rig.cycle(onTime + blockNs).whole());
	CHECK(rig.finish() == frameClock.heardRawNs(onTime + blockNs, block, {}));
	CHECK(rig.clock.clamped() == 1);
}

// docs/node-types.md#clock: the clamp acts only on the report after one the bound forced. Through bursts below the
// bound, catch-ups, dry ticks, a pause and a following card's latency changing, every report is the tick's own nominal
// finish, exactly: even one behind the last, when what lies downstream shrank, is told as it is.
TEST_CASE("Below the bound the clamp never acts: every report is its tick's nominal finish", "[sendspin][code]") {
	Rig rig;
	const std::int64_t blockNs = frameClock.ns(block);
	std::int64_t at = 0;
	rig.downstreamFrames = 4 * block + 32;
	std::size_t reports = 0;
	std::size_t backwards = 0;
	std::int64_t lastFinish = 0;
	std::uint64_t lastTold = 0;
	const auto check = [&](const ear::CycleFacts& facts, std::int64_t overdueNs) {
		INFO("the tick at " << at << " ns, causes " << int{facts.causes.bits()} << ", overdue " << overdueNs);
		if (rig.played() == lastTold) return;
		++reports;
		CHECK(overdueNs == 0);
		CHECK(rig.finish() == frameClock.heardRawNs(at, block, {rig.downstreamFrames, rig.downstreamFrames}));
		if (rig.finish() < lastFinish) ++backwards;
		lastFinish = rig.finish();
		lastTold = rig.played();
	};
	for (int round = 0; round < 40; ++round) {
		// What the round's ticks take but half a block: the fifth tick reads short, and the pause after it finds
		// nothing to skip or read.
		rig.write(6 * block + block / 2);
		const std::array<std::int64_t, 6> overdue{0, 9 * blockNs, 4 * blockNs, 0, 0, 0};
		for (std::size_t i = 0; i < overdue.size(); ++i) {
			if (i == 2 || i == 3) rig.driver.lose(1, block);
			if (i == 5) rig.driver.lose(1, block, false);
			at += blockNs;
			check(rig.cycle(at, overdue[i]), overdue[i]);
		}
		if (round == 20) rig.downstreamFrames = 32; // the card that followed is gone: what lies downstream shrinks
	}
	CHECK(rig.clock.dryTicks() > 0);
	CHECK(reports == 3 * 40);
	CHECK(backwards == 1); // at the shrink, told as it is
	CHECK(rig.clock.clamped() == 0);
}

TEST_CASE("Nothing read rings nothing, and plays zeros", "[sendspin][code]") {
	Rig rig;
	rig.left[0] = 9.0F;
	rig.cycle(1000);
	CHECK_FALSE(rig.rang());
	CHECK_FALSE(rig.clock.progress().has_value());
	CHECK(rig.left[0] == 0.0F);
	CHECK(rig.clock.dryTicks() == 1);
	CHECK(rig.clock.dryFrames() == block);
}

TEST_CASE("Cleared frames are never told: each stream's count begins at its base", "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	rig.cycle(1000);
	REQUIRE(rig.played() == block);
	REQUIRE(rig.rang());
	rig.notify();
	rig.write(2 * block);
	rig.clock.clear();
	rig.cycle(2000);
	CHECK(rig.clock.lastPlayed().read == 0);
	CHECK_FALSE(rig.rang()); // nothing of the next stream yet
	// A catch-up before the next stream plays has nothing to skip.
	rig.driver.lose(1, block);
	rig.cycle(3000);
	rig.write(block);
	rig.cycle(4000);
	REQUIRE(rig.rang());
	rig.notify();
	CHECK(rig.clock.progress()->base == block);
	REQUIRE(rig.told.size() == 2);
	CHECK(rig.told[1].frames == block); // the next stream's, none of the cleared two blocks
}

// docs/node-types.md#clock: a report published before a stream ended and read after it is of a stream the library
// forgot (it zeroed its count): the notifier drops it rather than tell its frames into the next stream's count.
TEST_CASE("A report of a stream that ended before the notifier read it is never told", "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	rig.tick();
	REQUIRE(rig.clock.progress().has_value());
	REQUIRE(rig.clock.progress()->stream == 0);
	rig.clock.clear(); // the stream ends before the notifier's turn
	rig.notify();
	CHECK(rig.told.empty());
	rig.write(block);
	fakeRaw += frameClock.ns(block);
	rig.tick();
	rig.notify();
	REQUIRE(rig.told.size() == 1);
	// The next stream's (what this tick read past the ended stream's 5 ms fade), none of the ended one's.
	CHECK(rig.clock.progress()->base == block);
	CHECK(rig.told[0].frames == rig.clock.lastPlayed().read);
}

// docs/node-types.md#clock: a stop drops what the ring holds without the clock asking, and the library counts every
// frame it wrote as buffered until it is told it played. The stop's first tick counts the drop played, so after a stop
// shorter than the undrained bound (no takeover) what the library wrote less what it was told is what the ring holds,
// as after a long one.
TEST_CASE("After a stop shorter than the bound, what the library wrote less what it was told is what the ring holds",
          "[sendspin][code]") {
	Rig rig(310'000 * msNs);
	const auto told = [&] {
		std::uint64_t sum = 0;
		for (const Rig::Told& one : rig.told) sum += one.frames;
		return sum;
	};
	for (int i = 0; i < 10; ++i) {
		rig.fill();
		rig.tick();
		rig.notify();
		fakeRaw += frameClock.ns(block);
	}
	rig.fill();
	REQUIRE(rig.written - told() == capacity); // full: the library writes ahead
	rig.stop();
	rig.away(500); // under the bound: the writer waits on the stopped ring, never paces
	REQUIRE(rig.clock.takeovers() == 0);
	fakeRaw += frameClock.ns(block);
	rig.tick(drive()); // the start: the stop's drop counted played
	CHECK(rig.clock.lastHeld().dropped == capacity);
	rig.notify();
	CHECK(rig.written - told() == 0); // the ring holds nothing
	REQUIRE(rig.fill() == capacity);
	for (int i = 0; i < 3; ++i) {
		fakeRaw += frameClock.ns(block);
		rig.tick();
		rig.notify();
		CHECK(rig.written - told() == capacity - (i + 1) * block);
	}
	CHECK(rig.clock.takeovers() == 0);
}

TEST_CASE("The clock's undrained bound is the ring's room behind a full advertised buffer at the PCM byte rate, less "
          "the library's wait, and never under two rings",
          "[sendspin][code]") {
	using std::chrono::nanoseconds;
	// A third of 1 MB at 24-bit stereo: 333333 B / 288000 B/s = 1.1574 s, less 100 ms.
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<48000>(), 2048) == nanoseconds{1'157'406'250 - 100'000'000});
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<96000>(), 4096) == nanoseconds{578'703'125 - 100'000'000});
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<192000>(), 8192) == nanoseconds{289'351'562 - 100'000'000});
	// A ring so long that two of it outlast the room: the floor.
	CHECK(undrainedBoundFor(ear::SampleRate::fixed<192000>(), 65536) ==
	      nanoseconds{ear::FrameClock(ear::SampleRate::fixed<192000>()).ns(2 * 65536)});
}

// ---- The hand-over: one timeline, two who advance it (docs/node-types.md#clock) ----

TEST_CASE("Past the bound the writer counts everything written played, paces at the graph's rate up to now, and the "
          "first tick back skips the stale frames uncounted",
          "[sendspin][code]") {
	Rig rig(50'000 * msNs);
	rig.tick();
	REQUIRE(rig.write(capacity) == capacity);
	rig.tick();
	CHECK(rig.played() == block);
	REQUIRE(rig.write(block) == block); // full again: 2048 held

	fakeRaw += 2000 * msNs; // no tick for 2 s: the writer takes the timeline over, its reach now
	REQUIRE(rig.write(1000) == 0);
	CHECK(rig.clock.takeovers() == 1);
	CHECK(rig.clock.lastStaleCounted() == capacity);
	CHECK(rig.played() == block + capacity); // told at once, everything written
	CHECK(rig.finish() == fakeRaw);
	CHECK(rig.rang());
	fakeRaw += 21 * msNs; // 1008 frames' time more
	REQUIRE(rig.write(1000) == 1000);
	CHECK(rig.played() == block + capacity + 1000);
	CHECK(rig.clock.pacedFrames() == capacity + 1000);
	const std::int64_t paced = rig.finish();

	rig.tick(drive()); // the start: takes the timeline back; what the ring held is stale
	CHECK(rig.clock.lastTookBack());
	CHECK(rig.clock.lastAnchored());
	CHECK(rig.clock.lastResumeFrom() == paced);
	CHECK(rig.clock.lastStaleSkipped() == capacity);
	CHECK(rig.clock.lastPlayed().skipped == capacity);
	CHECK(rig.clock.lastPlayed().read == 0);        // nothing fresh to read yet
	CHECK(rig.played() == block + capacity + 1000); // nothing counted twice
	REQUIRE(rig.write(capacity) == capacity);       // the tick is back: the ring takes it
	fakeRaw += 5 * msNs;
	rig.tick();
	// The frames written since were due from the pacer's last finish on: those heard by this tick's run, late, skipped.
	const std::int64_t late = frameClock.framesNearest(std::chrono::nanoseconds{fakeRaw - paced});
	CHECK(rig.clock.lastResumeSkipped() == late);
	CHECK(rig.last() == static_cast<float>(block + capacity + 1000 + late + block));
	CHECK(rig.played() == block + capacity + 1000 + late + block);
	CHECK(rig.finish() >= paced);
}

TEST_CASE("The finish told never steps back: across a stop and a start, and across a stall past the bound",
          "[sendspin][code]") {
	Rig rig(90'000 * msNs);
	const std::int64_t blockNs = frameClock.ns(block);
	const auto run = [&](int ticks, ear::CycleFacts facts) {
		for (int i = 0; i < ticks; ++i) {
			rig.fill();
			rig.tick(i == 0 ? facts : ear::CycleFacts{});
			rig.notify();
			fakeRaw += blockNs;
		}
	};
	run(50, {});
	rig.stop();
	rig.away(3000); // paced from 1.5 s on
	// The start's first tick is dated 3 ms before the writer's last look: its end is past the pacer's reach, the
	// block's 5.3 ms on, but the ticks back may begin before what the pacer reached.
	fakeRaw -= 3 * msNs;
	run(50, drive());
	rig.away(2500); // a stall past the bound, the ring left holding
	ear::CycleFacts caughtUp;
	caughtUp.causes = ear::CycleCause::catchUp;
	caughtUp.jumpFrames = frameClock.frames(2500ms);
	fakeRaw -= 12 * msNs; // its first two ticks back end before the pacer's reach: their reports wait
	run(50, caughtUp);

	CHECK(rig.clock.takeovers() == 2);
	REQUIRE(rig.told.size() > 100);
	for (std::size_t i = 1; i < rig.told.size(); ++i) {
		INFO("report " << i << " of " << rig.told.size());
		CHECK(rig.told[i].finishRawNs >= rig.told[i - 1].finishRawNs);
	}
}

// docs/node-types.md#clock: the pacer dates its frames with what lies downstream too, so the frames the first ticks
// back skip as late are counted on that one timeline: the first report back is the pacer's last finish plus the frames
// it tells, with nothing to correct.
TEST_CASE("Behind a following card, the first report after a hand-over is the pacer's finish plus the frames it tells",
          "[sendspin][code]") {
	constexpr std::uint32_t held = 512 + 32; // the card's buffer, as the framework carries it back
	Rig rig(120'000 * msNs, held);
	for (int i = 0; i < 20; ++i) {
		rig.fill();
		rig.tick();
		rig.notify();
		fakeRaw += frameClock.ns(block);
	}
	rig.stop();
	rig.away(2000);
	REQUIRE(rig.clock.takeovers() == 1);
	const Rig::Told lastPaced = rig.told.back();
	fakeRaw += 5 * msNs; // the start comes 5 ms after the last write
	rig.tick(drive());   // takes the timeline back: the ring empty (the stop dropped it)
	rig.notify();
	rig.fill();
	fakeRaw += frameClock.ns(block);
	rig.tick();
	CHECK(rig.clock.lastResumeSkipped() > 0);
	const std::size_t before = rig.told.size();
	rig.notify();
	REQUIRE(rig.told.size() == before + 1);
	const Rig::Told back = rig.told.back();
	INFO("the pacer's last finish " << lastPaced.finishRawNs << ", the report back " << back.finishRawNs << " for "
	                                << back.frames << " frames");
	// Within the late skip's rounding to the nearest frame.
	CHECK(
		std::abs(back.finishRawNs - (lastPaced.finishRawNs + frameClock.ns(static_cast<std::int64_t>(back.frames)))) <=
		frameClock.ns(1) / 2 + 2);
}

TEST_CASE("A stop that spans a stream's end keeps the pacer's timeline for the next stream's frames",
          "[sendspin][code]") {
	Rig rig(140'000 * msNs);
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);    // paced from 1.5 s on
	rig.clock.clear(); // the stream ends while stopped, and the next begins: the writer paces it too
	rig.away(100);
	const auto progress = rig.clock.progress();
	REQUIRE(progress.has_value());
	REQUIRE(progress->played > progress->base); // the library counts the next stream's frames
	fakeRaw += 5 * msNs;
	rig.tick(drive()); // the start: the clear applied, the timeline taken back
	CHECK(rig.clock.lastAnchored());
	rig.fill();
	fakeRaw += frameClock.ns(block);
	rig.tick();
	CHECK(rig.clock.lastResumeSkipped() ==
	      frameClock.framesNearest(std::chrono::nanoseconds{fakeRaw - progress->finishRawNs}));
}

// docs/node-types.md#clock: one owner, so a frame is never counted by both sides: a tick between its skip and its play
// holds the timeline, and a writer past the bound meanwhile waits on the ring; it takes over only from a tick that is
// done, counting what that tick did not.
TEST_CASE("A tick between its skip and its play holds the timeline: the writer takes over only once it is done",
          "[sendspin][code]") {
	Rig rig(160'000 * msNs);
	REQUIRE(rig.write(block) == block);
	rig.tick();
	REQUIRE(rig.played() == block);
	REQUIRE(rig.write(capacity) == capacity);
	fakeRaw += 1600 * msNs;
	// A tick in flight: asked, not yet told what it did.
	const ear::GraphCycle graphCycle{.startRawNs = fakeRaw.load()};
	const ear::SourceTick inFlight{
		.cycle = graphCycle, .frames = block, .rate = ear::SampleRate::fixed<rate>(), .playback = {}};
	REQUIRE(rig.clock.skipBefore(inFlight, ear::SourceHeld{.frames = capacity}) == 0);
	CHECK(rig.write(block) == 0); // into the full ring: no takeover while the tick holds the timeline
	CHECK(rig.clock.takeovers() == 0);
	rig.clock.played(inFlight, ear::SourcePlayed{.read = block});
	CHECK(rig.played() == 2 * block);
	fakeRaw += 1600 * msNs;
	CHECK(rig.write(block) == 0); // now it takes over, from the tick that read a block of the 2048
	CHECK(rig.clock.takeovers() == 1);
	CHECK(rig.clock.lastStaleCounted() == capacity - block);
	CHECK(rig.played() == block + capacity); // every frame written, each counted once
}

namespace {

/// A rig stopped for 2 s (paced from 1.5 s on, behind a card of 512 frames) and started 5 ms after the last write:
/// the tick that takes the timeline back, which reads nothing (the stop dropped the ring).
Rig& stoppedAndStarted(Rig& rig) {
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);
	REQUIRE(rig.clock.takeovers() == 1);
	fakeRaw += 5 * msNs;
	rig.tick(drive());
	REQUIRE(rig.clock.lastTookBack());
	REQUIRE(rig.clock.lastResumeSkipped() == 0);
	return rig;
}

/// Frames from the pacer's finish to when this tick's first frame is heard: what the timeline makes late.
std::int64_t lateNow(const Rig& rig) {
	return frameClock.framesNearest(
		std::chrono::nanoseconds{fakeRaw + frameClock.ns(rig.downstreamFrames) - rig.clock.lastResumeFrom()});
}

} // namespace

TEST_CASE("A catch-up while the pacer's timeline holds skips its late frames once, never its jump as well",
          "[sendspin][code]") {
	Rig rig(180'000 * msNs, 512);
	stoppedAndStarted(rig);
	REQUIRE(rig.fill() == capacity);
	constexpr std::int64_t jump = 512;
	fakeRaw += frameClock.ns(block + jump);
	ear::CycleFacts caughtUp;
	caughtUp.causes = ear::CycleCause::catchUp;
	caughtUp.jumpFrames = jump;
	const std::int64_t late = lateNow(rig);
	REQUIRE(late > jump);
	REQUIRE(late < capacity);
	rig.tick(caughtUp);
	CHECK(rig.clock.lastAnchored());
	CHECK(rig.clock.lastSkipped() == 0);
	CHECK(rig.clock.lastResumeSkipped() == late);
}

TEST_CASE("A gap in the library's writes after a hand-over ends the pacer's timeline after a ring's worth of ticks",
          "[sendspin][code]") {
	Rig rig(200'000 * msNs, 512);
	stoppedAndStarted(rig);
	// The library writes nothing for 100 ms (a network gap with no stream end): the ticks play zeros.
	std::uint32_t ran = block; // the tick that took it back
	for (; frameClock.ns(ran) < 100 * msNs; ran += block) {
		fakeRaw += frameClock.ns(block);
		rig.tick();
		INFO("tick " << ran / block);
		CHECK(rig.clock.lastAnchored() == (ran < capacity));
	}
	REQUIRE(rig.fill() == capacity); // its audio comes back
	fakeRaw += frameClock.ns(block);
	REQUIRE(lateNow(rig) > capacity);
	rig.tick();
	// The gap is the library's to correct, as outside a hand-over: what it wrote plays, none of it skipped.
	CHECK_FALSE(rig.clock.lastAnchored());
	CHECK(rig.clock.lastResumeSkipped() == 0);
	CHECK(rig.clock.lastPlayed().read == block);
}

// The resume state machine's pending state (a drop's finish not yet readable) is gone: the pacer writes its anchor
// before the owner word names it, so the tick that takes the timeline back reads it whole, at once, whatever the
// writer was doing.
TEST_CASE("The first tick back reads the pacer's anchor whole: anchored at once, at the last finish it told",
          "[sendspin][code]") {
	Rig rig(220'000 * msNs, 512);
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);
	const std::int64_t lastTold = rig.finish();
	fakeRaw += 5 * msNs;
	rig.tick(drive());
	CHECK(rig.clock.lastTookBack());
	CHECK(rig.clock.lastAnchored());
	CHECK(rig.clock.lastResumeFrom() == lastTold);
}

// docs/node-types.md#clock: the finish told never steps back across a hand-over, by construction rather than a clamp:
// the pacer never reaches past now, and a tick tells only once it ends past the pacer's reach, which the tick after
// the one that took the timeline back always does (it began after that one ran).
TEST_CASE("Back from the pacer, a tick that ends before the pacer's reach leaves its report to the next, which ends "
          "past it",
          "[sendspin][code]") {
	Rig rig(240'000 * msNs);
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);
	const std::int64_t reach = rig.finish();        // nothing downstream: the pacer's reach itself
	const std::int64_t start = fakeRaw - 10 * msNs; // on time, its start 10 ms before the pacer's reach
	rig.cycle(start, 0, drive());
	CHECK(rig.clock.lastTookBack());
	CHECK_FALSE(rig.clock.lastTold().has_value());
	CHECK(rig.clock.lastTellDeferred());
	CHECK(rig.finish() == reach);
	rig.cycle(start + frameClock.ns(block), 0, ear::CycleFacts{});
	REQUIRE(rig.clock.lastTold().has_value());
	CHECK(rig.clock.lastTold()->finishRawNs >= reach);
	CHECK(rig.clock.clamped() == 0);
}

// docs/node-types.md#clock: the deferral compares finishes, what lies downstream included, so a graph edited while the
// node was stopped (the card that followed it gone) cannot make the first report back step behind the pacer's last.
TEST_CASE("Back from the pacer with less downstream than it dated by, the reports wait until their finish is past "
          "its last",
          "[sendspin][code]") {
	Rig rig(250'000 * msNs, 1024);
	for (int i = 0; i < 20; ++i) {
		rig.fill();
		rig.tick();
		rig.notify();
		fakeRaw += frameClock.ns(block);
	}
	rig.stop();
	rig.away(2000);
	REQUIRE(rig.clock.takeovers() == 1);
	const std::int64_t pacedFinish = rig.told.back().finishRawNs; // its reach plus the card's 1024 frames (21.3 ms)
	rig.downstreamFrames = 0;                                     // the card is gone when the node starts
	fakeRaw += msNs;
	rig.tick(drive());
	CHECK(rig.clock.lastTookBack());
	CHECK(rig.clock.lastTellDeferred()); // its end is 6.3 ms past the pacer's reach, 15 ms short of its finish
	CHECK(rig.finish() == pacedFinish);  // the pacer's last report, now the tick's
	rig.notify();
	int deferred = 1;
	for (int i = 0; i < 6; ++i) {
		rig.fill();
		fakeRaw += frameClock.ns(block);
		rig.tick();
		deferred += rig.clock.lastTellDeferred() ? 1 : 0;
		if (const auto told = rig.clock.lastTold()) CHECK(told->finishRawNs >= pacedFinish);
		rig.notify();
	}
	CHECK(deferred == 3); // the ticks ending 6.3, 11.7 and 17 ms past the reach
	REQUIRE(rig.told.size() > 2);
	for (std::size_t i = 1; i < rig.told.size(); ++i) {
		INFO("report " << i << " of " << rig.told.size());
		CHECK(rig.told[i].finishRawNs >= rig.told[i - 1].finishRawNs);
	}
}

// docs/node-types.md#clock: each owner writes its own report box and the word names the one read, so a pacer held
// mid-report (its thread preempted) as a tick takes the timeline back neither holds the tick's report back nor keeps
// a reader waiting.
TEST_CASE("A pacer held mid-report as the tick takes the timeline back: the tick tells at once, and the reader takes "
          "its report without waiting",
          "[sendspin][code]") {
	Rig rig(270'000 * msNs);
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);
	REQUIRE(rig.clock.takeovers() == 1);
	rig.clock.holdPacerBox(true);
	fakeRaw += 10 * msNs; // past the pacer's last finish: nothing to wait for but the box
	rig.tick(drive());
	REQUIRE(rig.clock.lastTookBack());
	rig.fill();
	fakeRaw += frameClock.ns(block);
	rig.tick();
	CHECK_FALSE(rig.clock.lastTellDeferred());
	REQUIRE(rig.clock.lastTold().has_value());
	auto read = std::async(std::launch::async, [&] { return rig.clock.progress(); });
	const bool readable = read.wait_for(5s) == std::future_status::ready;
	rig.clock.holdPacerBox(false); // so a reader stuck on it returns
	CHECK(readable);
	CHECK(read.get() == rig.clock.lastTold());
}

TEST_CASE("A stream ended while paced, before anything of the next: the tick back ends the timeline and starts the "
          "next stream's count",
          "[sendspin][code]") {
	Rig rig(260'000 * msNs, 512);
	rig.fill();
	rig.tick();
	rig.stop();
	rig.away(2000);
	const std::uint64_t paced = rig.played();
	rig.clock.clear(); // the stream ends; the library writes nothing of the next before the start
	fakeRaw += 5 * msNs;
	rig.tick(drive());
	CHECK(rig.clock.lastTookBack());
	CHECK_FALSE(rig.clock.lastAnchored());
	rig.fill();
	fakeRaw += frameClock.ns(block);
	rig.tick();
	CHECK(rig.clock.lastResumeSkipped() == 0);
	const auto progress = rig.clock.progress();
	REQUIRE(progress.has_value());
	CHECK(progress->base == paced);
	CHECK(progress->played == paced + rig.clock.lastPlayed().read);
}

// docs/node-types.md#clock: a stop empties the ring, so the pacer's timeline ends at the stop's first tick unless that
// tick takes it back: the library's writes after a short stop are its own to place, never late by the stop's length.
TEST_CASE("A short stop soon after a hand-over back ends the pacer's timeline: the ticks after it skip nothing as late",
          "[sendspin][code]") {
	Rig rig(280'000 * msNs);
	const auto told = [&] {
		std::uint64_t sum = 0;
		for (const Rig::Told& one : rig.told) sum += one.frames;
		return sum;
	};
	rig.fill();
	rig.tick();
	rig.notify();
	rig.stop();
	rig.away(2000); // paced from 1.5 s on
	REQUIRE(rig.clock.takeovers() == 1);
	fakeRaw += 5 * msNs;
	rig.tick(drive()); // the start: the timeline taken back, the ring empty
	REQUIRE(rig.clock.lastTookBack());
	REQUIRE(rig.clock.lastAnchored());
	rig.notify();
	REQUIRE(rig.fill() == capacity); // the library writes, and the node stops again before a tick reads any of it
	rig.stop();
	rig.away(500); // under the bound: no takeover
	REQUIRE(rig.clock.takeovers() == 1);
	fakeRaw += frameClock.ns(block);
	rig.tick(drive()); // the start: the stop's drop counted played
	CHECK(rig.clock.lastHeld().stopEnded);
	CHECK_FALSE(rig.clock.lastAnchored());
	rig.notify();
	CHECK(rig.written - told() == 0);
	REQUIRE(rig.fill() == capacity);
	fakeRaw += frameClock.ns(block);
	rig.tick();
	rig.notify();
	CHECK(rig.clock.lastResumeSkipped() == 0);
	CHECK(rig.clock.lastSkipped() == 0);
	CHECK(rig.written - told() == capacity - block); // what the ring holds
}

// docs/node-types.md#clock: the stale frames the pacer counted are among what a stop drops, so the stop's first tick
// counts only the rest of its drop, and nothing stale is left to skip of what the library writes after it.
TEST_CASE("Stale frames a clearing tick back leaves, dropped by a stop, are counted once and skip nothing fresh",
          "[sendspin][code]") {
	Rig rig(290'000 * msNs);
	REQUIRE(rig.write(1024) == 1024);
	rig.tick();        // reads a block: 768 of the first stream held
	rig.clock.clear(); // the stream ends while ticking, and the library writes the next at once
	const std::uint32_t atClear = rig.written;
	REQUIRE(rig.write(1024) == 1024);
	fakeRaw += 2000 * msNs; // no tick for 2 s: the writer takes over, counting the next stream's 1024 played
	rig.write(block);
	REQUIRE(rig.clock.takeovers() == 1);
	REQUIRE(rig.clock.lastStaleCounted() == 1024);
	fakeRaw += 5 * msNs;
	rig.tick(
		drive()); // takes the timeline back and applies the clear: the ended stream's tail, then a few stale frames
	REQUIRE(rig.clock.lastTookBack());
	REQUIRE(rig.clock.lastHeld().clearing);
	REQUIRE(rig.clock.lastPlayed().read < 1024); // stale frames left in the ring
	rig.stop();
	rig.away(100); // under the bound: no takeover
	REQUIRE(rig.clock.takeovers() == 1);
	fakeRaw += frameClock.ns(block);
	rig.tick(drive()); // the start: the stop dropped the stale rest, already counted by the pacer
	CHECK(rig.clock.lastHeld().stopEnded);
	REQUIRE(rig.fill() == capacity);
	fakeRaw += frameClock.ns(block);
	rig.tick();
	CHECK(rig.clock.lastStaleSkipped() == 0);
	CHECK(rig.clock.lastPlayed().skipped == 0);
	const auto progress = rig.clock.progress();
	REQUIRE(progress.has_value());
	CHECK(rig.written - atClear - (progress->played - progress->base) == capacity - block); // what the ring holds
}

// The one owner, raced for real: the library's thread writing through the clock while the ticks come and go on
// another, stopping for past the bound and coming back. Every frame the clock took is counted played exactly once
// (no clear, no jump: every one is played or skipped as stale or late), and the count told never goes back.
TEST_CASE("Ticks and the pacer on two threads count every frame written exactly once, and the count told never goes "
          "back",
          "[sendspin][code]") {
	Rig rig(300'000 * msNs);
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> taken{0};
	std::thread library([&] {
		const std::vector<float> samples(std::size_t{64} * 2, 0.5F);
		while (!done.load()) {
			const std::uint32_t got = rig.clock.write(samples, 64, 0ms);
			taken.fetch_add(got);
			if (got == 0) std::this_thread::yield();
		}
	});
	std::uint64_t lastPlayed = 0;
	std::size_t backwards = 0;
	const std::int64_t blockNs = frameClock.ns(block);
	for (int round = 0; round < 30; ++round) {
		for (int i = 0; i < 200; ++i) {
			fakeRaw += blockNs;
			rig.tick();
			const std::uint64_t played = rig.played();
			if (played < lastPlayed) ++backwards;
			lastPlayed = played;
		}
		// No tick for past the bound, until the writer has taken the timeline over (its own event: takeovers()), and
		// a while more of pacing; the deadline only fails the test.
		const std::uint64_t before = rig.clock.takeovers();
		fakeRaw += 1600 * msNs;
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (rig.clock.takeovers() == before && std::chrono::steady_clock::now() < deadline)
			ear::sleeps::poll(50us, "between looks at the writer's takeovers(): the count decides");
		REQUIRE(rig.clock.takeovers() == before + 1);
		for (int i = 0; i < 20; ++i) {
			fakeRaw += 5 * msNs;
			ear::sleeps::pace(50us, "the stand-in graph's tick-less stretch, in steps the writer thread sees");
			const std::uint64_t played = rig.played();
			if (played < lastPlayed) ++backwards;
			lastPlayed = played;
		}
	}
	done = true;
	library.join();
	// Drain what is left, on time, until told.
	for (int i = 0; i < 64; ++i) {
		fakeRaw += blockNs;
		rig.tick();
	}
	CHECK(rig.clock.takeovers() == 30);
	CHECK(backwards == 0);
	CHECK(rig.played() == taken.load());
}
