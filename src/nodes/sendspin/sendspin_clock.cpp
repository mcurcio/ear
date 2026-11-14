// The sendspin node's clock (sendspin_clock.hpp; docs/node-types.md#clock holds its rules in words).
//
// The owner word: bit 63 says the pacer advances the timeline (else the tick), bit 62 that a tick is between its
// skipBefore and its played (only the tick sets it, and the pacer may take over only while it is clear), bit 61 which
// of the two anchors the pacer reads, and the rest a count: while ticking, the ticks' sequence (so a takeover that read
// a tick's facts fails if another tick ran since); while pacing, the frames paced since the anchor. Every change of
// owner is one compare-and-swap of it, so no frame is counted by both sides and the tick never waits for the pacer.
//
// The reports: each owner writes its own box (the tick's, the pacer's), each a seqlock with one writer, and a reader
// takes the box the word names. A box is written only while the word names its writer, or before the word comes to:
// the takeover fills the pacer's box before its swap, the tick that takes the timeline back copies the pacer's last
// report into its own at once. So a report the pacer finishes after the tick took the timeline back lands in a box
// nobody reads, and no owner ever waits for the other's write: who tells is decided by the word alone.
#include "sendspin_clock.hpp"

#include <algorithm>
#include <thread>

#include <time.h>

