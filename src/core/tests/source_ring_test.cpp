// SourceRing, every rule a source plays by (docs/writing-a-node-type.md#sources), driven cycle by cycle on the test's
// thread, each cycle's facts stated by the one rule (CycleAccount over a CutRing) as the engine states them: frames
// played exactly once faded in, keeping time across a catch-up of any length and a new clock on any other cause, the
// fades at a clear, an underrun and a return sample for sample, the shortfall only while the writer is not idle, a
// stop's drop, Played and its dating, a closed ring, a catch-up past 2^32 frames, and Played read whole from another
// thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/cut_ring.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/source_ring.hpp"

using namespace ear;

namespace {

/// At 1000 Hz a fade is 5 frames and a frame lasts 1 ms.
constexpr std::uint32_t kRate = 1000;
constexpr std::uint32_t kFade = 5;
constexpr std::int64_t kFrameNs = 1'000'000;

struct CountingSignals final : SourceRing::Signals {
	int room = 0;
	int played = 0;
	void roomFreed() noexcept override { ++room; }
	void playedChanged() noexcept override { ++played; }
};

/// The stream's frame k, channel c: nonzero and exact as a float, the channels apart.
float streamAt(std::uint64_t k, std::uint32_t c = 0) noexcept {
	return static_cast<float>(k + 1) * (c == 0 ? 1.F : -1.F);
}

/// Frames [from, from + count) of the stream, interleaved over `channels`.
std::vector<float> stream(std::uint64_t from, std::uint32_t count, std::uint32_t channels = 1) {
	std::vector<float> out;
	for (std::uint64_t k = from; k < from + count; ++k)
		for (std::uint32_t c = 0; c < channels; ++c) out.push_back(streamAt(k, c));
	return out;
}

/// One bank of `channels` spans, NaN-filled before each cycle so a frame left unwritten shows.
struct Bank {
	Bank(std::uint32_t channels, std::uint32_t frames) : data(channels, std::vector<float>(frames)) {
		for (auto& channel : data) spans.emplace_back(channel);
	}
	std::span<const std::span<float>> view() {
		for (auto& channel : data) std::ranges::fill(channel, std::numeric_limits<float>::quiet_NaN());
		return spans;
	}
	std::vector<std::vector<float>> data;
	std::vector<std::span<float>> spans;
};

/// The graph's driver as a block states its cycles (docs/writing-a-node-type.md#lost-cycles): numbered and cut in a
/// CutRing, each cycle's facts by CycleAccount, so the ring keeps time by the facts the engine would hand it. A cycle
/// starts `position` ms on the raw clock (one frame per ms).
class Clock {
public:
	explicit Clock(std::uint64_t position = 0) : position_(position) {}
	/// The next cycle the source plays, of `frames`.
	SourceCycle next(std::uint32_t frames, Latency downstream = {}) {
		const CycleFacts facts = tick(frames);
		return SourceCycle{.position = position_ - frames,
		                   .frames = frames,
		                   .startRawNs = static_cast<std::int64_t>(position_ - frames) * kFrameNs,
		                   .facts = facts,
		                   .downstream = downstream};
	}
	/// `count` cycles of `frames` the block did not run, each cut short by the driver: the next states a catch-up of
	/// them; with `cut` false, a pause (absent).
	void lose(std::uint32_t count, std::uint32_t frames, bool cut = true) {
		for (std::uint32_t i = 0; i < count; ++i) {
			cuts_.start();
			if (cut) cuts_.incomplete();
			position_ += frames;
		}
	}
	/// `count` cycles of `frames` the block ran with no code, so no source played (a stop's silence).
	void silent(std::uint32_t count, std::uint32_t frames) {
		for (std::uint32_t i = 0; i < count; ++i) tick(frames);
	}
	/// A drive installed a code: the next cycle states `drive`.
	void drive() { account_.driven(); }
	/// The next cycle's position moves by `frames` with no cycle missed: reposition.
	void reposition(std::int64_t frames) { position_ += static_cast<std::uint64_t>(frames); }
	/// The next cycle runs on another driver: driverChange.
	void changeDriver() { ++driver_; }

private:
	CycleFacts tick(std::uint32_t frames) {
		cuts_.start();
		const CycleFacts facts = account_
		                             .next(DriverTick{.driverId = driver_,
		                                              .graphCycle = cuts_.started(),
		                                              .position = position_,
		                                              .frames = frames,
		                                              .startRawNs = static_cast<std::int64_t>(position_) * kFrameNs},
		                                   frames)
		                             .facts;
		position_ += frames;
		return facts;
	}
	CutRing cuts_;
	CycleAccount account_{FrameClock(kRate), &cuts_};
	std::uint32_t driver_ = 1;
	std::uint64_t position_;
};

/// What the ring plays for `value` at fade level `level` (of kFade), as play() computes it.
float faded(float value, std::uint32_t level) {
	return value * (static_cast<float>(level) / static_cast<float>(kFade));
}

std::uint32_t offer(SourceRing& ring, std::uint64_t from, std::uint32_t count, std::uint32_t channels = 1) {
	return ring.offer(stream(from, count, channels), count);
}

} // namespace

