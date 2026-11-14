// Companions on the wire, feature `companions` (docs/control-port.md#companions): each node's `companions` list in
// snapshots and node.added, and node.companionsChanged, the whole list whenever it changes.
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
