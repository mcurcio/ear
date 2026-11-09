#include "sendspin_code.hpp"

namespace ear::sendspin {

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, std::uint32_t rate,
                           RtWriter& toServer, Mailbox<CaptureAnchor>& capture) noexcept
	: feed_(feed), progress_(progress), bell_(bell), clock_(rate), toServer_(toServer), capture_(capture) {}

void SendspinCode::prepare(const AudioFormat&) { started_ = false; }

void SendspinCode::release() noexcept {}

std::uint32_t SendspinCode::missedFrames(const GraphCycle& cycle) const noexcept {
	if (!started_) return 0;
	const auto gap = static_cast<std::int64_t>(cycle.position - nextPosition_);
	return gap > 0 && gap <= static_cast<std::int64_t>(feed_.capacityFrames()) ? static_cast<std::uint32_t>(gap) : 0;
}

void SendspinCode::process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
                           std::uint32_t frames, const GraphCycle& cycle) noexcept {
	// The cycle's start on the library's clock, CLOCK_MONOTONIC_RAW, as the framework made it from the graph's, never
	// from when this thread woke (docs/engine.md#graphcycle).
	const std::int64_t startRawNs = cycle.startRawNs;

	capture_.publish({framesWritten_, startRawNs});
	framesWritten_ += toServer_.write(inputs, frames);

	// A skipped cycle's frames are counted played: the audio moves on with the graph, and the library sees only the
	// jump.
	const std::uint32_t skipped = feed_.skip(missedFrames(cycle));
	started_ = true;
	nextPosition_ = cycle.position + frames;

	const std::uint32_t got = feed_.read(outputs, frames);
	if (skipped == 0 && got == 0) return;

	totalFrames_ += static_cast<std::uint64_t>(skipped) + got;
	// What this cycle writes is heard once what lies downstream of the outputs has played: a following card's buffer,
	// as the framework carries it back through the graph (docs/engine.md#latency-per-bank-pair). The
	// longest path is taken; the left and right outputs are one bank, so either tells it (docs/engine.md#dating).
	progress_.publish({totalFrames_, clock_.heardRawNs(startRawNs, got, cycle.downstreamOf(0))});
	bell_.ring();
}

} // namespace ear::sendspin