TEST_CASE("A source plays its ring's frames in order, fading in over its first 5 ms, and counts them played",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(2, 64, kRate, signals);
	CHECK(ring.capacityFrames() == 64);
	CHECK(offer(ring, 0, 40, 2) == 40);
	Bank bank(2, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < 16; ++i) {
		INFO("frame " << i);
		CHECK(bank.data[0][i] == faded(streamAt(i, 0), std::min(i, kFade)));
		CHECK(bank.data[1][i] == faded(streamAt(i, 1), std::min(i, kFade)));
	}
	CHECK(ring.played().totalFrames == 16);
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < 16; ++i) {
		CHECK(bank.data[0][i] == streamAt(16 + i, 0));
		CHECK(bank.data[1][i] == streamAt(16 + i, 1));
	}
	CHECK(ring.played().totalFrames == 32);
	CHECK(signals.played == 2);
	CHECK(signals.room == 2);
	CHECK(ring.shortfallFrames() == 0);
	CHECK(ring.droppedFrames() == 0);
	// A capacity that is no power of two is rounded up; a full ring takes what fits.
	CountingSignals other;
	SourceRing rounded(1, 100, kRate, other);
	CHECK(rounded.capacityFrames() == 128);
	CHECK(offer(rounded, 0, 200) == 128);
	CHECK(offer(rounded, 128, 1) == 0);
}

