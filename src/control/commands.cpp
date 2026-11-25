// The control port's methods as typed commands (docs/control-port.md, commands.hpp): decoded on the control thread with
// graph_json, run by the graph file keeper (docs/graph-file.md), their results encoded back on the control thread.
// encodeGraph lives in events.cpp beside the event encoders, so a node's and a route's status JSON has one home;
// encodeGraphFile lives here beside the file refusals, so a diagnostic's JSON has one home
// (docs/control-port.md#the-graph-file-over-the-port).
#include "ear/control/commands.hpp"

#include <format>
#include <string>
#include <utility>
#include <vector>

#include "ear/control/graph_json.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

/// -32602 with the pointer it names in data (docs/control-port.md#protocol-and-features).
std::unexpected<RpcError> refuse(const JsonError& error) {
	return std::unexpected(RpcError{invalidParams, error.detail, {{"pointer", error.pointer}}});
}

std::expected<Command, RpcError> decodeGetGraph(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {}); !checked) return refuse(checked.error());
	return GetGraph{};
}

std::expected<Command, RpcError> decodeAddNode(const json& params, const JsonPointer& at) {
	auto node = decodeNodeSpec(params, at);
	if (!node) return refuse(node.error());
	return AddNode{std::move(*node)};
}

/// An optional boolean member, false when absent.
std::expected<bool, JsonError> optionalFlag(const json& object, const JsonPointer& at, const std::string& key) {
	const auto found = object.find(key);
	if (found == object.end()) return false;
	if (!found->is_boolean())
		return std::unexpected(JsonError{(at / key).to_string() + ": must be a boolean", (at / key).to_string()});
	return found->get<bool>();
}

std::expected<Command, RpcError> decodeUpdateNode(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name", "settings", "dropRoutes"}); !checked)
		return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	auto settings = requireMember(params, at, "settings"); // passed through unchecked: the node's type judges it
	if (!settings) return refuse(settings.error());
	auto dropRoutes = optionalFlag(params, at, "dropRoutes");
	if (!dropRoutes) return refuse(dropRoutes.error());
	return UpdateNode{std::move(*name), **settings, *dropRoutes};
}

std::expected<Command, RpcError> decodeSetNode(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name", "settings", "dryRun", "dropRoutes"}); !checked)
		return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	auto settings = requireMember(params, at, "settings"); // the keeper merges it; the node's type judges the result
	if (!settings) return refuse(settings.error());
	auto dryRun = optionalFlag(params, at, "dryRun");
	if (!dryRun) return refuse(dryRun.error());
	auto dropRoutes = optionalFlag(params, at, "dropRoutes");
	if (!dropRoutes) return refuse(dropRoutes.error());
	return SetNode{std::move(*name), **settings, *dryRun, *dropRoutes};
}

std::expected<Command, RpcError> decodeRemoveNode(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name"}); !checked) return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	return RemoveNode{std::move(*name)};
}

std::expected<Command, RpcError> decodeAddRoute(const json& params, const JsonPointer& at) {
	auto route = decodeRoute(params, at);
	if (!route) return refuse(route.error());
	return AddRoute{std::move(*route)};
}

std::expected<Command, RpcError> decodeRemoveRoute(const json& params, const JsonPointer& at) {
	auto route = decodeRoute(params, at);
	if (!route) return refuse(route.error());
	return RemoveRoute{std::move(*route)};
}

std::expected<Command, RpcError> decodeGetGraphFile(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {}); !checked) return refuse(checked.error());
	return GetGraphFile{};
}

/// A string member, which may be empty (an empty ear.conf is a text).
std::expected<std::string, JsonError> requireText(const json& object, const JsonPointer& at, const std::string& key) {
	auto value = requireMember(object, at, key);
	if (!value) return std::unexpected(value.error());
	if (!(*value)->is_string())
		return std::unexpected(JsonError{(at / key).to_string() + ": must be a string", (at / key).to_string()});
	return (*value)->get<std::string>();
}

std::expected<Command, RpcError> decodeSetGraphFile(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"text", "version"}); !checked) return refuse(checked.error());
	auto text = requireText(params, at, "text");
	if (!text) return refuse(text.error());
	auto version = requireText(params, at, "version");
	if (!version) return refuse(version.error());
	return SetGraphFile{std::move(*text), std::move(*version)};
}

