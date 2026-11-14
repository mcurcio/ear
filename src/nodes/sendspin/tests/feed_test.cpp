// The feed across real threads: exact audio, zeros past the end, a full feed's wait, clear, and no allocation on the
// reading side (docs/node-types.md#sendspin).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "feed.hpp"

using ear::nodes::sendspin::Feed;
using namespace std::chrono_literals;

// Counts allocations made by a thread that asks for it (read() must make none). Replacing the global operator new applies
// to this whole test binary; it only forwards to malloc.
namespace {
thread_local bool countHere = false;
std::atomic<std::uint64_t> counted{0};

void* allocate(std::size_t size) {
	if (countHere) counted.fetch_add(1, std::memory_order_relaxed);
	if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
	throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

constexpr std::uint32_t capacity = 2048;

/// The left channel carries the frame's index below 2^24 exactly; the right, its negative.
void fill(std::vector<float>& interleaved, std::uint32_t from, std::uint32_t frames) {
	interleaved.resize(std::size_t{frames} * 2);
	for (std::uint32_t i = 0; i < frames; ++i) {
		interleaved[std::size_t{i} * 2] = static_cast<float>(from + i);
		interleaved[std::size_t{i} * 2 + 1] = -static_cast<float>(from + i);
	}
}

/// Writes frames [0, total) in chunks of `chunk`, waiting when full.
void writeRamp(Feed& feed, std::uint32_t total, std::uint32_t chunk) {
	std::vector<float> samples;
	for (std::uint32_t at = 0; at < total;) {
		const std::uint32_t frames = std::min(chunk, total - at);
		fill(samples, at, frames);
		at += feed.write(samples, frames, 20ms);
	}
}

/// Once the writer has begun its wait on the room, past its second look at the ring, so only a wake (or its patience) can
/// end it: the writer's own event (docs/testing.md#a-writers-own-event), never a guess that it has got there.
bool writerIsInItsWait(const Feed& feed) {
	const auto until = std::chrono::steady_clock::now() + 10s;
	while (feed.waitsBegun() == 0) {
		if (std::chrono::steady_clock::now() >= until) return false;
		std::this_thread::yield();
	}
	return true;
}

} // namespace

TEST_CASE("A ramp of 10 s written by one thread is read back exact by another", "[feed]") {
	constexpr std::uint32_t total = 480000;
	constexpr std::uint32_t block = 256;
	Feed feed(2, capacity);
	std::thread writer([&] { writeRamp(feed, total, 1021); });

	std::array<float, block> left{};
	std::array<float, block> right{};
	const std::array<std::span<float>, 2> out{left, right};
	std::uint32_t next = 0;
	std::uint64_t wrong = 0;
	const auto until = std::chrono::steady_clock::now() + 30s;
	while (next < total && std::chrono::steady_clock::now() < until) {
		const std::uint32_t got = feed.read(out, block);
		for (std::uint32_t i = 0; i < got; ++i) {
			if (left[i] != static_cast<float>(next) || right[i] != -static_cast<float>(next)) ++wrong;
			++next;
		}
		// The rest of each block is zeros, whatever the last read held.
		for (std::uint32_t i = got; i < block; ++i) {
			if (left[i] != 0.0F || right[i] != 0.0F) ++wrong;
		}
		if (got == 0) std::this_thread::yield();
	}
	writer.join();
	CHECK(next == total);
	CHECK(wrong == 0);
}

TEST_CASE("Reading past the end fills the rest with zeros", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 1, 3);
	REQUIRE(feed.write(samples, 3, 0ms) == 3);
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	left.fill(9.0F);
	right.fill(9.0F);
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 8) == 3);
	CHECK(left == std::array<float, 8>{1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
	CHECK(right == std::array<float, 8>{-1.0F, -2.0F, -3.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
}

TEST_CASE(
	"The feed counts each read it could not fill and the frames it lacked, and a whole read or a skip counts nothing",
	"[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 1, 8);
	REQUIRE(feed.write(samples, 8, 0ms) == 8);
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 3) == 3); // whole
	CHECK(feed.dryReads() == 0);
	CHECK(feed.dryFrames() == 0);
	CHECK(feed.skip(2) == 2); // a skip is no read
	CHECK(feed.dryReads() == 0);
	CHECK(feed.read(out, 8) == 3); // short by 5
	CHECK(feed.dryReads() == 1);
	CHECK(feed.dryFrames() == 5);
	CHECK(feed.read(out, 4) == 0); // empty: short by all 4
	CHECK(feed.dryReads() == 2);
	CHECK(feed.dryFrames() == 9);
	CHECK(feed.read(out, 0) == 0); // asked nothing, lacked nothing
	CHECK(feed.dryReads() == 2);
}

TEST_CASE("A full feed's write returns 0 at its patience", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity);
	const auto before = std::chrono::steady_clock::now();
	CHECK(feed.write(samples, 16, 20ms) == 0);
	CHECK(std::chrono::steady_clock::now() - before >= 20ms);
	CHECK(feed.waitsWoken() == 0); // its patience ended every wait it began
	CHECK(feed.write(samples, 16, 0ms) == 0);
}

