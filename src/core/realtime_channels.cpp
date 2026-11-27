#include "ear/core/realtime_channels.hpp"
#include <bit>
#include <cmath>
#include <utility>
namespace ear {
namespace {
/// A ramped member's value in its ramp's domain: a boolean's weight, a decimal itself, a log ramp's logarithm.
double domainOf(const LiveMember& member, std::uint64_t bits) noexcept {
	const double value = member.weight ? (bits != 0 ? 1.0 : 0.0) : std::bit_cast<double>(bits);
	return member.move == LiveMember::Move::log ? std::log(value) : value;
}
double valueOf(const LiveMember& member, std::uint64_t bits) noexcept {
	return member.weight ? (bits != 0 ? 1.0 : 0.0) : std::bit_cast<double>(bits);
}
/// At `bits`, with no ramp and no edge.
void seat(LiveState& state, const LiveMember& member, std::uint64_t bits) noexcept {
	state.bits = bits;
	state.before = bits;
	state.edged = false;
	if (member.move != LiveMember::Move::step) {
		state.from = state.to = domainOf(member, bits);
		state.toValue = valueOf(member, bits);
		state.startAt = state.endsAt = 0;
	}
}
} // namespace

RealtimeChannels::RealtimeChannels(std::vector<Initial> fields, RealtimeBell* bell, std::size_t triggers,
                                   std::vector<LiveMember> live)
	: initial_(std::move(fields)), words_(std::make_unique<Word[]>(initial_.size())), bell_(bell),
	  slots_(std::make_unique<RealtimeRecord[]>(ringSize)), members_(std::move(live)),
	  live_(std::make_unique<std::atomic<std::uint64_t>[]>(members_.size())),
	  state_(std::make_unique<LiveState[]>(members_.size())), triggerCount_(triggers),
	  triggers_(std::make_unique<std::atomic<std::uint32_t>[]>(triggers)),
	  seen_(std::make_unique<std::uint32_t[]>(triggers)) {
	for (std::size_t member = 0; member < members_.size(); ++member) {
		live_[member].store(members_[member].bits, std::memory_order_relaxed);
		seat(state_[member], members_[member], members_[member].bits);
	}
}

void RealtimeChannels::advanceLive(std::uint32_t frames, std::span<const std::uint8_t> idle, bool fresh) noexcept {
	position_ = next_;
	next_ = position_ + frames;
	const std::uint64_t now = liveGeneration_.load(std::memory_order_acquire);
	if (now == seenGeneration_ && !fresh) return; // the steady block: one load, nothing written but the positions
	const bool snap = (now >> 32) != (seenGeneration_ >> 32);
	if (now != seenGeneration_) reloads_.store(reloads_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	seenGeneration_ = now;
	for (std::size_t member = 0; member < members_.size(); ++member) {
		const std::uint64_t bits = live_[member].load(std::memory_order_relaxed);
		LiveState& state = state_[member];
		const LiveMember& declared = members_[member];
		const bool idleNow = member < idle.size() && idle[member] != 0;
		// An idle member, and one just made active, sit at their value: no code heard a ramp of it.
		if (snap || idleNow || state.idle) {
			if (snap || bits != state.bits || state.idle != idleNow) seat(state, declared, bits);
			state.idle = idleNow;
			continue;
		}
		if (bits == state.bits) continue;
		if (declared.move == LiveMember::Move::step) {
			state.before = state.bits;
			state.bits = bits;
			state.edgeAt = position_;
			state.edged = true;
			continue;
		}
		// A new ramp from the value the last frame had, over its whole length (docs/writing-a-node-type.md#live-rows).
		double from = state.from;
		if (position_ > state.startAt) {
			const std::uint64_t last = position_ - 1;
			from = last >= state.endsAt
			           ? state.to
			           : state.from + (state.to - state.from) * static_cast<double>(last - state.startAt + 1) /
			                              static_cast<double>(declared.frames);
		}
		state.from = from;
		state.to = domainOf(declared, bits);
		state.toValue = valueOf(declared, bits);
		state.bits = bits;
		state.startAt = position_;
		state.endsAt = position_ + declared.frames - 1;
	}
}

RealtimeChannels::Taken RealtimeChannels::take() {
	// Cleared before the read: a write after it rings again, so nothing the code writes waits for a later write. The
	// fence pairs with cycleEnded's: without both, the clear can wait in a store buffer while the read below sees the
	// old head, and the code's exchange still sees the bell pending, so a write is neither read nor rung for.
	pending_.store(false, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	Taken taken;
	const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
	const std::uint32_t head = head_.load(std::memory_order_acquire);
	taken.records.reserve(head - tail);
	for (std::uint32_t at = tail; at != head; ++at) taken.records.push_back(slots_[at % ringSize]);
	tail_.store(head, std::memory_order_release);
	for (std::size_t kind = 0; kind < dropped_.size(); ++kind)
		taken.dropped[kind] = dropped_[kind].exchange(0, std::memory_order_relaxed);
	if (!brokenTaken_ && brokenSet_.load(std::memory_order_acquire)) {
		brokenTaken_ = true;
		taken.broken = broken_;
	}
	return taken;
}

std::optional<RealtimeChannels::Sampled> RealtimeChannels::sample(std::size_t field) {
	Word& word = words_[field];
	const std::uint32_t transitions = word.transitions.load(std::memory_order_acquire);
	if (transitions == word.sampled) return std::nullopt;
	const std::uint32_t between = transitions - std::exchange(word.sampled, transitions);
	return Sampled{word.bits.load(std::memory_order_acquire), between};
}
} // namespace ear
