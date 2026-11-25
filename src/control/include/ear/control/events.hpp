#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "ear/control/companions_wire.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/core/counter_watch.hpp"
#include "ear/core/levels.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
// The graph notifications (docs/control-port.md) as typed events.
namespace ear::control {
/// node.added. Its `companions` (feature companions) is always [] and its `counters` zero: a node is told added before
/// NodeHost configures it (docs/control-port.md#companions).
struct NodeAdded {
	NodeSpec node;
	Ports ports;       // public ports only
	NodeHealth health; // from the spec alone
	std::chrono::system_clock::time_point since;
	std::vector<FieldValue> status{}; // feature status: every field of its type at its initial value
};
/// node.statusChanged (feature status, docs/control-port.md#status): the fields changed since the node's last, each at
/// its value now; with `reset`, every field at its initial value after its configuration closed.
struct NodeStatusChanged {
	NodeName node;
	std::vector<FieldValue> fields;
	bool reset = false;
};
/// node.eventRaised (feature status): one event past the node's event pace, `t` when it was written.
struct NodeEventRaised {
	NodeName node;
	RaisedEvent event;
	std::chrono::system_clock::time_point t;
};
/// node.diagnostic (feature status, docs/control-port.md#node-diagnostics): one of the node's diagnostics as NodeHost
/// kept it, `t` when it was said.
struct NodeDiagnosed {
	NodeName node;
	NodeDiagnostic diagnostic;
	std::chrono::system_clock::time_point t;
};
/// node.updated's members for a change of a node's ports (feature reshape, docs/control-port.md#notifications).
struct NodeReshaped {
	bool inPlace;     // a reshape's; carried only when `applied` is reshape
	PortsMoved ports; // the public ports added and removed
	Ports now;        // its public ports: `inputs` and `outputs`
};
struct NodeUpdated {
	NodeName node;
	NodeSettings settings;
	bool disabled;
	NodeOutcome applied;
	std::optional<NodeReshaped> reshaped{}; // its ports changed
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
/// counters.changed (feature counters, docs/control-port.md#counters): the graph's counters whole, `lostMs` encoded
/// from its cutCycles, and the whole counters of each node named.
struct CountersChanged {
	GraphCounts graph;
	std::uint64_t lostMs;
	std::vector<NamedCounts> nodes;
};
using GraphEvent = std::variant<NodeAdded, NodeUpdated, NodeRemoved, NodeChanged, NodeStopped, NodeGone, RouteAdded,
                                RouteRemoved, RouteChanged, CountersChanged, NodeCompanionsChanged, NodeStatusChanged,
                                NodeEventRaised, NodeDiagnosed>;
class EventSink { // control thread
public:
	virtual void graphChanged(GraphEvent event) = 0;
	virtual void graphFileChanged(GraphFileChanged change) = 0;

protected:
	~EventSink() = default;
};
std::string encodeEvent(const GraphEvent& event); // one notification
/// node.updated's `applied`: "restart" | "reconfigure" | "reshape" | "disable" | "enable" | "stored" | "unchanged".
std::string_view outcomeName(NodeOutcome outcome) noexcept;
std::string encodeLevels(std::span<const NodeLevels> levels); // one "levels" notification
/// One "graphFile.changed" notification: the GraphFile JSON (encodeGraphFile) plus "cause", plus
/// "droppedEdits" only when it is above 0.
std::string encodeGraphFileChanged(const GraphFileChanged& change);
} // namespace ear::control
