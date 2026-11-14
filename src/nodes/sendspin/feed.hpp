// The feed (docs/node-types.md#sendspin): audio from an ordinary thread to the realtime thread, the
// handoff's reverse, on the same ring. JACK's disk-reader pattern. With an undrained rule (FeedDrain) the writer is
// never held back for long by a reader that stopped: past the bound it plays what it is given into nothing at the
// graph's rate, as the reader would have, and says so for the notifier to tell (docs/node-types.md#clock).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <optional>
#include "ear/core/frame_ring.hpp"
#include "ear/core/sample_rate.hpp"
#include "event_fd.hpp"
namespace ear::nodes::sendspin {
/// How long the reader may leave the feed undrained before the writer stops waiting for it. Longer than the host
/// stalls the graph recovers from exactly (the clock tests' longest is 1 s), so such a stall keeps the clock's skip
/// and the library's one correction; shorter than the room a server that fills the player's advertised buffer leaves
/// in the library's inbound ring (a third of its 1 MB quota: about 2 s of FLAC 24 stereo at 48 kHz), whose pinning
/// closes the connection (docs/node-types.md#clock).
inline constexpr std::chrono::milliseconds undrainedBound{1500};
/// CLOCK_MONOTONIC_RAW in ns, the clock the reader's stamps are on (GraphCycle::ranRawNs).
std::int64_t rawNowNs() noexcept;
/// The writer's side of a reader that stopped draining: what a frame lasts, the bound, the clock it is judged on, and
/// the bell (the code's) rung after each drop, so the notifier tells it.
struct FeedDrain {
	SampleRate rate;
	std::chrono::nanoseconds bound = undrainedBound;
	std::int64_t (*nowRawNs)() noexcept = &rawNowNs;
	const EventFd* bell = nullptr;
};
/// A FrameRing and an eventfd, written only while the writer says it waits.
class Feed {
public:
	/// Throws std::system_error when it cannot make the eventfd. Without `drain` the writer waits for the reader
	/// however long it is gone.
	Feed(std::uint32_t channels, std::uint32_t capacityFrames, std::optional<FeedDrain> drain = std::nullopt);
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
	/// One writing thread. With no room waits up to `patience`. Returns frames accepted. With a FeedDrain, once the
	/// reader has not stamped drained() for its bound, it accepts frames at the graph's rate (a feed's worth ahead of
	/// now at most, waiting up to `patience` past that) and drops them, played into nothing in their time (dropped()),
	/// and marks what the feed still holds stale.
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience);
	/// Any ordinary thread. What was written so far is gone at the next read, not counted played, and what was dropped
	/// so far is forgotten (Dropped::forgotten).
	void clear() noexcept;
	/// Realtime, each cycle: the reader drained the feed in a cycle that ran at `ranRawNs` (CLOCK_MONOTONIC_RAW).
	void drained(std::int64_t ranRawNs) noexcept { drainedRawNs_.store(ranRawNs, std::memory_order_release); }
	/// Realtime: whether the writer began dropping since the last call, so what the feed holds is from before the reader
	/// stopped and is stale.
	bool takeStale() noexcept { return stale_.exchange(false, std::memory_order_acq_rel); }
	/// What the writer has played into nothing: every frame it dropped, those of them a clear made the library forget
	/// (dropped before the last clear), and when the frame after the last one dropped would play (CLOCK_MONOTONIC_RAW).
	struct Dropped {
		std::uint64_t total;
		std::uint64_t forgotten;
		std::int64_t finishRawNs;
	};
	/// Any thread; read after the bell, finishRawNs belongs to at least `total`.
	Dropped dropped() const noexcept {
		// The total first: the writer stores the finish before it adds to the total.
		const std::uint64_t total = dropped_.load(std::memory_order_acquire);
		return {total, forgotten_.load(std::memory_order_acquire), finishRawNs_.load(std::memory_order_acquire)};
	}
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
	std::uint32_t discard(std::uint32_t frames, std::chrono::milliseconds patience);
	void waitForRoom(std::chrono::milliseconds patience);
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
	std::optional<FeedDrain> drain_;
	std::atomic<std::int64_t> drainedRawNs_{0}; // the reader's last stamp; 0 before its first
	std::atomic<bool> stale_{false};
	std::atomic<std::uint64_t> dropped_{0};
	std::atomic<std::uint64_t> forgotten_{0};
	std::atomic<std::int64_t> finishRawNs_{0};
	bool discarding_ = false;    // writer only
	std::int64_t paceRawNs_ = 0; // writer only: when the next frame it drops would have played
};
} // namespace ear::nodes::sendspin
