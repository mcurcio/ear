// The reach index and the related-change rule (reach_index.hpp; docs/graph-file.md#reaches).
#include "reach_index.hpp"

#include <algorithm>

namespace ear::config {

const ReachEntry* ReachIndex::find(const GestureKey& at) const {
	const auto found = std::ranges::find(entries_, at, &ReachEntry::at);
	return found == entries_.end() ? nullptr : &*found;
}

std::vector<GestureKey> ReachIndex::reachOf(const GestureKey& at) const {
	const ReachEntry* own = find(at);
	if (own == nullptr) return {};
	std::vector<GestureKey> reach{at};
	if (!own->definition) return reach;
	for (const ReachEntry& entry : entries_)
		if (entry.definition == own->definition && !(entry.at == at)) reach.push_back(entry.at);
	return reach;
}

namespace {

std::vector<GestureKey> sorted(std::vector<GestureKey> keys) {
	std::ranges::sort(keys, [](const GestureKey& a, const GestureKey& b) {
		return a.node != b.node ? a.node < b.node : a.key < b.key;
	});
	return keys;
}

} // namespace

Relation relationOf(const ReachIndex& before, const ReachIndex& after, const std::vector<GestureKey>& reach) {
	bool valueChanged = false;
	bool reachChanged = false;
	for (const GestureKey& at : reach) {
		const ReachEntry* was = before.find(at);
		const ReachEntry* now = after.find(at);
		const bool nodeThere = std::ranges::any_of(after.entries(), [&](const ReachEntry& entry) {
			return entry.at.node == at.node && (was == nullptr || entry.type == was->type);
		});
		if (!nodeThere) return Relation::nodeRemoved;
		if (now == nullptr || !now->row.member || now->row.change != setting::Change::live) return Relation::keyRemoved;
		// Made inactive by this version: a member already inactive is still held, its live value seeding the channel a
		// later reshape makes active (docs/control-port.md#gestures-and-the-graph-file).
		if (!now->row.active && (was == nullptr || was->row.active)) return Relation::keyRemoved;
		if (was == nullptr) {
			reachChanged = true;
			continue;
		}
		if (now->value != was->value) valueChanged = true;
		if (now->definition.has_value() != was->definition.has_value() || now->definingLines != was->definingLines ||
		    sorted(before.reachOf(at)) != sorted(after.reachOf(at)))
			reachChanged = true;
	}
	if (valueChanged) return Relation::value;
	return reachChanged ? Relation::reach : Relation::none;
}

} // namespace ear::config
