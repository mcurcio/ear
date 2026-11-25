// The rules a port block applies each cycle, pure (docs/engine.md#port-blocks): the fade's ramp and when a fade-out
// completes (docs/engine.md#drive-and-fade), and the stamp a sink's chunk and a source's cycle carry
// (docs/engine.md#sinks, docs/engine.md#sources); each cycle's facts are CycleAccount's (ear/core/cycle_facts.hpp).
// ear_pipewire's block and the type harness's fake block both run these, so a type's tests see what the engine does
// (docs/writing-a-node-type.md#testing-a-type).
// Realtime-safe: nothing here locks, allocates or waits. Each object belongs to one thread, the one that runs the
// block's cycles (the data loop, or the test's thread in the harness).
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
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
	/// `rate`: the graph's.
	explicit FadeRamp(SampleRate rate) noexcept
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
		float gain = static_cast<float>(at_) / static_cast<float>(frames_); // no sample: the gain it holds
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

/// A block's fade and the fade-outs it owes an answer (docs/engine.md#drive-and-fade). Requests are applied in the
/// order asked; a drive completes when applied, a fade-out waits. Every fade-out waiting is acknowledged (its `done`
/// runs) by the first cycle that ends at zero, by a cycle too large to run (written as zeros), or at once when the
/// block is not processing; a drive applied while fade-outs wait cancels them (they complete, their `done` never
/// runs). Each call returns how many waiting fade-outs it completed, the oldest first: the block marks them in its
/// request ring, the harness in its pending list.
class BlockFade {
public:
	/// `rate`: the graph's.
	explicit BlockFade(SampleRate rate) noexcept : ramp_(rate) {}

	/// A fade-out applied: it ramps from the next sample, and waits.
	void fadeOut() noexcept {
		ramp_.fadeOut();
		++waiting_;
	}
	/// A drive applied: its code ramps in (with anything faded); the fade-outs waiting are cancelled.
	std::uint64_t driven(bool anyFaded) noexcept {
		ramp_.driven(anyFaded);
		return std::exchange(waiting_, 0);
	}
	/// What one cycle's fade did, and the fade-outs the cycle acknowledged by ending at zero.
	struct Applied {
		float lastGain;
		std::uint64_t acknowledged;
	};
	/// One cycle the code ran: FadeRamp::apply.
	Applied apply(std::span<const std::span<float>> outputs, std::span<const std::uint8_t> mask,
	              std::uint32_t frames) noexcept {
		const FadeRamp::Applied applied = ramp_.apply(outputs, mask, frames);
		return {applied.lastGain, applied.outEnded ? std::exchange(waiting_, 0) : 0};
	}
	/// A cycle too large to run, every output written as zeros: a fade-out under way ends at zero there.
	std::uint64_t wroteZeros() noexcept {
		if (!ramp_.fadingOut()) return 0;
		ramp_.silenced();
		return std::exchange(waiting_, 0);
	}
	/// The block is not processing, so no cycle will come to end a fade-out: the fade-outs waiting end at zero now.
	std::uint64_t notProcessing() noexcept {
		if (waiting_ == 0) return 0;
		ramp_.silenced();
		return std::exchange(waiting_, 0);
	}
	/// Fade-outs applied and not yet completed.
	std::uint64_t waiting() const noexcept { return waiting_; }
	std::uint32_t frames() const noexcept { return ramp_.frames(); }

private:
	FadeRamp ramp_;
	std::uint64_t waiting_ = 0;
};

/// The holds on a block's outputs (docs/engine.md#holds-and-swaps): a second gain per output, after the fade and before
/// the sinks and the meters, so a held output's meters and links read the silence. A hold ramps its outputs linearly to
/// zero over fadeLength (whole frames, at least one) and keeps them there; a release ramps them back to full gain. Each
/// hold waits for its acknowledgement: the end of the first cycle at which every held output is at zero, a cycle too
/// large to run (written as zeros), or at once on a block not processing. Each call returns how many holds waiting it
/// acknowledged. Sized at its making (main thread); nothing after allocates.
class HoldRamp {
public:
	/// `rate`: the graph's; `outputs`: the block's, outputs then inside outputs.
	HoldRamp(SampleRate rate, std::size_t outputs)
		: frames_(std::max(static_cast<std::uint32_t>(FrameClock(rate).frames(fadeLength)), 1U)), at_(outputs, frames_),
		  held_(outputs, 0) {}