TEST_CASE("A catch-up of any length is skipped and counted played, and any other cause is a new clock",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	Clock clock(1000);
	ring.play(bank.view(), clock.next(8));
	ring.play(bank.view(), clock.next(8)); // contiguous
	CHECK(ring.played().totalFrames == 16);
	// 24 frames lost, three cycles cut: skipped, counted played, so the stream keeps to the clock.
	clock.lose(3, 8);
	SourceCycle cycle = clock.next(8);
	REQUIRE(cycle.facts.causes == CycleCause::catchUp);
	ring.play(bank.view(), cycle);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(40 + i));
	CHECK(ring.played().totalFrames == 48);
	CHECK(ring.played().totalFrames == cycle.position + 8 - 1000);
	// The capacity's worth: a skip of the whole ring (the writer idle, so the dry cycle is no shortfall).
	REQUIRE(offer(ring, 64, 48) == 48);
	ring.markIdle();
	clock.lose(8, 8);
	ring.play(bank.view(), clock.next(8));
	CHECK(ring.played().totalFrames == 48 + 64);
	CHECK(ring.shortfallFrames() == 0);
	// Past the capacity: kept in step too. The ring's 40 are skipped and the rest owed, dropped from what comes next.
	REQUIRE(offer(ring, 112, 40) == 40);
	clock.lose(9, 8); // 72 frames
	ring.play(bank.view(), clock.next(8));
	CHECK(ring.played().totalFrames == 112 + 72);
	CHECK(ring.shortfallFrames() == 8); // dry: the writer, not idle, had not written them
	REQUIRE(offer(ring, 152, 40) == 40);
	ring.play(bank.view(), clock.next(8)); // the 32 owed dropped, then 184 on, faded in after the dry cycle
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == faded(streamAt(184 + i), std::min(i, kFade)));
	CHECK(ring.played().totalFrames == 192);
	CHECK(ring.droppedFrames() == 0); // owed frames are played, not dropped
	// Any other cause is a new clock: nothing skipped, the stream played on from where it was.
	REQUIRE(offer(ring, 192, 32) == 32);
	clock.reposition(-100); // back
	ring.play(bank.view(), cycle = clock.next(8));
	REQUIRE(cycle.facts.causes == CycleCause::reposition);
	clock.reposition(3); // by part of a cycle
	ring.play(bank.view(), cycle = clock.next(8));
	REQUIRE(cycle.facts.causes == CycleCause::reposition);
	clock.lose(2, 8, false); // a pause: missed, not cut
	ring.play(bank.view(), cycle = clock.next(8));
	REQUIRE(cycle.facts.causes == CycleCause::absent);
	clock.changeDriver();
	ring.play(bank.view(), cycle = clock.next(8));
	REQUIRE(cycle.facts.causes == CycleCause::driverChange);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(216 + i));
	CHECK(ring.played().totalFrames == 224);
	CHECK(ring.shortfallFrames() == 8);
}

// A catch-up's whole jump is counted, however long (the owner's decision: what is reported played keeps in step with
// the clock), and what the ring does not hold is owed from what is written next, past 2^32 frames too.
TEST_CASE("A catch-up past 2^32 frames is counted whole and owed whole", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	Clock clock;
	ring.play(bank.view(), clock.next(8));
	constexpr std::uint64_t jump = (std::uint64_t{1} << 32U) + 8;
	ring.play(bank.view(), SourceCycle{.position = 8 + jump,
	                                   .frames = 8,
	                                   .startRawNs = 0,
	                                   .facts = {.missed = static_cast<std::uint32_t>(jump / 8),
	                                             .missedCut = static_cast<std::uint32_t>(jump / 8),
	                                             .jumpFrames = static_cast<std::int64_t>(jump),
	                                             .causes = CycleCause::catchUp},
	                                   .downstream = {}});
	CHECK(ring.played().totalFrames == 8 + jump);
	// The ring's 56 are skipped; every frame written from here on is owed, 2^32 - 48 of them.
	for (std::uint64_t at = 64; at < 64 + 64 * 4; at += 64) {
		REQUIRE(offer(ring, at, 64) == 64);
		ring.play(bank.view(), SourceCycle{.position = 0, .frames = 8, .startRawNs = 0, .facts = {}, .downstream = {}});
		for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == 0.0F);
	}
	CHECK(ring.played().totalFrames == 8 + jump);
	CHECK(ring.droppedFrames() == 0);
	// A clear forgets it.
	ring.clear();
	REQUIRE(offer(ring, 1000, 8) == 8);
	ring.play(bank.view(), SourceCycle{.position = 0, .frames = 8, .startRawNs = 0, .facts = {}, .downstream = {}});
	CHECK(bank.data[0][kFade] == faded(streamAt(1000 + kFade), kFade));
}

