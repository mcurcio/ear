// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. run() through the graph file
// keeper and the GraphFileListener side are docs/design/knobly-config.md's (slice S5); their
// erratum to batch2.md lands in S7.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"
#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/executor.hpp"
// The graph as the control thread sees it (docs/design/batch2.md sections 2 and 4.5;
// docs/design/knobly-config.md, crossings C1 and C4).
namespace ear::control {
class GraphGateway {
public:
  /// Any thread. Sends `commands` whole to the graph file keeper, which runs them in order, each as
  /// its own step. `done` runs on the control executor with one result per command, in order, after
  /// every event the batch caused. Never runs anything inline.
  virtual void run(std::vector<Command> commands,
                   std::move_only_function<void(std::vector<CommandResult>)> done) = 0;
  /// Any thread, before the first run(). Takes effect in a main-loop turn; every graph event and
  /// graph file change after that reaches `sink` on the control executor. Events, file changes and
  /// run()'s replies arrive in the order they happened on the main loop. `sink` outlives the
  /// gateway's control executor.
  virtual void watch(EventSink& sink) = 0;
protected:
  ~GraphGateway() = default;
};
/// The one object that crosses between the main loop and the control thread, both ways, so
/// replies, file changes and events share one ordered stream. Control side: every request goes
/// whole to `requests` (the graph file keeper). Main-loop side: a listener of the keeper, an
/// observer of DesiredGraph and a listener of NodeHost and the Reconciler (each call copies what
/// it is told and posts it; never calls the sink inline); the keeper's reply posts `done` the same
/// way, so it follows every event the batch caused.
class MainLoopGateway final : public GraphGateway, public GraphFileListener, public DesiredNodeListener,
                              public DesiredRouteListener, public NodeStatusListener,
                              public RouteStatusListener {
public:
  MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests);   // all outlive it
  ~MainLoopGateway();
  /// requests.submit(commands, a reply that posts done(results) to `control`).
  void run(std::vector<Command> commands,
           std::move_only_function<void(std::vector<CommandResult>)> done) override;
  void watch(EventSink& sink) override;
  void graphFileChanged(const GraphFileChanged& change) override;   // posted to sink->graphFileChanged
  void graphFileWritten(const std::string& version) override;       // nothing: a status line only
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
