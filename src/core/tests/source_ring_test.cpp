// SourceRing, every rule a source plays by (docs/writing-a-node-type.md#sources), driven cycle by cycle on the test's
// thread: frames played exactly once faded in, keeping time across a forward jump and a new clock on any other, the
// fades at a clear, an underrun and a return sample for sample, the shortfall only while the writer is not idle, a stop's
// drop, Played and its dating, a closed ring, positions past 2^32, and Played read whole from another thread.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
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

/// A cycle at `position`, its start `position` ms on the raw clock (one frame per ms).
SourceCycle cycleAt(std::uint64_t position, std::uint32_t frames, Latency downstream = {}) {
	return SourceCycle{.position = position,
	                   .frames = frames,
	                   .startRawNs = static_cast<std::int64_t>(position) * kFrameNs,
	                   .discontinuity = false,
	                   .downstream = downstream};
}

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
	ring.play(bank.view(), cycleAt(0, 16));
	for (std::uint32_t i = 0; i < 16; ++i) {
		INFO("frame " << i);
		CHECK(bank.data[0][i] == faded(streamAt(i, 0), std::min(i, kFade)));
		CHECK(bank.data[1][i] == faded(streamAt(i, 1), std::min(i, kFade)));
	}
	CHECK(ring.played().totalFrames == 16);
	ring.play(bank.view(), cycleAt(16, 16));
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

TEST_CASE("A forward jump of at most the capacity is skipped and counted played, and any other jump is a new clock",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	ring.play(bank.view(), cycleAt(1000, 8));
	ring.play(bank.view(), cycleAt(1008, 8)); // contiguous
	CHECK(ring.played().totalFrames == 16);
	// 20 frames lost: skipped, counted played, so the stream keeps to the clock.
	ring.play(bank.view(), cycleAt(1036, 8));
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(36 + i));
	CHECK(ring.played().totalFrames == 44);
	CHECK(ring.played().totalFrames == 1036 + 8 - 1000);
	// Exactly the capacity ahead: still a skip, of the whole ring (the writer idle, so the dry cycle is no shortfall).
	REQUIRE(offer(ring, 64, 44) == 44);
	ring.markIdle();
	ring.play(bank.view(), cycleAt(1044 + 64, 8));
	CHECK(ring.played().totalFrames == 44 + 64);
	// More than the capacity ahead: a new clock, nothing skipped, the stream played on from where it was.
	REQUIRE(offer(ring, 108, 40) == 40);
	ring.play(bank.view(), cycleAt(1116 + 65, 8));
	CHECK(ring.played().totalFrames == 116);
	// Back: a new clock too.
	ring.play(bank.view(), cycleAt(10, 8));
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(116 + i));
	CHECK(ring.played().totalFrames == 124);
	CHECK(ring.shortfallFrames() == 0);
}

TEST_CASE("Positions past 2^32 keep time as any other", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 64) == 64);
	Bank bank(1, 8);
	const std::uint64_t first = (std::uint64_t{1} << 32U) - 4;
	ring.play(bank.view(), cycleAt(first, 8));
	ring.play(bank.view(), cycleAt(first + 8, 8)); // straddles nothing, contiguous past 2^32
	ring.play(bank.view(), cycleAt(first + 16 + 24, 8));
	CHECK(ring.played().totalFrames == 48);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(40 + i));
	// A jump of 2^32 + 8 is a new clock, not a skip of 8.
	ring.play(bank.view(), cycleAt(first + 48 + (std::uint64_t{1} << 32U) + 8, 8));
	CHECK(ring.played().totalFrames == 56);
	for (std::uint32_t i = 0; i < 8; ++i) CHECK(bank.data[0][i] == streamAt(48 + i));
}

TEST_CASE("A clear plays the next 5 ms faded to zero, counts them played, drops the rest, and the next audio fades in",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	ring.play(bank.view(), cycleAt(0, 16)); // faded in, now full
	ring.clear();
	CHECK(offer(ring, 1000, 8) == 8); // written after the clear: the new stream
	ring.play(bank.view(), cycleAt(16, 16));
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
	ring.play(bank.view(), cycleAt(0, 16));
	ring.clear(); // 2 frames left
	REQUIRE(offer(ring, 500, 20) == 20);
	ring.play(bank.view(), cycleAt(16, 16));
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
	ring.play(bank.view(), cycleAt(0, 16));
	ring.play(bank.view(), cycleAt(16, 16)); // 4 frames, then dry
	for (std::uint32_t i = 0; i < 4; ++i) CHECK(bank.data[1][i] == streamAt(16 + i, 1));
	for (std::uint32_t i = 0; i < kFade; ++i) {
		CHECK(bank.data[0][4 + i] == faded(streamAt(19, 0), kFade - i));
		CHECK(bank.data[1][4 + i] == faded(streamAt(19, 1), kFade - i));
	}
	for (std::uint32_t i = 4 + kFade; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 12);
	CHECK(ring.played().totalFrames == 20);
	const int published = signals.played;
	ring.play(bank.view(), cycleAt(32, 16)); // all dry: nothing played, nothing published
	CHECK(ring.shortfallFrames() == 28);
	CHECK(signals.played == published);
	// Back: from zero, over 5 ms.
	REQUIRE(offer(ring, 20, 16, 2) == 16);
	ring.play(bank.view(), cycleAt(48, 16));
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == faded(streamAt(20 + i), std::min(i, kFade)));
	CHECK(ring.played().totalFrames == 36);
	// A short gap decays partly and the return fades in from where the decay got to.
	REQUIRE(offer(ring, 36, 14, 2) == 14);
	ring.play(bank.view(), cycleAt(64, 16)); // 14 frames, 2 decaying (5, then 4)
	CHECK(bank.data[0][14] == faded(streamAt(49), 5));
	CHECK(bank.data[0][15] == faded(streamAt(49), 4));
	REQUIRE(offer(ring, 50, 16, 2) == 16);
	ring.play(bank.view(), cycleAt(80, 16));
	CHECK(bank.data[0][0] == faded(streamAt(50), 3));
	CHECK(bank.data[0][1] == faded(streamAt(51), 4));
	CHECK(bank.data[0][2] == streamAt(52));
}

