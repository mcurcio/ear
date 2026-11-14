// The graph as the client sees it (docs/control-port.md): the graph notifications
// (events.hpp), the levels notification, the Graph that graph.get answers (encodeGraph, declared
// in commands.hpp), and graphFile.changed (docs/graph-file.md). They share one encoding
// of a node, its health (docs/control-port.md#node-health) and a route's status, so a snapshot and the events
// after it always agree.
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
	return {{"name", node.name},
	        {"type", node.type},
	        {"settings", node.settings},
	        {"inputs", ports.inputs()},
	        {"outputs", ports.outputs()}};
}

/// A cause as the wire carries it (docs/control-port.md#node-health): {"kind"}, "keys" for a settings change, and a
/// restart's own kind (with its keys) under "restart".
json encodeCause(const NodeCause& cause) {
	json out = {{"kind", causeName(cause.kind)}};
	if (cause.kind == NodeCause::Kind::restarting && cause.restart) {
		json restart = {{"kind", causeName(*cause.restart)}};
		if (*cause.restart == NodeCause::Kind::settings) restart["keys"] = cause.keys;
		out["restart"] = std::move(restart);
	} else if (cause.kind == NodeCause::Kind::settings) {
		out["keys"] = cause.keys;
	}
	return out;
}

/// The health members (docs/control-port.md#node-health), one encoding for snapshots, node.added and node.state, so the
/// three cannot drift: state, phase, since (when known), wanted and blockLost always; cause, waitingFor and reason when
/// the health has them.
void addHealth(json& into, const NodeHealth& h, std::optional<std::chrono::system_clock::time_point> since) {
	into["state"] = stateName(h.state);
	into["phase"] = phaseName(h.phase);
	if (since) into["since"] = utcMicros(*since);
	into["wanted"] = wantedName(h.wanted);
	into["blockLost"] = h.blockLost;
	if (h.cause) into["cause"] = encodeCause(*h.cause);
	if (!h.waitingFor.empty()) {
		json waits = json::array();
		for (const WaitingFor& wait : h.waitingFor) {
			json one = {{"kind", waitingKindName(wait.kind)}};
			if (!wait.name.empty()) one["name"] = wait.name;
			waits.push_back(std::move(one));
		}
		into["waitingFor"] = std::move(waits);
	}
	if (h.reason) into["reason"] = {{"code", failureCode(h.reason->code)}, {"detail", h.reason->detail}};
}

/// A node's counters (docs/control-port.md#counters), one encoding for snapshots, node.added and counters.changed.
json encodeNodeCounts(const NodeCounts& counts) {
	return {{"missedCycles", counts.missedCycles},
	        {"overruns", counts.overruns},
	        {"repositions", counts.repositions},
	        {"driverChanges", counts.driverChanges}};
}

/// The graph's counters, lostMs encoded from cutCycles by whoever knows the graph's rate and block size.
json encodeGraphCounts(const GraphCounts& counts, std::uint64_t lostMs) {
	return {{"cycles", counts.cycles}, {"cutCycles", counts.cutCycles}, {"stalls", counts.stalls}, {"lostMs", lostMs}};
}

/// A node as snapshots and node.added carry it: its spec, its ports, its health, its counters, its companions and
/// `disabled` when true.
json encodeNodeWithHealth(const NodeSpec& node, const Ports& ports, const NodeHealth& health,
                          std::optional<std::chrono::system_clock::time_point> since, const NodeCounts& counts,
                          std::span<const CompanionView> companions) {
	json entry = encodeNode(node, ports);
	addHealth(entry, health, since);
	if (node.disabled) entry["disabled"] = true;
	entry["counters"] = encodeNodeCounts(counts);
	entry["companions"] = encodeCompanions(companions);
	return entry;
}

/// {"from", "to", "status", "detail"?}: detail only when refused (as the status log prints it).
json encodeRouteStatus(const RouteStatus& status) {
	json route = encodeRoute(status.route);
	route["status"] = matchName(status.match);
	if (status.match == RouteMatch::refused && !status.detail.empty()) route["detail"] = status.detail;
	return route;
}

/// Linear, 4 decimals.
double fourDecimals(float value) { return std::round(static_cast<double>(value) * 10000.0) / 10000.0; }

} // namespace

