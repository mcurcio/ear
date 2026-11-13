// The graph read: the join of the desired graph, the node framework and the reconciler
// (docs/architecture.md#packages, graph_view.hpp). It holds no state of its own: every read
// asks the three owners, so it can never disagree with them.
#include "ear/core/graph_view.hpp"

#include <map>
#include <utility>

namespace ear {

NodeHealth healthFromSpec(const NodeSpec& spec) {
	NodeHealth health{spec.name};
	if (spec.disabled) {
		health.state = NodeState::disabled;
		health.phase = NodePhase::disabled;
		health.wanted = Wanted::disabled;
	}
	return health;
}

GraphStatus::GraphStatus(EngineView engine, const NodeCatalogue& catalogue, const DesiredGraph& desired,
                         const NodeHost& nodes, const Reconciler& routes, const GraphCounterSource* graphCounts)
	: engine_(std::move(engine)), catalogue_(catalogue), desired_(desired), nodes_(nodes), routes_(routes),
	  graphCounts_(graphCounts) {}

GraphView GraphStatus::read() const {
	GraphView view{engine_, {}, {}, {}};
	for (const auto name : catalogue_.names()) view.types.emplace_back(name);

	// A wanted node is NodeHost's last entry of its name: an earlier one is a removed node of the
	// same name still stopping.
	std::map<NodeName, NodeHealth> latest;
	for (auto& health : nodes_.health()) latest.insert_or_assign(health.name, std::move(health));
	for (auto& wanted : desired_.nodes()) {
		NodeView node{std::move(wanted.spec), wanted.ports.publicOnly(), {}, std::nullopt};
		if (const auto it = latest.find(node.spec.name); it != latest.end()) node.health = std::move(it->second);
		else node.health = healthFromSpec(node.spec);
		node.health.superseded = false;
		node.counts = nodes_.counts(node.spec.name).value_or(NodeCounts{});
		view.nodes.push_back(std::move(node));
	}

	if (graphCounts_ != nullptr) view.counts = graphCounts_->graphCounts();

	std::map<Route, RouteStatus> linked;
	for (auto& status : routes_.status()) linked.insert_or_assign(status.route, std::move(status));
	for (auto& route : desired_.routes()) {
		const auto it = linked.find(route);
		view.routes.push_back(it != linked.end() ? it->second : RouteStatus{std::move(route), RouteMatch::pending, {}});
	}
	return view;
}

} // namespace ear
