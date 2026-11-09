// Every rule a source plays by (ear/core/source_ring.hpp; docs/writing-a-node-type.md#sources holds them in words): its
// ring, keeping time, the fades at its edges, the shortfall, the frames dropped at a clear or a stop, and Played. Pure:
// one FrameRing and atomics, no thread, descriptor or wait; play() allocates nothing (its state is made here, once).
#include "ear/core/source_ring.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

#include "ear/core/frame_ring.hpp"

namespace ear {
namespace {

/// clear()'s word: how many clears were asked (high half) and the ring's write position at the last (low half).
constexpr std::uint64_t oneClear = std::uint64_t{1} << 32U;

} // namespace

struct SourceRing::State {
	State(std::uint32_t channelCount, std::uint32_t capacity, std::uint32_t frequency, Signals& told)
		: ring(channelCount, capacity), channels(channelCount), rate(std::max(frequency, 1U)),
		  fadeFrames(std::max(frequency * 5 / 1000, 1U)), signals(told), last(channelCount, 0.0F),
		  tail(static_cast<std::size_t>(fadeFrames) * channelCount, 0.0F) {}

	FrameRing ring;
	std::uint32_t channels;
	std::uint32_t rate;
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
	// Played, published by play() under a sequence lock: odd while it writes, so a reader on any thread retries and
	// play() never waits.
	std::atomic<std::uint32_t> playedSequence{0};
	std::atomic<std::uint64_t> playedTotal{0};
	std::atomic<std::int64_t> playedHeard{0};

	// play()'s own (the realtime thread's).
	bool started = false;           // a cycle played since the source was made or last stopped
	std::uint64_t expected = 0;     // the position the next cycle starts at, if the graph lost no time
	std::uint32_t clearsSeen = 0;   // clearWord's count as play() last applied it
	std::uint32_t gain = 0;         // the fades' level, 0 (silent) to fadeFrames (full): gain / fadeFrames
	bool clearing = false;          // a clear's fade to zero is not over: new frames wait for it
	std::uint32_t debt = 0;         // frames the clock skipped that the ring did not hold yet: counted played, and
									// dropped from what is written next, so the stream keeps to the clock exactly
	std::vector<float> last;        // the last frame played, which an underrun or a clear decays from
	std::vector<float> tail;        // a clear's next 5 ms, played faded out; interleaved
	std::uint32_t tailAt = 0;       // frames of `tail` played
	std::uint32_t tailFrames = 0;   // frames `tail` holds
	std::uint64_t totalFrames = 0;  // Played::totalFrames, as play() counts it
	std::uint32_t consumedRing = 0; // frames this cycle took out of the ring (played, skipped or dropped)
	bool roomFreed = false;         // this cycle freed room, or ended a stop: a waiting writer may go on
	bool cycleEndedStop = false;    // this cycle is the first since a stop: its zeros are no shortfall

	float level() const noexcept { return static_cast<float>(gain) / static_cast<float>(fadeFrames); }

	/// What the ring holds now (exact on the reading thread).
	std::uint32_t held() const noexcept { return ring.writePosition() - ring.readPosition(); }

	void drop(std::uint64_t frames) noexcept {
		if (frames > 0) dropped.fetch_add(frames, std::memory_order_relaxed);
	}

	/// The first cycle after a stop: what a writer slipped in while the stop was dropping is dropped too, the clear
	/// in flight forgotten, the time kept afresh, and the writer let go (its wait is for room).
	void endStop() noexcept {
		const std::uint32_t stragglers = held();
		ring.skip(stragglers);
		drop(stragglers + (tailFrames - tailAt));
		tailAt = tailFrames = 0;
		clearing = false;
		debt = 0;
		gain = 0;
		started = false;
		clearsSeen = static_cast<std::uint32_t>(clearWord.load(std::memory_order_acquire) >> 32U);
		cycleEndedStop = true;
		roomFreed = true; // the writer waits out a stop as a full ring: always woken here
		stopped.store(false, std::memory_order_release);
	}

	/// A clear asked since the last cycle: the next 5 ms of what was written before it becomes the tail, faded out
	/// from here; the rest is dropped (counted, never played).
	void applyClear() noexcept {
		const std::uint64_t word = clearWord.load(std::memory_order_acquire);
		const auto asked = static_cast<std::uint32_t>(word >> 32U);
		if (asked == clearsSeen) return;
		clearsSeen = asked;
		debt = 0;                  // owed by the stream the clear ends, never by the next one
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
	}

	/// Keeping time: a forward jump of at most the capacity skips that many frames of the stream, counted played: a
	/// clear's tail first, then what the ring holds (not while a clear fades), and what it does not hold yet is a debt,
	/// dropped from the frames written next. A larger jump, or one back, is a new clock. Returns the frames skipped.
	std::uint32_t keepTime(const SourceCycle& cycle) noexcept {
		std::uint32_t skipped = 0;
		if (started && cycle.position > expected && cycle.position - expected <= ring.capacityFrames()) {
			// The whole gap is counted played, whatever of it the ring holds now.
			skipped = static_cast<std::uint32_t>(cycle.position - expected);
			std::uint32_t gap = skipped;
			const std::uint32_t fromTail = std::min(gap, tailFrames - tailAt);
			if (fromTail > 0) {
				tailAt += fromTail;
				std::copy_n(tail.begin() + static_cast<std::ptrdiff_t>(tailAt - 1) * channels, channels, last.begin());
			}
			gap -= fromTail;
			if (!clearing && gap > 0) {
				const std::uint32_t fromRing = ring.skip(gap);
				gap -= fromRing;
				consumedRing += fromRing;
			}
			debt = std::min(debt + gap, ring.capacityFrames());
		}
		started = true;
		expected = cycle.position + cycle.frames;
		return skipped;
	}

