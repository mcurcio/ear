// SourceRing, every rule a source plays by (docs/writing-a-node-type.md#sources), driven tick by tick on the test's
// thread, each tick's facts stated by the one rule (CycleAccount over a CutRing) as the engine states them: frames
// played exactly once faded in; no skip without a clock, whatever the tick states (a catch-up past 2^32 frames
// included); a clock's skip honoured exactly and capped at what the ring holds, nothing skipped on a tick that applies
// a clear; what the clock is told (SourcePlayed) on every shape: a clear, an underrun, a return, a stop; the clock wake
// signalled only when the clock asks; the fades at a clear, an underrun and a return sample for sample; the shortfall
// only while the writer is not idle; a stop's drop; a closed ring.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
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
	int asked = 0;
	void roomFreed() noexcept override { ++room; }
	void clockAsked() noexcept override { ++asked; }
};

/// A source's clock for the test: answers each tick's skipBefore with `skip` (then resets it to 0, so one stall's skip
/// is asked once) and each played with `ask`, and records what each call was given.
struct TestClock final : SourceClock {
	struct Heard {
		std::uint64_t position;
		CycleFacts facts;
		std::uint32_t frames;
		SampleRate rate;
		Latency playback;
		SourceHeld held;
		SourcePlayed played;
	};
	std::uint64_t skip = 0;
	bool ask = false;
	std::vector<Heard> heard;
	std::uint64_t skipBefore(const SourceTick& tick, SourceHeld held) noexcept override {
		heard.push_back(Heard{tick.cycle.position, tick.cycle.facts, tick.frames, tick.rate, tick.playback, held, {}});
		return std::exchange(skip, 0);
	}
	bool played(const SourceTick& tick, const SourcePlayed& played) noexcept override {
		// Told after the same tick it was asked about, with the same tick.
		if (heard.empty() || heard.back().position != tick.cycle.position)
			heard.push_back(Heard{0, {}, 0, tick.rate, {}, {}, {}});
		heard.back().played = played;
		return ask;
	}
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

/// One bank of `channels` spans, NaN-filled before each tick so a frame left unwritten shows.
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
/// CutRing, each cycle's facts by CycleAccount, so the ring is played with the facts the engine would hand its clock. A
/// cycle starts `position` ms on the raw clock (one frame per ms).
class Clock {
public:
	explicit Clock(std::uint64_t position = 0) : position_(position) {}
	/// The next cycle the source plays, of `frames`.
	GraphCycle next(std::uint32_t frames) {
		cuts_.start();
		const std::int64_t at = static_cast<std::int64_t>(position_) * kFrameNs;
		const CycleFacts facts = account_
		                             .next(DriverTick{.driverId = driver_,
		                                              .graphCycle = cuts_.started(),
		                                              .position = position_,
		                                              .frames = frames,
		                                              .startRawNs = at},
		                                   frames)
		                             .facts;
		const GraphCycle cycle{.startNs = at, .position = position_, .startRawNs = at, .facts = facts};
		position_ += frames;
		return cycle;
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
	/// The next cycle's position moves by `frames` with no cycle missed: reposition.
	void reposition(std::int64_t frames) { position_ += static_cast<std::uint64_t>(frames); }
	/// The next cycle runs on another driver: driverChange.
	void changeDriver() { ++driver_; }

private:
	CutRing cuts_;
	CycleAccount account_{FrameClock(ear::SampleRate::fixed<kRate>()), &cuts_};
	std::uint32_t driver_ = 1;
	std::uint64_t position_;
};

/// One tick of `cycle`, as the block plays it (its frames, at kRate), with `clock` (none by default).
SourcePlayed play(SourceRing& ring, Bank& bank, const GraphCycle& cycle, std::uint32_t frames,
                  SourceClock* clock = nullptr, Latency playback = {}) {
	return ring.play(
		bank.view(),
		SourceTick{.cycle = cycle, .frames = frames, .rate = ear::SampleRate::fixed<kRate>(), .playback = playback},
		clock);
}

/// What the ring plays for `value` at fade level `level` (of kFade), as play() computes it.
float faded(float value, std::uint32_t level) {
	return value * (static_cast<float>(level) / static_cast<float>(kFade));
}

std::uint32_t offer(SourceRing& ring, std::uint64_t from, std::uint32_t count, std::uint32_t channels = 1) {
	return ring.offer(stream(from, count, channels), count);
}

} // namespace

TEST_CASE("A source plays its ring's frames in order, fading in over its first 5 ms, and says what it read",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(2, 64, ear::SampleRate::fixed<kRate>(), signals);
	CHECK(ring.capacityFrames() == 64);
	CHECK(offer(ring, 0, 40, 2) == 40);
	Bank bank(2, 16);
	Clock clock;
	CHECK(play(ring, bank, clock.next(16), 16) == SourcePlayed{.skipped = 0, .read = 16, .ended = 0, .zeros = 0});
	for (std::uint32_t i = 0; i < 16; ++i) {
		INFO("frame " << i);
		CHECK(bank.data[0][i] == faded(streamAt(i, 0), std::min(i, kFade)));
		CHECK(bank.data[1][i] == faded(streamAt(i, 1), std::min(i, kFade)));
	}
	CHECK(play(ring, bank, clock.next(16), 16) == SourcePlayed{.read = 16});
	for (std::uint32_t i = 0; i < 16; ++i) {
		CHECK(bank.data[0][i] == streamAt(16 + i, 0));
		CHECK(bank.data[1][i] == streamAt(16 + i, 1));
	}
	CHECK(signals.room == 2);
	CHECK(signals.asked == 0); // no clock: nothing to ask after
	CHECK(ring.shortfallFrames() == 0);
	CHECK(ring.droppedFrames() == 0);
	// A capacity that is no power of two is rounded up; a full ring takes what fits.
	CountingSignals other;
	SourceRing rounded(1, 100, ear::SampleRate::fixed<kRate>(), other);
	CHECK(rounded.capacityFrames() == 128);
	CHECK(offer(rounded, 0, 200) == 128);
	CHECK(offer(rounded, 128, 1) == 0);
}

// The ring keeps no audio time (docs/architecture.md#the-timing-model, rule 1): a source with no clock plays its ring
// on in order whatever the tick states, a catch-up of any length included (revert proof: SourceRing::play skipping a
// catch-up's jump with no clock fails every row).
TEST_CASE("A source with no clock skips nothing, whatever the tick states", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	Clock clock(1000);
	CHECK(play(ring, bank, clock.next(8), 8).read == 8);
	// 24 frames lost, three cycles cut: a catch-up, and the stream plays on from frame 8.
	clock.lose(3, 8);
	GraphCycle cycle = clock.next(8);
	REQUIRE(cycle.facts.causes == CycleCause::catchUp);
	REQUIRE(cycle.facts.jumpFrames == 24);
	CHECK(play(ring, bank, cycle, 8) == SourcePlayed{.read = 8});
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(8 + i));
	// A catch-up past 2^32 frames: the same.
	constexpr std::uint64_t jump = (std::uint64_t{1} << 32U) + 8;
	const GraphCycle huge{.startNs = 0,
	                      .position = cycle.position + 8 + jump,
	                      .facts = {.missed = static_cast<std::uint32_t>(jump / 8),
	                                .missedCut = static_cast<std::uint32_t>(jump / 8),
	                                .jumpFrames = static_cast<std::int64_t>(jump),
	                                .causes = CycleCause::catchUp}};
	CHECK(play(ring, bank, huge, 8) == SourcePlayed{.read = 8});
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(16 + i));
	// Every other cause, likewise.
	clock.reposition(-100);
	CHECK(play(ring, bank, cycle = clock.next(8), 8).read == 8);
	REQUIRE(cycle.facts.causes == CycleCause::reposition);
	clock.lose(2, 8, false); // a pause: missed, not cut
	CHECK(play(ring, bank, cycle = clock.next(8), 8).read == 8);
	REQUIRE(cycle.facts.causes == CycleCause::absent);
	clock.changeDriver();
	CHECK(play(ring, bank, cycle = clock.next(8), 8).read == 8);
	REQUIRE(cycle.facts.causes == CycleCause::driverChange);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(40 + i));
	CHECK(ring.droppedFrames() == 0);
	CHECK(ring.shortfallFrames() == 0);
	CHECK(signals.asked == 0);
}

