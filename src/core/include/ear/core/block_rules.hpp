// The rules a port block applies each cycle, pure (docs/engine.md#port-blocks): the fade's ramp
// (docs/engine.md#drive-and-fade), when a cycle is a discontinuity (docs/engine.md#graphcycle), and the stamp a sink's
// chunk and a source's cycle carry (docs/engine.md#sinks, docs/engine.md#sources). ear_pipewire's block and the type
// harness's fake block both run these, so a type's tests see what the engine does (docs/writing-a-node-type.md#testing-a-type).
// Realtime-safe: nothing here locks, allocates or waits. Each object belongs to one thread, the one that runs the
// block's cycles (the data loop, or the test's thread in the harness).
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include "ear/core/latency.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/sink.hpp"
#include "ear/core/source.hpp"
namespace ear {

/// How long a fade lasts from full gain to zero, or back.
inline constexpr std::chrono::milliseconds fadeLength{5};

/// The fade on a block's faded outputs (docs/engine.md#drive-and-fade): a linear ramp of fadeLength in whole frames
/// (at least one), applied sample by sample after the code, exact at both ends. A drive starts it in from zero; a
/// fade-out ramps from the gain it has down to zero and ends when a cycle ended there.
class FadeRamp {
public:
	/// `rate` > 0: the graph's.
	explicit FadeRamp(std::uint32_t rate) noexcept
		: frames_(std::max(static_cast<std::uint32_t>(FrameClock(rate).frames(fadeLength)), 1U)) {}

	/// The ramp's length in frames.
	std::uint32_t frames() const noexcept { return frames_; }
	/// A drive was applied: the new code ramps in from zero, or, with nothing faded, plays at full gain.
	void driven(bool anyFaded) noexcept {
		mode_ = anyFaded ? Mode::in : Mode::none;
		at_ = 0;
	}
	/// A fade-out was applied: from the next sample the faded outputs ramp from the gain they have down to zero.
	void fadeOut() noexcept { mode_ = Mode::out; }
	/// The faded outputs are at zero at once: a cycle wrote zeros (one too large to run), or none will come to end the
	/// ramp (a block not processing). A fade-out under way has ended.
	void silenced() noexcept { at_ = 0; }
	/// A fade-out is applied and has not been undone by a drive (ended or not).
	bool fadingOut() const noexcept { return mode_ == Mode::out; }

	/// What one cycle's ramp did: the gain on the last sample (1 with no fade under way), and whether a fade-out
	/// ended at zero, which acknowledges it.
	struct Applied {
		float lastGain;
		bool outEnded;
	};
	/// The ramp over `frames` samples of every output whose `mask` entry is not zero (`mask` as long as `outputs`, each
	/// span at least `frames` long).
	Applied apply(std::span<const std::span<float>> outputs, std::span<const std::uint8_t> mask,
	              std::uint32_t frames) noexcept {
		if (mode_ == Mode::none) return {1.0F, false};
		float gain = 1.0F;
		for (std::uint32_t i = 0; i < frames; ++i) {
			gain = static_cast<float>(at_) / static_cast<float>(frames_);
			for (std::size_t o = 0; o < outputs.size() && o < mask.size(); ++o)
				if (mask[o] != 0) outputs[o][i] = at_ == 0 ? 0.0F : outputs[o][i] * gain;
			if (mode_ == Mode::in && at_ < frames_) ++at_;
			else if (mode_ == Mode::out && at_ > 0) --at_;
		}
		if (mode_ == Mode::in && at_ == frames_) mode_ = Mode::none;
		return {mode_ == Mode::none ? 1.0F : gain, mode_ == Mode::out && at_ == 0};
	}

private:
	enum class Mode : std::uint8_t { none, in, out };
	std::uint32_t frames_;
	Mode mode_ = Mode::none;
	std::uint32_t at_ = 0; // the gain's numerator: the next sample's gain is at_ / frames_
};

/// When a cycle is a discontinuity to the code (GraphCycle::discontinuity, docs/engine.md#graphcycle): the block's
/// first cycle, a new code's first, a new driver's first, a position that did not move on by the last cycle's frames,
/// and the cycle after one too large to run. Counts the cycles that started ahead of where the last ended on the same
/// driver (GraphCycle::xruns).
class CycleContinuity {
public:
	/// A drive installed a code: its first cycle is a discontinuity to it.
	void driven() noexcept { fresh_ = true; }

	/// One cycle of `frames` at `position` on driver `driverId`, in the order cycles come.
	struct Step {
		bool discontinuity;
		bool runs; // at most maxBlock frames: the code runs it (a larger cycle is written as zeros)
	};
	Step next(std::uint32_t driverId, std::uint64_t position, std::uint32_t frames, std::uint32_t maxBlock) noexcept {
		const bool sameDriver = cycles_ > 0 && driverId == lastDriver_;
		const bool discontinuity = !sameDriver || position != nextPosition_ || fresh_ || skipped_;
		if (sameDriver && position > nextPosition_) ++xruns_;
		fresh_ = false;
		skipped_ = frames > maxBlock;
		++cycles_;
		lastDriver_ = driverId;
		nextPosition_ = position + frames;
		return {discontinuity, !skipped_};
	}
	/// Where the next cycle starts on the same driver: the position a drive applied now takes effect from.
	std::uint64_t nextPosition() const noexcept { return nextPosition_; }
	std::uint64_t xruns() const noexcept { return xruns_; }

private:
	std::uint64_t cycles_ = 0;
	std::uint32_t lastDriver_ = 0;
	std::uint64_t nextPosition_ = 0;
	std::uint64_t xruns_ = 0;
	bool fresh_ = false;   // a drive installed a code since the last cycle
	bool skipped_ = false; // the last cycle was too large to run
};

/// The stamp a sink's tap is written with for `cycle` (docs/engine.md#sinks): the cycle's facts and the most of the
/// capture latency the block last told on the sink's bank.
constexpr ChunkStamp chunkStamp(const GraphCycle& cycle, Latency toldOnBank) noexcept {
	return ChunkStamp{.position = cycle.position,
	                  .startNs = cycle.startNs,
	                  .startRawNs = cycle.startRawNs,
	                  .discontinuity = cycle.discontinuity,
	                  .upstreamFrames = toldOnBank.maxFrames};
}

/// The cycle a source's tap plays for `cycle` of `frames` (docs/engine.md#sources): its facts and the playback
/// latency the block last told on the source's bank.
constexpr SourceCycle sourceCycle(const GraphCycle& cycle, std::uint32_t frames, Latency toldOnBank) noexcept {
	return SourceCycle{.position = cycle.position,
	                   .frames = frames,
	                   .startRawNs = cycle.startRawNs,
	                   .discontinuity = cycle.discontinuity,
	                   .downstream = toldOnBank};
}

} // namespace ear
