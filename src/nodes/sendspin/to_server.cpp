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
		// The piece's first frame, `done` into the chunk, from the chunk's cycle start. No upstream latency is counted
		// yet, so a card's capture is dated by when its cycle ran, not when it entered (docs/node-types.md#clock;
		// Chunk::upstreamFrames carries what is missing).
		const std::int64_t capturedNs = clock_.enteredRawNs(chunk.startRawNs, done, Latency{});
		sink_.writeAudio(scratch_.data(), std::size_t{piece} * bytesPerFrame, capturedNs / 1000);
		done += piece;
	}
}

} // namespace ear::nodes::sendspin
