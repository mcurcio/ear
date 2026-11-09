// The source (docs/writing-a-node-type.md#sources, the home of its rules; docs/engine.md#sources, how the block plays it):
// audio a type's ordinary thread writes, played into one of the node's inside input banks by the port block in its own
// cycle, before the code. One home for the source: what a type writes against (SourceShape, Played, Wake,
// SourceWriter) and what the framework runs a source with (SourceCycle, SourceTap, SourceBinding, Source,
// SourceAdapter). NodeHost implements CreateOf::source on it, ResourceRegistry takes a SourceAdapter, DriveRequest
// carries SourceBindings, core's SourceRing holds every rule, and ear_pipewire implements SourceAdapter as EventSources
// (ear/pipewire/sources.hpp). Pure: nothing but the standard library and ear_core.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
namespace ear {

// ---- What a type writes against ----

/// How much a source's ring holds, in frames at the graph frequency: 1 to maxSourceFrames (anything else is a rule
/// broken, failing the node start-failed). The channel count is its bank's size. It is also the most the source
/// declares upstream of its bank: Latency{0, frames} (rounded up to a power of two).
struct SourceShape {
	std::uint32_t frames;
};
/// The most frames a source's ring may hold: 2^24, about 5.8 minutes at 48 kHz, far past any network buffer, and small
/// enough that the ring, its latency and its positions stay sane.
inline constexpr std::uint32_t maxSourceFrames = std::uint32_t{1} << 24U;

/// What a source has played (docs/writing-a-node-type.md#sources), published after each cycle that played or skipped
/// frames.
struct Played {
	/// Frames played or skipped since the source was made; a skip (the graph lost time) counts as played, so the count
	/// moves with the graph's clock. Frames cleared or dropped at a stop are not counted.
	std::uint64_t totalFrames = 0;
	/// When the last frame counted is heard, on CLOCK_MONOTONIC_RAW: the cycle's start, plus the frames it played, plus
	/// the most latency downstream of the outputs its bank is paired with (a following card's buffer).
	std::int64_t heardRawNs = 0;
	friend bool operator==(const Played&, const Played&) = default;
};

/// A "look now" from any thread, the realtime one included, to one watcher
/// (docs/design/node-primitives.md#62-wakes-the-frameworks-for-now; slice 3c's EventWake implements it in ear_runtime
/// and Loop::onWake watches it). Rung at most once until answered.
class Wake {
public:
	/// Any thread: if not pending, marks it pending and writes its eventfd. Never blocks, locks or allocates.
	virtual void ring() noexcept = 0;
	/// The watcher: a descriptor readable while the wake is rung. Valid for the wake's life.
	virtual int descriptor() const noexcept = 0;
	/// The watcher, before it handles the wake: drains the descriptor, then clears pending, so a ring between the two
	/// still writes or is seen by the handler that follows.
	virtual void answer() noexcept = 0;

protected:
	~Wake() = default;
};

/// The ordinary-thread end of a source. Valid from configure until the configuration is freed (teardown step 5), so a
/// thread the instance owns may keep it; it is closed at teardown step 1, after which it accepts nothing.
class SourceWriter {
public:
	/// One writing thread at a time (a dedicated loop or a library's thread, never a strand; handed over only after the
	/// old writer's last call happens-before the new one's first). Takes up to `frames` interleaved frames
	/// (`interleaved.size() >= frames * channels`); with no room, waits up to `patience` for the block to play some.
	/// Returns the frames accepted. Closed: an already waiting call returns 0 at once; a later one accepts nothing and
	/// waits out its whole patience (a library thread calling in a loop sleeps and never spins). Stopped: waits for room
	/// as at a full ring.
	virtual std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames,
	                            std::chrono::milliseconds patience) = 0;
	/// The writing thread, with nothing to give: says the writer is idle (an empty ring is then no shortfall) and waits
	/// `patience`, accepting nothing; woken early by a close. The next write ends the idle.
	virtual void idle(std::chrono::milliseconds patience) = 0;
	/// Any ordinary thread: what was written so far is gone at the next cycle, not counted played; the next 5 ms of it
	/// is played faded to zero, counted played (docs/writing-a-node-type.md#sources).
	virtual void clear() noexcept = 0;
	/// Any thread: whether teardown closed the writer.
	virtual bool closed() const noexcept = 0;
	/// Any thread: the ring's capacity in frames (at least SourceShape::frames).
	virtual std::uint32_t capacityFrames() const noexcept = 0;
	/// Any thread: the last Played published (all zero before the first cycle that played).
	virtual Played played() const noexcept = 0;
	/// Rung after each cycle that published a new Played, at most once until answered. One watcher.
	virtual Wake& playedWake() noexcept = 0;

protected:
	~SourceWriter() = default;
};

