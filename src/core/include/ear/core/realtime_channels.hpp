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
#include <span>
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

/// One live member of a configuration (docs/architecture.md#the-realtime-channels): how the code's view of it moves
/// to a new value, and the value its slot starts at. Made by NodeHost from the type's live rows.
struct LiveMember {
	/// A step (the new value whole from the next block), or a ramp over `frames` on the value itself or on its
	/// logarithm (a log taper's row).
	enum class Move : std::uint8_t { step, linear, log };
	Move move = Move::step;
	bool weight = false;      // a boolean ramp: its bits 0 or 1 ramp as a weight from 0.0 to 1.0
	std::uint32_t frames = 1; // a ramp's length, 1 or more
	std::uint64_t bits = 0;   // the slot's first value
};
/// One live member as the advance last left it (docs/writing-a-node-type.md#live-rows), the code's alone: what io.param
/// builds its view from. Positions count the frames the configuration's codes have processed since its channels were
/// made, so a ramp or a step's edge is where it is whichever code runs.
struct LiveState {
	std::uint64_t bits = 0;    // the value (a step) or the ramp's target, as loaded from the slot
	double from = 0.0;         // a ramp's start, in its domain (ln v for a log ramp)
	double to = 0.0;           // a ramp's target, in its domain
	double toValue = 0.0;      // the target itself
	std::uint64_t startAt = 0; // a ramp's first frame
	std::uint64_t endsAt = 0;  // the first frame that holds the target: at or after it the ramp is steady
	std::uint64_t before = 0;  // a step's value before its last edge
	std::uint64_t edgeAt = 0;  // the block a step's last edge began
	bool edged = false;        // a step has had an edge since the last snap
	bool idle = false;         // above its family's count at the last advance: seated, never ramped
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
	/// with none: the tick drains what is written whenever it runs); `triggers` is how many triggers the spec has, each
	/// a counter (docs/writing-a-node-type.md#actions).
	RealtimeChannels(std::vector<Initial> fields, RealtimeBell* bell, std::size_t triggers = 0,
	                 std::vector<LiveMember> live = {});
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
	/// The fence pairs with take()'s: either the drain's read of the ring sees this cycle's writes, or this exchange
	/// sees the drain's clear and rings (docs/architecture.md#the-realtime-channels).
	void cycleEnded() noexcept {
		if (!wrote_) return;
		wrote_ = false;
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (!pending_.exchange(true, std::memory_order_acq_rel) && bell_ != nullptr) bell_->ring();
	}

	/// Whether trigger `trigger` was invoked since the code last asked: its counter against the last one a code of this
	/// configuration saw, so several invokes before a block are one and a code swapped in sees one unseen.
	bool triggered(std::size_t trigger) noexcept {
		const std::uint32_t now = triggers_[trigger].load(std::memory_order_acquire);
		if (now == seen_[trigger]) return false;
		seen_[trigger] = now;
		return true;
	}

	/// Before each process (docs/architecture.md#the-realtime-channels): a block of `frames` begins. One acquire load
	/// of the generation word when nothing moved since the last block; otherwise every slot is loaded, a snap seats
	/// every member at its slot, and a moved slot starts a ramp from the value its last frame had, or a step's edge.
	/// Never allocates, locks or waits.
	void advanceLive(std::uint32_t frames) noexcept { advanceLive(frames, {}, false); }
	/// As above, for a code whose `idle` members (one flag per member; empty for none) are above their family's count:
	/// each is seated at its slot, never ramped, and a member idle at the last advance and active now is seated too, so
	/// a channel a reshape makes active plays its value from its first sample (docs/writing-a-node-type.md#live-rows).
	/// `fresh`: the code's first block, whose members are loaded whatever the generation.
	void advanceLive(std::uint32_t frames, std::span<const std::uint8_t> idle, bool fresh) noexcept;
	/// The block's first frame, counted from the channels' making; each member's state and declaration.
	std::uint64_t livePosition() const noexcept { return position_; }
	const LiveState& liveState(std::size_t member) const noexcept { return state_[member]; }
	const LiveMember& liveMember(std::size_t member) const noexcept { return members_[member]; }
	/// How many blocks loaded the slots (the generation word had moved): a steady block loads none.
	std::uint64_t liveReloads() const noexcept { return reloads_.load(std::memory_order_relaxed); }

	// ---- The main loop's side ----

	/// How many live members there are: one slot each.
	std::size_t liveCount() const noexcept { return members_.size(); }
	/// Live member `member`'s slot now holds `bits`; the code sees it once publishLive() follows. Wait-free.
	void setLive(std::size_t member, std::uint64_t bits) noexcept {
		live_[member].store(bits, std::memory_order_relaxed);
	}
	/// What setLive wrote since the last publish is the code's from its next block, all in that block (or, when its
	/// load falls inside a batch, the rest one block later). The generation word's write count moves.
	void publishLive() noexcept { storeGeneration(generation_ + 1); }
	/// Before a drive (docs/architecture.md#the-realtime-channels): the next block seats every member at its slot, with
	/// no ramp and every step unchanged. The generation word's snap epoch moves.
	void snapLive() noexcept { storeGeneration((generation_ & ~std::uint64_t{0xffffffff}) + (std::uint64_t{1} << 32)); }
	/// Live member `member`'s slot as the main loop last wrote it.
	std::uint64_t live(std::size_t member) const noexcept { return live_[member].load(std::memory_order_relaxed); }

	/// Trigger `trigger` invoked: its counter moves, wait-free; the code sees it in its next block.
	void trigger(std::size_t trigger) noexcept { triggers_[trigger].fetch_add(1, std::memory_order_release); }
	std::size_t triggers() const noexcept { return triggerCount_; }

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
	/// The generation word (docs/architecture.md#the-realtime-channels): the write count in its low 32 bits, the snap
	/// epoch in its high 32. The main loop is its one writer and keeps its own copy, so neither side needs a
	/// read-modify-write.
	void storeGeneration(std::uint64_t next) noexcept {
		generation_ = next;
		liveGeneration_.store(next, std::memory_order_release);
	}
	const std::vector<LiveMember> members_;
	const std::unique_ptr<std::atomic<std::uint64_t>[]> live_; // the slots: the main loop's to write
	std::atomic<std::uint64_t> liveGeneration_{0};
	std::uint64_t generation_ = 0;             // main loop only: the word as it last stored it
	const std::unique_ptr<LiveState[]> state_; // the code's
	std::uint64_t seenGeneration_ = 0;         // the code's: the word at its last load
	std::uint64_t position_ = 0;               // the code's: this block's first frame
	std::uint64_t next_ = 0;                   // the code's: the next block's
	std::atomic<std::uint64_t> reloads_{0};    // the code's to write
	const std::size_t triggerCount_;
	const std::unique_ptr<std::atomic<std::uint32_t>[]> triggers_; // the main loop's to move
	const std::unique_ptr<std::uint32_t[]> seen_;                  // the code's: each counter as it last saw it
};
} // namespace ear
