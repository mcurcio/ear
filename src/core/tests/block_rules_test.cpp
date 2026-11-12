// The rules the engine's block and sinks share with the type harness, pure (docs/engine.md#port-blocks): cutChunk and
// SinkRing (docs/engine.md#sinks), FadeRamp (docs/engine.md#drive-and-fade) and the stamps (each cycle's facts:
// cycle_facts_test.cpp). ear_pipewire_tests runs the same through ThreadSink and a real block, and
// compares the harness's sink with ThreadSink.
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/block_rules.hpp"
#include "ear/core/sink_ring.hpp"

namespace {
using namespace ear;

/// One call a SinkRing's drain made, copied.
struct Call {
	std::vector<float> interleaved;
	std::uint32_t frames;
	ChunkStamp stamp;
};

std::vector<Call> drained(SinkRing& ring) {
	std::vector<Call> calls;
	ring.drain([&](const Chunk& chunk) {
		calls.push_back(Call{{chunk.interleaved.begin(), chunk.interleaved.end()},
		                     chunk.frames,
		                     ChunkStamp{.position = chunk.position,
		                                .startNs = chunk.startNs,
		                                .startRawNs = chunk.startRawNs,
		                                .discontinuity = chunk.discontinuity,
		                                .upstreamFrames = chunk.upstreamFrames}});
	});
	return calls;
}

/// One channel whose frame n is n, from `first`.
struct Counting {
	explicit Counting(std::uint32_t frames) : samples(frames), view{samples} {}
	std::span<const std::span<const float>> from(std::uint64_t first) {
		for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(first + i);
		return view;
	}
	std::vector<float> samples;
	std::array<std::span<const float>, 1> view;
};

ChunkStamp stampAt(std::uint64_t position, bool discontinuity) {
	return ChunkStamp{.position = position,
	                  .startNs = static_cast<std::int64_t>(position) * 1'000'000,
	                  .startRawNs = static_cast<std::int64_t>(position) * 1'000'000 + 7,
	                  .discontinuity = discontinuity,
	                  .upstreamFrames = 40};
}

} // namespace

TEST_CASE("cutChunk: one call when the chunk lies before the ring's end, else two, the second advanced by the "
          "first's frames and no discontinuity",
          "[block_rules]") {
	const FrameClock clock(1000); // a frame lasts 1 ms
	const ChunkStamp stamp = stampAt(500, true);
	struct Row {
		const char* what;
		std::uint32_t frames;
		std::uint32_t contiguous;
		std::uint32_t firstFrames;
		std::optional<std::uint32_t> secondFrames;
	};
	const std::vector<Row> rows{
		{"all before the end", 100, 256, 100, std::nullopt},
		{"exactly to the end", 100, 100, 100, std::nullopt},
		{"none before the end is no wrap", 100, 0, 100, std::nullopt},
		{"wraps after 60", 100, 60, 60, 40},
		{"wraps after one", 100, 1, 1, 99},
	};
	for (const Row& row : rows) {
		INFO(row.what);
		const ChunkCut cut = cutChunk(stamp, row.frames, row.contiguous, clock);
		CHECK(cut.first == stamp);
		CHECK(cut.firstFrames == row.firstFrames);
		REQUIRE(cut.second.has_value() == row.secondFrames.has_value());
		if (!cut.second) continue;
		CHECK(cut.secondFrames == *row.secondFrames);
		CHECK(cut.second->position == stamp.position + row.firstFrames);
		CHECK(cut.second->startNs == stamp.startNs + clock.ns(row.firstFrames));
		CHECK(cut.second->startRawNs == stamp.startRawNs + clock.ns(row.firstFrames));
		CHECK_FALSE(cut.second->discontinuity);
		CHECK(cut.second->upstreamFrames == stamp.upstreamFrames);
	}
}