// The one play rule's skip (docs/writing-a-node-type.md#a-sources-clock): the ring drops min(answer, held), of the
// current stream (revert proof: dropping the answer uncapped, or dropping nothing, fails it).
TEST_CASE("A clock's skip is honoured exactly and capped at what the ring holds, and the clock is told the tick",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	Clock clock(1000);
	CHECK(play(ring, bank, clock.next(8), 8, &time, Latency{3, 40}) == SourcePlayed{.read = 8});
	REQUIRE(time.heard.size() == 1);
	CHECK(time.heard[0].position == 1000);
	CHECK(time.heard[0].frames == 8);
	CHECK(time.heard[0].rate.hz() == kRate);
	CHECK(time.heard[0].playback == Latency{3, 40});
	CHECK(time.heard[0].held == SourceHeld{.frames = 64, .clearing = false}); // asked before the tick plays
	CHECK(time.heard[0].played == SourcePlayed{.read = 8});
	// Its answer, exactly: 13 of the 56 held dropped, the stream on from frame 21, the jump stated to it.
	clock.lose(2, 8);
	time.skip = 13;
	CHECK(play(ring, bank, clock.next(8), 8, &time) == SourcePlayed{.skipped = 13, .read = 8});
	CHECK(time.heard[1].facts.causes == CycleCause::catchUp);
	CHECK(time.heard[1].facts.jumpFrames == 16);
	CHECK(time.heard[1].held.frames == 56);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(21 + i));
	// On any tick it answers, whatever the facts say: a clock's own reckoning (its network fell behind).
	time.skip = 3;
	CHECK(play(ring, bank, clock.next(8), 8, &time) == SourcePlayed{.skipped = 3, .read = 8});
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(32 + i));
	// Capped at what is held: 24 held, 2^40 asked; the tick reads nothing and decays, counted as zeros.
	time.skip = std::uint64_t{1} << 40U;
	CHECK(play(ring, bank, clock.next(8), 8, &time) == SourcePlayed{.skipped = 24, .read = 0, .zeros = 8});
	for (std::uint32_t i = 0; i < kFade; ++i) CHECK(bank.data[0][i] == faded(streamAt(39), kFade - i));
	CHECK(ring.shortfallFrames() == 8); // the writer is not idle: the skip left it short
	CHECK(ring.droppedFrames() == 0);   // a skip is the clock's, told to it, not a drop
	// What comes next plays from its first frame, faded in.
	REQUIRE(offer(ring, 500, 8) == 8);
	CHECK(play(ring, bank, clock.next(8), 8, &time) == SourcePlayed{.read = 8});
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == faded(streamAt(500 + i), std::min(i, kFade)));
	CHECK(signals.asked == 0); // it never asked
}