	std::uint32_t frames() const noexcept { return frames_; }
	std::size_t outputs() const noexcept { return at_.size(); }
	/// Holds the outputs `mask` marks (one entry per output; a shorter mask leaves the rest): from the next sample each
	/// ramps to zero from the gain it has. Waits for its acknowledgement.
	void hold(std::span<const std::uint8_t> mask) noexcept {
		for (std::size_t o = 0; o < held_.size() && o < mask.size(); ++o)
			if (mask[o] != 0) held_[o] = 1;
		++asked_;
	}
	/// Releases the outputs `mask` marks: from the next sample each ramps back to full gain.
	void release(std::span<const std::uint8_t> mask) noexcept {
		for (std::size_t o = 0; o < held_.size() && o < mask.size(); ++o)
			if (mask[o] != 0) held_[o] = 0;
	}
	/// A drive applied: every hold ends at full gain at once (the drive's own fade-in covers the faded outputs), and the
	/// holds waiting are acknowledged.
	std::uint64_t driven() noexcept {
		std::ranges::fill(at_, frames_);
		std::ranges::fill(held_, std::uint8_t{0});
		return acknowledge();
	}
	/// The ramp over `frames` samples of every output (`outputs` as long as the ramp's outputs, each span at least
	/// `frames` long); the holds this cycle acknowledged by ending with every held output at zero.
	std::uint64_t apply(std::span<const std::span<float>> outputs, std::uint32_t frames) noexcept {
		for (std::size_t o = 0; o < outputs.size() && o < at_.size(); ++o) {
			std::uint32_t& at = at_[o];
			const bool held = held_[o] != 0;
			if (!held && at == frames_) continue; // full gain: nothing to do
			for (std::uint32_t i = 0; i < frames; ++i) {
				outputs[o][i] = at == 0 ? 0.0F : outputs[o][i] * (static_cast<float>(at) / static_cast<float>(frames_));
				if (held && at > 0) --at;
				else if (!held && at < frames_) ++at;
			}
		}
		return silentNow() ? acknowledge() : 0;
	}
	/// A cycle too large to run, every output written as zeros: the held outputs are at zero there.
	std::uint64_t wroteZeros() noexcept { return notProcessing(); }
	/// The block is not processing, so no cycle will come: the held outputs are at zero now, and every hold waiting is
	/// acknowledged.
	std::uint64_t notProcessing() noexcept {
		for (std::size_t o = 0; o < at_.size(); ++o)
			if (held_[o] != 0) at_[o] = 0;
		return acknowledge();
	}
	/// The gain output `output` has now (1 at full gain), and whether it is held.
	float gain(std::size_t output) const noexcept {
		return output < at_.size() ? static_cast<float>(at_[output]) / static_cast<float>(frames_) : 1.0F;
	}
	bool held(std::size_t output) const noexcept { return output < held_.size() && held_[output] != 0; }
	/// Holds applied and not yet acknowledged.
	std::uint64_t waiting() const noexcept { return asked_ - acked_; }
	/// A reshape's layout taking over from `from` (docs/engine.md#reshapes): output o takes the gain and the hold of
	/// `from`'s output map[o] (negative: a new output, at full gain and not held), and the holds waiting carry on.
	/// Realtime-safe: both are sized already.
	void carry(const HoldRamp& from, std::span<const std::int32_t> map) noexcept {
		for (std::size_t o = 0; o < at_.size(); ++o) {
			const std::int32_t old = o < map.size() ? map[o] : -1;
			const bool kept = old >= 0 && static_cast<std::size_t>(old) < from.at_.size();
			at_[o] = kept ? from.at_[static_cast<std::size_t>(old)] : frames_;
			held_[o] = kept ? from.held_[static_cast<std::size_t>(old)] : std::uint8_t{0};
		}
		asked_ = from.asked_;
		acked_ = from.acked_;
	}

private:
	bool silentNow() const noexcept {
		for (std::size_t o = 0; o < at_.size(); ++o)
			if (held_[o] != 0 && at_[o] != 0) return false;
		return true;
	}
	std::uint64_t acknowledge() noexcept { return asked_ - std::exchange(acked_, asked_); }
	std::uint32_t frames_;
	std::vector<std::uint32_t> at_;  // each output's gain numerator: the next sample's gain is at_ / frames_
	std::vector<std::uint8_t> held_; // each output held
	std::uint64_t asked_ = 0;        // holds applied
	std::uint64_t acked_ = 0;        // holds acknowledged
};

/// The frames the graph lost before a cycle, as a sink's chunk says them (Chunk::lostFrames): a catch-up's jump, else
/// none (docs/writing-a-node-type.md#sinks).
constexpr std::uint64_t lostFrames(const CycleFacts& facts) noexcept {
	return facts.causes.has(CycleCause::catchUp) && facts.jumpFrames > 0 ? static_cast<std::uint64_t>(facts.jumpFrames)
	                                                                     : 0;
}

/// The stamp a sink's tap is written with for `cycle` (docs/engine.md#sinks): the cycle's facts and the most of the
/// capture latency the block last told on the sink's bank.
constexpr ChunkStamp chunkStamp(const GraphCycle& cycle, Latency toldOnBank) noexcept {
	return ChunkStamp{.position = cycle.position,
	                  .startNs = cycle.startNs,
	                  .startRawNs = cycle.startRawNs,
	                  .causes = cycle.facts.causes,
	                  .lostFrames = lostFrames(cycle.facts),
	                  .upstreamFrames = toldOnBank.maxFrames};
}

/// The tick a source's tap plays for `cycle` of `frames` at the graph's `rate` (docs/engine.md#sources): the cycle
/// whole, and the playback latency the block last told on the source's bank. `cycle` outlives the tick.
constexpr SourceTick sourceTick(const GraphCycle& cycle, std::uint32_t frames, SampleRate rate,
                                Latency toldOnBank) noexcept {
	return SourceTick{.cycle = cycle, .frames = frames, .rate = rate, .playback = toldOnBank};
}

} // namespace ear
