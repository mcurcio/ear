// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"
#include "ear/core/executor.hpp"
// The graph as the control thread sees it (docs/design/batch2.md sections 2 and 4.5).
namespace ear::control {
class GraphGateway {
public:
  /// Any thread. Runs `commands` in order in one main-loop turn, then runs `done` on the control
  /// executor with one result per command, in order. Never runs anything inline.
  virtual void run(std::vector<Command> commands,
                   std::move_only_function<void(std::vector<CommandResult>)> done) = 0;
  /// Any thread, before the first run(). Takes effect in a main-loop turn; every graph event
  /// after that reaches `sink` on the control executor. Events and run()'s replies arrive in the
  /// order they happened on the main loop. `sink` outlives the gateway's control executor.
  virtual void watch(EventSink& sink) = 0;
protected:
  ~GraphGateway() = default;
};
/// The one object that crosses between the main loop and the control thread, both ways, so
/// replies and events share one ordered stream. Main-loop side: an observer of DesiredGraph and
/// a listener of NodeHost and the Reconciler (each call copies the event and posts it; never
/// calls the sink inline). It touches `graph` and `reader` only inside work it posts to
/// `mainLoop`.
class MainLoopGateway final : public GraphGateway, public DesiredNodeListener,
                              public DesiredRouteListener, public NodeStatusListener,
                              public RouteStatusListener {
public:
  MainLoopGateway(Executor& mainLoop, Executor& control, GraphControl& graph,
                  const GraphReader& reader);   // all outlive it
  ~MainLoopGateway();
  void run(std::vector<Command> commands,
           std::move_only_function<void(std::vector<CommandResult>)> done) override;
  void watch(EventSink& sink) override;
  void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
  void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
  void nodeRemoved(const NodeName& node) override;
  void routeAdded(const Route& route) override;
  void routeRemoved(const Route& route) override;
  void nodeChanged(const NodeStatus& status) override;
  void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
  void routeChanged(const RouteStatus& status) override;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