namespace ear::nodes::sendspin {
namespace {

constexpr std::uint64_t pacingBit = std::uint64_t{1} << 63U;
constexpr std::uint64_t busyBit = std::uint64_t{1} << 62U;
constexpr std::uint64_t anchorBit = std::uint64_t{1} << 61U;
constexpr std::uint64_t countMask = anchorBit - 1;

constexpr bool pacing(std::uint64_t word) noexcept { return (word & pacingBit) != 0; }
constexpr bool busy(std::uint64_t word) noexcept { return (word & busyBit) != 0; }
constexpr std::size_t anchorOf(std::uint64_t word) noexcept { return (word & anchorBit) != 0 ? 1 : 0; }
constexpr std::uint64_t countOf(std::uint64_t word) noexcept { return word & countMask; }
constexpr std::uint64_t wordOf(bool paces, bool ticking, std::size_t anchor, std::uint64_t count) noexcept {
	return (paces ? pacingBit : 0) | (ticking ? busyBit : 0) | (anchor != 0 ? anchorBit : 0) | (count & countMask);
}

/// One plain store of the next value: the counter has one writing thread (the realtime one).
void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by) noexcept {
	counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

/// The pacer's wait for a frame's time: at most a millisecond, so a tick taking the timeline back is seen soon.
void sleepUpTo(std::chrono::milliseconds patience) noexcept {
	const auto wait = std::min(patience, std::chrono::milliseconds{1});
	if (wait.count() <= 0) return;
	timespec left{.tv_sec = 0, .tv_nsec = static_cast<long>(std::chrono::nanoseconds{wait}.count())};
	while (::nanosleep(&left, &left) != 0) {
	}
}

} // namespace

std::int64_t rawNowNs() noexcept {
	timespec now{};
	::clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	return std::int64_t{now.tv_sec} * 1'000'000'000 + now.tv_nsec;
}

SendspinClock::SendspinClock(Config config) noexcept
	: clock_(config.rate), bound_(config.undrainedBound), now_(config.nowRawNs), lastRanRawNs_(config.nowRawNs()) {}

void SendspinClock::attach(SourceWriter& writer) noexcept {
	writer_ = &writer;
	capacity_ = writer.capacityFrames();
}

// ---- The tick ----

std::uint64_t SendspinClock::skipBefore(const SourceTick& tick, SourceHeld held) noexcept {
	lastHeld_ = held;
	lastTookBack_ = false;
	lastSkipped_ = 0;
	lastStaleSkipped_ = 0;
	lastResumeSkipped_ = 0;
	late_ = 0;
	jumpAsked_ = 0;
	lastTold_.reset();
	lastTellDeferred_ = false;

	// The timeline is the tick's from here to played(): taken back from the pacer if it had it, whatever the tick (an
	// overdue one too: the pacer must stop the moment a tick drains again). A failed swap means the pacer moved the
	// count or took over just now; it never holds the word, so this loop only retries what the pacer changed.
	std::uint64_t word = word_.load(std::memory_order_acquire);
	for (;;) {
		const std::uint64_t next = pacing(word) ? wordOf(false, true, anchorOf(word), 0)
		                                        : wordOf(false, true, anchorOf(word), countOf(word) + 1);
		if (word_.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_acquire)) break;
	}
	if (pacing(word)) {
		// The anchor was written whole before the word named it, and the pacer writes only the other one.
		const Anchor& anchor = anchors_[anchorOf(word)];
		const std::uint64_t paced = countOf(word);
		stream_ = anchor.stream.load(std::memory_order_relaxed);
		base_ = anchor.base.load(std::memory_order_relaxed);
		played_ = anchor.played.load(std::memory_order_relaxed) + paced;
		resumePlayed_ = played_;
		resumeFromRawNs_ =
			anchor.paceRawNs.load(std::memory_order_relaxed) +
			clock_.ns(static_cast<std::int64_t>(paced) + anchor.downstreamFrames.load(std::memory_order_relaxed));
		anchored_ = true;
		consumedSince_ = 0;
		ranSince_ = 0;
		// The pacer's last report becomes the tick's, as the word now names the tick's box: the count it reached and the
		// finish it dated (the frame after them, once what lay downstream as it dated them has played).
		publish(tickBox(), {.played = played_, .base = base_, .finishRawNs = resumeFromRawNs_, .stream = stream_});
		handedFinishNs_ = resumeFromRawNs_;
		// Whatever the pacer told, the next report this side may give is told again: the count told is cumulative.
		told_ = ~std::uint64_t{0};
		afterForced_ = false;
		untoldSince_.reset();
		lastTookBack_ = true;
	}
	// Which stream the ring plays (docs/node-types.md#clock): a later one than counted is a clear applied (its base is
	// where it begins, and a timeline from the pacer ends: the new stream is the library's to place); an earlier one is
	// the pacer's stream not yet applied by the ring, whose frames this tick does not count.
	countable_ = true;
	if (held.clears != stream_) {
		if (static_cast<std::int32_t>(held.clears - stream_) > 0) {
			stream_ = held.clears;
			base_ = played_;
			staleLeft_ = 0;
			anchored_ = false;
		} else countable_ = false;
	}
	// Back from the pacer, everything the ring holds of the stream was counted played as it took over (it wrote none
	// into it since): skipped now, uncounted. So was what a stop dropped of it, if the pacer took over during the stop;
	// otherwise those frames are counted played now, as a sink that played them would have (docs/node-types.md#clock):
	// the library counts every frame it wrote as buffered until it is told it played. A stop empties the ring, so what
	// the clock kept about the ring's frames ends at its first tick: the stale frames the pacer counted are among what
	// it dropped (counted once, by the pacer), and, unless this tick took the timeline back, the anchor ends: the frames
	// written after the stop are the library's to place, not the pacer's late ones.
	if (lastTookBack_ && countable_) staleLeft_ = held.frames;
	else if (countable_) played_ += held.dropped - std::min(staleLeft_, held.dropped);
	if (held.stopEnded && !lastTookBack_) {
		staleLeft_ = 0;
		anchored_ = false;
	}
	if (anchored_ && ranSince_ >= capacity_) anchored_ = false; // a ring's worth of ticks at most
	lastAnchored_ = anchored_ && countable_;
	if (!countable_) return 0;

	std::uint64_t ask = staleLeft_;
	if (anchored_) {
		// Each frame keeps the time the pacer gave it: what is heard by this tick's run and not consumed since is late.
		const std::int64_t heard = clock_.heardRawNs(tick.cycle.ranRawNs(), 0, tick.playback);
		late_ = clock_.framesNearest(std::chrono::nanoseconds{heard - resumeFromRawNs_}) -
		        static_cast<std::int64_t>(consumedSince_);
		if (late_ > 0) ask += static_cast<std::uint64_t>(late_);
	} else {
		// The graph's lost time: on the tick that states a catch-up or a pause, overdue or not, what the ring holds of it
		// is stale (the ring drops at most what it holds, and nothing on a clearing tick).
		const CycleFacts& facts = tick.cycle.facts;
		const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
		if (movedOn && facts.jumpFrames > 0) {
			jumpAsked_ = static_cast<std::uint64_t>(facts.jumpFrames);
			ask += jumpAsked_;
		}
	}
	return ask;
}

