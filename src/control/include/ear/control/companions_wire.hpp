// Companions on the wire, feature `companions` (docs/design/companions.md#8-module-design): each node's `companions`
// list in snapshots and node.added, and node.companionsChanged. Declarations only until slice 3b's step 3b.6 builds
// them with docs/control-port.md, test/protocol/companions.json and web/src/protocol.ts in one commit
// (docs/control-port.md#changing-the-wire); then NodeCompanionsChanged joins events.hpp's GraphEvent and "companions"
// joins commands.hpp's wireFeatures.
#pragma once
#include <span>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "ear/core/companions.hpp"
#include "ear/core/graph_spec.hpp"
namespace ear::control {
/// The feature's name in graph.get's `features`.
inline constexpr std::string_view companionsFeature = "companions";
/// node.companionsChanged {"name", "companions"}: the node's whole list (an empty one when a teardown begins).
struct NodeCompanionsChanged {
	NodeName node;
	std::vector<CompanionView> companions;
	friend bool operator==(const NodeCompanionsChanged&, const NodeCompanionsChanged&) = default;
};
/// {"name", "kind", "bank", "ready"}, plus "dropped" (a sink) or "shortfall" (a source); an absent counter is left out.
nlohmann::json encodeCompanion(const CompanionView& companion);
/// The list, in request order: a node's `companions` member and node.companionsChanged's.
nlohmann::json encodeCompanions(std::span<const CompanionView> companions);
} // namespace ear::control