// ---- What the framework runs a source with (no type sees these) ----

/// One cycle as a source's tap sees it: GraphCycle's facts, plus what lies downstream of its bank.
struct SourceCycle {
	std::uint64_t position;  // the driver's position of the cycle's first frame
	std::uint32_t frames;    // the cycle's frames, at most maxBlock
	std::int64_t startRawNs; // the cycle's start on CLOCK_MONOTONIC_RAW
	bool discontinuity;      // GraphCycle::discontinuity
	Latency downstream;      // the most downstream of any output bank the source's bank is paired with
};

/// The realtime end of one source: the port block calls play() once per cycle a code runs, before that code.
class SourceTap {
public:
	/// Realtime thread only: never blocks, locks, allocates or makes a syscall but a wake. Writes exactly
	/// `cycle.frames` frames into every span of `bank` (the source's inside input bank, `bank.size()` its channel
	/// count, each span at least `cycle.frames` long): the ring's frames, then zeros. Keeps time (a forward position jump
	/// of at most the capacity skips that many frames, counted played), fades at its edges, counts the shortfall,
	/// publishes Played and rings the played wake, and wakes a writer waiting for room.
	virtual void play(std::span<const std::span<float>> bank, const SourceCycle& cycle) noexcept = 0;

protected:
	~SourceTap() = default;
};

/// What a drive hands the block for one source (DriveRequest::sources): its tap, and where its bank sits in process()'s
/// input spans (inputs, then inside inputs). Outlives the drive's completion or the block, as the rest of a
/// DriveRequest does.
struct SourceBinding {
	SourceTap* tap;
	std::size_t firstInput; // index into process()'s input spans
	std::size_t channels;   // the bank's size
};

/// One source: its ring, its writer's room and played wakes, its counters. Made inside configure by CreateOf::source,
/// for the configuration's life. Main thread for every call but the counters.
class Source {
public:
	/// Main thread, at teardown step 5, after the instance (whose threads may hold the writer) is freed.
	virtual ~Source() = default;
	/// The type's end, valid for the source's life.
	virtual SourceWriter& writer() noexcept = 0;
	/// The realtime end, for the configuration's DriveRequest. Valid for the source's life.
	virtual SourceTap& tap() noexcept = 0;
	/// Main thread, once a stop's silence completed (no cycle plays it until the next drive): clears the ring, the
	/// frames counted dropped, so a start never plays audio written before it. A writer then waits for room or idles.
	virtual void stopped() noexcept = 0;
	/// Main thread, when a stop or a teardown begins (before the type's stop() and the fade): the writer counts as idle,
	/// so the fade and the silence that follow are no shortfall when the type's stop() stops its writer; its next write
	/// ends the idle.
	virtual void stopping() noexcept = 0;
	/// Main thread, at teardown step 1: closes the writer (SourceWriter::write's closing rules). Idempotent.
	virtual void close() noexcept = 0;
	/// Any thread: frames played as zeros while the writer was not idle, since the source was made.
	virtual std::uint64_t shortfallFrames() const noexcept = 0;
	/// Any thread: frames written and never played (dropped by a clear or a stop), since the source was made.
	virtual std::uint64_t droppedFrames() const noexcept = 0;
};

/// The source kind's adapter (ResourceRegistry's slot). ear_pipewire's EventSources implements it.
class SourceAdapter {
public:
	/// Main thread. A source of `channels` channels and `capacityFrames` frames of ring for `node`'s inside input bank
	/// `bank`, at the graph frequency `rate` (its fades and Played's dating). Never fails but by std::bad_alloc or
	/// std::system_error (an eventfd it cannot make), which NodeHost treats as a type bug's throw: the node fails
	/// start-failed.
	virtual std::unique_ptr<Source> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                                     std::uint32_t capacityFrames, std::uint32_t rate) = 0;

protected:
	~SourceAdapter() = default;
};

} // namespace ear