TEST_CASE("A source's shortfall counts only while its writer is not idle", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	Bank bank(1, 16);
	ring.play(bank.view(), cycleAt(0, 16)); // never written: idle
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	REQUIRE(offer(ring, 0, 8) == 8);
	ring.play(bank.view(), cycleAt(16, 16));
	CHECK(ring.shortfallFrames() == 8);
	ring.markIdle();
	ring.play(bank.view(), cycleAt(32, 16));
	CHECK(ring.shortfallFrames() == 8);
	CHECK(offer(ring, 8, 0) == 0); // no frames: still idle
	ring.play(bank.view(), cycleAt(48, 16));
	CHECK(ring.shortfallFrames() == 8);
	REQUIRE(offer(ring, 8, 4) == 4); // a write ends the idle
	ring.play(bank.view(), cycleAt(64, 16));
	CHECK(ring.shortfallFrames() == 20);
}

TEST_CASE("A stop drops and counts what the ring holds, plays nothing of it, refuses writes until the next cycle and "
          "keeps time afresh",
          "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	REQUIRE(offer(ring, 0, 40) == 40);
	Bank bank(1, 16);
	ring.play(bank.view(), cycleAt(0, 16));
	ring.dropForStop();
	CHECK(ring.droppedFrames() == 24);
	CHECK(offer(ring, 100, 8) == 0); // stopped: waits as at a full ring
	const int room = signals.room;
	// The first cycle after the next drive: silent (the decay of the last frame aside: none, the fade was reset), no
	// shortfall, and the writer let go; a jump in position is no skip.
	ring.play(bank.view(), cycleAt(5000, 16));
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(bank.data[0][i] == 0.0F);
	CHECK(ring.shortfallFrames() == 0);
	CHECK(signals.room == room + 1);
	CHECK(ring.played().totalFrames == 16);
	REQUIRE(offer(ring, 100, 16) == 16);
	ring.play(bank.view(), cycleAt(5016 + 30, 16)); // the time kept from the cycle above: 30 frames skipped
	CHECK(ring.played().totalFrames == 16 + 16);    // only what the ring held could be skipped: all of it
	CHECK(ring.droppedFrames() == 24);
}

TEST_CASE("Played is dated by the cycle's start, the frames played and the most latency downstream", "[source_ring]") {
	CountingSignals signals;
	SourceRing ring(1, 64, kRate, signals);
	CHECK(ring.played() == Played{});
	REQUIRE(offer(ring, 0, 24) == 24);
	Bank bank(1, 16);
	ring.play(bank.view(), cycleAt(100, 16, Latency{40, 100}));
	CHECK(ring.played() == Played{16, (100 + 16 + 100) * kFrameNs});
	ring.play(bank.view(), cycleAt(116, 16, Latency{0, 7})); // 8 played, then dry
	CHECK(ring.played() == Played{24, (116 + 8 + 7) * kFrameNs});
	// A cycle that only skipped is dated at its start.
	REQUIRE(offer(ring, 24, 4) == 4);
	ring.markIdle();
	ring.play(bank.view(), cycleAt(116 + 16 + 4, 16));
	CHECK(ring.played() == Played{28, (136 + 0) * kFrameNs});
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
	ring.play(bank.view(), cycleAt(0, 16));
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
	Bank bank(1, 16);
	std::uint64_t position = 0;
	for (int round = 0; round < 20'000; ++round) {
		ring.offer(stream(position, 16), 16);
		ring.play(bank.view(), cycleAt(position, 16));
		position += 16;
	}
	done.store(true, std::memory_order_release);
	reader.join();
	CHECK(reads.load() > 0);
	CHECK(torn.load() == 0);
	CHECK(ring.played().totalFrames == position);
}
