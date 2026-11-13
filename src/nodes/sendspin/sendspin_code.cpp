#include "sendspin_code.hpp"

namespace ear::sendspin {

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, std::uint32_t rate,
                           RtWriter& toServer, Mailbox<CaptureAnchor>& capture) noexcept
	: feed_(feed), progress_(progress), bell_(bell), clock_(rate), toServer_(toServer), capture_(capture),
	  tellWithin_(std::uint64_t{feed.capacityFrames()} + static_cast<std::uint64_t>(clock_.frames(longestChunk))) {}

void SendspinCode::prepare(const AudioFormat&) {}

void SendspinCode::release() noexcept {}

void SendspinCode::process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
                           std::uint32_t frames, const GraphCycle& cycle) noexcept {
	// The cycle's start on the library's clock, CLOCK_MONOTONIC_RAW, as the framework made it from the graph's, never
	// from when this thread woke (docs/engine.md#graphcycle).
	const std::int64_t startRawNs = cycle.startRawNs;

	capture_.publish({framesWritten_, startRawNs});
	framesWritten_ += toServer_.write(inputs, frames);

	// The stream keeps in step with the graph's clock (docs/node-types.md#clock). On a catch-up, however long, or a
	// pause (cycles missed and not cut, the position moved by exactly their frames), the graph moved on by `jumpFrames`:
	// that much of the stream is owed, skipped from what the feed holds now and, as the library refills it, from what it
	// writes next, before any of it is read. A catch-up's burst drains the feed faster than the library refills it, so
	// most of a long stall is paid that way. A reposition, a driver change or a first cycle is a new clock: nothing owed.
	// A clear ends the stream that owed, and before a stream plays nothing is owed (it has no place on the clock yet):
	// a stall then skips only what the feed holds.
	const CycleFacts& facts = cycle.facts;
	const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
	if (feed_.takeCleared()) {
		owed_ = 0;
		catchingUp_ = false;
		playing_ = false;
	} else if (movedOn && facts.jumpFrames > 0) {
		owed_ += static_cast<std::uint64_t>(facts.jumpFrames);
		catchingUp_ = playing_;
	}
	const std::uint32_t skipped = feed_.skip(owed_);
	owed_ = playing_ ? owed_ - skipped : 0;
	lastHeld_ = feed_.heldAtSkip();

	const std::uint32_t got = feed_.read(outputs, frames);
	lastGot_ = got;
	playing_ = playing_ || got > 0;
	totalFrames_ += static_cast<std::uint64_t>(skipped) + got;
	sinceTold_ += frames;
	// While a catch-up is untold, the library cannot see a cycle the feed left short either (its zeros are time the
	// stream lost too), so that is owed as well.
	if (catchingUp_) owed_ += frames - got;
	// The library is told only from a cycle that lost nothing, once what was owed is paid: a cycle that caught lost time
	// up is dated by the driver's own clock, behind raw time until its catch-up burst ends
	// (docs/engine.md#the-internal-clock), and a frame dropped after the library was told it played would be taken from
	// audio still to play. Whatever is still owed when it is told is forgotten, so the library is never told a frame ear
	// did not take from what it wrote. At most `tellWithin_` frames of cycles go untold, however the host behaves.
	const bool due = sinceTold_ >= tellWithin_;
	if (!due && (movedOn || owed_ > 0)) return;
	if (totalFrames_ == toldFrames_) {
		if (due) {
			owed_ = 0;
			sinceTold_ = 0;
			catchingUp_ = false;
		}
		return;
	}
	toldFrames_ = totalFrames_;
	owed_ = 0;
	sinceTold_ = 0;
	catchingUp_ = false;
	// The finish told is where the library's next frame plays (sync_task.cpp: the finish plus what it still holds): the
	// end of this cycle, which played zeros for any frames the feed lacked, heard once what lies downstream of the
	// outputs has played: a following card's buffer, as the framework carries it back through the graph
	// (docs/engine.md#latency-per-bank-pair). The longest path is taken; the left and right outputs are one bank, so
	// either tells it (docs/engine.md#dating).
	progress_.publish({totalFrames_, clock_.heardRawNs(startRawNs, frames, cycle.downstreamOf(0))});
	bell_.ring();
}

} // namespace ear::sendspin
