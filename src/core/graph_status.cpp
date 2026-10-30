// The graph read: the join of the desired graph, the node framework and the reconciler
// (docs/design/batch2.md section 4.2, graph_view.hpp). It holds no state of its own: every read
// asks the three owners, so it can never disagree with them.
#include "ear/core/graph_view.hpp"

#include <map>
#include <utility>

namespace ear {

GraphStatus::GraphStatus(EngineView engine, const NodeCatalogue& catalogue, const DesiredGraph& desired,
                         const NodeHost& nodes, const Reconciler& routes)
	: engine_(std::move(engine)), catalogue_(catalogue), desired_(desired), nodes_(nodes), routes_(routes) {}

GraphView GraphStatus::read() const {
	GraphView view{engine_, {}, {}, {}};
	for (const auto name : catalogue_.names()) view.types.emplace_back(name);

	// A wanted node is NodeHost's last entry of its name: an earlier one is a removed node of the
	// same name still stopping.
	std::map<NodeName, NodeStatus> latest;
	for (auto& status : nodes_.status()) latest.insert_or_assign(status.name, std::move(status));
	for (auto& wanted : desired_.nodes()) {
		NodeView node{std::move(wanted.spec), Ports{std::move(wanted.ports.inputs), std::move(wanted.ports.outputs)},
		              NodeState::starting, std::nullopt};
		if (const auto it = latest.find(node.spec.name); it != latest.end()) {
			node.state = it->second.state;
			node.reason = it->second.reason;
		}
		view.nodes.push_back(std::move(node));
	}

	std::map<Route, RouteStatus> linked;
	for (auto& status : routes_.status()) linked.insert_or_assign(status.route, std::move(status));
	for (auto& route : desired_.routes()) {
		const auto it = linked.find(route);
		view.routes.push_back(it != linked.end() ? it->second : RouteStatus{std::move(route), RouteMatch::pending, {}});
	}
	return view;
}

} // namespace ear
