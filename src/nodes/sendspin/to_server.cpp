#include "to_server.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

#include "pcm.hpp"
#include "settings.hpp"

namespace ear::sendspin {
namespace {

constexpr std::uint32_t bytesPerFrame = nodeChannels * sourceBits / 8;

} // namespace

ToServer::ToServer(std::uint32_t rate, SourceSink& sink, const Mailbox<CaptureAnchor>& anchor)
	: clock_(rate), sink_(sink), anchor_(anchor),
	  pieceFrames_(static_cast<std::uint32_t>(clock_.frames(std::chrono::milliseconds{100}))),
	  scratch_(std::size_t{pieceFrames_} * bytesPerFrame) {}

void ToServer::consume(std::span<const float> interleaved, std::uint32_t frames) {
	const std::uint64_t first = consumed_;
	consumed_ += frames;
	if (!streaming_.load(std::memory_order_acquire)) return;
	const std::optional<CaptureAnchor> anchor = anchor_.read();
	for (std::uint32_t done = 0; done < frames;) {
		const std::uint32_t piece = std::min(frames - done, pieceFrames_);
		pcm::fromFloat(interleaved.data() + std::size_t{done} * nodeChannels, std::size_t{piece} * nodeChannels,
		               sourceBits / 8, scratch_.data());
		sink_.writeAudio(scratch_.data(), std::size_t{piece} * bytesPerFrame,
		                 anchor ? capturedAtUs(*anchor, first + done) : 0);
		done += piece;
	}
}

std::int64_t ToServer::capturedAtUs(const CaptureAnchor& anchor, std::uint64_t frame) const noexcept {
	// The frame's offset from the anchor's cycle, negative for one taken before it. No upstream latency is counted yet:
	// the anchor carries only the cycle's start, so a card's capture is dated by when its cycle ran, not when it entered
	// (docs/node-types.md#clock; a sink's Chunk::upstreamFrames carries what is missing).
	const auto after = static_cast<std::int64_t>(frame) - static_cast<std::int64_t>(anchor.framesWritten);
	return clock_.enteredRawNs(anchor.atRawNs, after, Latency{}) / 1000;
}

} // namespace ear::sendspin