// A clear ends the stream: nothing of the next is skipped for the old one's lost time (revert proof: honouring the
// answer on the clearing tick drops the new stream's first frames).
TEST_CASE("Nothing is skipped on a tick that applies a clear, and the clock is told it clears", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16, &time);
	ring.clear();
	REQUIRE(offer(ring, 1000, 20) == 20);
	clock.lose(3, 16);
	time.skip = 30;
	const SourcePlayed cleared = play(ring, bank, clock.next(16), 16, &time);
	CHECK(time.heard.back().held == SourceHeld{.frames = 20, .clearing = true});
	CHECK(cleared == SourcePlayed{.skipped = 0, .read = 11, .ended = kFade, .zeros = 0});
	for (std::uint32_t i = 0; i < kFade; ++i) CHECK(bank.data[0][i] == faded(streamAt(16 + i), kFade - i));
	for (std::uint32_t i = kFade; i < 16; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(1000 + i - kFade), std::min(i - kFade, kFade)));
	// The tick after, the new stream is the current one: a skip of it is honoured.
	time.skip = 4;
	CHECK(play(ring, bank, clock.next(4), 4, &time) == SourcePlayed{.skipped = 4, .read = 4});
	for (std::uint32_t i = 0; i < 4; ++i) CHECK(bank.data[0][i] == streamAt(1015 + i));
}

