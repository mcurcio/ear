// A sendspin node's audio code (docs/node-types.md#clock): on each cycle the feed goes to the outputs, and what was
// played goes, with the time it finishes on the library's clock, to the notifier; the inputs go to the handoff, and
// when they were captured to its consumer.
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include "ear/core/node_type.hpp"
#include "event_fd.hpp"
#include "feed.hpp"
#include "mailbox.hpp"
namespace ear::sendspin {
/// Frames played so far, and when the library's next frame plays at the driver's device (the end of the cycle that
/// told it), on CLOCK_MONOTONIC_RAW.
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
/// How long, in the graph's nominal time (`GraphCycle::startRawNs`, the cycles' starts as the driver dates them), the
/// library may go untold while no cycle begins on time, before it is told from the next cycle even if that one is
/// overdue (docs/node-types.md#clock). The deferral's bound and nothing else.
inline constexpr std::chrono::nanoseconds untoldBound = std::chrono::seconds{2};
class SendspinCode final : public AudioCode {
public:
	/// `bell`: rung when progress changed. All outlive the code.
	SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, SampleRate rate, RtWriter& toServer,
	             Mailbox<CaptureAnchor>& capture) noexcept;
	void prepare(const AudioFormat& format) override;
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override;
	void release() noexcept override;
	/// Test seams (docs/testing.md#a-writers-own-event), the data thread's own, read on it after process(): what the
	/// feed held as the last process() began (after any clear), and what it skipped of that for the graph's lost time.
	/// What it read short, the feed counts itself (Feed::dryFrames).
	std::uint32_t lastHeldAtStart() const noexcept { return lastHeldAtStart_; }
	std::uint32_t lastSkipped() const noexcept { return lastSkipped_; }
	/// Test seam, read on the data thread: the reports whose finish the clamp moved. It acts only on the report after one
	/// the bound forced from an overdue cycle (docs/node-types.md#clock); a test holds it idle otherwise.
	std::uint64_t clamped() const noexcept { return clamped_; }

private:
	Feed& feed_;
	Mailbox<PlayProgress>& progress_;
	const EventFd& bell_;
	FrameClock clock_; // the graph's rate: what plays is dated by it (docs/engine.md#dating)
	RtWriter& toServer_;
	Mailbox<CaptureAnchor>& capture_;
	std::uint64_t framesWritten_ = 0;
	std::uint64_t played_ = 0;                // every frame of the library's skipped or read, in order
	std::uint64_t told_ = 0;                  // of them, those the library was told played
	std::int64_t lastFinish_ = 0;             // the last finish told
	bool afterForced_ = false;                // the last report was one the bound forced from an overdue cycle
	std::optional<std::int64_t> untoldSince_; // the start, on the raw clock, of the first cycle not yet told
	std::uint32_t lastHeldAtStart_ = 0;       // test seam: lastHeldAtStart()
	std::uint32_t lastSkipped_ = 0;           // test seam: lastSkipped()
	std::uint64_t clamped_ = 0;               // test seam: clamped()
};
} // namespace ear::sendspin
