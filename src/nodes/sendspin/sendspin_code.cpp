#include "sendspin_code.hpp"

namespace ear::sendspin {

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, std::uint32_t rate,
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

	// The stream keeps in step with the graph's clock (docs/node-types.md#clock): on a catch-up, however long, or a
	// pause (cycles missed and not cut, the position moved by exactly their frames), the graph moved on by `jumpFrames`,
	// and the feed skips what it holds of them, each frame counted played, rather than play stale audio late. What it
	// did not hold is the library's to catch up: it is told only what it wrote and ear consumed, with the cycle's real
	// finish, and so sees the rest as its own lateness. A reposition, a driver change or a first cycle is a new clock:
	// nothing skipped.
	const CycleFacts& facts = cycle.facts;
	const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
	const std::uint64_t lost = movedOn && facts.jumpFrames > 0 ? static_cast<std::uint64_t>(facts.jumpFrames) : 0;
	const std::uint32_t skipped = feed_.skip(lost);
	lastHeld_ = feed_.heldAtSkip();

	const std::uint32_t got = feed_.read(outputs, frames);
	lastGot_ = got;
	totalFrames_ += static_cast<std::uint64_t>(skipped) + got;
	// A cycle that caught lost time up is dated by the driver's own clock, behind raw time until its catch-up burst
	// ends (docs/engine.md#the-internal-clock): a finish told from it would put the library's playtime that far behind
	// now, and a chunk it synced meanwhile would be written late. So what it skipped and read is told with the next
	// cycle that lost nothing, dated on raw time again.
	if (totalFrames_ == toldFrames_ || movedOn) return;
	toldFrames_ = totalFrames_;
	// The finish told is where the library's next frame plays (sync_task.cpp: the finish plus what it still holds): the
	// end of this cycle, which played zeros for any frames the feed lacked, heard once what lies downstream of the
	// outputs has played: a following card's buffer, as the framework carries it back through the graph
	// (docs/engine.md#latency-per-bank-pair). The longest path is taken; the left and right outputs are one bank, so
	// either tells it (docs/engine.md#dating).
	progress_.publish({totalFrames_, clock_.heardRawNs(startRawNs, frames, cycle.downstreamOf(0))});
	bell_.ring();
}

} // namespace ear::sendspin
