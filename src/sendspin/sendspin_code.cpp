#include "sendspin_code.hpp"

#include <ctime>

namespace ear::sendspin {
namespace {

std::int64_t nowNs(clockid_t clock) noexcept {
	timespec now{};
	::clock_gettime(clock, &now);
	return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

} // namespace

SendspinCode::SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, std::uint32_t rate,
                           RtWriter& toServer, Mailbox<CaptureAnchor>& capture) noexcept
	: feed_(feed), progress_(progress), bell_(bell), rate_(rate), toServer_(toServer), capture_(capture) {}

void SendspinCode::prepare(const AudioFormat&) { started_ = false; }

void SendspinCode::release() noexcept {}

std::uint32_t SendspinCode::missedFrames(const GraphCycle& cycle) const noexcept {
	if (!started_) return 0;
	const auto gap = static_cast<std::int64_t>(cycle.position - nextPosition_);
	return gap > 0 && gap <= static_cast<std::int64_t>(feed_.capacityFrames()) ? static_cast<std::uint32_t>(gap) : 0;
}

void SendspinCode::process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
                           std::uint32_t frames, const GraphCycle& cycle) noexcept {
	// The cycle's start on the library's clock, from the graph's, never from when this thread woke. Both clocks are read
	// together, so a late wake cancels out (section 7).
	const std::int64_t raw = nowNs(CLOCK_MONOTONIC_RAW);
	const std::int64_t mono = nowNs(CLOCK_MONOTONIC);
	const std::int64_t startRawNs = cycle.startNs + (raw - mono);

	capture_.publish({framesWritten_, startRawNs});
	framesWritten_ += toServer_.write(inputs, frames);

	// A skipped cycle's frames are counted played: the audio moves on with the graph, and the library sees only the
	// jump (section 5(f2)).
	const std::uint32_t skipped = feed_.skip(missedFrames(cycle));
	started_ = true;
	nextPosition_ = cycle.position + frames;

	const std::uint32_t got = feed_.read(outputs, frames);
	if (skipped == 0 && got == 0) return;

	totalFrames_ += static_cast<std::uint64_t>(skipped) + got;
	// What this cycle writes is heard once what the driver holds has played (section 7).
	const std::int64_t heardFrames = static_cast<std::int64_t>(got) + cycle.delayFrames;
	progress_.publish({totalFrames_, startRawNs + heardFrames * 1'000'000'000 / rate_});
	bell_.ring();
}

} // namespace ear::sendspin