json encodeGraph(const GraphView& view) {
	const AudioFormat& format = view.engine.format;
	const std::uint64_t lostMs = view.counts.lostMs(FrameClock(format.rate), format.maxBlock);
	json nodes = json::array();
	for (const NodeView& node : view.nodes)
		nodes.push_back(
			encodeNodeWithHealth(node.spec, node.ports, node.health, node.since, node.counts, node.companions));
	json routes = json::array();
	for (const RouteStatus& route : view.routes) routes.push_back(encodeRouteStatus(route));
	json features = json::array();
	for (const std::string_view feature : wireFeatures) features.push_back(feature);
	return {{"protocol", wireProtocol},
	        {"features", std::move(features)},
	        {"graphFrequency", view.engine.format.rate.hz()},
	        {"blockSize", view.engine.format.maxBlock},
	        {"realtime", view.engine.realtime},
	        {"scheduling", view.engine.scheduling},
	        {"types", view.types},
	        {"nodes", std::move(nodes)},
	        {"routes", std::move(routes)},
	        {"counters", encodeGraphCounts(view.counts, lostMs)}};
}

std::string encodeEvent(const GraphEvent& event) {
	return std::visit(
		[](const auto& e) -> std::string {
			using E = std::decay_t<decltype(e)>;
			if constexpr (std::is_same_v<E, NodeAdded>) {
				return encodeNotification("node.added",
			                              encodeNodeWithHealth(e.node, e.ports, e.health, e.since, e.counts, {}));
			} else if constexpr (std::is_same_v<E, NodeUpdated>) {
				json params = {{"name", e.node}, {"settings", e.settings}, {"applied", outcomeName(e.applied)}};
				if (e.disabled) params["disabled"] = true;
				return encodeNotification("node.updated", params);
			} else if constexpr (std::is_same_v<E, NodeRemoved>) {
				return encodeNotification("node.removed", {{"name", e.node}});
			} else if constexpr (std::is_same_v<E, NodeChanged>) {
				json params = {{"name", e.health.name}};
				addHealth(params, e.health, e.since);
				return encodeNotification("node.state", params);
			} else if constexpr (std::is_same_v<E, NodeStopped>) {
				return encodeNotification("node.stopped", {{"name", e.node}, {"droppedFrames", e.droppedFrames}});
			} else if constexpr (std::is_same_v<E, NodeGone>) {
				return encodeNotification("node.gone", {{"name", e.node}, {"droppedFrames", e.droppedFrames}});
			} else if constexpr (std::is_same_v<E, RouteAdded>) {
				return encodeNotification("route.added", encodeRoute(e.route));
			} else if constexpr (std::is_same_v<E, RouteRemoved>) {
				return encodeNotification("route.removed", encodeRoute(e.route));
			} else if constexpr (std::is_same_v<E, RouteChanged>) {
				return encodeNotification("route.status", encodeRouteStatus(e.status));
			} else if constexpr (std::is_same_v<E, NodeCompanionsChanged>) {
				return encodeNotification("node.companionsChanged",
			                              {{"name", e.node}, {"companions", encodeCompanions(e.companions)}});
			} else {
				static_assert(std::is_same_v<E, CountersChanged>);
				json nodes = json::object();
				for (const NamedCounts& node : e.nodes) nodes[node.name] = encodeNodeCounts(node.counts);
				return encodeNotification(
					"counters.changed", {{"graph", encodeGraphCounts(e.graph, e.lostMs)}, {"nodes", std::move(nodes)}});
			}
		},
		event);
}

std::string_view outcomeName(NodeOutcome outcome) noexcept {
	switch (outcome) {
	case NodeOutcome::restart:
		return "restart";
	case NodeOutcome::disable:
		return "disable";
	case NodeOutcome::enable:
		return "enable";
	case NodeOutcome::stored:
		return "stored";
	}
	return "restart";
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

std::string encodeGraphFileChanged(const GraphFileChanged& change) {
	json params = encodeGraphFile(change.file);
	params["cause"] = causeName(change.cause);
	if (change.droppedEdits > 0) params["droppedEdits"] = change.droppedEdits;
	return encodeNotification("graphFile.changed", params);
}

} // namespace ear::control
