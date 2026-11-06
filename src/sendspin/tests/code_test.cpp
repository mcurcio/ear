// A sendspin node's audio code, cycle by cycle, with a real feed and real clocks (docs/design/sendspin.md section 7,
// section 14, S1 criterion 7).
#include <array>
#include <cstdint>
#include <ctime>
#include <span>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "feed.hpp"
#include "sendspin_code.hpp"

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

struct Rig {
	Rig() : feed(2, capacity), bell(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)), code(feed, progress, bell, rate) {
		code.prepare({rate, block});
		left.resize(block);
		right.resize(block);
	}
	~Rig() { close(bell); }

	/// Frames whose left channel counts on from what was written before.
	void write(std::uint32_t frames) {
		std::vector<float> samples;
		for (std::uint32_t i = 0; i < frames; ++i) {
			samples.push_back(static_cast<float>(++written));
			samples.push_back(0.0F);
		}
		REQUIRE(feed.write(samples, frames, 0ms) == frames);
	}
	void cycle(std::uint64_t position, std::int64_t startNs = 1000) {
		const std::array<std::span<float>, 2> outputs{left, right};
		code.process({}, outputs, block, ear::GraphCycle{startNs, position});
	}
	/// Whether the bell rang since the last look.
	bool rang() {
		std::uint64_t rings = 0;
		return read(bell, &rings, sizeof rings) == sizeof rings;
	}

	Feed feed;
	Mailbox<PlayProgress> progress;
	int bell;
	SendspinCode code;
	std::vector<float> left;
	std::vector<float> right;
	std::uint32_t written = 0;
};

} // namespace

TEST_CASE("A block read counts its frames, and finishes at the cycle's start plus the frames' duration on the raw clock",
          "[sendspin][code]") {
	Rig rig;
	rig.write(block);
	const std::int64_t rawBefore = nowNs(CLOCK_MONOTONIC_RAW);
	const std::int64_t monoBefore = nowNs(CLOCK_MONOTONIC);
	const std::int64_t start = monoBefore - 5000000;   // a wake 5 ms late cancels out
	rig.cycle(0, start);
	const std::int64_t monoAfter = nowNs(CLOCK_MONOTONIC);
	const std::int64_t rawAfter = nowNs(CLOCK_MONOTONIC_RAW);

	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == block);
	const std::int64_t blockNs = std::int64_t{block} * 1000000000 / rate;
	// (raw - mono) measured around the call brackets the one the code took, within how long the call ran.
	const std::int64_t expected = start + (rawBefore - monoBefore) + blockNs;
	const std::int64_t tolerance = (monoAfter - monoBefore) + (rawAfter - rawBefore) + 1000000;
	INFO("finish " << progress->finishRawNs << " expected " << expected);
	CHECK(std::abs(progress->finishRawNs - expected) < tolerance);
	CHECK(rig.rang());
	CHECK(rig.left[0] == 1.0F);
	CHECK(rig.left[block - 1] == static_cast<float>(block));
}

TEST_CASE("A position ahead by a block skips that block and counts it played", "[sendspin][code]") {
	Rig rig;
	rig.write(3 * block);
	rig.cycle(0);
	rig.cycle(2 * block);   // the driver missed the cycle at `block`
	const auto progress = rig.progress.read();
	REQUIRE(progress.has_value());
	CHECK(progress->totalFrames == 3 * block);
	CHECK(rig.left[0] == static_cast<float>(2 * block + 1));   // that block's frames are gone
}

TEST_CASE("A position ahead by the feed's capacity still skips; by more, or behind, skips none", "[sendspin][code]") {
	SECTION("just within") {
		Rig rig;
		rig.write(capacity);
		rig.cycle(0);
		rig.cycle(block + capacity);
		CHECK(rig.progress.read()->totalFrames == capacity);   // what it held, all of it skipped
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