bool SendspinClock::played(const SourceTick& tick, const SourcePlayed& played) noexcept {
	lastPlayed_ = played;
	if (played.read < tick.frames) {
		bump(dryTicks_, 1);
		bump(dryFrames_, tick.frames - played.read);
	}
	if (countable_) {
		// What the pacer counted comes first in the ring: skipped first, then read on a clearing tick (which skips none).
		const std::uint64_t consumed = played.skipped + played.read;
		const auto stale = static_cast<std::uint32_t>(std::min<std::uint64_t>(staleLeft_, consumed));
		staleLeft_ -= stale;
		lastStaleSkipped_ = stale;
		const std::uint64_t freshSkipped = played.skipped - std::min<std::uint64_t>(stale, played.skipped);
		const std::uint64_t fresh = consumed - stale;
		if (anchored_) {
			lastResumeSkipped_ = static_cast<std::uint32_t>(freshSkipped);
			consumedSince_ += fresh;
			ranSince_ += tick.frames;
			// Done once a tick reads fresh frames with nothing late left.
			if (staleLeft_ == 0 && fresh > freshSkipped && late_ <= static_cast<std::int64_t>(freshSkipped))
				anchored_ = false;
		} else lastSkipped_ = static_cast<std::uint32_t>(freshSkipped);
		// Counted played: what was skipped and read of the stream, never the zeros nor a cleared stream's frames.
		played_ += fresh;
	}

	bool told = false;
	// Told only of the stream the library writes now (it zeroed its count at the clear), and only from a tick that
	// began on time, every report true when told (docs/node-types.md#clock): a burst's tick is dated neither by its
	// nominal start (behind now) nor by its run (not where the next frame plays while the burst runs). Untold for
	// untoldBound of the graph's nominal time, the next tick tells even if overdue, dated by when it ran, and the report
	// after that one is clamped to it: dated nominally it would step back. Reports from on-time ticks follow one
	// another by their nominal starts, so the clamp has no other case. Back from the pacer, a tick tells only once its
	// own finish, downstream included, is at or past the pacer's last, so the finish never steps back across a
	// hand-over by construction, whatever the graph's edits did to what lies downstream meanwhile
	// (docs/node-types.md#clock).
	if (played_ != told_ && stream_ == writerStream_.load(std::memory_order_acquire)) {
		const std::int64_t startRawNs = tick.cycle.startRawNs;
		if (!untoldSince_) untoldSince_ = startRawNs;
		const bool onTime = tick.cycle.overdueNs <= 0;
		// Where the library's next frame plays: the end of this tick (zeros for what the ring lacked), once what lies
		// downstream of the outputs has played (a following card's buffer).
		const std::int64_t heard = clock_.heardRawNs(tick.cycle.ranRawNs(), tick.frames, tick.playback);
		const std::int64_t finish = afterForced_ ? std::max(heard, lastFinish_) : heard;
		if (handedFinishNs_ && finish < *handedFinishNs_) lastTellDeferred_ = true;
		else if (onTime || startRawNs - *untoldSince_ >= untoldBound.count()) {
			lastTold_ = PlayProgress{.played = played_, .base = base_, .finishRawNs = finish, .stream = stream_};
			publish(tickBox(), *lastTold_);
			if (afterForced_ && heard < lastFinish_) ++clamped_;
			lastFinish_ = finish;
			afterForced_ = !onTime;
			told_ = played_;
			untoldSince_.reset();
			handedFinishNs_.reset();
			told = true;
		}
	}

	// What a pacer taking over would need, then the timeline free for it.
	lastRanRawNs_.store(tick.cycle.ranRawNs(), std::memory_order_relaxed);
	tickPlayed_.store(played_, std::memory_order_relaxed);
	tickBase_.store(base_, std::memory_order_relaxed);
	tickStream_.store(stream_, std::memory_order_relaxed);
	downstreamFrames_.store(tick.playback.maxFrames, std::memory_order_relaxed);
	word_.store(word_.load(std::memory_order_relaxed) & ~busyBit, std::memory_order_release);
	return told;
}

// ---- The writer ----

