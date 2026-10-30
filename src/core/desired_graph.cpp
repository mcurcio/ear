// What the user wants: nodes and routes, checked against the catalogue and each node's declared
// ports (docs/design/m1-white-to-file.md section 2, desired_graph.hpp). It never knows what
// exists in the engine.
#include "ear/core/desired_graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ear {
namespace {

bool declares(const std::vector<PortName>& ports, const PortName& port) {
	return std::find(ports.begin(), ports.end(), port) != ports.end();
}

/// The node type's ports invariant (node_type.hpp): a breach is a type bug, not a user error.
void requireUniquePortNames(const NodeSpec& node, const Ports& ports) {
	std::set<PortName> seen;
	for (const auto* list : {&ports.inputs, &ports.outputs})
		for (const auto& port : *list)
			if (!seen.insert(port).second)
				throw std::logic_error("node type " + node.type + " declares port " + port + " twice on node " +
				                       node.name);
}

} // namespace

struct DesiredGraph::Impl {
	struct WantedNode {
		NodeSpec spec;
		std::unique_ptr<const NodeConfig> config;
	};

	const NodeCatalogue& catalogue;
	std::map<NodeName, WantedNode, std::less<>> nodes;
	std::set<Route> routes;
	std::vector<DesiredNodeListener*> nodeListeners;
	std::vector<DesiredRouteListener*> routeListeners;

	/// The node's declared ports, or the reason a route end cannot name them.
	std::expected<const Ports*, CommandError> portsOf(const PortRef& end) const {
		const auto it = nodes.find(end.node);
		if (it == nodes.end()) return std::unexpected(UnknownNode{end.node});
		const Ports& ports = it->second.config->ports();
		if (!declares(ports.inputs, end.port) && !declares(ports.outputs, end.port))
			return std::unexpected(UnknownPort{end});
		return &ports;
	}
};

DesiredGraph::DesiredGraph(const NodeCatalogue& catalogue) : impl_(new Impl{catalogue, {}, {}, {}, {}}) {}

DesiredGraph::~DesiredGraph() = default;

void DesiredGraph::subscribe(DesiredNodeListener& listener) { impl_->nodeListeners.push_back(&listener); }

void DesiredGraph::subscribe(DesiredRouteListener& listener) { impl_->routeListeners.push_back(&listener); }

std::expected<void, CommandError> DesiredGraph::addNode(const NodeSpec& node) {
	if (const auto existing = impl_->nodes.find(node.name); existing != impl_->nodes.end()) {
		if (existing->second.spec == node) return {};
		return std::unexpected(DuplicateNode{node.name});
	}
	const NodeType* type = impl_->catalogue.find(node.type);
	if (type == nullptr) return std::unexpected(UnknownNodeType{node.type});
	auto config = type->parse(node.name, node.settings);
	if (!config) return std::unexpected(std::move(config.error()));
	requireUniquePortNames(node, (*config)->ports());

	const auto it = impl_->nodes.emplace(node.name, Impl::WantedNode{node, std::move(*config)}).first;
	for (auto* listener : impl_->nodeListeners) listener->nodeAdded(it->first, *it->second.config);
	return {};
}

std::expected<void, CommandError> DesiredGraph::addRoute(const Route& route) {
	const auto from = impl_->portsOf(route.from);
	if (!from) return std::unexpected(from.error());
	const auto to = impl_->portsOf(route.to);
	if (!to) return std::unexpected(to.error());
	if (!declares((*from)->outputs, route.from.port) || !declares((*to)->inputs, route.to.port))
		return std::unexpected(WrongDirection{route});
	if (!impl_->routes.insert(route).second) return {};

	for (auto* listener : impl_->routeListeners) listener->routeAdded(route);
	return {};
}

} // namespace ear
