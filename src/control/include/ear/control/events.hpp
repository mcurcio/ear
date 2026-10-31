// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. encodeGraphFileChanged is
// docs/design/knobly-config.md's (slice S3); its erratum to batch2.md lands in S7.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include "ear/control/graph_file_state.hpp"
#include "ear/core/levels.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
// The graph notifications (docs/design/batch2.md section 6) as typed events.
namespace ear::control {
struct NodeAdded { NodeSpec node; Ports ports; };          // public ports only
struct NodeUpdated { NodeName node; NodeSettings settings; };
struct NodeRemoved { NodeName node; };
struct NodeChanged { NodeStatus status; };
struct NodeStopped { NodeName node; std::uint64_t droppedFrames; };
struct RouteAdded { Route route; };
struct RouteRemoved { Route route; };
struct RouteChanged { RouteStatus status; };
using GraphEvent = std::variant<NodeAdded, NodeUpdated, NodeRemoved, NodeChanged, NodeStopped,
                                RouteAdded, RouteRemoved, RouteChanged>;
class EventSink {   // control thread
public:
  virtual void graphChanged(GraphEvent event) = 0;
protected:
  ~EventSink() = default;
};
std::string encodeEvent(const GraphEvent& event);              // one notification
std::string encodeLevels(std::span<const NodeLevels> levels);  // one "levels" notification
/// One "graphFile.changed" notification: the GraphFile JSON (encodeGraphFile) plus "cause", plus
/// "droppedEdits" only when it is above 0.
std::string encodeGraphFileChanged(const GraphFileChanged& change);
}