std::expected<Command, RpcError> decodeSetNodeText(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name", "text", "version"}); !checked) return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	auto text = requireText(params, at, "text");
	if (!text) return refuse(text.error());
	auto version = requireText(params, at, "version");
	if (!version) return refuse(version.error());
	return SetNodeText{std::move(*name), std::move(*text), std::move(*version)};
}

std::expected<Command, RpcError> decodeAddNodeText(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"text", "version"}); !checked) return refuse(checked.error());
	auto text = requireText(params, at, "text");
	if (!text) return refuse(text.error());
	auto version = requireText(params, at, "version");
	if (!version) return refuse(version.error());
	return AddNodeText{std::move(*text), std::move(*version)};
}

/// node.invoke: {"name", "action", "args"?}; the args pass through unchecked but for being an object: the action's
/// table judges them (docs/control-port.md#actions).
std::expected<Command, RpcError> decodeInvokeNode(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name", "action", "args"}); !checked) return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	auto action = requireText(params, at, "action");
	if (!action) return refuse(action.error());
	if (action->empty())
		return refuse(JsonError{(at / "action").to_string() + ": must not be empty", (at / "action").to_string()});
	json args = nullptr;
	if (const auto given = params.find("args"); given != params.end()) {
		if (!given->is_object())
			return refuse(JsonError{(at / "args").to_string() + ": must be an object", (at / "args").to_string()});
		args = *given;
	}
	return InvokeNode{std::move(*name), std::move(*action), std::move(args)};
}

/// node.start, node.stop, node.restart, node.disable and node.enable: {"name"}.
template <class Verb> std::expected<Command, RpcError> decodeNamed(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name"}); !checked) return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	return Verb{std::move(*name)};
}

json encodeRoutes(const std::vector<Route>& routes) {
	json all = json::array();
	for (const Route& route : routes) all.push_back(encodeRoute(route));
	return all;
}

/// {"line", "column"} added to `into`.
void addPlace(json& into, const TextPlace& place) {
	into["line"] = place.line;
	into["column"] = place.column;
}

json encodeDiagnostics(const std::vector<Diagnostic>& diagnostics) {
	json all = json::array();
	for (const Diagnostic& d : diagnostics) {
		json entry = {{"severity", severityName(d.severity)}, {"code", d.code}, {"message", d.message}};
		addPlace(entry, d.place);
		all.push_back(std::move(entry));
	}
	return all;
}

RpcError encodeFileRefusal(const FileRefusal& refusal) {
	json data = {{"code", std::string(fileRefusalCode(refusal))}};
	if (const auto* invalid = std::get_if<InvalidText>(&refusal))
		data["diagnostics"] = encodeDiagnostics(invalid->diagnostics);
	if (const auto* shared = std::get_if<SharedDefinition>(&refusal)) addPlace(data, shared->place);
	return RpcError{commandRefused, describe(refusal), std::move(data)};
}

using Decoder = std::expected<Command, RpcError> (*)(const json&, const JsonPointer&);
struct Method {
	std::string_view name;
	Decoder decode;
};

/// The graph and graph file methods (docs/control-port.md#methods; levels.subscribe is the hub's, not a graph
/// command).
constexpr Method methods[] = {
	{"graph.get", decodeGetGraph},
	{"graph.subscribe", decodeGetGraph},
	{"node.add", decodeAddNode},
	{"node.update", decodeUpdateNode},
	{"node.set", decodeSetNode},
	{"node.remove", decodeRemoveNode},
	{"route.add", decodeAddRoute},
	{"route.remove", decodeRemoveRoute},
	{"graphFile.get", decodeGetGraphFile},
	{"graphFile.subscribe", decodeGetGraphFile},
	{"graphFile.set", decodeSetGraphFile},
	{"node.start", decodeNamed<StartNode>},
	{"node.stop", decodeNamed<StopNode>},
	{"node.restart", decodeNamed<RestartNode>},
	{"node.disable", decodeNamed<DisableNode>},
	{"node.enable", decodeNamed<EnableNode>},
	{"node.setText", decodeSetNodeText},
	{"node.addText", decodeAddNodeText},
	{"node.invoke", decodeInvokeNode},
};

