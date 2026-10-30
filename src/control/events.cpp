// The graph as the client sees it (docs/design/batch2.md section 6): the graph notifications
// (events.hpp), the levels notification, and the Graph that graph.get answers (encodeGraph,
// declared in commands.hpp). They share one encoding of a node, its state and a route's status,
// so a snapshot and the events after it always agree.
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"

#include <cmath>
#include <type_traits>

#include "ear/control/graph_json.hpp"
#include "ear/control/rpc.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

/// {"name", "type", "settings", "inputs", "outputs"}: public ports only.
json encodeNode(const NodeSpec& node, const Ports& ports) {
	return {{"name", node.name},     {"type", node.type},       {"settings", node.settings},
	        {"inputs", ports.inputs}, {"outputs", ports.outputs}};
}

/// Adds "state" and, while failed, "reason": {"code", "detail"}.
void addState(json& into, NodeState state, const std::optional<FailureReason>& reason) {
	into["state"] = stateName(state);
	if (reason) into["reason"] = {{"code", failureCode(reason->code)}, {"detail", reason->detail}};
}

/// {"from", "to", "status", "detail"?}: detail only when refused (as the status log prints it).
json encodeRouteStatus(const RouteStatus& status) {
	json route = encodeRoute(status.route);
	route["status"] = matchName(status.match);
	if (status.match == RouteMatch::refused && !status.detail.empty()) route["detail"] = status.detail;
	return route;
}

/// Linear, 4 decimals (section 6).
double fourDecimals(float value) { return std::round(static_cast<double>(value) * 10000.0) / 10000.0; }

} // namespace

json encodeGraph(const GraphView& view) {
	json nodes = json::array();
	for (const NodeView& node : view.nodes) {
		json entry = encodeNode(node.spec, node.ports);
		addState(entry, node.state, node.reason);
		nodes.push_back(std::move(entry));
	}
	json routes = json::array();
	for (const RouteStatus& route : view.routes) routes.push_back(encodeRouteStatus(route));
	return {{"graphFrequency", view.engine.format.rate},
	        {"blockSize", view.engine.format.maxBlock},
	        {"realtime", view.engine.realtime},
	        {"scheduling", view.engine.scheduling},
	        {"types", view.types},
	        {"nodes", std::move(nodes)},
	        {"routes", std::move(routes)}};
}

std::string encodeEvent(const GraphEvent& event) {
	return std::visit(
		[](const auto& e) -> std::string {
			using E = std::decay_t<decltype(e)>;
			if constexpr (std::is_same_v<E, NodeAdded>) {
				return encodeNotification("node.added", encodeNode(e.node, e.ports));
			} else if constexpr (std::is_same_v<E, NodeUpdated>) {
				return encodeNotification("node.updated", {{"name", e.node}, {"settings", e.settings}});
			} else if constexpr (std::is_same_v<E, NodeRemoved>) {
				return encodeNotification("node.removed", {{"name", e.node}});
			} else if constexpr (std::is_same_v<E, NodeChanged>) {
				json params = {{"name", e.status.name}};
				addState(params, e.status.state, e.status.reason);
				return encodeNotification("node.state", params);
			} else if constexpr (std::is_same_v<E, NodeStopped>) {
				return encodeNotification("node.stopped", {{"name", e.node}, {"droppedFrames", e.droppedFrames}});
			} else if constexpr (std::is_same_v<E, RouteAdded>) {
				return encodeNotification("route.added", encodeRoute(e.route));
			} else if constexpr (std::is_same_v<E, RouteRemoved>) {
				return encodeNotification("route.removed", encodeRoute(e.route));
			} else {
				static_assert(std::is_same_v<E, RouteChanged>);
				return encodeNotification("route.status", encodeRouteStatus(e.status));
			}
		},
		event);
}

std::string encodeLevels(std::span<const NodeLevels> levels) {
	json nodes = json::object();
	for (const NodeLevels& node : levels) {
		json ports = json::object();
		for (const PortLevel& port : node.ports)
			ports[port.port] = json::array({fourDecimals(port.level.peak), fourDecimals(port.level.rms)});
		nodes[node.node] = std::move(ports);
	}
	return encodeNotification("levels", {{"nodes", std::move(nodes)}});
}

} // namespace ear::control