TEST_CASE(
	"A SinkRing hands each stamped chunk in order, in two calls where it wraps, each dated by its own first frame",
	"[block_rules]") {
	SinkRing ring(1, 256, 1000);
	Counting signal(100);
	std::vector<Call> calls;
	for (std::uint64_t at = 0; at < 300; at += 100) {
		REQUIRE(ring.write(signal.from(at), 100, stampAt(at, at == 0)));
		for (Call& call : drained(ring)) calls.push_back(std::move(call));
	}
	REQUIRE(calls.size() == 4); // 100, 100, then 56 to the end and 44 from its start
	const std::vector<std::uint32_t> frames{100, 100, 56, 44};
	std::uint64_t next = 0;
	for (std::size_t i = 0; i < calls.size(); ++i) {
		INFO("call " << i);
		CHECK(calls[i].frames == frames[i]);
		CHECK(calls[i].stamp.position == next);
		CHECK(calls[i].stamp.startNs == static_cast<std::int64_t>(next) * 1'000'000);
		CHECK(calls[i].stamp.discontinuity == (i == 0));
		for (std::uint32_t f = 0; f < calls[i].frames; ++f)
			REQUIRE(calls[i].interleaved[f] == static_cast<float>(next + f));
		next += calls[i].frames;
	}
	CHECK(ring.droppedFrames() == 0);
}

TEST_CASE(
	"A SinkRing's two rings drop together: a full stamp ring drops the whole chunk, a full frame ring the rest of "
	"one, and a discard skips what no consumer can take",
	"[block_rules]") {
	SinkRing stamps(1, 256, 1000); // 64 stamps: 1-frame chunks run them out first
	Counting one(1);
	for (std::uint64_t at = 0; at < 64; ++at) REQUIRE(stamps.write(one.from(at), 1, stampAt(at, false)));
	CHECK_FALSE(stamps.write(one.from(64), 1, stampAt(64, false)));
	CHECK(stamps.droppedFrames() == 1);
	CHECK(drained(stamps).size() == 64);

	SinkRing frames(1, 256, 1000);
	Counting hundred(100);
	REQUIRE(frames.write(hundred.from(0), 100, stampAt(0, false)));
	REQUIRE(frames.write(hundred.from(100), 100, stampAt(100, false)));
	REQUIRE(frames.write(hundred.from(200), 100, stampAt(200, false))); // 56 taken, 44 dropped
	CHECK_FALSE(frames.write(hundred.from(300), 100, stampAt(300, false)));
	CHECK(frames.droppedFrames() == 144);
	const std::vector<Call> calls = drained(frames);
	REQUIRE(calls.size() == 3);
	CHECK(calls.back().frames == 56); // the chunk cut short keeps its stamp and only the frames it took
	CHECK(calls.back().stamp.position == 200);

	REQUIRE(frames.write(hundred.from(400), 100, stampAt(400, false)));
	frames.discard();
	CHECK(drained(frames).empty());
}

