// A sendspin node's audio code, cycle by cycle, with a real feed and real clocks (docs/design/sendspin.md section 7,
// section 14, S1 criterion 7).
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
	/// One cycle as the framework tells it: its start on the raw clock, and `downstreamFrames` downstream of both
	/// outputs (a following card's buffer).
	void cycle(std::uint64_t position, std::int64_t startRawNs = 1000, std::uint32_t downstreamFrames = 0) {
		const std::array<std::span<float>, 2> outputs{left, right};
		const std::array<std::span<const float>, 2> inputs{inLeft, inRight};
		const std::array<ear::Latency, 2> downstream{ear::Latency{downstreamFrames, downstreamFrames},
		                                             ear::Latency{downstreamFrames, downstreamFrames}};
		code.process(inputs, outputs, block,
		             ear::GraphCycle{.startNs = startRawNs - 7, // another clock: the code must not read it
		                             .position = position,
		                             .startRawNs = startRawNs,
		                             .downstream = downstream});
	}
	/// Whether the bell rang since the last look.
	bool rang() { return bell.drain(); }

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
	rig.cycle(0, start);

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
	plain.cycle(0, start);
	delayed.cycle(0, start, held);
	const std::int64_t difference = delayed.progress.read()->finishRawNs - plain.progress.read()->finishRawNs;
	CHECK(difference == std::int64_t{block + held} * 1000000000 / rate - std::int64_t{block} * 1000000000 / rate);
}

TEST_CASE("The lead a player reports covers what still plays after a clear, whatever the block", "[sendspin][code]") {
	CHECK(requiredLeadMs(2048, 256, 48000) == 48);   // 42.67 + 5.33 ms; the library's own 150 ms then rules
	CHECK(requiredLeadMs(8192, 4096, 48000) == 256); // past it: a block of 2049 to 4096 frames
	CHECK(requiredLeadMs(8193, 4096, 48000) == 257); // rounded up, never under
}

TEST_CASE("A position ahead by a block skips that block and counts it played", "[sendspin][code]") {
	Rig rig;
	rig.write(3 * block);
	rig.cycle(0);
	rig.cycle(2 * block); // the driver missed the cycle at `block`
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 3 * block);
	CHECK(rig.left[0] == static_cast<float>(2 * block + 1)); // that block's frames are gone
}

TEST_CASE("A position ahead by the feed's capacity still skips, and by more, or behind, skips none",
          "[sendspin][code]") {
	SECTION("just within") {
		Rig rig;
		rig.write(capacity);
		rig.cycle(0);
		rig.cycle(block + capacity);
		CHECK(rig.progress.read()->totalFrames == capacity); // what it held, all of it skipped
	}
	SECTION("further: a change of driver") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(0);
		rig.cycle(block + capacity + 1);
		CHECK(rig.progress.read()->totalFrames == 2 * block);
		CHECK(rig.left[0] == static_cast<float>(block + 1));
	}
	SECTION("behind: a change of driver") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(100000);
		rig.cycle(512);
		CHECK(rig.progress.read()->totalFrames == 2 * block);
		CHECK(rig.left[0] == static_cast<float>(block + 1));
	}
	SECTION("the first cycle knows no gap") {
		Rig rig;
		rig.write(3 * block);
		rig.cycle(block);
		CHECK(rig.progress.read()->totalFrames == block);
	}
}

TEST_CASE("A skip tells the jump even when the block that follows finds nothing", "[sendspin][code]") {
	Rig rig;
	rig.write(2 * block);
	rig.cycle(0);
	REQUIRE(rig.rang());
	rig.cycle(2 * block);
	CHECK(rig.progress.read()->totalFrames == 2 * block);
	CHECK(rig.rang());
	CHECK(rig.left[0] == 0.0F);
}

TEST_CASE("Nothing read rings nothing, and plays zeros", "[sendspin][code]") {
	Rig rig;
	rig.left[0] = 9.0F;
	rig.right[0] = 9.0F;
	rig.cycle(0);
	CHECK_FALSE(rig.rang());
	CHECK_FALSE(rig.progress.read().has_value());
	CHECK(rig.left[0] == 0.0F);
	CHECK(rig.right[0] == 0.0F);
}

TEST_CASE("Cleared frames are never reported", "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	rig.cycle(0);
	REQUIRE(rig.progress.read()->totalFrames == block);
	REQUIRE(rig.rang());

	rig.write(2 * block);
	rig.feed.clear();
	rig.cycle(block);
	CHECK(rig.progress.read()->totalFrames == block);
	CHECK_FALSE(rig.rang());
	CHECK(rig.left[0] == 0.0F);

	// A gap right after a clear skips nothing it did not hold.
	rig.cycle(3 * block);
	CHECK(rig.progress.read()->totalFrames == block);
	CHECK_FALSE(rig.rang());
}

TEST_CASE("Inputs go to the handoff whatever the feed holds, side for side", "[sendspin][code]") {
	Rig rig;
	rig.cycle(0);
	REQUIRE(rig.toServer.fromLeft.size() == block);
	CHECK(rig.toServer.fromLeft[0] == 0.25F);
	CHECK(rig.toServer.fromRight[block - 1] == -0.5F);
}

TEST_CASE("The capture anchor dates each cycle's first frame from the cycle's start on the raw clock",
          "[sendspin][code]") {
	Rig rig;
	const std::int64_t start = nowNs(CLOCK_MONOTONIC_RAW) - 5000000;
	rig.cycle(0, start);

	const auto first = rig.capture.read();
	REQUIRE(first.has_value());
	CHECK(first->framesWritten == 0);
	CHECK(first->atRawNs == start);

	rig.cycle(block, start);
	CHECK(rig.capture.read()->framesWritten == block);
}

TEST_CASE("Frames the handoff refuses are not counted in the anchor, so what it took stays dated", "[sendspin][code]") {
	Rig rig;
	rig.toServer.room = block + 10;
	rig.cycle(0);
	rig.cycle(block);
	rig.cycle(2 * block);
	rig.cycle(3 * block);
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
