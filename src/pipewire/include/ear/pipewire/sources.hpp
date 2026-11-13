// The sources (docs/engine.md#sources): ear_core's SourceAdapter over core's SourceRing per source, an eventfd the
// writer waits on for room, and one for the clock wake. A source has no thread of its own: its writer is the type's
// thread and its reader the port block's process (SourceTap::play, with the source's clock). The eventfd wake moves to
// ear_runtime's EventWake in slice 3c.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include "ear/core/source.hpp"
namespace ear::pipewire {
/// Each source: SourceRing(channels, capacityFrames, rate) and its clock (null: none); `room`, an eventfd written by
/// play() when it freed frames (or ended a stop) while a writer waits, and by close(); `clockWake`, a Wake whose ring()
/// writes its eventfd once until answer(), rung when the clock asks. write() and idle() wait with poll() on `room` up
/// to their patience, never spinning.
class EventSources final : public SourceAdapter {
public:
	EventSources() noexcept = default;
	/// Main thread. Throws std::system_error when it cannot make an eventfd.
	std::unique_ptr<Source> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                             std::uint32_t capacityFrames, std::uint32_t rate,
	                             std::unique_ptr<SourceClock> clock) override;
	/// A test seam, any thread: how many waits on its room a writer of `source` (made by an EventSources; 0 for any
	/// other) has begun, in write(), in idle() or after the close. A test waits for it to move, the writer's own event,
	/// instead of sleeping for a time it guesses is long enough (docs/testing.md#a-writers-own-event).
	static std::uint64_t waitsBegun(const Source& source) noexcept;
	/// A test seam, any thread: how many of those waits the room descriptor ended (a play that freed room, or the
	/// close), not the patience.
	static std::uint64_t waitsWoken(const Source& source) noexcept;
	/// A test seam: when a wait of `patience` begun now ends (the patience clamped to [0, a year], so
	/// milliseconds::max() is a year away, never an overflow).
	static std::chrono::steady_clock::time_point waitDeadline(std::chrono::milliseconds patience) noexcept;
};
} // namespace ear::pipewire
