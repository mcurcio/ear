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

	// The stream keeps in step with the graph's clock (docs/node-types.md#clock): on a catch-up, however long, the
	// graph lost `jumpFrames` of time, and the feed skips what it holds of them, each frame counted played. What it did
	// not hold is the library's to catch up: it is told only what it wrote and ear consumed, with the cycle's real
	// finish, and so sees the rest as its own lateness. Any other cause is a new clock: nothing lost.
	const CycleFacts& facts = cycle.facts;
	const std::uint64_t lost =
		facts.causes.has(CycleCause::catchUp) ? static_cast<std::uint64_t>(facts.jumpFrames) : std::uint64_t{0};
	const std::uint32_t skipped = feed_.skip(lost);
	lastHeld_ = feed_.heldAtSkip();

	const std::uint32_t got = feed_.read(outputs, frames);
	lastGot_ = got;
	if (skipped == 0 && got == 0) return;

	totalFrames_ += static_cast<std::uint64_t>(skipped) + got;
	// What this cycle writes is heard once what lies downstream of the outputs has played: a following card's buffer,
	// as the framework carries it back through the graph (docs/engine.md#latency-per-bank-pair). The
	// longest path is taken; the left and right outputs are one bank, so either tells it (docs/engine.md#dating).
	progress_.publish({totalFrames_, clock_.heardRawNs(startRawNs, got, cycle.downstreamOf(0))});
	bell_.ring();
}

} // namespace ear::sendspin