TEST_CASE("A clear plays the next 5 ms faded to zero, counts them played, drops the rest, and the next audio fades in",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16)); // faded in, now full
	ring.clear();
	CHECK(offer(ring, 1000, 8) == 8); // written after the clear: the new stream
	ring.play(bank.view(), clock.next(16));
	// The tail: frames 16..20 from full level down, one step a frame; then silence until the fade reached zero.
	for (std::uint32_t i = 0; i < kFade; ++i) CHECK(bank.data[0][i] == faded(streamAt(16 + i), kFade - i));
	// The fade is over: the new stream fades in from zero in the same cycle.
	for (std::uint32_t i = kFade; i < kFade + 8; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(1000 + i - kFade), std::min(i - kFade, kFade)));
	for (std::uint32_t i = kFade + 8; i < 16; ++i) // dry: a decay from the new stream's last frame
		CHECK(bank.data[0][i] == faded(streamAt(1007), kFade - (i - kFade - 8)));
	CHECK(ring.played().totalFrames == 16 + kFade + 8);
	CHECK(ring.droppedFrames() == 40 - 16 - kFade);
}

TEST_CASE("A clear with less than 5 ms written decays from its last frame to zero before the next audio",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 18) == 18);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	ring.clear(); // 2 frames left
	REQUIRE(offer(ring, 500, 20) == 20);
	ring.play(bank.view(), clock.next(16));
	CHECK(bank.data[0][0] == faded(streamAt(16), 5));
	CHECK(bank.data[0][1] == faded(streamAt(17), 4));
	CHECK(bank.data[0][2] == faded(streamAt(17), 3)); // held, decaying
	CHECK(bank.data[0][3] == faded(streamAt(17), 2));
	CHECK(bank.data[0][4] == faded(streamAt(17), 1));
	for (std::uint32_t i = 5; i < 16; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(500 + i - 5), std::min(i - 5, kFade)));
	CHECK(ring.played().totalFrames == 16 + 2 + 11);
	CHECK(ring.shortfallFrames() == 0); // the clear's fade is no shortfall
}

TEST_CASE("At an underrun a source decays from its last frame over 5 ms and counts the shortfall, and fades in when "
          "audio returns",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(2, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 20, 2) == 20);
	Bank bank(2, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	ring.play(bank.view(), clock.next(16)); // 4 frames, then dry
	for (std::uint32_t i = 0; i < 4; ++i) CHECK(bank.data[1][i] == streamAt(16 + i, 1));
	for (std::uint32_t i = 0; i < kFade; ++i) {
		CHECK(bank.data[0][4 + i] == faded(streamAt(19, 0), kFade - i));
		CHECK(bank.data[1][4 + i] == faded(streamAt(19, 1), kFade - i));
	}
	for (std::uint32_t i = 4 + kFade; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 12);
	CHECK(ring.played().totalFrames == 20);
	const int published = signals.played;
	ring.play(bank.view(), clock.next(16)); // all dry: nothing played, nothing published
	CHECK(ring.shortfallFrames() == 28);
	CHECK(signals.played == published);
	// Back: from zero, over 5 ms.
	REQUIRE(offer(ring, 20, 16, 2) == 16);
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(20 + i), std::min(i, kFade)));
	CHECK(ring.played().totalFrames == 36);
	// A short gap decays partly and the return fades in from where the decay got to.
	REQUIRE(offer(ring, 36, 14, 2) == 14);
	ring.play(bank.view(), clock.next(16)); // 14 frames, 2 decaying (5, then 4)
	CHECK(bank.data[0][14] == faded(streamAt(49), 5));
	CHECK(bank.data[0][15] == faded(streamAt(49), 4));
	REQUIRE(offer(ring, 50, 16, 2) == 16);
	ring.play(bank.view(), clock.next(16));
	CHECK(bank.data[0][0] == faded(streamAt(50), 3));
	CHECK(bank.data[0][1] == faded(streamAt(51), 4));
	CHECK(bank.data[0][2] == streamAt(52));
}

TEST_CASE("A source's shortfall counts only while its writer is not idle", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16)); // never written: idle
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	REQUIRE(offer(ring, 0, 8) == 8);
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.shortfallFrames() == 8);
	ring.markIdle();
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.shortfallFrames() == 8);
	CHECK(offer(ring, 8, 0) == 0); // no frames: still idle
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.shortfallFrames() == 8);
	REQUIRE(offer(ring, 8, 4) == 4); // a write ends the idle
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.shortfallFrames() == 20);
}

