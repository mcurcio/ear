// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <ostream>
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear::control {
/// M1's event stream (the control port replaces it). One flushed line per event:
///   node <n> starting|running|stopping
///   node <n> failed <failureCode>: <detail>
///   node <n> stopped dropped=<frames>
///   route <a>:<p> -> <b>:<q> pending|linked|refused[: <detail>]
class StatusLog final : public NodeStatusListener, public RouteStatusListener {
public:
  explicit StatusLog(std::ostream& out);
  void nodeChanged(const NodeStatus& status) override;
  void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
  void routeChanged(const RouteStatus& status) override;
private:
  std::ostream& out_;
};
}
