#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"
#include "ear/control/requests.hpp"
#include "ear/core/companions.hpp"
#include "ear/core/counter_watch.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/executor.hpp"
// The graph as the control thread sees it (docs/architecture.md#threads;
// docs/architecture.md#how-a-change-moves).
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
/// observer of DesiredGraph and a listener of NodeHost, the Reconciler and CounterWatch, and of both tellers of a
/// node's companion list (each call copies what it is told and posts it; never calls the sink inline); the keeper's reply posts `done` the same
/// way, so it follows every event the batch caused.
class MainLoopGateway final : public GraphGateway,
							  public GraphFileListener,
							  public DesiredNodeListener,
							  public DesiredRouteListener,
							  public NodeHealthListener,
							  public RouteStatusListener,
							  public CounterListener,
							  public NodeCompanionsListener,
							  public SnapshotStamp {
public:
	/// `clock` stamps each node's `since` when its phase changes.
	MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests,
	                WallClock clock = &std::chrono::system_clock::now); // all outlive it
	~MainLoopGateway();
	/// requests.submit(commands, a reply that posts done(results) to `control`).
	void run(std::vector<Command> commands, std::move_only_function<void(std::vector<CommandResult>)> done) override;
	void watch(EventSink& sink) override;
	void graphFileChanged(const GraphFileChanged& change) override; // posted to sink->graphFileChanged
	void graphFileWritten(const std::string& version) override;     // nothing: a status line only
	/// Main thread, before the first event: where node.added reads the counters a port-caused replace carries on
	/// (docs/control-port.md#counters). Without it every node.added carries zero counters.
	void readCounts(const NodeCounterSource& nodes);
	/// node.added with the spec's health, `since` stamped, and zero counters: a node added counts from zero.
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// node.added as nodeAdded; a port-caused replace's carries the counters it carries on, the older node's now
	/// (docs/engine.md#counters), read before NodeHost hears the replace.
	void nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config,
	                  const NodeChange& change) override;
	/// node.updated with `applied` restart (told only by a listener's default nodeChanged, which this one overrides).
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// DesiredNodeListener (observer): a node kept by name whose spec changed: node.updated with the outcome and
	/// `disabled`.
	void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) override;
	void nodeRemoved(const NodeName& node) override;
	void routeAdded(const Route& route) override;
	void routeRemoved(const Route& route) override;
	/// Posted as node.state whenever any health member differs from the node's last one sent
	/// (docs/control-port.md#node-health), with `since` re-stamped only when its phase differs.
	void nodeChanged(const NodeHealth& health) override;
	/// Posts node.stopped (deprecated) then node.gone (docs/control-port.md#node-health).
	void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
	/// The wire keys health by name and holds the newer node of a name
	/// (docs/architecture.md#phases-states-and-what-is-wanted).
	bool hearsSuperseded() const noexcept override { return false; }
	void routeChanged(const RouteStatus& status) override;
	/// CounterListener: posted as counters.changed (docs/control-port.md#counters).
	void countersChanged(const GraphCounts& graph, std::uint64_t lostMs, std::span<const NamedCounts> nodes) override;
	/// NodeCompanionsListener, told by NodeHost (an entry added or removed, or its `ready`) and by CounterWatch (a
	/// counter's move): posted as node.companionsChanged, in order with the node's health, only for a node a client
	/// holds (added, not removed since) and only when the list differs from the last one sent for it (the [] of its
	/// node.added until then), so the two tellers never send one list twice (docs/control-port.md#companions).
	void companionsChanged(const NodeName& node, std::span<const CompanionView> companions) override;
	/// SnapshotStamp (main loop): each node's `since` as its last node.state carried it, for a node told of.
	void stamp(GraphView& view) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::control
