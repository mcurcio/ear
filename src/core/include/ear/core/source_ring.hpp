// The rules every source plays by, in one place (docs/writing-a-node-type.md#sources): its ring, keeping time, the
// fades at its edges, the shortfall, the frames dropped at a clear or a stop, and Played. Pure: no thread, no
// descriptor, no wait; ear_pipewire's EventSources wraps it with the writer's wait for room and the played wake's
// eventfd, and the type harness's fake source wraps it with a writer that never waits, so a type's tests and the engine
// play by the same code.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include "ear/core/source.hpp"
namespace ear {

class SourceRing final : public SourceTap {
public:
	/// What the ring tells its wrapper. Called on the realtime thread, inside play(): never blocks, locks or allocates;
	/// a wrapper makes at most one syscall in each (an eventfd write), and only when someone waits for it.
	class Signals {
	public:
		/// play() consumed frames (played, skipped or cleared them): a writer waiting for room may go on.
		virtual void roomFreed() noexcept = 0;
		/// play() published a new Played.
		virtual void playedChanged() noexcept = 0;

	protected:
		~Signals() = default;
	};

	/// Main thread. `capacityFrames` is rounded up to a power of two (FrameRing's); `rate` sets the 5 ms fades
	/// (rate * 5 / 1000 frames, at least 1) and Played's dating. `signals` outlives the ring. May allocate; throws
	/// std::bad_alloc only.
	SourceRing(std::uint32_t channels, std::uint32_t capacityFrames, std::uint32_t rate, Signals& signals);
	~SourceRing();
	SourceRing(const SourceRing&) = delete;
	SourceRing& operator=(const SourceRing&) = delete;

	/// SourceTap: the realtime thread, once per cycle a code runs.
	void play(std::span<const std::span<float>> bank, const SourceCycle& cycle) noexcept override;

	/// The writing thread: takes what fits now, never waits; ends an idle. Returns frames accepted (0 once closed).
	std::uint32_t offer(std::span<const float> interleaved, std::uint32_t frames) noexcept;
	/// The writing thread: the writer is idle until its next offer (an empty ring is no shortfall meanwhile).
	void markIdle() noexcept;
	/// Any ordinary thread: SourceWriter::clear's rule (the next 5 ms played faded out, the rest dropped unplayed).
	void clear() noexcept;
	/// Main thread, once a stop's silence completed: everything in the ring dropped and counted, no fade (nothing plays
	/// until the next drive), the time kept from the next played cycle afresh.
	void dropForStop() noexcept;
	/// Main thread: from now on offer() accepts nothing. Idempotent.
	void close() noexcept;

	// Any thread.
	bool closed() const noexcept;
	std::uint32_t capacityFrames() const noexcept;
	Played played() const noexcept;
	std::uint64_t shortfallFrames() const noexcept;
	std::uint64_t droppedFrames() const noexcept;

private:
	/// The FrameRing, the realtime side's fade and time-keeping state, the atomics; made at construction.
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace ear
