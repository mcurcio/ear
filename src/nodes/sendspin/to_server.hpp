// The source role's audio (docs/node-types.md#threads): the handoff's thread hands it the inputs, and while the server
// wants them they go on, each piece dated from the newest capture anchor. While nobody does they are dropped here, so a
// server decides when the source streams and the handoff never fills.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "ear/core/node_type.hpp"
#include "mailbox.hpp"
#include "sendspin_code.hpp"
namespace ear::sendspin {
/// Where the pieces go: the library's source role. `stampUs` on CLOCK_MONOTONIC_RAW; 0 asks it to stamp the piece as ending
/// now, for the moment before the first anchor.
class SourceSink {
public:
	virtual void writeAudio(const std::uint8_t* pcm, std::size_t bytes, std::int64_t stampUs) = 0;

protected:
	~SourceSink() = default;
};
inline constexpr std::uint32_t sourceBits = 24;
class ToServer final : public HandoffConsumer {
public:
	/// `sink` and `anchor` outlive every consume(): the handoff closes before the worker stops.
	ToServer(std::uint32_t rate, SourceSink& sink, const Mailbox<CaptureAnchor>& anchor);
	/// The server's commands: whether anyone wants the audio.
	void streaming(bool wanted) noexcept { streaming_.store(wanted, std::memory_order_release); }
	/// Pieces of at most 100 ms: one write must fit 5/8 of the 500 ms capture buffer. When this thread woke does
	/// not move a stamp.
	void consume(std::span<const float> interleaved, std::uint32_t frames) override;
	void finish() override {}

private:
	std::int64_t capturedAtUs(const CaptureAnchor& anchor, std::uint64_t frame) const noexcept;
	std::uint32_t rate_;
	SourceSink& sink_;
	const Mailbox<CaptureAnchor>& anchor_;
	std::uint32_t pieceFrames_;
	std::vector<std::uint8_t> scratch_; // the handoff thread's
	std::uint64_t consumed_ = 0;        // the handoff thread's
	std::atomic<bool> streaming_{false};
};
} // namespace ear::sendspin