// The clock is told exactly what each tick did, and only its own true rings its wake (revert proof: signalling
// clockAsked after every tick, or never, fails it).
TEST_CASE("The clock wake is signalled only when the clock asks", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	Clock clock;
	for (int i = 0; i < 3; ++i) play(ring, bank, clock.next(8), 8, &time);
	CHECK(signals.asked == 0);
	time.ask = true;
	play(ring, bank, clock.next(8), 8, &time);
	CHECK(signals.asked == 1);
	time.ask = false;
	play(ring, bank, clock.next(8), 8, &time);
	CHECK(signals.asked == 1);
	time.ask = true;
	ring.markIdle();
	for (int i = 0; i < 8; ++i) play(ring, bank, clock.next(8), 8, &time); // dry ticks too: the clock decides
	CHECK(signals.asked == 9);
	CHECK(time.heard.size() == 13);
}

TEST_CASE("A clear plays the next 5 ms faded to zero, drops the rest, and the next audio fades in", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16, &time); // faded in, now full
	ring.clear();
	CHECK(offer(ring, 1000, 8) == 8); // written after the clear: the new stream
	// The ended stream's 5 ms, the new stream's 8, the decay from its last: what the clock is told.
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.read = 8, .ended = kFade, .zeros = 3});
	// The tail: frames 16..20 from full level down, one step a frame; then silence until the fade reached zero.
	for (std::uint32_t i = 0; i < kFade; ++i) CHECK(bank.data[0][i] == faded(streamAt(16 + i), kFade - i));
	// The fade is over: the new stream fades in from zero in the same tick.
	for (std::uint32_t i = kFade; i < kFade + 8; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(1000 + i - kFade), std::min(i - kFade, kFade)));
	for (std::uint32_t i = kFade + 8; i < 16; ++i) // dry: a decay from the new stream's last frame
		CHECK(bank.data[0][i] == faded(streamAt(1007), kFade - (i - kFade - 8)));
	CHECK(ring.droppedFrames() == 40 - 16 - kFade);
}

TEST_CASE("A clear with less than 5 ms written decays from its last frame to zero before the next audio",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	REQUIRE(offer(ring, 0, 18) == 18);
	Bank bank(1, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16);
	ring.clear(); // 2 frames left
	REQUIRE(offer(ring, 500, 20) == 20);
	// Two of the ended stream's, three of its decay (zeros: no stream's frames), then 11 of the new.
	CHECK(play(ring, bank, clock.next(16), 16) == SourcePlayed{.read = 11, .ended = 2, .zeros = 3});
	CHECK(bank.data[0][0] == faded(streamAt(16), 5));
	CHECK(bank.data[0][1] == faded(streamAt(17), 4));
	CHECK(bank.data[0][2] == faded(streamAt(17), 3)); // held, decaying
	CHECK(bank.data[0][3] == faded(streamAt(17), 2));
	CHECK(bank.data[0][4] == faded(streamAt(17), 1));
	for (std::uint32_t i = 5; i < 16; ++i)
		CHECK(bank.data[0][i] == faded(streamAt(500 + i - 5), std::min(i - 5, kFade)));
	CHECK(ring.shortfallFrames() == 0); // the clear's fade is no shortfall
}

