// A sendspin node's clock (docs/node-types.md#clock): the one owner of the played timeline the library is told. The
// source on `from_server` plays the library's audio into the node's bank; this clock decides on each tick what of it
// to skip, counts what played, and tells the library when its next frame is heard. While no tick drains the source
// past its bound (a stop, a host stall), the library's own writes advance the same timeline at the graph's rate
// instead, so its inbound ring never pins (which would close the connection). Who advances it is one atomic word:
// a hand-over either way is one compare-and-swap, never two tellers merged after the fact.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include "ear/core/latency.hpp"
#include "ear/core/source.hpp"
namespace ear::nodes::sendspin {

/// CLOCK_MONOTONIC_RAW in ns, the clock the ticks' stamps are on (GraphCycle::startRawNs, ranRawNs).
std::int64_t rawNowNs() noexcept;

/// How long, in the graph's nominal time (`GraphCycle::startRawNs`, the ticks' starts as the driver dates them), the
/// library may go untold while no tick begins on time, before it is told from the next tick even if that one is
/// overdue (docs/node-types.md#clock). The deferral's bound and nothing else.
inline constexpr std::chrono::nanoseconds untoldBound = std::chrono::seconds{2};

/// What the library is told: the timeline as a cumulative count over every stream, where the current stream's count
/// began (frames below `base` are of a stream the library forgot at its end, never told), and when the library's next
/// frame is heard, on CLOCK_MONOTONIC_RAW.
struct PlayProgress {
	std::uint64_t played = 0;
	std::uint64_t base = 0;
	std::int64_t finishRawNs = 0;
	friend bool operator==(const PlayProgress&, const PlayProgress&) = default;
};

class SendspinClock final : public SourceClock {
public:
	struct Config {
		SampleRate rate;
		/// How long no tick may drain the source before the writer paces (undrainedBoundFor, worker.hpp).
		std::chrono::nanoseconds undrainedBound;
		/// The raw clock; a test's own in the unit rows.
		std::int64_t (*nowRawNs)() noexcept = &rawNowNs;
	};
	/// Counts as drained when made, so a configuration made during a stream waits for its first tick as through a stall.
	explicit SendspinClock(Config config) noexcept;
	SendspinClock(const SendspinClock&) = delete;
	SendspinClock& operator=(const SendspinClock&) = delete;
	/// Configure, once, before any thread writes: the source this is the clock of, which outlives it.
	void attach(SourceWriter& writer) noexcept;

	// SourceClock: the realtime thread, one tick at a time.
	std::uint64_t skipBefore(const SourceTick& tick, SourceHeld held) noexcept override;
	bool played(const SourceTick& tick, const SourcePlayed& played) noexcept override;

	/// The library's sync thread, the one writer: while ticks drain the source, into its ring (SourceWriter::write's
	/// rules); once no tick ran for the bound, paced into nothing at the graph's rate, up to now and never ahead, each
	/// frame told played. Returns the frames taken.
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience);
	/// The worker's on_stream_end, never while write() runs (the library ends a stream only once its sync task, the
	/// writer, is idle): the stream is over, what was written of it goes at the next tick, and nothing of it is told
	/// from now on (the library zeroed its count). The writer counts as idle until it writes again.
	void clear() noexcept;
	/// Any thread: the newest report, nothing before the first.
	std::optional<PlayProgress> progress() const noexcept;
	/// The source's clock wake: rung after each report, by the tick or the pacer. One watcher (the notifier).
	Wake& wake() noexcept { return writer_->clockWake(); }
	std::uint32_t capacityFrames() const noexcept { return capacity_; }

