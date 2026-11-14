#include "readiness.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ear::detail {

std::size_t ReadinessList::add(ReadinessItem item) {
	if (begun_) throw std::logic_error("a readiness item added after the list began");
	entries_.push_back(Entry{std::move(item)});
	return entries_.size() - 1;
}

void ReadinessList::begin(Ask ask) {
	if (begun_) throw std::logic_error("a readiness list begun twice");
	begun_ = true;
	ask_ = std::move(ask);
	askNext();
}

void ReadinessList::askNext() {
	if (!begun_ || ended_) return;
	// The earliest step with an item not ready: later steps wait for it.
	const auto pending = std::ranges::find_if(entries_, [&](const Entry& entry) {
		return !entry.ready && std::ranges::none_of(entries_, [&](const Entry& other) {
			return !other.ready && other.item.step < entry.item.step;
		});
	});
	if (pending == entries_.end()) return;
	const ReadinessStep step = pending->item.step;
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		Entry& entry = entries_[i];
		if (entry.item.step != step || entry.asked) continue;
		entry.asked = true;
		if (!entry.ready) entry.bound = ask_(i);
	}
}

void ReadinessList::ready(std::size_t index) {
	if (index >= entries_.size() || entries_[index].ready) return;
	Entry& entry = entries_[index];
	entry.ready = true;
	entry.bound.reset();
	askNext();
}

std::optional<ReadinessOverdue> ReadinessList::passed(std::size_t index) {
	if (ended_ || index >= entries_.size()) return std::nullopt;
	Entry& entry = entries_[index];
	if (!entry.asked || entry.ready) return std::nullopt;
	entry.bound.reset();
	ended_ = true;
	return ReadinessOverdue{entry.item.fact, entry.item.overdue ? entry.item.overdue() : std::string()};
}

std::optional<ReadinessOverdue> ReadinessList::failed(std::size_t index, std::string detail) {
	if (ended_ || index >= entries_.size()) return std::nullopt;
	Entry& entry = entries_[index];
	if (!entry.asked || entry.ready) return std::nullopt;
	entry.bound.reset();
	ended_ = true;
	return ReadinessOverdue{entry.item.fact, std::move(detail)};
}

bool ReadinessList::isAsked(std::size_t index) const noexcept {
	return index < entries_.size() && entries_[index].asked;
}

void ReadinessList::disarm() noexcept {
	for (Entry& entry : entries_) entry.bound.reset();
}

bool ReadinessList::isReady(std::size_t index) const noexcept {
	return index < entries_.size() && entries_[index].ready;
}

bool ReadinessList::done() const noexcept {
	return std::ranges::all_of(entries_, [](const Entry& entry) { return entry.ready; });
}

std::vector<WaitingFor> ReadinessList::waiting() const {
	std::vector<WaitingFor> waits;
	for (const Entry& entry : entries_) {
		if (entry.ready) continue;
		WaitingFor wait{entry.item.kind, entry.item.name};
		if (std::ranges::find(waits, wait) == waits.end()) waits.push_back(std::move(wait));
	}
	return waits;
}

} // namespace ear::detail
