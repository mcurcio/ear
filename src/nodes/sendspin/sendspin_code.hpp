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

private:
	/// How many frames the driver missed since last cycle: 0 unless the graph's position moved ahead by no more than
	/// the feed holds. A larger or backward jump is a change of driver, not a skip.
	std::uint32_t missedFrames(const GraphCycle& cycle) const noexcept;
	Feed& feed_;
	Mailbox<PlayProgress>& progress_;
	const EventFd& bell_;
	FrameClock clock_; // the graph's rate: what plays is dated by it (docs/engine.md#dating)
	RtWriter& toServer_;
	Mailbox<CaptureAnchor>& capture_;
	std::uint64_t framesWritten_ = 0;
	bool started_ = false;
	std::uint64_t nextPosition_ = 0;
	std::uint64_t totalFrames_ = 0;
};
} // namespace ear::sendspin
