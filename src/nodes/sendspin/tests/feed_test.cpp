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

using ear::sendspin::Feed;
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

TEST_CASE("skip discards the oldest frames and says how many", "[feed]") {
	Feed feed(2, capacity);
	std::vector<float> samples;
	fill(samples, 0, 10);
	REQUIRE(feed.write(samples, 10, 0ms) == 10);
	CHECK(feed.skip(4) == 4);
	std::array<float, 8> left{};
	std::array<float, 8> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(feed.read(out, 2) == 2);
	CHECK(left[0] == 4.0F);
	CHECK(feed.skip(100) == 4);
	CHECK(feed.skip(100) == 0);
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