	// Test seams (docs/testing.md#a-writers-own-event). The data thread's own, read on it after a tick:
	SourceHeld lastHeld() const noexcept { return lastHeld_; }          // what the ring held as the tick began
	SourcePlayed lastPlayed() const noexcept { return lastPlayed_; }    // what the tick did, as the ring told it
	std::uint32_t lastSkipped() const noexcept { return lastSkipped_; } // skipped for the graph's lost time
	/// Skipped or read of what the pacer had already counted played (what the ring held as it took over): not counted.
	std::uint32_t lastStaleSkipped() const noexcept { return lastStaleSkipped_; }
	bool lastTookBack() const noexcept { return lastTookBack_; } // this tick took the timeline from the pacer
	bool lastAnchored() const noexcept { return lastAnchored_; } // it measured lateness from the pacer's finish
	std::int64_t lastResumeFrom() const noexcept { return resumeFromRawNs_; }       // that finish, once taken back
	std::uint32_t lastResumeSkipped() const noexcept { return lastResumeSkipped_; } // skipped as late on it
	std::uint64_t lastResumePlayed() const noexcept { return resumePlayed_; } // the count it took back, once taken
	/// What this tick told, if it told (the pacer's reports are not the tick's), and whether it would have told but
	/// left its report to the next tick: it ended before the pacer's reach, or found the pacer mid-publish.
	std::optional<PlayProgress> lastTold() const noexcept { return lastTold_; }
	bool lastTellDeferred() const noexcept { return lastTellDeferred_; }
	/// The reports whose finish the clamp moved: only the one after a report the bound forced from an overdue tick.
	std::uint64_t clamped() const noexcept { return clamped_; }
	// Any thread:
	/// Ticks that read short of their frames, and the frames they lacked (zeros where the stream should be).
	std::uint64_t dryTicks() const noexcept { return dryTicks_.load(std::memory_order_relaxed); }
	std::uint64_t dryFrames() const noexcept { return dryFrames_.load(std::memory_order_relaxed); }
	/// Every frame the pacer counted played: what the ring held as it took over (lastStaleCounted) and what it paced.
	std::uint64_t pacedFrames() const noexcept { return pacedFrames_.load(std::memory_order_acquire); }
	std::uint64_t lastStaleCounted() const noexcept { return lastStaleCounted_.load(std::memory_order_acquire); }
	std::uint64_t takeovers() const noexcept { return takeovers_.load(std::memory_order_acquire); }

private:
	/// The pacer's anchor: written whole before the owner word names it, never while named, so a tick reads it whole.
	struct Anchor {
		std::atomic<std::uint32_t> stream{0};
		std::atomic<std::uint64_t> base{0};
		std::atomic<std::uint64_t> played{0};           // the timeline's count at the anchor
		std::atomic<std::int64_t> paceRawNs{0};         // when the frame after them would be played, before downstream
		std::atomic<std::uint32_t> downstreamFrames{0}; // what lies downstream of the outputs, as the last tick told
	};
	std::uint32_t pace(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience,
	                   std::int64_t now);
	bool takeOver(std::uint64_t word, std::int64_t now) noexcept;
	bool reanchor(std::uint64_t word, std::uint64_t played, std::uint64_t base, std::uint32_t stream,
	              std::int64_t paceRawNs) noexcept;
	bool tryLockBox() noexcept;
	void lockBox() noexcept;
	void publish(const PlayProgress& progress) noexcept; // under the box's lock, which it releases
	void unlockBox() noexcept;

	FrameClock clock_;
	std::chrono::nanoseconds bound_;
	std::int64_t (*now_)() noexcept;
	SourceWriter* writer_ = nullptr;
	std::uint32_t capacity_ = 0;

	std::atomic<std::uint64_t> word_{0}; // who advances the timeline (sendspin_clock.cpp)
	std::array<Anchor, 2> anchors_{};

	// Published by the tick inside its busy window, read by the pacer as it takes over a quiescent tick.
	std::atomic<std::int64_t> lastRanRawNs_;
	std::atomic<std::uint64_t> tickPlayed_{0};
	std::atomic<std::uint64_t> tickBase_{0};
	std::atomic<std::uint32_t> tickStream_{0};
	std::atomic<std::uint32_t> downstreamFrames_{0};

	// The writer's side: the stream it writes (clears asked) and what it wrote of it, ring or paced.
	std::atomic<std::uint32_t> writerStream_{0};
	std::atomic<std::uint64_t> written_{0};

	// The report box: a seqlock two owners write, the tick by one try, the pacer waiting its turn.
	std::atomic<std::uint64_t> boxSeq_{0};
	std::atomic<std::uint64_t> boxPlayed_{0};
	std::atomic<std::uint64_t> boxBase_{0};
	std::atomic<std::int64_t> boxFinish_{0};
	std::atomic<bool> published_{false};

	// The tick's own.
	std::uint64_t played_ = 0; // the timeline's count, over every stream
	std::uint64_t base_ = 0;   // where the stream the tick counts began
	std::uint32_t stream_ = 0; // that stream: the clears applied to it
	bool countable_ = true;    // this tick's frames are of that stream
	std::uint64_t told_ = 0;   // played_ as last told
	std::int64_t lastFinish_ = 0;
	bool afterForced_ = false;
	std::optional<std::int64_t> untoldSince_;
	std::uint32_t staleLeft_ = 0;            // frames in the ring the pacer counted played, not yet consumed
	bool anchored_ = false;                  // back from the pacer: late frames measured from its finish
	std::int64_t resumeFromRawNs_ = 0;       // the pacer's finish: when the first frame written since is heard
	std::optional<std::int64_t> reachRawNs_; // the pacer's reach, until a tick that ends past it reports
	std::uint64_t consumedSince_ = 0;        // fresh frames skipped and read since the hand-over
	std::uint64_t ranSince_ = 0;             // frames of the ticks run since: the anchor expires at a ring's worth
	std::int64_t late_ = 0;                  // skipBefore to played: what it found late
	std::uint64_t jumpAsked_ = 0;            // and what it asked for the graph's lost time
	std::uint64_t clamped_ = 0;
	SourceHeld lastHeld_{};
	SourcePlayed lastPlayed_{};
	std::uint32_t lastSkipped_ = 0;
	std::uint32_t lastStaleSkipped_ = 0;
	std::uint32_t lastResumeSkipped_ = 0;
	std::uint64_t resumePlayed_ = 0;
	std::optional<PlayProgress> lastTold_;
	bool lastTellDeferred_ = false;
	bool lastTookBack_ = false;
	bool lastAnchored_ = false;
	std::atomic<std::uint64_t> dryTicks_{0};
	std::atomic<std::uint64_t> dryFrames_{0};
	std::atomic<std::uint64_t> pacedFrames_{0};
	std::atomic<std::uint64_t> lastStaleCounted_{0};
	std::atomic<std::uint64_t> takeovers_{0};
};

} // namespace ear::nodes::sendspin