TEST_CASE("At an underrun a source decays from its last frame over 5 ms and counts the shortfall, and fades in when "
          "audio returns",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(2, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 20, 2) == 20);
	Bank bank(2, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16, &time);
	// 4 frames, then dry: what the clock is told, the decay counted as zeros.
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.read = 4, .zeros = 12});
	for (std::uint32_t i = 0; i < 4; ++i) CHECK(bank.data[1][i] == streamAt(16 + i, 1));
	for (std::uint32_t i = 0; i < kFade; ++i) {
		CHECK(bank.data[0][4 + i] == faded(streamAt(19, 0), kFade - i));
		CHECK(bank.data[1][4 + i] == faded(streamAt(19, 1), kFade - i));
	}
	for (std::uint32_t i = 4 + kFade; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 12);
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.zeros = 16}); // all dry
	CHECK(ring.shortfallFrames() == 28);
	// Back: from zero, over 5 ms.
	REQUIRE(offer(ring, 20, 16, 2) == 16);
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.read = 16});
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(20 + i), std::min(i, kFade)));
	// A short gap decays partly and the return fades in from where the decay got to.
	REQUIRE(offer(ring, 36, 14, 2) == 14);
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.read = 14, .zeros = 2}); // decaying: 5, then 4
	CHECK(bank.data[0][14] == faded(streamAt(49), 5));
	CHECK(bank.data[0][15] == faded(streamAt(49), 4));
	REQUIRE(offer(ring, 50, 16, 2) == 16);
	play(ring, bank, clock.next(16), 16, &time);
	CHECK(bank.data[0][0] == faded(streamAt(50), 3));
	CHECK(bank.data[0][1] == faded(streamAt(51), 4));
	CHECK(bank.data[0][2] == streamAt(52));
}

TEST_CASE("A source's shortfall counts only while its writer is not idle", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	Bank bank(1, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16); // never written: idle
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	REQUIRE(offer(ring, 0, 8) == 8);
	play(ring, bank, clock.next(16), 16);
	CHECK(ring.shortfallFrames() == 8);
	ring.markIdle();
	play(ring, bank, clock.next(16), 16);
	CHECK(ring.shortfallFrames() == 8);
	CHECK(offer(ring, 8, 0) == 0); // no frames: still idle
	play(ring, bank, clock.next(16), 16);
	CHECK(ring.shortfallFrames() == 8);
	REQUIRE(offer(ring, 8, 4) == 4); // a write ends the idle
	play(ring, bank, clock.next(16), 16);
	CHECK(ring.shortfallFrames() == 20);
}

TEST_CASE("A stop drops and counts what the ring holds, plays nothing of it and refuses writes until the next tick",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	TestClock time;
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	Clock clock;
	play(ring, bank, clock.next(16), 16, &time);
	ring.dropForStop();
	CHECK(ring.droppedFrames() == 24);
	CHECK(offer(ring, 100, 8) == 0); // stopped: waits as at a full ring
	const int room = signals.room;
	// The first tick after the next drive: silent (the fade was reset), no shortfall, the writer let go; the clock is
	// told zeros, holds nothing to skip, and an answer is capped at that.
	clock.lose(3, 16);
	time.skip = 48;
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.zeros = 16});
	CHECK(time.heard.back().held == SourceHeld{});
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	CHECK(signals.room == room + 1);
	REQUIRE(offer(ring, 100, 16) == 16);
	CHECK(play(ring, bank, clock.next(16), 16, &time) == SourcePlayed{.read = 16});
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(100 + i), std::min(i, kFade)));
	CHECK(ring.droppedFrames() == 24);
}

TEST_CASE("A closed source accepts nothing and still plays what it holds", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, ear::SampleRate::fixed<kRate>(), signals);
	REQUIRE(offer(ring, 0, 16) == 16);
	CHECK_FALSE(ring.closed());
	ring.close();
	ring.close(); // idempotent
	CHECK(ring.closed());
	CHECK(offer(ring, 16, 8) == 0);
	Bank bank(1, 16);
	Clock clock;
	CHECK(play(ring, bank, clock.next(16), 16).read == 16);
}
