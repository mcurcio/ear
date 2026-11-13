// Every rule a source plays by (ear/core/source_ring.hpp; docs/writing-a-node-type.md#sources holds them in words): its
// ring, the fades at its edges, the shortfall, the frames dropped at a clear or a stop, and the one play rule around a
// source's clock. It keeps no audio time (docs/architecture.md#the-timing-model, rule 1): what to skip is the clock's
// alone. Pure: one FrameRing and atomics, no thread, descriptor or wait; play() allocates nothing (its state is made
// here, once).
#include "ear/core/source_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

#include "ear/core/frame_ring.hpp"
#include "ear/core/latency.hpp"

namespace ear {
namespace {

/// clear()'s word: how many clears were asked (high half) and the ring's write position at the last (low half).
constexpr std::uint64_t oneClear = std::uint64_t{1} << 32U;

/// 5 ms at `rate`, at least one frame: every fade's length.
std::uint32_t fadeFramesAt(std::uint32_t rate) {
	return std::max(static_cast<std::uint32_t>(FrameClock(std::max(rate, 1U)).frames(std::chrono::milliseconds{5})),
	                1U);
}

} // namespace

struct SourceRing::State {
	State(std::uint32_t channelCount, std::uint32_t capacity, std::uint32_t frequency, Signals& told)
		: ring(channelCount, capacity), channels(channelCount), fadeFrames(fadeFramesAt(frequency)), signals(told),
		  last(channelCount, 0.0F), tail(static_cast<std::size_t>(fadeFrames) * channelCount, 0.0F) {}

	FrameRing ring;
	std::uint32_t channels;
	std::uint32_t fadeFrames; // 5 ms: every fade's length
	Signals& signals;

	// Written by the writer, the main thread or a clearing thread; read by play().
	std::atomic<bool> closed{false};
	std::atomic<bool> idle{true};     // until the writer's first offer, an empty ring is no shortfall
	std::atomic<bool> stopped{false}; // dropForStop ran and no cycle played since: offer accepts nothing
	std::atomic<std::uint64_t> clearWord{0};
	// Counters, any thread.
	std::atomic<std::uint64_t> shortfall{0};
	std::atomic<std::uint64_t> dropped{0};

	// play()'s own (the realtime thread's).
	std::uint32_t clearsSeen = 0;   // clearWord's count as play() last applied it
	std::uint32_t gain = 0;         // the fades' level, 0 (silent) to fadeFrames (full): gain / fadeFrames
	bool clearing = false;          // a clear's fade to zero is not over: new frames wait for it
	std::vector<float> last;        // the last frame played, which an underrun or a clear decays from
	std::vector<float> tail;        // a clear's next 5 ms, played faded out; interleaved
	std::uint32_t tailAt = 0;       // frames of `tail` played
	std::uint32_t tailFrames = 0;   // frames `tail` holds
	std::uint32_t consumedRing = 0; // frames this tick took out of the ring (played, skipped or dropped)
	bool roomFreed = false;         // this tick freed room, or ended a stop: a waiting writer may go on
	bool tickEndedStop = false;     // this tick is the first since a stop: its zeros are no shortfall

	float level() const noexcept { return static_cast<float>(gain) / static_cast<float>(fadeFrames); }

	/// What the ring holds now (exact on the reading thread).
	std::uint32_t held() const noexcept { return ring.writePosition() - ring.readPosition(); }

	void drop(std::uint64_t frames) noexcept {
		if (frames > 0) dropped.fetch_add(frames, std::memory_order_relaxed);
	}

	/// The first tick after a stop: what a writer slipped in while the stop was dropping is dropped too, the clear in
	/// flight forgotten, and the writer let go (its wait is for room).
	void endStop() noexcept {
		const std::uint32_t stragglers = held();
		ring.skip(stragglers);
		drop(stragglers + (tailFrames - tailAt));
		tailAt = tailFrames = 0;
		clearing = false;
		gain = 0;
		clearsSeen = static_cast<std::uint32_t>(clearWord.load(std::memory_order_acquire) >> 32U);
		tickEndedStop = true;
		roomFreed = true; // the writer waits out a stop as a full ring: always woken here
		stopped.store(false, std::memory_order_release);
	}

