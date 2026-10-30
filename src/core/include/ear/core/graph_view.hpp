// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear {
/// Engine facts fixed at boot.
struct EngineView {
  AudioFormat format;          // rate = graphFrequency, maxBlock = blockSize
  bool realtime;               // the data thread got realtime priority
  std::string scheduling;      // e.g. "SCHED_FIFO 88", "SCHED_OTHER"
};
struct NodeView {
  NodeSpec spec;
  Ports ports;                               // public only (inside lists empty)
  NodeState state;
  std::optional<FailureReason> reason;
};
struct GraphView {
  EngineView engine;
  std::vector<std::string> types;            // sorted
  std::vector<NodeView> nodes;               // wanted nodes, name order
  std::vector<RouteStatus> routes;           // wanted routes, Route order
};
class GraphReader {
public:
  virtual GraphView read() const = 0;        // main thread
protected:
  ~GraphReader() = default;
};
/// The join of the desired graph, the node framework and the reconciler. A wanted node's state
/// is that of NodeHost's last status entry of its name.
class GraphStatus final : public GraphReader {
public:
  GraphStatus(EngineView engine, const NodeCatalogue& catalogue, const DesiredGraph& desired,
              const NodeHost& nodes, const Reconciler& routes);   // all outlive it
  GraphView read() const override;
private:
  EngineView engine_; const NodeCatalogue& catalogue_; const DesiredGraph& desired_;
  const NodeHost& nodes_; const Reconciler& routes_;
};
}
