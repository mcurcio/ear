#pragma once
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "ear/core/node_catalogue.hpp"
namespace ear {
struct WantedNode {
	NodeSpec spec;
	Ports ports;
};
/// What an edit asks of a node it keeps by name (docs/architecture.md#restarts-and-changes-of-settings). Decided by
/// `disabled`, before and after: false to true `disable`, true to false `enable`, true to true `stored`; false to false
/// by the ports and the keys that are not live (changeOf): other ports `reshape`, no changed key `unchanged`, only live
/// keys changed `retune`, every other changed key's row marked inPlace `reconfigure`, else `restart`. The live keys
/// are retuned beside whatever outcome the others decide (NodeChange::retuned).
enum class NodeOutcome { restart, disable, enable, stored, reconfigure, reshape, unchanged, retune };
/// The outcome of a change of a node of `type` kept with the same type and ports: `keys` the changed keys (live ones
/// among them), the `disabled` before and after. Pure.
NodeOutcome outcomeFor(const NodeType& type, std::span<const std::string> keys, bool wasDisabled, bool isDisabled);
/// One live key a change moves (docs/architecture.md#retunes): its key, and its value as the settings held it before
/// and hold it now (a default when the key is not given), as the control port writes it. `from` and `to` are null
/// when the change was made from keys alone (changeOf).
struct Retuned {
	std::string key;
	NodeSettings from{};
	NodeSettings to{};
	std::optional<std::uint32_t> member{}; // the key's slot among its node's live members
	std::uint64_t bits = 0;                // the slot's new bits

	friend bool operator==(const Retuned&, const Retuned&) = default;
};
/// A node kept by name with the same type whose spec changed, told by nodeChanged: what the change asks of it, the
/// changed keys, and, when its ports changed, the ports before and after.
struct NodeChange {
	NodeOutcome outcome;
	/// The settings' keys whose values differ, added and removed ones included, but the live ones; key order. What the
	/// lifecycle acts on: the only keys a health's `keys` or a restart's cause names.
	std::vector<std::string> keys;
	/// The ports before and after (equal unless the ports changed: a `reshape`, or a `disable`, `enable` or `stored`
	/// whose ports changed too; docs/architecture.md#reshapes).
	Ports oldPorts{};
	Ports ports{};
	/// A reshape every changed key of which changes in place: taken in place by a node whose type has a change version.
	bool inPlace = false;
	/// The changed keys whose rows are live, key order (docs/architecture.md#retunes): moved in the node's live values
	/// and its slots at once, whatever the outcome.
	std::vector<Retuned> retuned{};
	bool portsChanged() const { return oldPorts != ports; }
};
/// The plan's change of a node of `type` kept by name with its type (docs/architecture.md#reshapes): outcomeFor's, but a
/// node staying enabled whose ports differ is a `reshape`, `inPlace` when there is a changed key and every one changes
/// in place; the live keys go to `retuned`, the others to `keys`. Pure; the plan's and the type harness's one rule.
NodeChange changeOf(const NodeType& type, std::vector<std::string> keys, bool wasDisabled, bool isDisabled,
                    const Ports& oldPorts, const Ports& ports);
class DesiredNodeListener {
public:
	/// After the node is recorded. The listener may keep `config` for as long as it needs it
	/// (the node framework keeps it until the insides it started are destroyed).
	virtual void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) = 0;
	/// New settings recorded. Told by nodeChanged's default.
	virtual void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) = 0;
	/// A node kept with the same type whose spec changed (its settings, its `disabled`, or both; its ports with them),
	/// with what the change asks of it. The default tells nodeUpdated, so a listener that tells no outcome apart needs
	/// nothing.
	virtual void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
		static_cast<void>(change);
		nodeUpdated(node, std::move(config));
	}
	/// After routeRemoved for each of its routes.
	virtual void nodeRemoved(const NodeName& node) = 0;
	/// A replaced node's removal (another type under the name), told in nodeRemoved's place and order; nodeReplaced
	/// follows in nodeAdded's. The defaults tell nodeRemoved and nodeAdded, so a listener that tells a replace from a
	/// removal and an addition by nothing needs nothing (only the node framework does:
	/// docs/architecture.md#restarts-and-changes-of-settings).
	virtual void nodeReplacing(const NodeName& node, const NodeChange& change) {
		static_cast<void>(change);
		nodeRemoved(node);
	}
	virtual void nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config,
	                          const NodeChange& change) {
		static_cast<void>(change);
		nodeAdded(node, std::move(config));
	}

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
	bool empty() const noexcept; // commit() would tell no listener anything
	/// The outcome for each node kept by name whose spec changes, in name order: a node told by nodeChanged, and a
	/// replaced one (another type, told as a removal and an addition), whose outcome is its `disabled` change's (a
	/// replace of a node staying disabled is `stored`, of one staying enabled `restart`). Nodes only added or only
	/// removed are not in it. What an edit's reply says it applied (docs/architecture.md#restarts-and-changes-of-settings).
	std::vector<std::pair<NodeName, NodeOutcome>> outcomes() const;
	/// The change for a node outcomes() names (a kept node's whole NodeChange: its ports, inPlace; a replaced one's
	/// outcome and keys); std::nullopt for any other name. What node.set's reply carries (docs/control-port.md#methods).
	std::optional<NodeChange> change(const NodeName& node) const;

private:
	friend class DesiredGraph;
	struct Impl;
	explicit Plan(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};
/// What the user wants. Main thread only. Its one editor is the graph file keeper's main-loop side.
class DesiredGraph final {
public:
	explicit DesiredGraph(const NodeCatalogue& catalogue); // catalogue outlives this
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
	/// are one route. A node equal to a held one is no change. The same type with other settings or another
	/// `disabled` is a change, told by nodeChanged with its NodeChange (other ports, which its type's parse decides, a
	/// reshape: its routes still wanted are kept). Another type is a replacement: the node is removed and added under
	/// the same name, and its held routes that are still wanted are removed and added again.
	std::expected<Plan, CommandError> plan(std::span<const NodeSpec> nodes, std::span<const Route> routes) const;
	/// Main thread. Applies `plan` in one go, each event to observers then subscribers, in this order:
	/// routeRemoved (Route order), nodeRemoved (name order), nodeChanged (name order), nodeAdded (name
	/// order), routeAdded (Route order). A replaced node appears in nodeRemoved's order as nodeReplacing and in
	/// nodeAdded's as nodeReplaced, both with its NodeChange. After close():
	/// Stopping, and nothing changes. std::logic_error when `plan` came from another graph, or when the graph changed
	/// since the plan was made (a wiring bug).
	std::expected<void, CommandError> commit(Plan plan);
	/// Main thread. From now on plan() and commit() return Stopping; nodes() and routes() still answer.
	/// Calling it again returns another proof and changes nothing.
	GraphClosed close();
	std::vector<WantedNode> nodes() const; // name order; ports as the config declares them
	std::vector<Route> routes() const;     // Route order
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear
// The name rule's reason: a node's sound cards are engine nodes named "<node>.playback" and
// "<node>.capture" (NodeHost, the one home of that form); '.' is never in a user's node name, so
// they cannot collide. The rule lives in desired_graph.cpp only.
