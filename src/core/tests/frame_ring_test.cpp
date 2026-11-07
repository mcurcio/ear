// The ring of the handoff and the feed: its own arithmetic. The handoff's tests (real threads, real writer) and the
// feed's cover it across threads.
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/frame_ring.hpp"

using ear::FrameRing;

namespace {

/// Interleaved frames (i, -i) from `from`.
std::vector<float> frames(std::uint32_t from, std::uint32_t count) {
	std::vector<float> samples;
	for (std::uint32_t i = from; i < from + count; ++i) {
		samples.push_back(static_cast<float>(i));
		samples.push_back(-static_cast<float>(i));
	}
	return samples;
}

} // namespace

TEST_CASE("A ring's size is the power of two at or above what was asked", "[frame_ring]") {
	CHECK(FrameRing(2, 1920).capacityFrames() == 2048);
	CHECK(FrameRing(2, 2048).capacityFrames() == 2048);
	CHECK(FrameRing(1, 0).capacityFrames() == 1);
}

TEST_CASE("Frames written either way read back either way, across the ring's end", "[frame_ring]") {
	FrameRing ring(2, 8);
	// Six frames in and out, so the next write wraps.
	CHECK(ring.writeInterleaved(frames(0, 6), 6) == 6);
	CHECK(ring.skip(6) == 6);

	const std::vector<float> in = frames(100, 5);
	CHECK(ring.writeInterleaved(in, 5) == 5);
	const std::span<const float> run = ring.readSpan();
	CHECK(run.size() == 2 * 2); // only as far as the ring's end
	CHECK(run[0] == 100.0F);
	CHECK(ring.skip(2) == 2);

	std::array<float, 3> left{};
	std::array<float, 3> right{};
	const std::array<std::span<float>, 2> out{left, right};
	CHECK(ring.readPlanar(out, 3) == 3);
	CHECK(left == std::array<float, 3>{102.0F, 103.0F, 104.0F});
	CHECK(right == std::array<float, 3>{-102.0F, -103.0F, -104.0F});

	const std::array<float, 2> a{1.0F, 2.0F};
	const std::array<float, 2> b{3.0F, 4.0F};
	const std::array<std::span<const float>, 2> planar{a, b};
	CHECK(ring.writePlanar(planar, 2) == 2);
	CHECK(ring.readSpan().size() == 4);
	CHECK(ring.readSpan()[1] == 3.0F);
}

TEST_CASE("A full ring accepts what fits, and an empty one gives nothing", "[frame_ring]") {
	FrameRing ring(2, 4);
	CHECK(ring.writeInterleaved(frames(0, 6), 6) == 4);
	CHECK(ring.writeInterleaved(frames(0, 1), 1) == 0);
	CHECK(ring.skip(9) == 4);
	CHECK(ring.skip(1) == 0);
	CHECK(ring.readSpan().empty());
}

TEST_CASE("skipTo discards what came before a position, and never goes back", "[frame_ring]") {
	FrameRing ring(2, 8);
	ring.writeInterleaved(frames(0, 3), 3);
	const std::uint32_t position = ring.writePosition();
	ring.writeInterleaved(frames(3, 2), 2);
	ring.skipTo(position);
	CHECK(ring.readSpan()[0] == 3.0F);
	ring.skip(2);
	ring.skipTo(position); // read past it already
	CHECK(ring.readSpan().empty());
}
