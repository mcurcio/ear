// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <memory>
#include <span>
#include <vector>
#include "ear/core/node_catalogue.hpp"
namespace ear {
struct WantedNode { NodeSpec spec; Ports ports; };
class DesiredNodeListener {
public:
  /// After the node is recorded. The listener may keep `config` for as long as it needs it
  /// (the node framework keeps it until the insides it started are destroyed).
  virtual void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) = 0;
  /// New settings recorded (ports unchanged).
  virtual void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) = 0;
  /// After routeRemoved for each of its routes.
  virtual void nodeRemoved(const NodeName& node) = 0;
protected:
  ~DesiredNodeListener() = default;
};
class DesiredRouteListener {
public:
  virtual void routeAdded(const Route& route) = 0;
  virtual void routeRemoved(const Route& route) = 0;
protected:
  ~DesiredRouteListener() = default;
};
/// Proof that the graph takes no more edits. Only DesiredGraph::close() makes one.
class GraphClosed {
  friend class DesiredGraph;
  GraphClosed() = default;
};
/// The control calls: one per question. Main thread only. After close(), every one returns
/// Stopping and changes nothing.
class GraphControl {
public:
  virtual ~GraphControl() = default;
  /// Same spec again: succeeds, no event. Errors: InvalidName, UnknownNodeType, InvalidSettings,
  /// DuplicateNode.
  virtual std::expected<void, CommandError> addNode(const NodeSpec& node) = 0;
  /// Same settings: succeeds, no event. Errors: UnknownNode, InvalidSettings, PortsChanged.
  /// Success: nodeUpdated; the node's insides restart, its port block and routes stay.
  virtual std::expected<void, CommandError> updateNode(const NodeName& node,
                                                       const NodeSettings& settings) = 0;
  /// Errors: UnknownNode. Success: routeRemoved for every route naming it, then nodeRemoved.
  virtual std::expected<void, CommandError> removeNode(const NodeName& node) = 0;
  /// Existing route: succeeds, no event. Errors: UnknownNode, UnknownPort, WrongDirection.
  virtual std::expected<void, CommandError> addRoute(const Route& route) = 0;
  /// Errors: UnknownRoute. Success: routeRemoved.
  virtual std::expected<void, CommandError> removeRoute(const Route& route) = 0;
};
/// The changes that take the desired graph from what it holds to one wanted graph. Only
/// DesiredGraph::plan makes one. It is committed at most once, on that graph, before the graph changes otherwise.
class Plan {
public:
  Plan(Plan&& other) noexcept;
  Plan& operator=(Plan&& other) noexcept;
  Plan(const Plan&) = delete;
  Plan& operator=(const Plan&) = delete;
  ~Plan();
  bool empty() const noexcept;   // commit() would tell no listener anything
private:
  friend class DesiredGraph;
  struct Impl;
  explicit Plan(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};
class DesiredGraph final : public GraphControl {
public:
  explicit DesiredGraph(const NodeCatalogue& catalogue);   // catalogue outlives this
  ~DesiredGraph() override;
  /// Each change is told to every observer (in observe order), then to every subscriber (in
  /// subscribe order). Observers record and report; they never act on the graph or call back.
  /// Subscribers act (the node framework, the reconciler). Register before the first call.
  /// Every change, by commit() or by a GraphControl call, makes every outstanding Plan stale.
  void observe(DesiredNodeListener& listener);
  void observe(DesiredRouteListener& listener);
  void subscribe(DesiredNodeListener& listener);
  void subscribe(DesiredRouteListener& listener);
  std::expected<void, CommandError> addNode(const NodeSpec& node) override;
  std::expected<void, CommandError> updateNode(const NodeName& node,
                                               const NodeSettings& settings) override;
  std::expected<void, CommandError> removeNode(const NodeName& node) override;
  std::expected<void, CommandError> addRoute(const Route& route) override;
  std::expected<void, CommandError> removeRoute(const Route& route) override;
  /// Main thread; changes nothing. Returns the graph `nodes` and `routes` describe, as changes from what
  /// it holds. Errors (the first found; nodes in the given order, then routes in the given order):
  /// InvalidName, UnknownNodeType, InvalidSettings, DuplicateNode (two given nodes share a name);
  /// UnknownNode, UnknownPort, WrongDirection (a route); Stopping after close(). Identical routes
  /// are one route. A node equal to a held one is no change. Same type and ports with other settings
  /// is an update. Another type or other ports (its type's parse decides) is a replacement: the node is
  /// removed and added under the same name, and its held routes that are still wanted are removed
  /// and added again.
  std::expected<Plan, CommandError> plan(std::span<const NodeSpec> nodes, std::span<const Route> routes) const;
  /// Main thread. Applies `plan` in one go, each event to observers then subscribers, in this order:
  /// routeRemoved (Route order), nodeRemoved (name order), nodeUpdated (name order), nodeAdded (name
  /// order), routeAdded (Route order). A replaced node appears in nodeRemoved and in nodeAdded. After close():
  /// Stopping, and nothing changes. std::logic_error when `plan` came from another graph, or when the graph changed
  /// since the plan was made (a wiring bug).
  std::expected<void, CommandError> commit(Plan plan);
  /// Main thread. From now on every edit returns Stopping; nodes() and routes() still answer.
  /// Calling it again returns another proof and changes nothing.
  GraphClosed close();
  std::vector<WantedNode> nodes() const;   // name order; ports as the config declares them
  std::vector<Route> routes() const;       // Route order
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
// The name rule's reason: a node's sound cards are engine nodes named "<node>.playback" and
// "<node>.capture" (NodeHost, the one home of that form); '.' is never in a user's node name, so
// they cannot collide. The rule lives in desired_graph.cpp only.
