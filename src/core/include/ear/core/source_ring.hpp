// The rules every source plays by, in one place (docs/writing-a-node-type.md#sources): its ring, the fades at its edges,
// the shortfall, the frames dropped at a clear or a stop, and the one play rule that asks a source's clock what to skip
// and tells it what played. The ring keeps no audio time (docs/architecture.md#the-timing-model, rule 1): a skip is
// only ever its clock's. Pure: no thread, no descriptor, no wait; ear_pipewire's EventSources wraps it with the
// writer's wait for room and the clock wake's eventfd, and the type harness's fake source wraps it with a writer that
// never waits, so a type's tests and the engine play by the same code.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include "ear/core/source.hpp"
namespace ear {

class SourceRing final {
public:
	/// What the ring tells its wrapper. Called on the realtime thread, inside play(): never blocks, locks or allocates;
	/// a wrapper makes at most one syscall in each (an eventfd write), and only when someone waits for it.
	class Signals {
	public:
		/// play() consumed frames (played, skipped or cleared them), or ended a stop: a writer waiting for room may go
		/// on.
		virtual void roomFreed() noexcept = 0;
		/// The source's clock asked after this tick (SourceClock::played returned true): ring its clock wake.
		virtual void clockAsked() noexcept = 0;

	protected:
		~Signals() = default;
	};

	/// Main thread. `capacityFrames` is rounded up to a power of two (FrameRing's); `rate` sets the 5 ms fades
	/// (FrameClock::frames(5 ms), at least 1). `signals` outlives the ring. May allocate; throws std::bad_alloc only.
	SourceRing(std::uint32_t channels, std::uint32_t capacityFrames, SampleRate rate, Signals& signals);
	~SourceRing();
	SourceRing(const SourceRing&) = delete;
	SourceRing& operator=(const SourceRing&) = delete;

	/// The realtime thread, once per tick, by whoever plays the ring (the block through a source's tap, the harness, a
	/// converter): the one play rule (docs/writing-a-node-type.md#sources). Applies a clear asked for; asks `clock` (null
	/// for a source with no clock, which skips nothing) how much of the current stream to skip and drops min(that,
	/// held), nothing on a tick that applies a clear; writes exactly `tick.frames` frames into every span of `bank`;
	/// tells `clock` what the tick did and signals clockAsked when it asks; signals roomFreed when anything was
	/// consumed. Returns what it told the clock (the same with no clock). Never blocks, locks or allocates.
	SourcePlayed play(std::span<const std::span<float>> bank, const SourceTick& tick, SourceClock* clock) noexcept;

	/// The writing thread: takes what fits now, never waits; ends an idle. Returns frames accepted (0 once closed).
	std::uint32_t offer(std::span<const float> interleaved, std::uint32_t frames) noexcept;
	/// The writing thread: the writer is idle until its next offer (an empty ring is no shortfall meanwhile).
	void markIdle() noexcept;
	/// Any ordinary thread: SourceWriter::clear's rule (the next 5 ms played faded out, the rest dropped unplayed).
	void clear() noexcept;
	/// Main thread, once a stop's silence completed: everything in the ring dropped and counted, no fade (nothing plays
	/// until the next drive).
	void dropForStop() noexcept;
	/// Main thread: from now on offer() accepts nothing. Idempotent.
	void close() noexcept;

	// Any thread.
	bool closed() const noexcept;
	std::uint32_t capacityFrames() const noexcept;
	std::uint64_t shortfallFrames() const noexcept;
	std::uint64_t droppedFrames() const noexcept;

private:
	/// The FrameRing, the realtime side's fade state, the atomics; made at construction.
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace ear
