// A node's companion list as the wire carries it (docs/control-port.md#companions): one encoding for snapshots,
// node.added and node.companionsChanged, so they cannot drift.
#include "ear/control/companions_wire.hpp"

namespace ear::control {

nlohmann::json encodeCompanion(const CompanionView& companion) {
	nlohmann::json entry = {{"name", companion.name},
	                        {"kind", companionKindName(companion.kind)},
	                        {"bank", companion.bank},
	                        {"ready", companion.ready}};
	if (companion.dropped) entry["dropped"] = *companion.dropped;
	if (companion.shortfall) entry["shortfall"] = *companion.shortfall;
	return entry;
}

nlohmann::json encodeCompanions(std::span<const CompanionView> companions) {
	nlohmann::json list = nlohmann::json::array();
	for (const CompanionView& companion : companions) list.push_back(encodeCompanion(companion));
	return list;
}

} // namespace ear::control
