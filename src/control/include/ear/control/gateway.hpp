// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. run() through the graph file
// keeper and the GraphFileListener side are docs/design/knobly-config.md's (slice S5); their
// erratum to batch2.md lands in S7. The clock that stamps since, nodeChanged with its outcome, health sent on every
// change and node.gone are docs/design/node-lifecycle-modules.md 5.4's (slice 3a, step L-6).
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
	virtual void run(std::vector<Command> commands, std::move_only_function<void(std::vector<CommandResult>)> done) = 0;
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
class MainLoopGateway final : public GraphGateway,
							  public GraphFileListener,
							  public DesiredNodeListener,
							  public DesiredRouteListener,
							  public NodeHealthListener,
							  public RouteStatusListener,
							  public SnapshotStamp {
public:
	/// `clock` stamps each node's `since` when its phase changes (D-M11).
	MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests,
	                WallClock clock = &std::chrono::system_clock::now); // all outlive it
	~MainLoopGateway();
	/// requests.submit(commands, a reply that posts done(results) to `control`).
	void run(std::vector<Command> commands, std::move_only_function<void(std::vector<CommandResult>)> done) override;
	void watch(EventSink& sink) override;
	void graphFileChanged(const GraphFileChanged& change) override; // posted to sink->graphFileChanged
	void graphFileWritten(const std::string& version) override;     // nothing: a status line only
	/// node.added with the spec's health (D-M12), `since` stamped.
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// node.updated with `applied` restart (told only by a listener's default nodeChanged, which this one overrides).
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// DesiredNodeListener (observer): a node kept by name whose spec changed: node.updated with the outcome and
	/// `disabled`.
	void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) override;
	void nodeRemoved(const NodeName& node) override;
	void routeAdded(const Route& route) override;
	void routeRemoved(const Route& route) override;
	/// Posted as node.state whenever any health member differs from the node's last one sent (control-plane.md 4.8),
	/// with `since` re-stamped only when its phase differs.
	void nodeChanged(const NodeHealth& health) override;
	/// Posts node.stopped (deprecated) then node.gone (control-plane.md 4.8).
	void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
	/// The wire keys health by name and holds the newer node of a name (node-lifecycle.md 5.7, D-L36).
	bool hearsSuperseded() const noexcept override { return false; }
	void routeChanged(const RouteStatus& status) override;
	/// SnapshotStamp (main loop): each node's `since` as its last node.state carried it, for a node told of.
	void stamp(GraphView& view) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::control
