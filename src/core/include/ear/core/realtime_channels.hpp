// What a node's code writes on the realtime thread (docs/architecture.md#the-realtime-channels): one configuration's
// realtime channels, made on the main loop when the configuration is made and freed at teardown step 5, after the
// code. Its `rt` status fields are one atomic word each with a transition counter beside it; its events, status
// changes and log codes are fixed-size records in a single-producer, single-consumer ring the main loop drains; and a
// bell, rung after `process` at most once until the next drain, wakes the main loop. The code's side (put, setWord,
// broke, cycleEnded) never allocates, locks or waits, and makes no system call but the bell's. Io<spec> is its only
// writer (type_spec.hpp); NodeHost its only reader.
//
// Pure: nothing but the standard library.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
namespace ear {
/// Wakes the main loop from the realtime thread (docs/architecture.md#the-realtime-channels): ring() is realtime-safe and never
/// blocks; several rings before the main loop wakes are one. Implemented outside core (ear_pipewire's, a pw_loop event
/// on the main loop), which tells NodeHost::realtimeRung on the main loop.
class RealtimeBell {
public:
	virtual void ring() noexcept = 0;

protected:
	~RealtimeBell() = default;
};

/// One record of the ring, 48 bytes: what it is, the field's, event's or log code's index in its spec's table, how many
/// values it carries, the cycle's start (GraphCycle::startNs, CLOCK_MONOTONIC) and up to four values, each as the
/// 64 bits its kind is stored in (realtimeBits).
struct RealtimeRecord {
	enum class Kind : std::uint8_t { status, event, log };
	Kind kind = Kind::status;
	std::uint8_t count = 0;
	std::uint16_t index = 0;
	std::uint32_t spare = 0;
	std::int64_t startNs = 0;
	std::array<std::uint64_t, 4> values{};
};
static_assert(sizeof(RealtimeRecord) == 48, "a realtime record is 48 bytes");

/// A value the code broke its declaration with (a number outside its range, a decimal that is not finite, a choice
/// index past its values): the first one only, read by the drain, which fails the node with its rule.
struct RealtimeBroken {
	RealtimeRecord::Kind kind = RealtimeRecord::Kind::status;
	std::uint16_t index = 0; // the field's, event's or log code's
	std::uint8_t arg = 0;    // an event's or log code's argument
	std::uint64_t bits = 0;
};

class RealtimeChannels {
public:
	/// The ring's records, and how many of them a log record may find in use: past it a log record is refused, so
	/// status and events keep the last quarter.
	static constexpr std::uint32_t ringSize = 256;
	static constexpr std::uint32_t logRoom = 192;
	/// One field's start: its initial value's bits, when it has one (null has none).
	struct Initial {
		bool known = false;
		std::uint64_t bits = 0;
	};
	/// Main loop. `fields` is one entry per status field of the spec, in declaration order; `bell` may be null (a host
	/// with none: the tick drains what is written whenever it runs).
	RealtimeChannels(std::vector<Initial> fields, RealtimeBell* bell);
	RealtimeChannels(const RealtimeChannels&) = delete;
	RealtimeChannels& operator=(const RealtimeChannels&) = delete;

	// ---- The code's side: the realtime thread, one writer at a time (the code, then a code swapped in after it) ----

	/// Field `field` now holds `bits`: when that differs from the last value the code wrote (its initial value before
	/// any), the word is stored, its transition counted and a status record put in the ring.
	void setWord(std::size_t field, std::uint64_t bits, std::int64_t startNs) noexcept {
		Word& word = words_[field];
		const std::uint32_t transitions = word.transitions.load(std::memory_order_relaxed);
		const bool known = transitions != 0 || initial_[field].known;
		const std::uint64_t last = transitions != 0 ? word.bits.load(std::memory_order_relaxed) : initial_[field].bits;
		if (known && last == bits) return;
		word.bits.store(bits, std::memory_order_release);
		word.transitions.store(transitions + 1, std::memory_order_release);
		put(RealtimeRecord{.kind = RealtimeRecord::Kind::status,
		                   .count = 1,
		                   .index = static_cast<std::uint16_t>(field),
		                   .startNs = startNs,
		                   .values = {bits, 0, 0, 0}});
	}
	/// One record into the ring; refused and counted by kind when the ring is full (a log record once logRoom are in
	/// use).
	void put(const RealtimeRecord& record) noexcept {
		wrote_ = true;
		const std::uint32_t head = head_.load(std::memory_order_relaxed);
		const std::uint32_t used = head - tail_.load(std::memory_order_acquire);
		const bool room = record.kind == RealtimeRecord::Kind::log ? used < logRoom : used < ringSize;
		if (!room) {
			dropped_[static_cast<std::size_t>(record.kind)].fetch_add(1, std::memory_order_relaxed);
			return;
		}
		slots_[head % ringSize] = record;
		head_.store(head + 1, std::memory_order_release);
	}
	/// The code broke a declaration: the first such value is kept for the drain, the rest are moot (the node fails).
	void broke(const RealtimeBroken& broken) noexcept {
		wrote_ = true;
		if (brokenSet_.load(std::memory_order_relaxed)) return;
		broken_ = broken;
		brokenSet_.store(true, std::memory_order_release);
	}
	/// After each process: when the code wrote anything this cycle, the bell is rung, unless it already was since the
	/// last drain.
	void cycleEnded() noexcept {
		if (!wrote_) return;
		wrote_ = false;
		if (!pending_.exchange(true, std::memory_order_acq_rel) && bell_ != nullptr) bell_->ring();
	}

	// ---- The main loop's side ----

	/// What one drain read, in order: the ring's records, the records the ring refused since the last drain by kind,
	/// and the broken value, if the code broke one (said once).
	struct Taken {
		std::vector<RealtimeRecord> records;
		std::array<std::uint64_t, 3> dropped{};
		std::optional<RealtimeBroken> broken;
	};
	/// Everything written since the last take; the next write rings the bell again.
	Taken take();
	/// Field `field`'s word, when the code wrote it since the last sample: its bits and how many times it changed in
	/// between.
	struct Sampled {
		std::uint64_t bits;
		std::uint32_t transitions;
	};
	std::optional<Sampled> sample(std::size_t field);
	std::size_t fields() const noexcept { return initial_.size(); }

private:
	struct Word {
		std::atomic<std::uint64_t> bits{0};
		std::atomic<std::uint32_t> transitions{0};
		std::uint32_t sampled = 0; // main loop only: the transitions at the last sample
	};
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free,
	              "the realtime channels' words are lock-free");
	const std::vector<Initial> initial_;
	const std::unique_ptr<Word[]> words_;
	RealtimeBell* const bell_;
	std::unique_ptr<RealtimeRecord[]> slots_;
	std::atomic<std::uint32_t> head_{0}; // the code's
	std::atomic<std::uint32_t> tail_{0}; // the drain's
	std::array<std::atomic<std::uint64_t>, 3> dropped_{};
	RealtimeBroken broken_{}; // written once by the code before brokenSet_, read by the drain after it
	std::atomic<bool> brokenSet_{false};
	bool brokenTaken_ = false; // main loop only
	std::atomic<bool> pending_{false};
	bool wrote_ = false; // the code's: it wrote in this cycle
};
} // namespace ear
