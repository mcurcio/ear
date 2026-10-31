// What the user wants: nodes and routes, checked against the catalogue and each node's declared
// ports (docs/design/batch2.md section 4.1, desired_graph.hpp). It never knows what exists in
// the engine. plan() works out the changes to a whole wanted graph and commit() applies them in one
// fixed order (docs/design/knobly-config.md); every change is told to the observers first, then to
// the subscribers (rule 4), and close() shuts it to edits before the node framework stops (rule 5).
#include "ear/core/desired_graph.hpp"

#include <algorithm>
#include <cstdint>
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
	/// Bumped by every change; a Plan records it, so commit() can tell a stale plan.
	std::uint64_t generation = 0;

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
};

DesiredGraph::DesiredGraph(const NodeCatalogue& catalogue) : impl_(std::make_unique<Impl>(catalogue)) {}

DesiredGraph::~DesiredGraph() = default;

void DesiredGraph::observe(DesiredNodeListener& listener) { impl_->nodeObservers.push_back(&listener); }

void DesiredGraph::observe(DesiredRouteListener& listener) { impl_->routeObservers.push_back(&listener); }

void DesiredGraph::subscribe(DesiredNodeListener& listener) { impl_->nodeSubscribers.push_back(&listener); }

void DesiredGraph::subscribe(DesiredRouteListener& listener) { impl_->routeSubscribers.push_back(&listener); }

struct Plan::Impl {
	const void* graph;                 // the graph it was made on (its Impl)
	std::uint64_t generation;          // that graph's generation then
	std::vector<Route> routesRemoved;  // Route order
	std::vector<NodeName> nodesRemoved;   // name order; replaced nodes included
	std::vector<std::pair<NodeSpec, std::shared_ptr<const NodeConfig>>> nodesUpdated;   // name order
	std::vector<std::pair<NodeSpec, std::shared_ptr<const NodeConfig>>> nodesAdded;     // name order; replaced included
	std::vector<Route> routesAdded;    // Route order
};

Plan::Plan(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Plan::Plan(Plan&& other) noexcept = default;
Plan& Plan::operator=(Plan&& other) noexcept = default;
Plan::~Plan() = default;

bool Plan::empty() const noexcept {
	return impl_ == nullptr || (impl_->routesRemoved.empty() && impl_->nodesRemoved.empty() &&
	                            impl_->nodesUpdated.empty() && impl_->nodesAdded.empty() && impl_->routesAdded.empty());
}

std::expected<Plan, CommandError> DesiredGraph::plan(std::span<const NodeSpec> nodes,
                                                     std::span<const Route> routes) const {
	if (impl_->closed) return std::unexpected(Stopping{});

	// The wanted nodes, checked in the given order; a node equal to a held one keeps its config.
	std::map<NodeName, Impl::Wanted, std::less<>> wanted;
	for (const auto& node : nodes) {
		if (!validName(node.name)) return std::unexpected(InvalidName{node.name});
		std::shared_ptr<const NodeConfig> config;
		if (const auto held = impl_->nodes.find(node.name); held != impl_->nodes.end() && held->second.spec == node) {
			config = held->second.config;
		} else {
			auto parsed = impl_->parse(node);
			if (!parsed) return std::unexpected(std::move(parsed.error()));
			config = std::move(*parsed);
		}
		if (!wanted.emplace(node.name, Impl::Wanted{node, std::move(config)}).second)
			return std::unexpected(DuplicateNode{node.name});
	}

	// The wanted routes, checked in the given order against the wanted nodes' ports.
	std::set<Route> wantedRoutes;
	const auto portsOf = [&](const PortRef& end) -> std::expected<const Ports*, CommandError> {
		const auto it = wanted.find(end.node);
		if (it == wanted.end()) return std::unexpected(UnknownNode{end.node});
		const Ports& ports = it->second.config->ports();
		if (!declares(ports.inputs, end.port) && !declares(ports.outputs, end.port))
			return std::unexpected(UnknownPort{end});
		return &ports;
	};
	for (const auto& route : routes) {
		const auto from = portsOf(route.from);
		if (!from) return std::unexpected(from.error());
		const auto to = portsOf(route.to);
		if (!to) return std::unexpected(to.error());
		if (!declares((*from)->outputs, route.from.port) || !declares((*to)->inputs, route.to.port))
			return std::unexpected(WrongDirection{route});
		wantedRoutes.insert(route);
	}

	auto changes = std::make_unique<Plan::Impl>();
	changes->graph = impl_.get();
	changes->generation = impl_->generation;

	// Nodes, in name order: removed, replaced (removed and added), updated, added.
	std::set<NodeName, std::less<>> replaced;
	for (const auto& [name, held] : impl_->nodes) {
		const auto it = wanted.find(name);
		if (it == wanted.end()) {
			changes->nodesRemoved.push_back(name);
			continue;
		}
		const auto& next = it->second;
		if (next.spec == held.spec) continue;
		if (next.spec.type == held.spec.type && samePorts(next.config->ports(), held.config->ports())) {
			changes->nodesUpdated.emplace_back(next.spec, next.config);
		} else {
			changes->nodesRemoved.push_back(name);
			replaced.insert(name);
		}
	}
	for (const auto& [name, next] : wanted)
		if (!impl_->nodes.contains(name) || replaced.contains(name)) changes->nodesAdded.emplace_back(next.spec, next.config);

	// Routes, in Route order: a held route goes when it is not wanted or names a replaced node; a
	// wanted route comes when it is not held or names a replaced node.
	const auto namesReplaced = [&](const Route& route) {
		return replaced.contains(route.from.node) || replaced.contains(route.to.node);
	};
	for (const auto& route : impl_->routes)
		if (!wantedRoutes.contains(route) || namesReplaced(route)) changes->routesRemoved.push_back(route);
	for (const auto& route : wantedRoutes)
		if (!impl_->routes.contains(route) || namesReplaced(route)) changes->routesAdded.push_back(route);

	return Plan(std::move(changes));
}

std::expected<void, CommandError> DesiredGraph::commit(Plan plan) {
	if (plan.impl_ == nullptr || plan.impl_->graph != impl_.get())
		throw std::logic_error("DesiredGraph::commit: the plan was made on another graph");
	if (impl_->closed) return std::unexpected(Stopping{});
	if (plan.impl_->generation != impl_->generation)
		throw std::logic_error("DesiredGraph::commit: the graph changed since the plan was made");

	auto& changes = *plan.impl_;
	++impl_->generation;
	for (const auto& route : changes.routesRemoved) {
		impl_->routes.erase(route);
		impl_->tellRoutes([&](DesiredRouteListener& listener) { listener.routeRemoved(route); });
	}
	for (const auto& name : changes.nodesRemoved) {
		impl_->nodes.erase(name);
		impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeRemoved(name); });
	}
	for (auto& [spec, config] : changes.nodesUpdated) {
		auto& held = impl_->nodes.at(spec.name);
		held.spec = std::move(spec);
		held.config = std::move(config);
		impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeUpdated(held.spec, held.config); });
	}
	for (auto& [spec, config] : changes.nodesAdded) {
		const auto name = spec.name;
		const auto& added = impl_->nodes.emplace(name, Impl::Wanted{std::move(spec), std::move(config)}).first->second;
		impl_->tellNodes([&](DesiredNodeListener& listener) { listener.nodeAdded(added.spec, added.config); });
	}
	for (const auto& route : changes.routesAdded) {
		impl_->routes.insert(route);
		impl_->tellRoutes([&](DesiredRouteListener& listener) { listener.routeAdded(route); });
	}
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