std::uint32_t SendspinClock::write(std::span<const float> interleaved, std::uint32_t frames,
                                   std::chrono::milliseconds patience) {
	const std::int64_t now = now_();
	const std::uint64_t word = word_.load(std::memory_order_acquire);
	if (pacing(word)) return pace(interleaved, frames, patience, now);
	// No tick has drained the source for the bound: the writer stops waiting on it (docs/node-types.md#clock).
	if (!busy(word) && now - lastRanRawNs_.load(std::memory_order_relaxed) > bound_.count() && takeOver(word, now))
		return pace(interleaved, frames, patience, now);
	const std::uint32_t taken = writer_->write(interleaved, frames, patience);
	written_.fetch_add(taken, std::memory_order_relaxed);
	return taken;
}

/// The tick's facts were read after a word that showed it idle, and the swap fails if any tick ran since, so they are
/// of one quiescent tick. Everything the writer wrote of the stream counts played now, dated at the pacer's reach:
/// what the ring still holds (a stall) or a stop dropped. Told at once, so the library leaves its priming and runs.
bool SendspinClock::takeOver(std::uint64_t word, std::int64_t now) noexcept {
	const std::uint32_t stream = writerStream_.load(std::memory_order_acquire);
	const std::uint64_t tickPlayed = tickPlayed_.load(std::memory_order_relaxed);
	const std::uint64_t base =
		tickStream_.load(std::memory_order_relaxed) == stream ? tickBase_.load(std::memory_order_relaxed) : tickPlayed;
	const std::uint64_t played = std::max(base + written_.load(std::memory_order_relaxed), tickPlayed);
	const std::int64_t reach = now;
	const std::uint32_t downstream = downstreamFrames_.load(std::memory_order_relaxed);
	// Into the pacer's box before the word names it: no reader takes it until then, and a failed swap leaves it
	// unread until the next takeover writes it again.
	publish(pacerBox(),
	        {.played = played, .base = base, .finishRawNs = reach + clock_.ns(downstream), .stream = stream});
	if (!reanchor(word, played, base, stream, reach)) return false;
	lastStaleCounted_.store(played - tickPlayed, std::memory_order_release);
	pacedFrames_.fetch_add(played - tickPlayed, std::memory_order_acq_rel);
	takeovers_.fetch_add(1, std::memory_order_acq_rel);
	writer_->clockWake().ring();
	return true;
}