TEST_CASE("A stop drops and counts what the ring holds, plays nothing of it, refuses writes until the next cycle and "
          "keeps time afresh",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	ring.dropForStop();
	CHECK(ring.droppedFrames() == 24);
	CHECK(offer(ring, 100, 8) == 0); // stopped: waits as at a full ring
	const int room = signals.room;
	// The first cycle after the next drive: silent (the decay of the last frame aside: none, the fade was reset), no
	// shortfall, and the writer let go; a catch-up stated with the drive is no skip, the time kept afresh.
	clock.silent(20, 16);
	clock.lose(3, 16);
	clock.drive();
	const SourceCycle first = clock.next(16);
	REQUIRE(first.facts.causes == (CycleCause::drive | CycleCause::catchUp));
	ring.play(bank.view(), first);
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	CHECK(signals.room == room + 1);
	CHECK(ring.played().totalFrames == 16);
	REQUIRE(offer(ring, 100, 16) == 16);
	clock.lose(2, 16);
	ring.play(bank.view(), clock.next(16));      // the time kept from the cycle above: 32 frames skipped
	CHECK(ring.played().totalFrames == 16 + 32); // all 32 counted: 16 the ring held, 16 owed
	CHECK(ring.droppedFrames() == 24);
}

TEST_CASE("A catch-up past what a writer that fell behind has written is owed: the next frames written are dropped, "
          "counted played, so the stream keeps to the clock",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 21) == 21);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16)); // 5 frames left
	clock.lose(2, 16);
	ring.play(bank.view(), clock.next(16));
	// 32 lost: the 5 held skipped, 27 owed; all 32 counted at once; nothing to play.
	CHECK(ring.played().totalFrames == 16 + 32);
	for (std::uint32_t i = kFade; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	// The writer catches up from where it was: stream frames 21..47 are the debt, dropped; 48 on play, at the clock.
	REQUIRE(offer(ring, 21, 60) == 60);
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(48 + i), std::min(i, kFade)));
	// The clock's 80 frames since the first position, but for the dry cycle's 16 zeros (a shortfall, not played).
	CHECK(ring.played().totalFrames == 80 - 16);
	CHECK(ring.shortfallFrames() == 16);
	CHECK(ring.droppedFrames() == 0); // owed frames are played, not dropped
	// A clear forgets a debt: the next stream owes nothing.
	clock.lose(2, 16);
	ring.play(bank.view(), clock.next(16)); // 32 lost, the ring holds 17: 15 owed
	CHECK(ring.played().totalFrames == 64 + 32);
	ring.clear();
	REQUIRE(offer(ring, 500, 16) == 16);
	ring.play(bank.view(), clock.next(16));
	CHECK(bank.data[0][kFade] == faded(streamAt(500 + kFade), kFade)); // nothing of 500.. was dropped
}

// An idle writer has nothing to give: its next frames begin a stream with no place on the clock yet, so a catch-up
// while it is idle owes nothing (however many come before it writes again), and the count still keeps to the clock.
TEST_CASE("A catch-up while the writer is idle skips what the ring holds and owes nothing", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 20) == 20);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16)); // 4 frames left
	ring.markIdle();
	for (int stall = 0; stall < 3; ++stall) {
		clock.lose(4, 16);
		ring.play(bank.view(), clock.next(16));
	}
	CHECK(ring.played().totalFrames == 16 + 3 * 64);
	REQUIRE(offer(ring, 1000, 16) == 16);
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(1000 + i), std::min(i, kFade)));
	CHECK(ring.shortfallFrames() == 0);
}

