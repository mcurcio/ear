#include "ear/core/realtime_channels.hpp"
#include <utility>
namespace ear {
RealtimeChannels::RealtimeChannels(std::vector<Initial> fields, RealtimeBell* bell)
	: initial_(std::move(fields)), words_(std::make_unique<Word[]>(initial_.size())), bell_(bell),
	  slots_(std::make_unique<RealtimeRecord[]>(ringSize)) {}

RealtimeChannels::Taken RealtimeChannels::take() {
	// Cleared before the read: a write after it rings again, so nothing the code writes waits for a later write.
	pending_.store(false, std::memory_order_release);
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