	void publish(std::uint64_t total, std::int64_t heardRawNs) noexcept {
		const std::uint32_t sequence = playedSequence.load(std::memory_order_relaxed);
		playedSequence.store(sequence + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		playedTotal.store(total, std::memory_order_relaxed);
		playedHeard.store(heardRawNs, std::memory_order_relaxed);
		playedSequence.store(sequence + 2, std::memory_order_release);
	}
};

SourceRing::SourceRing(std::uint32_t channels, std::uint32_t capacityFrames, std::uint32_t rate, Signals& signals)
	: state_(std::make_unique<State>(std::max(channels, 1U), capacityFrames, rate, signals)) {}

SourceRing::~SourceRing() = default;

void SourceRing::play(std::span<const std::span<float>> bank, const SourceCycle& cycle) noexcept {
	State& s = *state_;
	const std::uint32_t frames = cycle.frames;
	const std::size_t spans = std::min<std::size_t>(bank.size(), s.channels);
	// Spans past the source's channels (a bank wider than the ring: never, by NodeHost) are silent.
	for (std::size_t c = spans; c < bank.size(); ++c) std::fill_n(bank[c].begin(), frames, 0.0F);
	s.consumedRing = 0;
	s.roomFreed = false;
	s.cycleEndedStop = false;
	if (s.stopped.load(std::memory_order_acquire)) s.endStop();
	s.applyClear();
	const std::uint32_t skipped = s.keepTime(cycle);

	const auto put = [&](std::uint32_t at, const float* frame) noexcept {
		const float gain = s.level();
		for (std::size_t c = 0; c < spans; ++c) bank[c][at] = frame[c] * gain;
	};
	std::uint32_t i = 0;
	std::uint32_t played = 0;
	std::uint32_t playedEnd = 0; // where the last frame played this cycle ends
	// A clear's tail, faded out from the level the stream had, counted played.
	for (; i < frames && s.tailAt < s.tailFrames; ++i, ++s.tailAt) {
		const float* frame = s.tail.data() + static_cast<std::size_t>(s.tailAt) * s.channels;
		put(i, frame);
		std::copy_n(frame, s.channels, s.last.begin());
		if (s.gain > 0) --s.gain;
		++played;
		playedEnd = i + 1;
	}
	// The clear's fade goes on from its last frame until silent; only then does the new stream fade in.
	for (; i < frames && s.clearing && s.gain > 0; ++i) {
		put(i, s.last.data());
		--s.gain;
	}
	if (s.tailAt == s.tailFrames && s.gain == 0) s.clearing = false;
	// The ring's frames, fading in from the level the last cycle left.
	while (i < frames && !s.clearing) {
		if (s.debt > 0) { // the frames the clock already passed: dropped, counted played when skipped
			const std::uint32_t paid = s.ring.skip(s.debt);
			s.debt -= paid;
			s.consumedRing += paid;
			if (s.debt > 0) break; // the ring is dry
		}
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
		played += got;
		playedEnd = i;
	}
	// Dry: a decay from the last frame played, then zeros; a shortfall unless the writer is idle (or the cycle ends a
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
	if (dry > 0 && !waitingForClear && !s.cycleEndedStop && !s.idle.load(std::memory_order_relaxed))
		s.shortfall.fetch_add(dry, std::memory_order_relaxed);

	if (played + skipped > 0) {
		s.totalFrames += static_cast<std::uint64_t>(played) + skipped;
		const std::int64_t heardFrames = static_cast<std::int64_t>(playedEnd) + cycle.downstream.maxFrames;
		s.publish(s.totalFrames, cycle.startRawNs + heardFrames * 1'000'000'000 / s.rate);
		s.signals.playedChanged();
	}
	if (s.consumedRing > 0 || s.roomFreed) s.signals.roomFreed();
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
	// First, so an offer from here on accepts nothing; what one slipped in meanwhile the next cycle drops.
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

Played SourceRing::played() const noexcept {
	const State& s = *state_;
	for (;;) {
		const std::uint32_t before = s.playedSequence.load(std::memory_order_acquire);
		const Played seen{s.playedTotal.load(std::memory_order_relaxed), s.playedHeard.load(std::memory_order_relaxed)};
		std::atomic_thread_fence(std::memory_order_acquire);
		if ((before & 1U) == 0 && s.playedSequence.load(std::memory_order_relaxed) == before) return seen;
	}
}

std::uint64_t SourceRing::shortfallFrames() const noexcept { return state_->shortfall.load(std::memory_order_relaxed); }

std::uint64_t SourceRing::droppedFrames() const noexcept { return state_->dropped.load(std::memory_order_relaxed); }

} // namespace ear
