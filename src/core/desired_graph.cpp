// What the user wants: nodes and routes, checked against the catalogue and each node's declared
// ports (docs/design/batch2.md section 4.1, desired_graph.hpp). It never knows what exists in
// the engine. Every change is told to the observers first, then to the subscribers (rule 4), and
// close() shuts it to edits before the node framework stops (rule 5).
#include "ear/core/desired_graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ear {
namespace {

bool declares(const std::vector<PortName>& ports, const PortName& port) {
	return std::find(ports.begin(), ports.end(), port) != ports.end();
}

/// The node type's ports invariant (node_type.hpp): a breach is a type bug, not a user error.
void requireUniquePortNames(const NodeSpec& node, const Ports& ports) {
	std::set<PortName> seen;
	for (const auto* list : {&ports.inputs, &ports.outputs, &ports.insideInputs, &ports.insideOutputs})
		for (const auto& port : *list)
			if (!seen.insert(port).second)
				throw std::logic_error("node type " + node.type + " declares port " + port + " twice on node " +
				                       node.name);
}

/// The node name rule (its one home): 1-64 characters of [A-Za-z0-9_-]. '.' is never allowed,
/// so a node's sound cards ("<node>.playback", "<node>.capture") cannot collide with a node.
bool validName(const NodeName& name) {
	if (name.empty() || name.size() > 64) return false;
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

bool samePorts(const Ports& a, const Ports& b) {
	return a.inputs == b.inputs && a.outputs == b.outputs && a.insideInputs == b.insideInputs &&
	       a.insideOutputs == b.insideOutputs;
}

bool namesNode(const Route& route, const NodeName& node) {
	return route.from.node == node || route.to.node == node;
}

} // namespace

struct DesiredGraph::Impl {
	struct Wanted {
		NodeSpec spec;
		std::shared_ptr<const NodeConfig> config;
	};

	const NodeCatalogue& catalogue;
	std::map<NodeName, Wanted, std::less<>> nodes;
	std::set<Route> routes;
	/// Observers first, then subscribers: the one event order (rule 4).
	std::vector<DesiredNodeListener*> nodeObservers;
	std::vector<DesiredRouteListener*> routeObservers;
	std::vector<DesiredNodeListener*> nodeSubscribers;
	std::vector<DesiredRouteListener*> routeSubscribers;
	bool closed = false;

	explicit Impl(const NodeCatalogue& c) : catalogue(c) {}

	template <class Call> void tellNodes(Call call) const {
		for (auto* listener : nodeObservers) call(*listener);
		for (auto* listener : nodeSubscribers) call(*listener);
	}
	template <class Call> void tellRoutes(Call call) const {
		for (auto* listener : routeObservers) call(*listener);
		for (auto* listener : routeSubscribers) call(*listener);
	}

	/// The node's checked config from its type, or why it has none.
	std::expected<std::shared_ptr<const NodeConfig>, CommandError> parse(const NodeSpec& node) const {
		const NodeType* type = catalogue.find(node.type);
		if (type == nullptr) return std::unexpected(UnknownNodeType{node.type});
		auto config = type->parse(node.name, node.settings);
		if (!config) return std::unexpected(std::move(config.error()));
		requireUniquePortNames(node, (*config)->ports());
		return std::shared_ptr<const NodeConfig>(std::move(*config));
	}

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

DesiredGraph::DesiredGraph(const NodeCatalogue& catalogue) : impl_(std::make_unique<Impl>(catalogue)) {}

DesiredGraph::~DesiredGraph() = default;

void DesiredGraph::observe(DesiredNodeListener& listener) { impl_->nodeObservers.push_back(&listener); }

void DesiredGraph::observe(DesiredRouteListener& listener) { impl_->routeObservers.push_back(&listener); }

void DesiredGraph::subscribe(DesiredNodeListener& listener) { impl_->nodeSubscribers.push_back(&listener); }

void DesiredGraph::subscribe(DesiredRouteListener& listener) { impl_->routeSubscribers.push_back(&listener); }

std::expected<void, CommandError> DesiredGraph::addNode(const NodeSpec& node) {
	if (impl_->closed) return std::unexpected(Stopping{});
	if (!validName(node.name)) return std::unexpected(InvalidName{node.name});
	if (const auto existing = impl_->nodes.find(node.name); existing != impl_->nodes.end()) {
		if (existing->second.spec == node) return {};
		return std::unexpected(DuplicateNode{node.name});
	}
	auto config = impl_->parse(node);
	if (!config) return std::unexpected(std::move(config.error()));

	const auto& added = impl_->nodes.emplace(node.name, Impl::Wanted{node, std::move(*config)}).first->second;
	impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeAdded(added.spec, added.config); });
	return {};
}