TEST_CASE("A full feed's write wakes on a read and takes what fits", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity);

	std::atomic<std::uint32_t> accepted{0};
	const auto patience = 20s;
	std::thread writer([&] {
		std::vector<float> more;
		fill(more, capacity, 512);
		accepted = feed.write(more, 512, patience);
	});
	REQUIRE(writerIsInItsWait(feed));
	std::array<float, 256> left{};
	std::array<float, 256> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 256) == 256);
	writer.join();
	CHECK(accepted == 256);
	CHECK(feed.waitsBegun() == 1);
	CHECK(feed.waitsWoken() == 1); // the wake ended it, not its patience
}

TEST_CASE("After clear the next read returns nothing, and what is written after it plays", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 1, 100);
	REQUIRE(feed.write(samples, 100, 0ms) == 100);
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};

	CHECK(feed.read(out, 4) == 4);
	feed.clear();
	// What arrives between the clear and the next read belongs to the next stream.
	fill(samples, 500, 2);
	REQUIRE(feed.write(samples, 2, 0ms) == 2);
	CHECK(feed.read(out, 8) == 2);
	CHECK(left[0] == 500.0F);
	CHECK(left[2] == 0.0F);

	feed.clear();
	CHECK(feed.read(out, 8) == 0);
	CHECK(left[0] == 0.0F);
}

TEST_CASE("A clear frees room for a writer that waits", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity);
	std::atomic<std::uint32_t> accepted{0};
	const auto patience = 20s;
	std::thread writer([&] { accepted = feed.write(samples, 16, patience); });
	REQUIRE(writerIsInItsWait(feed));
	feed.clear();
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 8) == 0);
	writer.join();
	CHECK(accepted == 16);
	CHECK(feed.waitsBegun() == 1);
	CHECK(feed.waitsWoken() == 1); // the read that applied the clear woke it, not its patience
}

TEST_CASE("skip discards the oldest frames and says how many, and what it held", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, 10);
	REQUIRE(feed.write(samples, 10, 0ms) == 10);
	CHECK(feed.skip(4) == 4);
	CHECK(feed.heldAtSkip() == 10);
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 2) == 2);
	CHECK(left[0] == 4.0F);
	CHECK(feed.skip(100) == 4);
	CHECK(feed.heldAtSkip() == 4);
	CHECK(feed.skip(100) == 0);
	CHECK(feed.heldAtSkip() == 0);
	// A count past 2^32 (a catch-up's jump has no bound) skips what it holds, never a wrapped few.
	REQUIRE(feed.write(samples, 10, 0ms) == 10);
	CHECK(feed.skip((std::uint64_t{1} << 32U) + 2) == 10);
	CHECK(feed.read(out, 2) == 0);
}

TEST_CASE("takeCleared applies a clear asked for and says whether one was applied since, here or by a read", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, 10);
	REQUIRE(feed.write(samples, 10, 0ms) == 10);
	CHECK_FALSE(feed.takeCleared());
	feed.clear();
	CHECK(feed.takeCleared()); // applied here
	CHECK_FALSE(feed.takeCleared());
	CHECK(feed.skip(10) == 0); // what was written before it is gone
	REQUIRE(feed.write(samples, 10, 0ms) == 10);
	feed.clear();
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 8) == 0); // applied by the read
	CHECK(feed.takeCleared());
	CHECK_FALSE(feed.takeCleared());
}

TEST_CASE("read and skip allocate nothing", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity);
	std::array<float, 256> left{};
	std::array<float, 256> right{};
	const std::array<std::span<float>, 2> out{left, right};
	std::uint64_t allocations = ~std::uint64_t{0};
	std::thread reader([&] {
		countHere = true;
		const std::uint64_t before = counted.load();
		for (int i = 0; i < 16; ++i) {
			feed.read(out, 256);
			feed.skip(16);
			if (i == 8) feed.clear();
		}
		allocations = counted.load() - before;
		countHere = false;
	});
	reader.join();
	CHECK(allocations == 0);
}

// The undrained rule (docs/node-types.md#clock), on a clock the test moves: the writer is held back by a reader that
// stopped only for the bound, then takes what it is given at the graph's rate, up to now, and drops it.
namespace {

std::int64_t fakeNow = 0;
std::int64_t fakeRawNow() noexcept { return fakeNow; }

constexpr std::int64_t msNs = 1'000'000;
constexpr std::uint32_t kRate = 48000;

ear::nodes::sendspin::FeedDrain fakeDrain() {
	return {.rate = ear::SampleRate::fixed<kRate>(), .bound = 1500ms, .nowRawNs = &fakeRawNow};
}

} // namespace

