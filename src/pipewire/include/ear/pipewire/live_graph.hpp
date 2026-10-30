// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/reconciler.hpp"
#include "ear/pipewire/engine.hpp"
// registry -> LivePort events; joins port globals (node.id, port.name, port.direction) to node
// globals (node.name), in either arrival order.
namespace ear::pipewire {
class LiveGraph {
public:
  explicit LiveGraph(Engine& engine);
  ~LiveGraph();
  void subscribe(LivePortListener& listener);   // existing ports arrive later, never inside this call
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
