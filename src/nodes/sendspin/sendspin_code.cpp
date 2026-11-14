#include "sendspin_code.hpp"

#include <algorithm>

namespace ear::sendspin {

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, SampleRate rate,
                           RtWriter& toServer, Mailbox<CaptureAnchor>& capture) noexcept
	: feed_(feed), progress_(progress), bell_(bell), clock_(rate), toServer_(toServer), capture_(capture) {}

void SendspinCode::prepare(const AudioFormat&) {}

void SendspinCode::release() noexcept {}

void SendspinCode::process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
                           std::uint32_t frames, const GraphCycle& cycle) noexcept {
	// The cycle's start on the library's clock, CLOCK_MONOTONIC_RAW, as the framework made it from the graph's, never
	// from when this thread woke (docs/engine.md#graphcycle).
	const std::int64_t startRawNs = cycle.startRawNs;

	capture_.publish({framesWritten_, startRawNs});
	framesWritten_ += toServer_.write(inputs, frames);

	// Every frame plays in order (docs/node-types.md#clock). The one exception is the graph's lost time: on the cycle
	// that states a catch-up or a pause (the graph moved on by `jumpFrames` while the block did not run), overdue or not,
	// what the feed holds of that time is stale and is skipped, at most all it holds. What it did not hold is the
	// library's to correct: it sees it in the next finish it is told, true when told, and corrects once. A clear ends
	// the stream, so nothing of the next is skipped for the old one's lost time.
	const CycleFacts& facts = cycle.facts;
	const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
	const bool cleared = feed_.takeCleared();
	lastHeldAtStart_ = feed_.held();
	lastSkipped_ =
		!cleared && movedOn && facts.jumpFrames > 0 ? feed_.skip(static_cast<std::uint64_t>(facts.jumpFrames)) : 0;
	const std::uint32_t got = feed_.read(outputs, frames);
	// Counted played: what was skipped and what was read, never the zeros a short cycle played (the library wrote
	// none of them).
	played_ += std::uint64_t{lastSkipped_} + got;
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

} // namespace ear::sendspin
