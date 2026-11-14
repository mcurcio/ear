// The feed (docs/node-types.md#sendspin): audio from an ordinary thread to the realtime thread, the
// handoff's reverse, on the same ring. JACK's disk-reader pattern.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include "ear/core/frame_ring.hpp"
#include "event_fd.hpp"
namespace ear::sendspin {
/// A FrameRing and an eventfd, written only while the writer says it waits.
class Feed {
public:
	/// Throws std::system_error when it cannot make the eventfd.
	Feed(std::uint32_t channels, std::uint32_t capacityFrames);
	Feed(const Feed&) = delete;
	Feed& operator=(const Feed&) = delete;
	std::uint32_t capacityFrames() const noexcept { return ring_.capacityFrames(); }
	/// Realtime; no lock, allocation or waiting. Planar, zeros after what it holds. Returns frames read.
	std::uint32_t read(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept;
	/// Realtime; discards up to `frames` of the oldest frames, all it holds when it holds fewer (any count: a catch-up's
	/// jump has no bound). Returns how many.
	std::uint32_t skip(std::uint64_t frames) noexcept;
	/// Realtime: applies a clear asked for and not yet applied, and says whether any clear was applied since the last
	/// call (here, or by a read or a skip): the stream it ended is over, and nothing of the next is skipped for it.
	bool takeCleared() noexcept;
	/// One writing thread. With no room waits up to `patience`. Returns frames accepted.
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience);
	/// Any ordinary thread. What was written so far is gone at the next read, not counted played.
	void clear() noexcept;
	/// Whether a writer has said it waits and no read has woken it yet: the tests' window on the wake.
	bool writerWaiting() const noexcept { return writerWaits_.load(std::memory_order_relaxed); }
	/// Test seams, the writer's own events (docs/testing.md#a-writers-own-event): the waits on the room it began, each
	/// past its second look at the ring, so only a read, a clear or its patience ends one; and how many of them a read
	/// or a clear ended rather than the patience. Counted on the writer's side, never the realtime one.
	std::uint64_t waitsBegun() const noexcept { return waitsBegun_.load(std::memory_order_acquire); }
	std::uint64_t waitsWoken() const noexcept { return waitsWoken_.load(std::memory_order_acquire); }
	/// Realtime: what the feed holds now (exact on the reading thread).
	std::uint32_t held() const noexcept { return ring_.writePosition() - ring_.readPosition(); }
	/// Test seam: what the feed held at the last skip(), after any clear; the reading thread's own, read on it.
	std::uint32_t heldAtSkip() const noexcept { return heldAtSkip_; }
	/// Test seams, the reader's own events (docs/testing.md#a-writers-own-event): the reads that found the feed short of
	/// what they asked (a dry cycle: zeros played where the stream should be), and the frames they lacked. Counted on
	/// the reading thread as it reads; any thread may read them, after whatever published that read.
	std::uint64_t dryReads() const noexcept { return dryReads_.load(std::memory_order_relaxed); }
	std::uint64_t dryFrames() const noexcept { return dryFrames_.load(std::memory_order_relaxed); }

private:
	bool applyClear() noexcept;
	void countDry(std::uint32_t asked, std::uint32_t got) noexcept;
	void wakeWriter() noexcept;
	FrameRing ring_;
	EventFd room_; // what the writer waits on
	std::atomic<bool> writerWaits_{false};
	std::atomic<std::uint64_t> clearTo_{0};    // pending bit, and the ring's write position when it was asked
	std::atomic<std::uint64_t> waitsBegun_{0}; // test seams: waitsBegun and waitsWoken
	std::atomic<std::uint64_t> waitsWoken_{0};
	std::atomic<std::uint64_t> dryReads_{0}; // test seams: dryReads and dryFrames, the reader's alone to write
	std::atomic<std::uint64_t> dryFrames_{0};
	std::uint32_t heldAtSkip_ = 0; // reader only
	bool clearedSince_ = false;    // reader only: a clear was applied since the last takeCleared()
};
} // namespace ear::sendspin
