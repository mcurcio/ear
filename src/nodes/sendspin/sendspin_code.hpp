// A sendspin node's playback (docs/node-types.md#clock): on each cycle the feed goes to the outputs `from`, and what
// was played goes, with the time it finishes on the library's clock, to the notifier. The node's code
// (sendspin_spec.hpp's SendspinNodeCode) runs it, and passes the inputs `to` to the sink on `to_server`.
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include "ear/core/node_type.hpp"
#include "ear/core/latency.hpp"
#include "event_fd.hpp"
#include "feed.hpp"
#include "mailbox.hpp"
namespace ear::nodes::sendspin {
/// Frames played so far, and when the library's next frame plays at the driver's device (the end of the cycle that
/// told it), on CLOCK_MONOTONIC_RAW.
struct PlayProgress {
	std::uint64_t totalFrames;
	std::int64_t finishRawNs;
};
/// How long, in the graph's nominal time (`GraphCycle::startRawNs`, the cycles' starts as the driver dates them), the
/// library may go untold while no cycle begins on time, before it is told from the next cycle even if that one is
/// overdue (docs/node-types.md#clock). The deferral's bound and nothing else.
inline constexpr std::chrono::nanoseconds untoldBound = std::chrono::seconds{2};
class SendspinCode final {
public:
	/// `bell`: rung when progress changed. All outlive the code.
	SendspinCode(Feed& feed, Mailbox<PlayProgress>& progress, const EventFd& bell, SampleRate rate) noexcept;
	/// Realtime: one cycle of `frames` into `outputs` (the left and right of `from`).
	void process(std::span<const std::span<float>> outputs, std::uint32_t frames, const GraphCycle& cycle) noexcept;
	/// Test seams (docs/testing.md#a-writers-own-event), the data thread's own, read on it after process(): what the
	/// feed held as the last process() began (after any clear), and what it skipped of that for the graph's lost time.
	/// What it read short, the feed counts itself (Feed::dryFrames).
	std::uint32_t lastHeldAtStart() const noexcept { return lastHeldAtStart_; }
	std::uint32_t lastSkipped() const noexcept { return lastSkipped_; }
	/// Test seam, the data thread's own: what the last process() skipped as stale, the feed having been left undrained
	/// past its bound (counted by the feed's writer, never here).
	std::uint32_t lastStaleSkipped() const noexcept { return lastStaleSkipped_; }
	/// Test seams, the data thread's own: whether the last process() came back after a drop, and what it skipped as
	/// late for the drop's timeline.
	bool lastStale() const noexcept { return lastStale_; }
	std::uint32_t lastResumeSkipped() const noexcept { return lastResumeSkipped_; }
	/// Test seam, read on the data thread: the reports whose finish the clamp moved. It acts only on the report after one
	/// the bound forced from an overdue cycle (docs/node-types.md#clock); a test holds it idle otherwise.
	std::uint64_t clamped() const noexcept { return clamped_; }

private:
	Feed& feed_;
	Mailbox<PlayProgress>& progress_;
	const EventFd& bell_;
	FrameClock clock_;                        // the graph's rate: what plays is dated by it (docs/engine.md#dating)
	std::uint64_t played_ = 0;                // every frame of the library's skipped or read, in order
	std::uint64_t told_ = 0;                  // of them, those the library was told played
	std::int64_t lastFinish_ = 0;             // the last finish told
	bool afterForced_ = false;                // the last report was one the bound forced from an overdue cycle
	std::optional<std::int64_t> untoldSince_; // the start, on the raw clock, of the first cycle not yet told
	std::uint32_t lastHeldAtStart_ = 0;       // test seam: lastHeldAtStart()
	std::uint32_t lastSkipped_ = 0;           // test seam: lastSkipped()
	std::uint32_t lastStaleSkipped_ = 0;      // test seam: lastStaleSkipped()
	std::uint64_t countedTwice_ = 0;          // frames read that the feed's writer also counted dropped
	bool lastStale_ = false;                  // test seam: lastStale()
	std::uint32_t lastResumeSkipped_ = 0;     // test seam: lastResumeSkipped()
	// Back after a drop: none, the drop's last finish not yet read (the writer mid-publish), or anchored to it.
	enum class Resume : std::uint8_t { none, pending, anchored };
	Resume resume_ = Resume::none;
	std::int64_t resumeFromRawNs_ = 0;      // anchored: when the first frame written since the drop is heard
	std::uint64_t consumedSinceResume_ = 0; // frames skipped and read since the feed was taken back
	std::uint64_t clamped_ = 0;             // test seam: clamped()
};
} // namespace ear::nodes::sendspin
