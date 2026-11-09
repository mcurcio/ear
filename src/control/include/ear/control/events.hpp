#pragma once
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include "ear/control/graph_file_state.hpp"
#include "ear/core/levels.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
// The graph notifications (docs/control-port.md) as typed events.
namespace ear::control {
struct NodeAdded {
	NodeSpec node;
	Ports ports;       // public ports only
	NodeHealth health; // from the spec alone
	std::chrono::system_clock::time_point since;
};
struct NodeUpdated {
	NodeName node;
	NodeSettings settings;
	bool disabled;
	NodeOutcome applied;
};
struct NodeRemoved {
	NodeName node;
};
struct NodeChanged {
	NodeHealth health;
	std::chrono::system_clock::time_point since; // when its phase began
};
/// Deprecated (protocol 1 sends it, then node.gone; it goes at protocol 2: docs/control-port.md#notifications).
struct NodeStopped {
	NodeName node;
	std::uint64_t droppedFrames;
};
/// Everything the node made is gone (feature `lifecycle`).
struct NodeGone {
	NodeName node;
	std::uint64_t droppedFrames;
};
struct RouteAdded {
	Route route;
};
struct RouteRemoved {
	Route route;
};
struct RouteChanged {
	RouteStatus status;
};
using GraphEvent = std::variant<NodeAdded, NodeUpdated, NodeRemoved, NodeChanged, NodeStopped, NodeGone, RouteAdded,
                                RouteRemoved, RouteChanged>;
class EventSink { // control thread
public:
	virtual void graphChanged(GraphEvent event) = 0;
	virtual void graphFileChanged(GraphFileChanged change) = 0;

protected:
	~EventSink() = default;
};
std::string encodeEvent(const GraphEvent& event); // one notification
/// node.updated's `applied`: "restart" | "disable" | "enable" | "stored".
std::string_view outcomeName(NodeOutcome outcome) noexcept;
std::string encodeLevels(std::span<const NodeLevels> levels); // one "levels" notification
/// One "graphFile.changed" notification: the GraphFile JSON (encodeGraphFile) plus "cause", plus
/// "droppedEdits" only when it is above 0.
std::string encodeGraphFileChanged(const GraphFileChanged& change);
} // namespace ear::control