TEST_CASE("Undrained past its bound the feed drops what it is given at the graph's rate, never ahead of now, and marks "
          "what it holds stale",
          "[feed]") {
	fakeNow = 10'000 * msNs;
	Feed feed(2, capacity, fakeDrain());
	feed.drained(fakeNow); // the reader's stamp
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity); // full: the reader keeps up, so far
	CHECK(feed.write(samples, 100, 0ms) == 0);               // full and drained 0 ms ago: it waits, as ever
	CHECK_FALSE(feed.skipStale());

	fakeNow += 1500 * msNs; // at the bound: still held back
	CHECK(feed.write(samples, 100, 0ms) == 0);
	fakeNow += 1; // past it: what the ring holds is dropped now, and what comes from now on
	CHECK(feed.write(samples, capacity, 0ms) == 0);
	CHECK(feed.dropped().total == capacity); // the ring's, consumed as the writer began
	CHECK(feed.dropped().finishRawNs == fakeNow);
	fakeNow += 10 * msNs; // 480 frames of the graph's time
	CHECK(feed.write(samples, capacity, 0ms) == 480);
	CHECK(feed.write(samples, capacity, 0ms) == 0); // up to now and no further
	CHECK(feed.held() == capacity);                 // the ring untouched until the reader skips it
	CHECK(feed.dropped().total == capacity + 480);
	CHECK(feed.dropped().finishRawNs == fakeNow); // when the frame after them would play: now, no later
	fakeNow += 1000 * msNs; // a writer away for a second is credited with a feed's worth of it, no more
	CHECK(feed.write(samples, capacity * 2, 0ms) == capacity - 1); // the 2048th frame's time not yet whole
	CHECK(feed.dropped().finishRawNs <= fakeNow);

	const auto stale = feed.skipStale(); // the reader is back: it skips what the writer counted, none of it read
	REQUIRE(stale.has_value());
	CHECK(stale->skipped == capacity);
	CHECK(stale->readSince == 0);
	CHECK_FALSE(feed.skipStale());
	feed.drained(fakeNow); // writes wait on the ring again
	CHECK(feed.held() == 0);
	CHECK(feed.write(samples, 100, 0ms) == 100);
	CHECK(feed.held() == 100);
	CHECK(feed.dropped().total == capacity + 480 + capacity - 1);
}

TEST_CASE("A feed counts as drained when it is made, so a writer before the first cycle waits rather than drops",
          "[feed]") {
	fakeNow = 20'000 * msNs;
	Feed feed(2, capacity, fakeDrain()); // made now: drained now
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity); // buffered for the first cycle
	CHECK(feed.write(samples, 1, 0ms) == 0);                 // full: waits, as for a reader's stall
	fakeNow += 1500 * msNs;
	CHECK(feed.write(samples, 1, 0ms) == 0);
	CHECK(feed.dropped().total == 0);
	CHECK_FALSE(feed.skipStale());
	fakeNow += 1 + 10 * msNs; // no first cycle within the bound: dropping, as after any reader gone that long
	CHECK(feed.write(samples, 1, 0ms) == 0);
	fakeNow += msNs;
	CHECK(feed.write(samples, capacity, 0ms) == 48);
	CHECK(feed.dropped().total == capacity + 48);
	CHECK(feed.skipStale().has_value());
}

TEST_CASE("What a reader read in the instant the writer began dropping is said, so it is not counted twice", "[feed]") {
	fakeNow = 30'000 * msNs;
	Feed feed(2, capacity, fakeDrain());
	std::vector<float> samples;
	fill(samples, 0, capacity);
	REQUIRE(feed.write(samples, capacity, 0ms) == capacity);
	fakeNow += 1600 * msNs;
	CHECK(feed.write(samples, 1, 0ms) == 0); // the writer begins: the 2048 held counted dropped
	std::vector<float> left(256);
	std::vector<float> right(256);
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 256) == 256); // the reader, back that instant, read 256 of them
	const auto stale = feed.skipStale();
	REQUIRE(stale.has_value());
	CHECK(stale->readSince == 256);
	CHECK(stale->skipped == capacity - 256);
	CHECK(feed.held() == 0);
}

TEST_CASE("A clear forgets the frames dropped so far, and a feed with no rule never drops", "[feed]") {
	fakeNow = 10'000 * msNs;
	Feed feed(2, capacity, fakeDrain());
	std::vector<float> samples;
	fill(samples, 0, 256);
	fakeNow += 2000 * msNs; // never drained since it was made
	CHECK(feed.write(samples, 256, 0ms) == 0);
	fakeNow += 10 * msNs;
	CHECK(feed.write(samples, 256, 0ms) == 256);
	CHECK(feed.held() == 0);
	feed.clear();
	CHECK(feed.dropped().forgotten == 256); // the library forgets them at a stream's end, so they are never told

	Feed plain(2, capacity);
	fill(samples, 0, capacity);
	REQUIRE(plain.write(samples, capacity, 0ms) == capacity);
	CHECK(plain.write(samples, 1, 0ms) == 0);
	CHECK(plain.dropped().total == 0);
}