TEST_CASE("FadeRamp: in from zero over its frames after a drive, out from the gain it has, ending at zero",
          "[block_rules]") {
	FadeRamp ramp(1000); // 5 ms: 5 frames
	REQUIRE(ramp.frames() == 5);
	std::vector<float> faded(8, 1.0F);
	std::vector<float> kept(8, 1.0F);
	const std::array<std::span<float>, 2> outputs{faded, kept};
	const std::array<std::uint8_t, 2> mask{1, 0};

	ramp.driven(true);
	FadeRamp::Applied applied = ramp.apply(outputs, mask, 3);
	CHECK(faded[0] == 0.0F);
	CHECK(faded[1] == Approx(0.2F));
	CHECK(faded[2] == Approx(0.4F));
	CHECK(applied.lastGain == Approx(0.4F));
	CHECK(kept[2] == 1.0F); // an output not faded is untouched
	CHECK_FALSE(applied.outEnded);

	// Out from 3/5, before the in ended: 0.6, 0.4, then 0.2 and zero, ending there.
	std::ranges::fill(faded, 1.0F);
	ramp.fadeOut();
	applied = ramp.apply(outputs, mask, 2);
	CHECK(faded[0] == Approx(0.6F));
	CHECK(faded[1] == Approx(0.4F));
	CHECK_FALSE(applied.outEnded);
	std::ranges::fill(faded, 1.0F);
	applied = ramp.apply(outputs, mask, 2);
	CHECK(faded[0] == Approx(0.2F));
	CHECK(faded[1] == 0.0F);
	CHECK(applied.outEnded);
	std::ranges::fill(faded, 1.0F);
	CHECK(ramp.apply(outputs, mask, 4).outEnded); // it stays at zero, still ended
	CHECK(faded[3] == 0.0F);

	// A drive with nothing faded plays at full gain; silenced ends a fade-out at once.
	ramp.driven(false);
	std::ranges::fill(faded, 0.5F);
	applied = ramp.apply(outputs, mask, 8);
	CHECK(faded[0] == 0.5F);
	CHECK(applied.lastGain == 1.0F);
	ramp.driven(true);
	ramp.apply(outputs, mask, 8); // in, ended at full
	ramp.fadeOut();
	CHECK(ramp.fadingOut());
	ramp.silenced();
	std::ranges::fill(faded, 1.0F);
	CHECK(ramp.apply(outputs, mask, 1).outEnded);
	CHECK(faded[0] == 0.0F);

	// A cycle of no samples changes nothing and reports the gain the ramp holds.
	ramp.driven(true);
	ramp.apply(outputs, mask, 3); // at 3/5
	ramp.fadeOut();
	applied = ramp.apply(outputs, mask, 0);
	CHECK(applied.lastGain == Approx(0.6F));
	CHECK_FALSE(applied.outEnded);
}

TEST_CASE("BlockFade: fade-outs wait until a cycle ends at zero, a cycle too large to run or a block not processing "
          "acknowledges them, and a drive applied while they wait cancels them",
          "[block_rules]") {
	BlockFade fade(1000); // 5 frames
	std::vector<float> samples(8, 1.0F);
	const std::array<std::span<float>, 1> outputs{samples};
	const std::array<std::uint8_t, 1> mask{1};

	fade.driven(true);
	fade.apply(outputs, mask, 8); // in, then full
	fade.fadeOut();
	fade.fadeOut();
	CHECK(fade.waiting() == 2);
	CHECK(fade.apply(outputs, mask, 3).acknowledged == 0); // 5/5, 4/5, 3/5: not yet at zero
	CHECK(fade.apply(outputs, mask, 3).acknowledged == 2); // 2/5, 1/5, zero: both, the oldest first
	CHECK(fade.waiting() == 0);
	CHECK(fade.apply(outputs, mask, 3).acknowledged == 0); // nothing waits

	fade.driven(true);
	fade.fadeOut();
	CHECK(fade.driven(true) == 1); // cancelled
	CHECK(fade.waiting() == 0);
	CHECK(fade.notProcessing() == 0);

	fade.fadeOut();
	CHECK(fade.notProcessing() == 1);
	std::ranges::fill(samples, 1.0F);
	fade.apply(outputs, mask, 1);
	CHECK(samples[0] == 0.0F); // silenced at once

	fade.driven(true);
	CHECK(fade.wroteZeros() == 0); // no fade-out under way
	fade.fadeOut();
	CHECK(fade.wroteZeros() == 1);
}

TEST_CASE("The stamps carry the cycle's facts and the most of the latency told on the bank", "[block_rules]") {
	const GraphCycle cycle{.startNs = 11, .position = 22, .startRawNs = 33, .discontinuity = true};
	CHECK(chunkStamp(cycle, Latency{30, 40}) ==
	      ChunkStamp{.position = 22, .startNs = 11, .startRawNs = 33, .discontinuity = true, .upstreamFrames = 40});
	const SourceCycle played = sourceCycle(cycle, 64, Latency{7, 9});
	CHECK(played.position == 22);
	CHECK(played.frames == 64);
	CHECK(played.startRawNs == 33);
	CHECK(played.discontinuity);
	CHECK(played.downstream == Latency{7, 9});
}
