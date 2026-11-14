#include "sendspin_code.hpp"

#include <algorithm>

namespace ear::nodes::sendspin {

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, SampleRate rate) noexcept
	: feed_(feed), progress_(progress), bell_(bell), clock_(rate) {}

void SendspinCode::process(std::span<const std::span<float>> outputs, std::uint32_t frames,
                           const GraphCycle& cycle) noexcept {
	// The cycle's start on the library's clock, CLOCK_MONOTONIC_RAW, as the framework made it from the graph's, never
	// from when this thread woke (docs/engine.md#graphcycle).
	const std::int64_t startRawNs = cycle.startRawNs;

	// Every frame plays in order (docs/node-types.md#clock). The one exception is the graph's lost time: on the cycle
	// that states a catch-up or a pause (the graph moved on by `jumpFrames` while the block did not run), overdue or not,
	// what the feed holds of that time is stale and is skipped, at most all it holds. What it did not hold is the
	// library's to correct: it sees it in the next finish it is told, true when told, and corrects once. A clear ends
	// the stream, so nothing of the next is skipped for the old one's lost time.
	const CycleFacts& facts = cycle.facts;
	const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
	// Undrained past its bound (a stop, a stall the graph does not come back from in time), the feed's writer played
	// what the library wrote into nothing at the graph's rate rather than wait (docs/node-types.md#clock), counting
	// what the feed held then with it, and the notifier told both: that is skipped here, stale, and not counted again.
	// Taken first (before a clear is applied, and before the stamp, so the writer drops until this cycle has skipped what
	// it left). What this side read of it in the instant the writer began is counted twice; as many of its next frames
	// go uncounted. The drop covered the graph's lost time, so this cycle skips nothing for its jump.
	lastStaleSkipped_ = 0;
	lastStale_ = false;
	if (const auto stale = feed_.skipStale()) {
		lastStale_ = true;
		lastStaleSkipped_ = stale->skipped;
		countedTwice_ += stale->readSince;
		// The library's next frame was due when the frame after the last one dropped would have played; the graph
		// takes the frames it writes from now from a cycle of its own, later by up to a block and its wait.
		resumeFromRawNs_ = feed_.dropped().finishRawNs;
		consumedSinceResume_ = 0;
	}
	// After the stale skip, so a clear the reader had not yet applied cannot pass for frames it read.
	const bool cleared = feed_.takeCleared();
	if (cleared) resumeFromRawNs_.reset(); // a new stream keeps no time of the old one
	feed_.drained(cycle.ranRawNs());
	lastHeldAtStart_ = feed_.held();
	lastSkipped_ = !cleared && !lastStale_ && movedOn && facts.jumpFrames > 0
	                   ? feed_.skip(static_cast<std::uint64_t>(facts.jumpFrames))
	                   : 0;
	// Back after a drop, each frame keeps the time the drop gave it: what is due by this cycle's run and not consumed
	// is late and skipped, so the stream the library is told plays on from the drop's timeline with no error for it
	// to correct (it would take its slow sync seconds for the part of a block the graph's cycle came later). Done once
	// a cycle reads with nothing late left.
	lastResumeSkipped_ = 0;
	std::int64_t late = 0;
	if (resumeFromRawNs_) {
		late = clock_.framesNearest(std::chrono::nanoseconds{cycle.ranRawNs() - *resumeFromRawNs_}) -
		       static_cast<std::int64_t>(consumedSinceResume_);
		if (late > 0) lastResumeSkipped_ = feed_.skip(static_cast<std::uint64_t>(late));
	}
	const std::uint32_t got = feed_.read(outputs, frames);
	if (resumeFromRawNs_) {
		consumedSinceResume_ += std::uint64_t{lastResumeSkipped_} + got;
		if (got > 0 && late <= static_cast<std::int64_t>(lastResumeSkipped_)) resumeFromRawNs_.reset();
	}
	// Counted played: what was skipped and read, never the zeros a short cycle played (the library wrote none of
	// them).
	const std::uint64_t counted = std::uint64_t{lastSkipped_} + lastResumeSkipped_ + got;
	const std::uint64_t repaid = std::min(countedTwice_, counted);
	countedTwice_ -= repaid;
	played_ += counted - repaid;
	if (played_ == told_) return;
	if (!untoldSince_) untoldSince_ = startRawNs;

	// The library is told only from a cycle that began on time, so every finish it is told is true when told. A cycle
	// that began while the next was already due (overdueNs) is one of a catch-up's burst, whose start is the driver's
	// nominal time, behind now until the burst ends, and whose run time is not where the library's next frame plays
	// while the burst runs either: told either, the library would correct where nothing was lost. Its frames are told
	// with the next on-time cycle's report. Untold for untoldBound of the graph's nominal time (the cycles' starts as the
	// driver dates them, so a burst of short cycles does not stretch it), the library is told from the next cycle even
	// if it is overdue, so a host that never runs a cycle on time does not leave it untold for good; that report is
	// dated by when the cycle really ran (GraphCycle::ranRawNs).
	const bool onTime = cycle.overdueNs <= 0;
	if (!onTime && startRawNs - *untoldSince_ < untoldBound.count()) return;
	// The finish told is where the library's next frame plays (sync_task.cpp: the finish plus what it still holds): the
	// end of this cycle, which played zeros for any frames the feed lacked, heard once what lies downstream of the
	// outputs has played: a following card's buffer, as the framework carries it back through the graph
	// (docs/engine.md#latency-per-bank-pair). The longest path is taken; the left and right outputs are one bank, so
	// either tells it (docs/engine.md#dating). The cycle is dated by when it ran: its nominal start when on time, and
	// only a report the bound forces is from an overdue one. The report after that one is clamped to it: dated by its
	// nominal start, earlier than the forced one's run time by its overdue time, it would step back. Reports from
	// on-time cycles follow one another by their nominal starts, so the clamp has no other case.
	const std::int64_t heard = clock_.heardRawNs(cycle.ranRawNs(), frames, cycle.downstreamOf(0));
	if (afterForced_ && heard < lastFinish_) ++clamped_;
	lastFinish_ = afterForced_ ? std::max(heard, lastFinish_) : heard;
	afterForced_ = !onTime;
	told_ = played_;
	untoldSince_.reset();
	progress_.publish({told_, lastFinish_});
	bell_.ring();
}

} // namespace ear::nodes::sendspin
