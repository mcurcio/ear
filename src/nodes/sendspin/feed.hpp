// The feed (docs/node-types.md#sendspin): audio from an ordinary thread to the realtime thread, the
// handoff's reverse, on the same ring. JACK's disk-reader pattern. With an undrained rule (FeedDrain) the writer is
// never held back for long by a reader that stopped: past the bound it plays what it is given into nothing at the
// graph's rate, as the reader would have, and says so for the notifier to tell (docs/node-types.md#clock).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include "ear/core/frame_ring.hpp"
#include "ear/core/sample_rate.hpp"
#include "event_fd.hpp"
namespace ear::nodes::sendspin {
/// CLOCK_MONOTONIC_RAW in ns, the clock the reader's stamps are on (GraphCycle::ranRawNs).
std::int64_t rawNowNs() noexcept;
/// The writer's side of a reader that stopped draining: what a frame lasts, how long the reader may leave the feed
/// undrained before the writer stops waiting for it (undrainedBoundFor, worker.hpp), the clock it is judged on, and the
/// bell (the code's) rung after each drop, so the notifier tells it.
struct FeedDrain {
	SampleRate rate;
	std::chrono::nanoseconds bound;
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
	/// reader has not stamped drained() for its bound, it counts what the feed holds as dropped then (stale: the reader
	/// skips it when it is back; a clear asked and not yet applied discards its part first), and accepts frames at the
	/// graph's rate up to now (waiting up to `patience` for a frame's time to come) and drops them, each played into
	/// nothing in its time (dropped()), until the reader takes the feed back (skipStale()): a drop under way then is
	/// refused, and its frames go to the ring.
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience);
	/// Any ordinary thread, never while write() runs: the library fires on_stream_end only once its sync task, the
	/// writer, is idle, and starts the next stream's writes only after it (player_role.cpp's sync-idle gate), so no drop
	/// is mid-publish while this reads the total. What was written so far is gone at the next read, not counted played,
	/// and what was dropped so far is forgotten (Dropped::forgotten).
	void clear() noexcept;
	/// Realtime, each cycle: the reader drained the feed in a cycle that ran at `ranRawNs` (CLOCK_MONOTONIC_RAW). The
	/// feed counts as drained when it is made, so a configuration that starts during a stream waits for its first
	/// cycle as a reader's stall, rather than dropping the stream's first chunks.
	void drained(std::int64_t ranRawNs) noexcept { drainedRawNs_.store(ranRawNs, std::memory_order_release); }
	/// Realtime, each cycle: what lies downstream of the reader's outputs, in ns (a following card's buffer), as its
	/// finishes add it. The writer dates what it drops by it too, so both tellers' finishes are on one timeline: when a
	/// frame is heard, not when it leaves the feed (docs/node-types.md#clock).
	void downstream(std::int64_t ns) noexcept { downstreamNs_.store(ns, std::memory_order_relaxed); }
	/// What skipStale() did: the frames it skipped, and those the reader read after the writer counted them dropped (a
	/// reader back in the instant the writer began: counted twice, so the reader counts as many fewer of its own).
	struct StaleSkip {
		std::uint32_t skipped;
		std::uint32_t readSince;
	};
	/// Realtime: when the writer began dropping since the last call, takes the feed back from it (no drop is counted
	/// after this call: one the writer had under way when it came is refused, and the writer waits on the ring again)
	/// and skips what the feed held then (stale: from before the reader stopped, and counted dropped by the writer as it
	/// began), nothing written since. Call it after drained(), so a refused writer sees the reader back.
	std::optional<StaleSkip> skipStale() noexcept;
	/// What the writer has played into nothing: every frame it dropped, those of them a clear made the library forget
	/// (dropped before the last clear), and when the frame after the last one dropped would be heard
	/// (CLOCK_MONOTONIC_RAW, with the reader's downstream()).
	struct Dropped {
		std::uint64_t total;
		std::uint64_t forgotten;
		std::int64_t finishRawNs;
	};
	/// Any ordinary thread: the total and its finish, always of one drop (the writer publishes them under a sequence
	/// count, and this reads again while it is under way).
	Dropped dropped() const noexcept;
	/// Realtime: the same in one attempt, or nothing while the writer is publishing a drop.
	std::optional<Dropped> tryDropped() const noexcept;
	/// Test seam, never while write() runs: holds the drop's sequence count odd, as a writer preempted mid-publish
	/// would, until called again with false, so a test makes tryDropped() find nothing for as long as it likes (and
	/// dropped() wait for it).
	void holdMidPublish(bool held) noexcept {
		if (((dropSeq_.load(std::memory_order_relaxed) & 1U) != 0) != held)
			dropSeq_.fetch_add(1, std::memory_order_acq_rel);
	}
	/// Test seam: what the writer counted stale as it last began dropping (what the feed held then).
	std::uint32_t lastStaleFrames() const noexcept { return lastStaleFrames_.load(std::memory_order_acquire); }
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
	/// The reader's mark as the writer begins dropping: where it had read to (past a clear asked and not yet applied,
	/// which discards what lies before it), the frames it had read and skipped of its own (never a clear's or a stale
	/// skip's), and the clears it had applied.
	struct ReaderMark {
		std::uint32_t from;
		std::uint32_t consumed;
		std::uint32_t clears;
	};
	bool applyClear() noexcept;
	void beginMove() noexcept;
	void endMove() noexcept;
	ReaderMark readerMark() const noexcept;
	bool publishDrop(std::uint64_t frames, std::int64_t finishRawNs, bool begins) noexcept;
	std::uint32_t writeRing(std::span<const float> interleaved, std::uint32_t frames,
	                        std::chrono::milliseconds patience);
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
	// The reader's moves, under a sequence count the reader alone writes (odd while it moves), so the writer's mark is of
	// one instant: the read position, what the reader counted of its own and the clears it applied.
	std::atomic<std::uint32_t> readSeq_{0};
	std::atomic<std::uint32_t> consumed_{0};
	std::atomic<std::uint32_t> clears_{0};
	std::uint32_t consumedAtClear_ = 0; // reader only: consumed_ when it last applied a clear
	std::optional<FeedDrain> drain_;
	std::atomic<std::int64_t> drainedRawNs_; // the reader's last stamp; the feed's making before its first
	std::atomic<std::int64_t> downstreamNs_{0};
	std::atomic<bool> stale_{false};
	std::atomic<std::uint32_t> staleFrom_{0}; // the reader's mark and the write position when the writer began dropping
	std::atomic<std::uint32_t> staleTo_{0};
	std::atomic<std::uint32_t> staleConsumed_{0};
	std::atomic<std::uint32_t> staleClears_{0};
	std::atomic<std::uint32_t> lastStaleFrames_{0}; // test seam: lastStaleFrames()
	// What was dropped, its top bit set once the reader took the feed back (skipStale), which refuses any later drop
	// until the writer begins again; the total and its finish under the writer's own sequence count (odd while it
	// publishes).
	std::atomic<std::uint64_t> dropped_{0};
	std::atomic<std::uint64_t> dropSeq_{0};
	std::atomic<std::uint64_t> forgotten_{0};
	std::atomic<std::int64_t> finishRawNs_{0};
	bool discarding_ = false;    // writer only
	std::int64_t paceRawNs_ = 0; // writer only: when the next frame it drops would have played
};
} // namespace ear::nodes::sendspin
