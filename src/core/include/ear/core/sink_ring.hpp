// Every rule of a sink's rings, pure (docs/engine.md#sinks): the frame ring and the stamp ring beside it, the two
// dropping together, each stamp paired with its frames, and each chunk cut where the ring wraps (cutChunk).
// ear_pipewire's ThreadSink wraps one with its thread and wake, and the type harness's sink wraps one drained at once
// on the test's thread, so a type's tests get the chunks the engine hands, split where it splits them
// (docs/writing-a-node-type.md#testing-a-type). One writer (the block's tap, realtime) and one reader (the sink's
// thread); nothing here locks, allocates or waits but the constructor.
#pragma once
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "ear/core/frame_ring.hpp"
#include "ear/core/latency.hpp"
#include "ear/core/sink.hpp"
namespace ear {

class SinkRing {
public:
	/// A ring of `channels` and at least `capacityFrames` frames (FrameRing rounds up to a power of two) at the graph's
	/// `rate` (by which a chunk handed in two calls dates the second), with a stamp entry per 32 frames of it, at
	/// least 64.
	SinkRing(std::uint32_t channels, std::uint32_t capacityFrames, SampleRate rate)
		: clock_(rate), ring_(channels, capacityFrames),
		  stamps_(std::bit_ceil(std::max<std::uint32_t>(64, ring_.capacityFrames() / 32))) {}
	SinkRing(const SinkRing&) = delete;
	SinkRing& operator=(const SinkRing&) = delete;

	std::uint32_t channels() const noexcept { return ring_.channels(); }
	std::uint32_t capacityFrames() const noexcept { return ring_.capacityFrames(); }

	/// Writer (SinkTap::write's rule): with no free stamp entry nothing is written and the whole chunk is dropped; else
	/// the frames the frame ring has room for are written and stamped, and the rest dropped. True when any frame was
	/// taken: the reader has something to drain.
	bool write(std::span<const std::span<const float>> bank, std::uint32_t frames, const ChunkStamp& stamp) noexcept {
		const std::uint32_t written = stampsWritten_.load(std::memory_order_relaxed);
		if (written - stampsRead_.load(std::memory_order_acquire) >= stamps_.size()) {
			dropped_.fetch_add(frames, std::memory_order_relaxed);
			return false;
		}
		const std::uint32_t accepted = ring_.writePlanar(bank, frames);
		if (accepted < frames) dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
		if (accepted == 0) return false;
		stamps_[written & (stamps_.size() - 1)] = Stamped{stamp, accepted};
		stampsWritten_.store(written + 1, std::memory_order_release);
		return true;
	}

	/// Any thread: the frames the writer refused since the ring was made.
	std::uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }

	/// Reader: every stamped chunk written, in order, handed to `consume(const Chunk&)` as cutChunk cuts it (one call,
	/// or two where it wraps the ring's end). Each chunk is valid only during its call.
	template <class Consume> void drain(Consume&& consume) {
		for (;;) {
			const std::uint32_t read = stampsRead_.load(std::memory_order_relaxed);
			if (read == stampsWritten_.load(std::memory_order_acquire)) return;
			const Stamped stamped = stamps_[read & (stamps_.size() - 1)];
			const std::uint32_t contiguous = contiguousFrames();
			// A stamp's frames are in the ring before the stamp is published, so some lie before its end; were none
			// there, nothing is handed and the stamp waits rather than a call reading past what the ring holds.
			if (contiguous == 0) return;
			const ChunkCut cut = cutChunk(stamped.stamp, stamped.frames, contiguous, clock_);
			hand(consume, cut.first, cut.firstFrames);
			if (cut.second) hand(consume, *cut.second, cut.secondFrames);
			stampsRead_.store(read + 1, std::memory_order_release);
		}
	}

	/// Reader: every stamped chunk written is discarded (no begun consumer can take it).
	void discard() noexcept {
		for (;;) {
			const std::uint32_t read = stampsRead_.load(std::memory_order_relaxed);
			if (read == stampsWritten_.load(std::memory_order_acquire)) return;
			ring_.skip(stamps_[read & (stamps_.size() - 1)].frames);
			stampsRead_.store(read + 1, std::memory_order_release);
		}
	}

private:
	/// What the writer pushed beside one cycle's frames: its stamp and how many frames the frame ring took.
	struct Stamped {
		ChunkStamp stamp;
		std::uint32_t frames;
	};

	/// Reader: the frames before the ring's end, of those it holds.
	std::uint32_t contiguousFrames() const noexcept {
		return static_cast<std::uint32_t>(ring_.readSpan().size() / ring_.channels());
	}

	/// Reader: one call of `frames` frames from the ring's oldest, stamped `stamp`; then they are taken.
	template <class Consume> void hand(Consume& consume, const ChunkStamp& stamp, std::uint32_t frames) {
		const std::span<const float> run = ring_.readSpan();
		consume(Chunk{.interleaved = run.first(static_cast<std::size_t>(frames) * ring_.channels()),
		              .frames = frames,
		              .position = stamp.position,
		              .startNs = stamp.startNs,
		              .startRawNs = stamp.startRawNs,
		              .upstreamFrames = stamp.upstreamFrames,
		              .causes = stamp.causes,
		              .lostFrames = stamp.lostFrames});
		ring_.skip(frames);
	}

	FrameClock clock_; // the graph's rate
	FrameRing ring_;
	std::vector<Stamped> stamps_; // a power of two entries
	alignas(64) std::atomic<std::uint32_t> stampsWritten_{0};
	alignas(64) std::atomic<std::uint32_t> stampsRead_{0};
	std::atomic<std::uint64_t> dropped_{0};
};

} // namespace ear