RpcError encodeControlRefusal(const ControlRefusal& refusal) {
	json data = {{"code", std::string(controlRefusalCode(refusal))}};
	if (const auto* lost = std::get_if<NotRestartable>(&refusal))
		data["reason"] = std::string(reasonName(lost->reason));
	if (const auto* inUse = std::get_if<PortsInUse>(&refusal)) {
		data["ports"] = inUse->ports;
		data["routes"] = encodeRoutes(inUse->routes);
	}
	if (const auto* unknown = std::get_if<UnknownAction>(&refusal)) data["action"] = unknown->action;
	if (const auto* invalid = std::get_if<InvalidArgs>(&refusal)) {
		if (!invalid->key.empty()) data["key"] = invalid->key;
		data["detail"] = std::string(argsWords(*invalid));
	}
	if (const auto* notRunning = std::get_if<NotRunning>(&refusal))
		data["phase"] = std::string(phaseName(notRunning->phase));
	if (const auto* failed = std::get_if<ActionFailed>(&refusal)) data["detail"] = failed->detail;
	return RpcError{commandRefused, describe(refusal), std::move(data)};
}

} // namespace

std::expected<Command, RpcError> decodeCommand(std::string_view method, const json& params) {
	const JsonPointer root;
	// Named params only: positional params are refused whatever the method.
	if (params.is_array()) return refuse(requireObject(params, root, {}).error());
	const json empty = json::object();
	const json& named = params.is_null() ? empty : params; // null: params absent
	for (const Method& known : methods)
		if (known.name == method) return known.decode(named, root);
	return std::unexpected(RpcError{methodNotFound, "Method not found", nullptr});
}

std::expected<json, RpcError> encodeCommandResult(const CommandResult& result) {
	if (const auto* view = std::get_if<GraphView>(&result)) return encodeGraph(*view);
	if (const auto* file = std::get_if<GraphFile>(&result)) return encodeGraphFile(*file);
	if (const auto* refusal = std::get_if<FileRefusal>(&result)) return std::unexpected(encodeFileRefusal(*refusal));
	if (const auto* applied = std::get_if<Applied>(&result)) {
		json out = {{"applied", appliedName(applied->kind)}};
		if (applied->inPlace) out["inPlace"] = *applied->inPlace;
		if (applied->ports) out["ports"] = encodePortsMoved(*applied->ports);
		if (applied->routesDropped) out["routesDropped"] = encodeRoutes(*applied->routesDropped);
		if (applied->routes) out["routes"] = encodeRoutes(*applied->routes);
		return out;
	}
	if (const auto* refusal = std::get_if<ControlRefusal>(&result))
		return std::unexpected(encodeControlRefusal(*refusal));
	const auto& edit = std::get<std::expected<void, CommandError>>(result);
	if (edit) return json::object();
	json data = {{"code", std::string(errorCode(edit.error()))}};
	// The key and the words apart, so a client places the refusal under its field (docs/control-port.md#error-codes).
	if (const auto* invalid = std::get_if<InvalidSettings>(&edit.error())) {
		if (!invalid->key.empty()) data["key"] = invalid->key;
		data["detail"] = std::string(refusalWords(*invalid));
	}
	return std::unexpected(RpcError{commandRefused, describe(edit.error()), std::move(data)});
}

json encodeGraphFile(const GraphFile& file) {
	json out = {{"version", file.version}, {"text", file.text}, {"diagnostics", encodeDiagnostics(file.diagnostics)}};
	if (file.running) out["running"] = *file.running;
	if (file.refusal) {
		json refusal = {{"code", std::string(errorCode(file.refusal->error))},
		                {"message", describe(file.refusal->error)}};
		if (file.refusal->place) addPlace(refusal, *file.refusal->place);
		out["refusal"] = std::move(refusal);
	}
	json pending = json::array();
	for (const FormatKey key : file.pendingRestart) pending.push_back(formatKeyName(key));
	out["pendingRestart"] = std::move(pending);
	if (file.writeFailed) out["writeFailed"] = *file.writeFailed;
	if (file.nodeTexts) {
		json texts = json::array();
		for (const NodeText& node : *file.nodeTexts) {
			json entry = {{"name", node.name}, {"start", json::object()}, {"end", json::object()}};
			addPlace(entry["start"], node.start);
			addPlace(entry["end"], node.end);
			if (node.shared) entry["shared"] = true;
			texts.push_back(std::move(entry));
		}
		out["nodeTexts"] = std::move(texts);
	}
	return out;
}

