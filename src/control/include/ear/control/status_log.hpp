// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit. The graph-file lines are
// docs/design/knobly-config.md's (slice S3); their erratum to m1-white-to-file.md lands in S7.
#pragma once
#include <ostream>
#include "ear/control/requests.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear::control {
/// M1's event stream (the control port replaces it), and the graph file's. One flushed line per event:
///   node <n> starting|running|stopping
///   node <n> failed <failureCode>: <detail>
///   node <n> stopped dropped=<frames>
///   route <a>:<p> -> <b>:<q> pending|linked|refused[: <detail>]
/// and per graph file change, the first line always, then each that applies, in this order:
///   graph-file <edit|disk|write> <version> running=<version|none>
///   graph-file error <line>:<column> <code>: <message>      (one per error diagnostic)
///   graph-file refused <code> [at <line>:<column>]
///   graph-file dropped <n> edits                              (droppedEdits > 0)
///   graph-file pending-restart <graphFrequency|blockSize>...  (when not empty)
///   graph-file write-failed <detail>
/// and per write:
///   graph-file written <version>
/// and the last write's failure, as ear exits (no change carries it):
///   graph-file write-failed <detail>
class StatusLog final : public NodeStatusListener, public RouteStatusListener, public GraphFileListener {
public:
  explicit StatusLog(std::ostream& out);
  void nodeChanged(const NodeStatus& status) override;
  void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
  void routeChanged(const RouteStatus& status) override;
  void graphFileChanged(const GraphFileChanged& change) override;
  void graphFileWritten(const std::string& version) override;
  void graphFileWriteFailed(const std::string& detail);
private:
  std::ostream& out_;
};
}
