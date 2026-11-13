#include "sendspin_code.hpp"

#include <algorithm>

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

	// The stream keeps in step with the graph's clock (docs/node-types.md#clock), and ear and the library never both
	// correct the same lost time. On a catch-up, however long, or a pause (cycles missed and not cut, the position moved
	// by exactly their frames), the graph moved on by `jumpFrames`. While the library cannot realign itself (it holds
	// frames it was not told played), that much of the stream is owed: skipped from what the feed holds now and, as the
	// library refills it, from what it writes next, before any of it is read. A library that sees itself starved
	// realigns its next chunk to now on its own (its floor), so nothing is owed then, and a debt is forgotten the moment
	// it does. A reposition, a driver change or a first cycle is a new clock: nothing owed. A clear ends the stream.
	const CycleFacts& facts = cycle.facts;
	const bool movedOn = facts.causes.has(CycleCause::catchUp) || facts.causes.has(CycleCause::absent);
	const auto lost = movedOn && facts.jumpFrames > 0 ? static_cast<std::uint64_t>(facts.jumpFrames) : 0U;
	const bool cleared = feed_.takeCleared();
	if (cleared) {
		owed_ = 0;
		catchingUp_ = false;
	}
	lastHeldAtStart_ = feed_.held();
	std::uint64_t toSkip = owed_;
	if (!cleared && lost > 0) {
		toSkip += lost; // the feed's stale audio of that time is skipped whatever else holds
		if (!owingSuspended_ && !view_.starved(lastHeldAtStart_)) {
			owed_ += lost;
			catchingUp_ = true;
		}
	}
	const std::uint32_t skipped = feed_.skip(toSkip);
	owed_ -= std::min<std::uint64_t>(owed_, skipped);
	lastHeld_ = feed_.heldAtSkip();

	const std::uint32_t got = feed_.read(outputs, frames);
	lastGot_ = got;
	view_.consumed(static_cast<std::uint64_t>(skipped) + got);
	// While a catch-up is untold, the library cannot see a cycle the feed left short either (its zeros are time the
	// stream lost too), so that is owed as well.
	if (catchingUp_) owed_ += frames - got;
	// The library sees itself starved: it realigns on its own, and whatever ear owed is forgotten (it would be paid
	// twice).
	lastHeldAtEnd_ = feed_.held();
	if (view_.starved(lastHeldAtEnd_)) {
		owed_ = 0;
		catchingUp_ = false;
	}
	// The library is told only from a cycle that lost nothing, once what was owed is paid: a cycle that caught lost time
	// up is dated by the driver's own clock, behind raw time until its catch-up burst ends
	// (docs/engine.md#the-internal-clock), and a frame dropped after the library was told it played would be taken from
	// audio still to play. When it has gone untold for `untoldBound` of the graph's time, the debt is forgotten and no
	// more is owed until it is told, so the library realigns itself; it is still told only from a cycle that lost
	// nothing.
	if (view_.untold() > 0 && !untoldSince_) untoldSince_ = startRawNs;
	if (untoldSince_ && startRawNs - *untoldSince_ >= untoldBound.count() && !owingSuspended_) {
		owed_ = 0;
		catchingUp_ = false;
		owingSuspended_ = true;
	}
	if (movedOn || owed_ > 0 || view_.untold() == 0) return;
	view_.told();
	untoldSince_.reset();
	owingSuspended_ = false;
	catchingUp_ = false;
	// The finish told is where the library's next frame plays (sync_task.cpp: the finish plus what it still holds): the
	// end of this cycle, which played zeros for any frames the feed lacked, heard once what lies downstream of the
	// outputs has played: a following card's buffer, as the framework carries it back through the graph
	// (docs/engine.md#latency-per-bank-pair). The longest path is taken; the left and right outputs are one bank, so
	// either tells it (docs/engine.md#dating).
	progress_.publish({view_.consumedFrames(), clock_.heardRawNs(startRawNs, frames, cycle.downstreamOf(0))});
	bell_.ring();
}

} // namespace ear::sendspin
