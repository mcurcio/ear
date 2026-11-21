#include "to_server.hpp"

#include <algorithm>
#include <chrono>

#include "pcm.hpp"
#include "settings.hpp"

namespace ear::nodes::sendspin {
namespace {

constexpr std::uint32_t bytesPerFrame = nodeChannels * sourceBits / 8;

} // namespace

ToServer::ToServer(SampleRate rate, SourceSink& sink)
	: clock_(rate), sink_(sink),
	  pieceFrames_(static_cast<std::uint32_t>(clock_.frames(std::chrono::milliseconds{100}))),
	  scratch_(std::size_t{pieceFrames_} * bytesPerFrame) {}

void ToServer::consume(const Chunk& chunk) {
	if (!streaming_.load(std::memory_order_acquire)) return;
	for (std::uint32_t done = 0; done < chunk.frames;) {
		const std::uint32_t piece = std::min(chunk.frames - done, pieceFrames_);
		pcm::fromFloat(chunk.interleaved.data() + std::size_t{done} * nodeChannels, std::size_t{piece} * nodeChannels,
		               sourceBits / 8, scratch_.data());
		// The piece's first frame, `done` into the chunk, entered the graph the chunk's upstream before its place in
		// the cycle, so a card's capture is dated by when the card took it, not when its cycle ran
		// (docs/engine.md#dating, docs/node-types.md#clock).
		const std::int64_t capturedNs =
			clock_.enteredRawNs(chunk.startRawNs, done, Latency{.minFrames = 0, .maxFrames = chunk.upstreamFrames});
		sink_.writeAudio(scratch_.data(), std::size_t{piece} * bytesPerFrame, capturedNs / 1000);
		done += piece;
	}
}

} // namespace ear::nodes::sendspin
