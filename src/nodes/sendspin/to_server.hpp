// The source role's audio (docs/node-types.md#threads): the sink on `to_server` hands it what reached the inputs, and
// while the server wants it it goes on, each piece dated by when it entered the graph. While nobody does it is
// dropped here, so a server decides when the source streams and the sink's ring never fills.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ear/core/latency.hpp"
#include "ear/core/sink.hpp"
namespace ear::nodes::sendspin {
/// Where the pieces go: the library's source role. `stampUs` on CLOCK_MONOTONIC_RAW, when the piece's first frame was
/// captured.
class SourceSink {
public:
	virtual void writeAudio(const std::uint8_t* pcm, std::size_t bytes, std::int64_t stampUs) = 0;

protected:
	~SourceSink() = default;
};
inline constexpr std::uint32_t sourceBits = 24;
/// The sink's consumer, a member of the worker's state (cx.sink(toServer, shape, worker, &Worker::toServer)): every call on the sink's
/// own thread.
class ToServer final : public SinkConsumer {
public:
	/// `sink` outlives every consume(): the worker's state holds both, and the sink finishes before it is destroyed.
	ToServer(SampleRate rate, SourceSink& sink);
	/// The server's commands: whether anyone wants the audio. Any thread.
	void streaming(bool wanted) noexcept { streaming_.store(wanted, std::memory_order_release); }
	/// Pieces of at most 100 ms: one write must fit 5/8 of the 500 ms capture buffer. Each is dated from the chunk's
	/// cycle start by its offset in the chunk, less the capture latency upstream of the chunk (Chunk::upstreamFrames),
	/// so when this thread woke does not move a stamp (docs/node-types.md#clock).
	void consume(const Chunk& chunk) override;
	void finish() override {}

private:
	FrameClock clock_; // the graph's rate: each piece's length and its dating (docs/engine.md#dating)
	SourceSink& sink_;
	std::uint32_t pieceFrames_;
	std::vector<std::uint8_t> scratch_; // the sink's thread's
	std::atomic<bool> streaming_{false};
};
} // namespace ear::nodes::sendspin
