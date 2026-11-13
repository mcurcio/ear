// A sendspin node's audio code (docs/node-types.md#clock): on each cycle the feed goes to the outputs, and what was
// played goes, with the time it finishes on the library's clock, to the notifier; the inputs go to the handoff, and
// when they were captured to its consumer.
#pragma once
#include <cstdint>
#include <span>
#include "ear/core/node_type.hpp"
#include "event_fd.hpp"
#include "feed.hpp"
#include "mailbox.hpp"
namespace ear::sendspin {
/// Frames played so far, and when the last of them finishes at the driver's device, on CLOCK_MONOTONIC_RAW.
struct PlayProgress {
	std::uint64_t totalFrames;
	std::int64_t finishRawNs;
};
/// The frame the handoff took as number `framesWritten` is this cycle's first, captured at `atRawNs` on
/// CLOCK_MONOTONIC_RAW, so a late wake of the handoff's thread does not move the stamp of what it hands on.
struct CaptureAnchor {
	std::uint64_t framesWritten;
	std::int64_t atRawNs;
};
class SendspinCode final : public AudioCode {
public:
	/// `bell`: rung when progress changed. All outlive the code.
	SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, std::uint32_t rate,
	             RtWriter& toServer, Mailbox<CaptureAnchor>& capture) noexcept;
	void prepare(const AudioFormat& format) override;
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override;
	void release() noexcept override;
	/// Test seams (docs/testing.md#a-writers-own-event): what the feed held as the last process() kept time, and the
	/// frames it read from the feed to its outputs, the rest of the cycle zeros because the feed held no more. What it
	/// skipped for the graph's lost time follows from the cycle's facts and the first. The data thread's own; read on
	/// it, after process().
	std::uint32_t lastHeld() const noexcept { return lastHeld_; }
	std::uint32_t lastGot() const noexcept { return lastGot_; }

private:
	Feed& feed_;
	Mailbox<PlayProgress>& progress_;
	const EventFd& bell_;
	FrameClock clock_; // the graph's rate: what plays is dated by it (docs/engine.md#dating)
	RtWriter& toServer_;
	Mailbox<CaptureAnchor>& capture_;
	std::uint64_t framesWritten_ = 0;
	std::uint64_t totalFrames_ = 0;
	std::uint32_t lastGot_ = 0; // test seams: lastGot() and lastHeld()
	std::uint32_t lastHeld_ = 0;
};
} // namespace ear::sendspin