TEST_CASE("A catch-up during a clear's fade skips the tail, and the rest is owed by the new stream", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	ring.clear();
	REQUIRE(offer(ring, 1000, 40) == 40);
	// 16 lost: the tail's 5 frames (16..20) skipped, 11 owed by the new stream.
	clock.lose(1, 16);
	ring.play(bank.view(), clock.next(16));
	for (std::uint32_t i = 0; i < kFade; ++i) CHECK(bank.data[0][i] == faded(streamAt(20), kFade - i)); // the decay
	for (std::uint32_t i = kFade; i < 16; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(1011 + i - kFade), std::min(i - kFade, kFade)));
	CHECK(ring.played().totalFrames == 16 + 16 + 11);
	CHECK(ring.played().totalFrames == 48 - kFade); // on the clock, but for the decay's frames
}

TEST_CASE("Played is dated by the cycle's start, the frames played and the most latency downstream", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	CHECK(ring.played() == Played{});
	REQUIRE(offer(ring, 0, 24) == 24);
	Bank bank(1, 16);
	Clock clock(100);
	ring.play(bank.view(), clock.next(16, Latency{40, 100}));
	CHECK(ring.played() == Played{16, (100 + 16 + 100) * kFrameNs});
	ring.play(bank.view(), clock.next(16, Latency{0, 7})); // 8 played, then dry
	CHECK(ring.played() == Played{24, (116 + 8 + 7) * kFrameNs});
	// A cycle that only skipped is dated at its start.
	REQUIRE(offer(ring, 24, 4) == 4);
	ring.markIdle();
	clock.lose(1, 16);
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.played() == Played{24 + 16, (132 + 16 + 0) * kFrameNs});
}

TEST_CASE("A closed source accepts nothing and still plays what it holds", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 16) == 16);
	CHECK_FALSE(ring.closed());
	ring.close();
	ring.close(); // idempotent
	CHECK(ring.closed());
	CHECK(offer(ring, 16, 8) == 0);
	Bank bank(1, 16);
	Clock clock;
	ring.play(bank.view(), clock.next(16));
	CHECK(ring.played().totalFrames == 16);
}

TEST_CASE("Played is read whole on another thread while cycles publish it", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 4096, kRate, signals);
	std::atomic<bool> done{false};
	std::atomic<std::uint64_t> torn{0};
	std::atomic<std::uint64_t> reads{0};
	std::thread reader([&] {
		while (!done.load(std::memory_order_acquire)) {
			const Played seen = ring.played();
			// Contiguous cycles from position 0 with nothing downstream: heard is always totalFrames ms.
			if (seen.heardRawNs != static_cast<std::int64_t>(seen.totalFrames) * kFrameNs) torn.fetch_add(1);
			reads.fetch_add(1, std::memory_order_relaxed);
		}
	});
	// Gated on the reader, never on how the host schedules it: cycles start once it has read, and go on (at least
	// 20,000) until it has read 1,000 times more while they publish. A deadline only fails the test.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	const auto overdue = [&] { return std::chrono::steady_clock::now() > deadline; };
	while (reads.load(std::memory_order_relaxed) == 0 && !overdue()) std::this_thread::yield();
	const std::uint64_t readsBefore = reads.load(std::memory_order_relaxed);
	Bank bank(1, 16);
	Clock clock;
	std::uint64_t position = 0;
	bool late = readsBefore == 0;
	for (int round = 0; !late && (round < 20'000 || reads.load(std::memory_order_relaxed) - readsBefore < 1'000);
	     ++round) {
		ring.offer(stream(position, 16), 16);
		ring.play(bank.view(), clock.next(16));
		position += 16;
		if (round % 1024 == 0) late = overdue();
	}
	done.store(true, std::memory_order_release);
	reader.join();
	INFO("the reader read " << readsBefore << " times before the cycles and " << reads.load() - readsBefore
	                        << " while they published");
	REQUIRE_FALSE(late);
	CHECK(torn.load() == 0);
	CHECK(ring.played().totalFrames == position);
}