	/// A clear asked since the last tick: the next 5 ms of what was written before it becomes the tail, faded out from
	/// here; the rest is dropped (counted, never played). True when this tick applied one.
	bool applyClear() noexcept {
		const std::uint64_t word = clearWord.load(std::memory_order_acquire);
		const auto asked = static_cast<std::uint32_t>(word >> 32U);
		if (asked == clearsSeen) return false;
		clearsSeen = asked;
		drop(tailFrames - tailAt); // an earlier clear's tail, not played yet
		tailAt = tailFrames = 0;
		const auto to = static_cast<std::uint32_t>(word);
		const auto before = static_cast<std::int32_t>(to - ring.readPosition());
		std::uint32_t cleared = before > 0 ? std::min(static_cast<std::uint32_t>(before), held()) : 0;
		while (tailFrames < fadeFrames && cleared > 0) {
			const std::span<const float> run = ring.readSpan();
			const std::uint32_t frames =
				std::min({static_cast<std::uint32_t>(run.size() / channels), fadeFrames - tailFrames, cleared});
			if (frames == 0) break;
			std::copy_n(run.data(), static_cast<std::size_t>(frames) * channels,
			            tail.begin() + static_cast<std::ptrdiff_t>(tailFrames) * channels);
			ring.skip(frames);
			tailFrames += frames;
			cleared -= frames;
			consumedRing += frames;
		}
		consumedRing += ring.skip(cleared);
		drop(cleared);
		clearing = true;
		return true;
	}
};

SourceRing::SourceRing(std::uint32_t channels, std::uint32_t capacityFrames, std::uint32_t rate, Signals& signals)
	: state_(std::make_unique<State>(std::max(channels, 1U), capacityFrames, rate, signals)) {}

SourceRing::~SourceRing() = default;

SourcePlayed SourceRing::play(std::span<const std::span<float>> bank, const SourceTick& tick,
                              SourceClock* clock) noexcept {
	State& s = *state_;
	const std::uint32_t frames = tick.frames;
	const std::size_t spans = std::min<std::size_t>(bank.size(), s.channels);
	// Spans past the source's channels (a bank wider than the ring: never, by NodeHost) are silent.
	for (std::size_t c = spans; c < bank.size(); ++c) std::fill_n(bank[c].begin(), frames, 0.0F);
	s.consumedRing = 0;
	s.roomFreed = false;
	s.tickEndedStop = false;
	if (s.stopped.load(std::memory_order_acquire)) s.endStop();
	const bool clearedNow = s.applyClear();

	SourcePlayed played{};
	// The clock's skip, of the current stream only, never more than the ring holds of it, and nothing on the tick that
	// applies a clear (docs/writing-a-node-type.md#a-sources-clock). With no clock nothing is skipped: the ring keeps
	// no time of its own (docs/architecture.md#the-timing-model, rule 1).
	if (clock != nullptr) {
		const SourceHeld held{.frames = s.held(), .clearing = clearedNow};
		const std::uint64_t asked = clock->skipBefore(tick, held);
		if (!held.clearing && asked > 0) {
			const auto skip = static_cast<std::uint32_t>(std::min<std::uint64_t>(asked, held.frames));
			s.ring.skip(skip);
			s.consumedRing += skip;
			played.skipped = skip;
		}
	}

	const auto put = [&](std::uint32_t at, const float* frame) noexcept {
		const float gain = s.level();
		for (std::size_t c = 0; c < spans; ++c) bank[c][at] = frame[c] * gain;
	};
	std::uint32_t i = 0;
	// A clear's tail, faded out from the level the stream had: the ended stream's frames.
	for (; i < frames && s.tailAt < s.tailFrames; ++i, ++s.tailAt) {
		const float* frame = s.tail.data() + static_cast<std::size_t>(s.tailAt) * s.channels;
		put(i, frame);
		std::copy_n(frame, s.channels, s.last.begin());
		if (s.gain > 0) --s.gain;
		++played.ended;
	}
	// The clear's fade goes on from its last frame until silent; only then does the new stream fade in.
	for (; i < frames && s.clearing && s.gain > 0; ++i) {
		put(i, s.last.data());
		--s.gain;
		++played.zeros;
	}
	if (s.tailAt == s.tailFrames && s.gain == 0) s.clearing = false;
	// The ring's frames, fading in from the level the last tick left.
	while (i < frames && !s.clearing) {
		const std::span<const float> run = s.ring.readSpan();
		const std::uint32_t got = std::min(static_cast<std::uint32_t>(run.size() / s.channels), frames - i);
		if (got == 0) break;
		for (std::uint32_t f = 0; f < got; ++f) {
			put(i + f, run.data() + static_cast<std::size_t>(f) * s.channels);
			if (s.gain < s.fadeFrames) ++s.gain;
		}
		std::copy_n(run.data() + static_cast<std::size_t>(got - 1) * s.channels, s.channels, s.last.begin());
		s.ring.skip(got);
		s.consumedRing += got;
		i += got;
		played.read += got;
	}
	// Dry: a decay from the last frame played, then zeros; a shortfall unless the writer is idle (or the tick ends a
	// stop, the writer held back until now) or a clear's fade is what keeps the frames back.
	const std::uint32_t dry = frames - i;
	const bool waitingForClear = s.clearing;
	for (; i < frames; ++i) {
		if (s.gain > 0) {
			put(i, s.last.data());
			--s.gain;
		} else {
			for (std::size_t c = 0; c < spans; ++c) bank[c][i] = 0.0F;
		}
	}
	played.zeros += dry;
	if (dry > 0 && !waitingForClear && !s.tickEndedStop && !s.idle.load(std::memory_order_relaxed))
		s.shortfall.fetch_add(dry, std::memory_order_relaxed);

	if (clock != nullptr && clock->played(tick, played)) s.signals.clockAsked();
	if (s.consumedRing > 0 || s.roomFreed) s.signals.roomFreed();
	return played;
}

std::uint32_t SourceRing::offer(std::span<const float> interleaved, std::uint32_t frames) noexcept {
	State& s = *state_;
	if (frames == 0 || s.closed.load(std::memory_order_acquire) || s.stopped.load(std::memory_order_acquire)) return 0;
	s.idle.store(false, std::memory_order_relaxed);
	const auto whole = static_cast<std::uint32_t>(std::min<std::size_t>(interleaved.size() / s.channels, frames));
	return s.ring.writeInterleaved(interleaved, whole);
}

void SourceRing::markIdle() noexcept { state_->idle.store(true, std::memory_order_relaxed); }

void SourceRing::clear() noexcept {
	State& s = *state_;
	std::uint64_t word = s.clearWord.load(std::memory_order_relaxed);
	std::uint64_t next = 0;
	do {
		next = ((word & ~std::uint64_t{0xFFFFFFFFU}) + oneClear) | s.ring.writePosition();
	} while (!s.clearWord.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_relaxed));
}

void SourceRing::dropForStop() noexcept {
	State& s = *state_;
	// First, so an offer from here on accepts nothing; what one slipped in meanwhile the next tick drops.
	s.stopped.store(true, std::memory_order_release);
	const std::uint32_t from = s.ring.readPosition();
	const std::uint32_t to = s.ring.writePosition();
	s.ring.skipTo(to);
	s.drop(to - from);
	s.idle.store(true, std::memory_order_relaxed);
}

void SourceRing::close() noexcept { state_->closed.store(true, std::memory_order_release); }

bool SourceRing::closed() const noexcept { return state_->closed.load(std::memory_order_acquire); }

std::uint32_t SourceRing::capacityFrames() const noexcept { return state_->ring.capacityFrames(); }

std::uint64_t SourceRing::shortfallFrames() const noexcept { return state_->shortfall.load(std::memory_order_relaxed); }

std::uint64_t SourceRing::droppedFrames() const noexcept { return state_->dropped.load(std::memory_order_relaxed); }

} // namespace ear
