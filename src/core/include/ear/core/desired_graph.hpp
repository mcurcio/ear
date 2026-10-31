// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. plan() and commit() replace
// GraphControl's five edit calls (docs/design/knobly-config.md, slice S5); the erratum lands in S7.
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
/// What the user wants. Main thread only. Its one editor is the graph file keeper's main-loop side.
class DesiredGraph final {
public:
  explicit DesiredGraph(const NodeCatalogue& catalogue);   // catalogue outlives this
  ~DesiredGraph();
  /// Each change is told to every observer (in observe order), then to every subscriber (in
  /// subscribe order). Observers record and report; they never act on the graph or call back.
  /// Subscribers act (the node framework, the reconciler). Register before the first call.
  /// Every commit() makes every outstanding Plan stale.
  void observe(DesiredNodeListener& listener);
  void observe(DesiredRouteListener& listener);
  void subscribe(DesiredNodeListener& listener);
  void subscribe(DesiredRouteListener& listener);
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
  /// Main thread. From now on plan() and commit() return Stopping; nodes() and routes() still answer.
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