// ---- lifecycle_wire.hpp: the one home of the lifecycle's wire strings ----

std::string_view appliedName(AppliedKind kind) noexcept {
	switch (kind) {
	case AppliedKind::start:
		return "start";
	case AppliedKind::stop:
		return "stop";
	case AppliedKind::restart:
		return "restart";
	case AppliedKind::retry:
		return "retry";
	case AppliedKind::disable:
		return "disable";
	case AppliedKind::enable:
		return "enable";
	case AppliedKind::stored:
		return "stored";
	case AppliedKind::unchanged:
		return "unchanged";
	case AppliedKind::reconfigure:
		return "reconfigure";
	case AppliedKind::reshape:
		return "reshape";
	}
	return "unchanged";
}

std::string_view controlRefusalCode(const ControlRefusal& refusal) noexcept {
	if (std::holds_alternative<NodeDisabled>(refusal)) return "node-disabled";
	if (std::holds_alternative<PortsInUse>(refusal)) return "ports-in-use";
	if (std::holds_alternative<UnknownAction>(refusal)) return "unknown-action";
	if (std::holds_alternative<InvalidArgs>(refusal)) return "invalid-args";
	if (std::holds_alternative<NotRunning>(refusal)) return "not-running";
	if (std::holds_alternative<Restarted>(refusal)) return "restarted";
	if (std::holds_alternative<ActionFailed>(refusal)) return "action-failed";
	if (std::holds_alternative<ActionTimeout>(refusal)) return "action-timeout";
	return "not-restartable";
}

std::string_view argsWords(const InvalidArgs& refusal) noexcept {
	const std::string_view text = refusal.text;
	if (refusal.key.empty() || !text.starts_with(refusal.key)) return text;
	const std::string_view rest = text.substr(refusal.key.size());
	return rest.starts_with(": ") ? rest.substr(2) : text;
}

std::string_view reasonName(NotRestartable::Reason reason) noexcept {
	return reason == NotRestartable::Reason::blockLost ? "block-lost" : "not-started";
}

std::string describe(const ControlRefusal& refusal) {
	if (const auto* disabled = std::get_if<NodeDisabled>(&refusal))
		return std::string(controlRefusalCode(refusal)) + " " + disabled->node;
	if (const auto* inUse = std::get_if<PortsInUse>(&refusal)) {
		std::string ports;
		for (const PortName& port : inUse->ports) ports += (ports.empty() ? "" : ", ") + port;
		const std::size_t n = inUse->routes.size();
		return std::format("{} {}: {} ({} route{})", controlRefusalCode(refusal), inUse->node, ports, n,
		                   n == 1 ? "" : "s");
	}
	const std::string code(controlRefusalCode(refusal));
	if (const auto* unknown = std::get_if<UnknownAction>(&refusal))
		return code + " " + unknown->node + ": " + unknown->action;
	if (const auto* invalid = std::get_if<InvalidArgs>(&refusal))
		return code + " " + invalid->node + " " + invalid->action + ": " + invalid->text;
	if (const auto* notRunning = std::get_if<NotRunning>(&refusal))
		return code + " " + notRunning->node + ": " + std::string(phaseName(notRunning->phase));
	if (const auto* restarted = std::get_if<Restarted>(&refusal)) return code + " " + restarted->node;
	if (const auto* failed = std::get_if<ActionFailed>(&refusal))
		return failed->detail; // its text (docs/control-port.md#error-codes)
	if (const auto* timeout = std::get_if<ActionTimeout>(&refusal))
		return code + " " + timeout->node + " " + timeout->action;
	const auto& lost = std::get<NotRestartable>(refusal);
	return code + " " + lost.node + ": " + std::string(reasonName(lost.reason));
}

std::string_view verbName(LifecycleVerb verb) noexcept {
	switch (verb) {
	case LifecycleVerb::start:
		return "start";
	case LifecycleVerb::stop:
		return "stop";
	case LifecycleVerb::restart:
		return "restart";
	}
	return "start";
}

} // namespace ear::control