std::expected<void, CommandError> DesiredGraph::updateNode(const NodeName& node, const NodeSettings& settings) {
	if (impl_->closed) return std::unexpected(Stopping{});
	const auto it = impl_->nodes.find(node);
	if (it == impl_->nodes.end()) return std::unexpected(UnknownNode{node});
	auto& wanted = it->second;
	if (wanted.spec.settings == settings) return {};
	NodeSpec spec = wanted.spec;
	spec.settings = settings;
	auto config = impl_->parse(spec);
	if (!config) return std::unexpected(std::move(config.error()));
	if (!samePorts((*config)->ports(), wanted.config->ports())) return std::unexpected(PortsChanged{node});

	wanted.spec = std::move(spec);
	wanted.config = std::move(*config);
	impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeUpdated(wanted.spec, wanted.config); });
	return {};
}

std::expected<void, CommandError> DesiredGraph::removeNode(const NodeName& node) {
	if (impl_->closed) return std::unexpected(Stopping{});
	const auto it = impl_->nodes.find(node);
	if (it == impl_->nodes.end()) return std::unexpected(UnknownNode{node});
	const NodeName name = it->first;   // `node` may refer to the record erased below

	std::vector<Route> gone;
	for (const auto& route : impl_->routes)
		if (namesNode(route, name)) gone.push_back(route);
	for (const auto& route : gone) {
		impl_->routes.erase(route);
		impl_->tellRoutes([&](DesiredRouteListener& listener) { listener.routeRemoved(route); });
	}
	impl_->nodes.erase(it);
	impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeRemoved(name); });
	return {};
}

std::expected<void, CommandError> DesiredGraph::addRoute(const Route& route) {
	if (impl_->closed) return std::unexpected(Stopping{});
	const auto from = impl_->portsOf(route.from);
	if (!from) return std::unexpected(from.error());
	const auto to = impl_->portsOf(route.to);
	if (!to) return std::unexpected(to.error());
	if (!declares((*from)->outputs, route.from.port) || !declares((*to)->inputs, route.to.port))
		return std::unexpected(WrongDirection{route});
	if (!impl_->routes.insert(route).second) return {};

	impl_->tellRoutes([&](DesiredRouteListener& listener) { listener.routeAdded(route); });
	return {};
}

std::expected<void, CommandError> DesiredGraph::removeRoute(const Route& route) {
	if (impl_->closed) return std::unexpected(Stopping{});
	const auto it = impl_->routes.find(route);
	if (it == impl_->routes.end()) return std::unexpected(UnknownRoute{route});
	const Route removed = *it;   // `route` may refer to the record erased below
	impl_->routes.erase(it);
	impl_->tellRoutes([&](DesiredRouteListener& listener) { listener.routeRemoved(removed); });
	return {};
}

GraphClosed DesiredGraph::close() {
	impl_->closed = true;
	return GraphClosed{};
}

std::vector<WantedNode> DesiredGraph::nodes() const {
	std::vector<WantedNode> all;
	all.reserve(impl_->nodes.size());
	for (const auto& [name, wanted] : impl_->nodes) all.push_back(WantedNode{wanted.spec, wanted.config->ports()});
	return all;
}

std::vector<Route> DesiredGraph::routes() const { return {impl_->routes.begin(), impl_->routes.end()}; }

} // namespace ear
