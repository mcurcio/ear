// The source (docs/writing-a-node-type.md#sources, the home of its rules; docs/engine.md#sources, how the block plays it):
// audio a type's ordinary thread writes, played into one of the node's inside input banks by the port block in its own
// cycle, before the code. One home for the source: what a type writes against (SourceShape, Wake, SourceWriter, and a
// source's own clock: SourceClock, told SourceTick, SourceHeld and SourcePlayed) and what the framework runs a source
// with (SourceTap, SourceBinding, Source, SourceAdapter). NodeHost implements CreateOf::source on it, ResourceRegistry
// takes a SourceAdapter, DriveRequest carries SourceBindings, core's SourceRing holds every rule, and ear_pipewire
// implements SourceAdapter as EventSources (ear/pipewire/sources.hpp). Pure: nothing but the standard library and
// ear_core.
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

/// What a source's ring holds as a tick begins, for its clock to decide on.
struct SourceHeld {
	std::uint32_t frames = 0; // of the current stream, after any clear this tick applies
	bool clearing = false;    // this tick applies a clear: the ended stream's 5 ms plays faded, the rest is dropped
	friend bool operator==(const SourceHeld&, const SourceHeld&) = default;
};

/// What one tick did with a source's ring, every count in the ring's frames (its rate: SourceTick::rate). `read`,
/// `ended` and `zeros` add up to the tick's frames.
struct SourcePlayed {
	std::uint64_t skipped =
		0;                   // of the current stream, dropped unplayed at the clock's word: at most SourceHeld::frames
	std::uint32_t read = 0;  // of the current stream, played into the bank (faded in where audio came back)
	std::uint32_t ended = 0; // of a stream a clear ended, played faded to zero: never the current stream's
	std::uint32_t zeros = 0; // played as zeros, an underrun's decay or a clear's fade past the ended stream's frames
	friend bool operator==(const SourcePlayed&, const SourcePlayed&) = default;
};

/// One tick as a source's clock sees it.
struct SourceTick {
	const GraphCycle& cycle; // the tick and its facts (docs/engine.md#graphcycle): the graph's, at the graph rate
	std::uint32_t frames;    // ring frames this tick plays: the cycle's (behind a converter, what its resampler asks)
	std::uint32_t rate;      // the ring's rate: the graph frequency (behind a converter, its node rate)
	Latency playback;        // the most playback latency the block told on the source's bank, in graph frames
};

/// A source's own clock, reconciled with the tick inside the node (docs/architecture.md#the-timing-model, rule 3;
/// docs/writing-a-node-type.md#a-sources-clock). Realtime thread only, called by whoever plays the ring
/// (SourceRing::play), never concurrently: never blocks, locks, allocates or makes a system call. Owned by its source
/// and freed with it (teardown step 5, after the instance and every thread it owns), so a reference the type keeps to
/// read its reports never dangles.
class SourceClock {
public:
	virtual ~SourceClock() = default;
	/// Before the ring plays this tick: how much of the current stream to drop unplayed first. The ring drops
	/// min(answer, held.frames), and nothing on a tick whose held.clearing is set (a clear ends the stream: nothing of
	/// the next is skipped for the old one's lost time). Any count: a stall has no bound.
	virtual std::uint64_t skipBefore(const SourceTick& tick, SourceHeld held) noexcept = 0;
	/// After it played: what the tick did, exactly. True rings the source's clock wake (SourceWriter::clockWake), at
	/// most once until answered.
	virtual bool played(const SourceTick& tick, const SourcePlayed& played) noexcept = 0;
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
	/// Any ordinary thread: what was written so far is gone at the next cycle: its next 5 ms is played faded to zero,
	/// the rest dropped unplayed (docs/writing-a-node-type.md#sources).
	virtual void clear() noexcept = 0;
	/// Any thread: whether teardown closed the writer.
	virtual bool closed() const noexcept = 0;
	/// Any thread: the ring's capacity in frames (at least SourceShape::frames).
	virtual std::uint32_t capacityFrames() const noexcept = 0;
	/// Rung when the source's clock asks after a tick (SourceClock::played returned true), at most once until answered.
	/// One watcher (a loop's onWake, slice 3c). Never rung for a source with no clock.
	virtual Wake& clockWake() noexcept = 0;

protected:
	~SourceWriter() = default;
};

// ---- What the framework runs a source with (no type sees these) ----

/// The realtime end of one source: the port block calls play() once per cycle a code runs, before that code.
class SourceTap {
public:
	/// Realtime thread only: never blocks, locks, allocates or makes a syscall but a wake. Plays the tick by
	/// SourceRing::play with the source's clock (none for a source with no clock): exactly `tick.frames` frames into
	/// every span of `bank` (the source's inside input bank, `bank.size()` its channel count, each span at least
	/// `tick.frames` long).
	virtual void play(std::span<const std::span<float>> bank, const SourceTick& tick) noexcept = 0;

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

/// One source: its ring, its clock (if it has one), its writer's room and clock wakes, its counters. Made inside
/// configure by CreateOf::source, for the configuration's life. Main thread for every call but the counters.
class Source {
public:
	/// Main thread, at teardown step 5, after the instance (whose threads may hold the writer or the clock) is freed:
	/// frees the clock with the ring.
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
	/// Any thread: frames written and never played (dropped by a clear or a stop; not those its clock skipped), since
	/// the source was made.
	virtual std::uint64_t droppedFrames() const noexcept = 0;
};

/// The source kind's adapter (ResourceRegistry's slot). ear_pipewire's EventSources implements it.
class SourceAdapter {
public:
	/// Main thread. A source of `channels` channels and `capacityFrames` frames of ring for `node`'s inside input bank
	/// `bank`, at the graph frequency `rate` (its fades' length), owning `clock` (null for a source with no clock) for
	/// its life. Refuses only by std::bad_alloc or std::system_error (an eventfd it cannot make): the node fails
	/// start-failed. A std::logic_error is its own bug: the node fails lost, told as ear.internal-error
	/// (docs/architecture.md#faults-by-owner).
	virtual std::unique_ptr<Source> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                                     std::uint32_t capacityFrames, std::uint32_t rate,
	                                     std::unique_ptr<SourceClock> clock) = 0;

protected:
	~SourceAdapter() = default;
};

} // namespace ear