/// A new anchor in the slot the word does not name (no tick reads it: a tick reads an anchor only in the busy window
/// right after it took the word back, and a takeover waits for that window to close), then the word swapped to it.
bool SendspinClock::reanchor(std::uint64_t word, std::uint64_t played, std::uint64_t base, std::uint32_t stream,
                             std::int64_t paceRawNs) noexcept {
	const std::size_t next = 1 - anchorOf(word);
	Anchor& anchor = anchors_[next];
	anchor.stream.store(stream, std::memory_order_relaxed);
	anchor.base.store(base, std::memory_order_relaxed);
	anchor.played.store(played, std::memory_order_relaxed);
	anchor.paceRawNs.store(paceRawNs, std::memory_order_relaxed);
	anchor.downstreamFrames.store(downstreamFrames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
	return word_.compare_exchange_strong(word, wordOf(true, false, next, 0), std::memory_order_acq_rel,
	                                     std::memory_order_acquire);
}

/// Plays what the library writes into nothing at the graph's rate, up to now and never ahead,
/// as a tick would have taken it, until a tick takes the timeline back: then into the ring again.
std::uint32_t SendspinClock::pace(std::span<const float> interleaved, std::uint32_t frames,
                                  std::chrono::milliseconds patience, std::int64_t now) {
	for (;;) {
		const std::uint64_t word = word_.load(std::memory_order_acquire);
		if (!pacing(word)) {
			const std::uint32_t taken = writer_->write(interleaved, frames, patience);
			written_.fetch_add(taken, std::memory_order_relaxed);
			return taken;
		}
		const Anchor& anchor = anchors_[anchorOf(word)];
		const std::uint64_t paced = countOf(word);
		const std::uint64_t base = anchor.base.load(std::memory_order_relaxed);
		const std::uint64_t played = anchor.played.load(std::memory_order_relaxed) + paced;
		const std::int64_t paceRawNs = anchor.paceRawNs.load(std::memory_order_relaxed);
		const std::uint32_t downstream = anchor.downstreamFrames.load(std::memory_order_relaxed);
		const std::int64_t reach = paceRawNs + clock_.ns(static_cast<std::int64_t>(paced));
		// A stream ended while pacing: time goes on, the count starts the new stream at its base.
		const std::uint32_t stream = writerStream_.load(std::memory_order_acquire);
		if (stream != anchor.stream.load(std::memory_order_relaxed)) {
			reanchor(word, played, played, stream, reach);
			continue;
		}
		// A writer away for longer than the ring lasts gets no credit for the time it missed: as a tick that kept
		// draining would have taken no more than the ring held, its reach moves up to a ring's length behind now.
		const std::int64_t ceiling = now;
		const std::int64_t floor = ceiling - clock_.ns(capacity_);
		if (reach < floor) {
			reanchor(word, played, base, stream, floor);
			continue;
		}
		const std::int64_t room = clock_.frames(std::chrono::nanoseconds{ceiling - reach});
		if (room <= 0) {
			sleepUpTo(patience);
			return 0;
		}
		const auto taken = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, room));
		std::uint64_t expected = word;
		if (!word_.compare_exchange_strong(expected, word + taken, std::memory_order_acq_rel,
		                                   std::memory_order_acquire))
			continue; // taken back meanwhile: the next look writes the ring
		written_.fetch_add(taken, std::memory_order_relaxed);
		pacedFrames_.fetch_add(taken, std::memory_order_acq_rel);
		// Read only while the word names the pacer: once a tick took the timeline back, this lands unread, its count in
		// the tick's.
		publish(pacerBox(),
		        {.played = played + taken,
		         .base = base,
		         .finishRawNs = paceRawNs + clock_.ns(static_cast<std::int64_t>(paced + taken) + downstream),
		         .stream = stream});
		writer_->clockWake().ring();
		return taken;
	}
}

void SendspinClock::clear() noexcept {
	// The stream counted first, so no tick tells a frame of the ended one once the library zeroed its count.
	writerStream_.fetch_add(1, std::memory_order_acq_rel);
	written_.store(0, std::memory_order_release);
	writer_->clear();
	writer_->idle(std::chrono::milliseconds{0}); // no stream: an empty ring is no shortfall
}

// ---- The reports ----

void SendspinClock::publish(Box& box, const PlayProgress& progress) noexcept {
	const std::uint64_t sequence = box.sequence.load(std::memory_order_relaxed);
	box.sequence.store(sequence + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	box.played.store(progress.played, std::memory_order_relaxed);
	box.base.store(progress.base, std::memory_order_relaxed);
	box.finishRawNs.store(progress.finishRawNs, std::memory_order_relaxed);
	box.stream.store(progress.stream, std::memory_order_relaxed);
	box.sequence.store(sequence + 2, std::memory_order_release);
}

std::optional<PlayProgress> SendspinClock::progress() const noexcept {
	// Who owns the timeline and which anchor it paces from: a box read while these held is the owner's report (a
	// takeover's report, written before its swap, is read only once the swap named it).
	constexpr std::uint64_t owner = pacingBit | anchorBit;
	for (;;) {
		const std::uint64_t word = word_.load(std::memory_order_acquire);
		const Box& box = boxes_[pacing(word) ? 1 : 0];
		const std::uint64_t before = box.sequence.load(std::memory_order_acquire);
		if (before == 0) return std::nullopt;
		if ((before & 1U) == 0) {
			const PlayProgress value{.played = box.played.load(std::memory_order_relaxed),
			                         .base = box.base.load(std::memory_order_relaxed),
			                         .finishRawNs = box.finishRawNs.load(std::memory_order_relaxed),
			                         .stream = box.stream.load(std::memory_order_relaxed)};
			std::atomic_thread_fence(std::memory_order_acquire);
			if (box.sequence.load(std::memory_order_relaxed) == before &&
			    (word_.load(std::memory_order_relaxed) & owner) == (word & owner))
				return value;
		}
		// Its owner mid-write, for four stores, or the owner changed: look again, at the word first.
		std::this_thread::yield();
	}
}

} // namespace ear::nodes::sendspin
