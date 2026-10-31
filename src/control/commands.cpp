// The control port's methods as typed commands (docs/design/batch2.md sections 4.5 and 6,
// commands.hpp): decoded on the control thread with graph_json, run by the graph file keeper
// (docs/design/knobly-config.md), their results encoded back on the control thread. encodeGraph lives in events.cpp beside the event
// encoders, so a node's and a route's status JSON has one home; encodeGraphFile lives here beside
// the file refusals, so a diagnostic's JSON has one home (docs/design/knobly-config.md, "Wire
// additions").
#include "ear/control/commands.hpp"

#include <string>
#include <utility>
#include <vector>

#include "ear/control/graph_json.hpp"

namespace ear::control {
namespace {

using nlohmann::json;

std::unexpected<RpcError> refuse(const JsonError& error) {
	return std::unexpected(RpcError{invalidParams, error.detail, nullptr});
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

std::expected<Command, RpcError> decodeUpdateNode(const json& params, const JsonPointer& at) {
	if (auto checked = requireObject(params, at, {"name", "settings"}); !checked) return refuse(checked.error());
	auto name = requireName(params, at, "name");
	if (!name) return refuse(name.error());
	auto settings = requireMember(params, at, "settings");   // passed through unchecked: the node's type judges it
	if (!settings) return refuse(settings.error());
	return UpdateNode{std::move(*name), **settings};
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
	if (!(*value)->is_string()) return std::unexpected(JsonError{(at / key).to_string() + ": must be a string"});
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
	if (const auto* invalid = std::get_if<InvalidText>(&refusal)) data["diagnostics"] = encodeDiagnostics(invalid->diagnostics);
	if (const auto* shared = std::get_if<SharedDefinition>(&refusal)) addPlace(data, shared->place);
	return RpcError{commandRefused, describe(refusal), std::move(data)};
}

using Decoder = std::expected<Command, RpcError> (*)(const json&, const JsonPointer&);
struct Method { std::string_view name; Decoder decode; };

/// The graph and graph file methods of section 6 and docs/design/knobly-config.md (levels.subscribe
/// is the hub's, not a graph command).
constexpr Method methods[] = {
	{"graph.get", decodeGetGraph},          {"graph.subscribe", decodeGetGraph},
	{"node.add", decodeAddNode},            {"node.update", decodeUpdateNode},
	{"node.remove", decodeRemoveNode},      {"route.add", decodeAddRoute},
	{"route.remove", decodeRemoveRoute},    {"graphFile.get", decodeGetGraphFile},
	{"graphFile.subscribe", decodeGetGraphFile}, {"graphFile.set", decodeSetGraphFile},
};

} // namespace

std::expected<Command, RpcError> decodeCommand(std::string_view method, const json& params) {
	const JsonPointer root;
	// Named params only: positional params are refused whatever the method (section 6).
	if (params.is_array()) return refuse(requireObject(params, root, {}).error());
	const json empty = json::object();
	const json& named = params.is_null() ? empty : params;   // null: params absent
	for (const Method& known : methods)
		if (known.name == method) return known.decode(named, root);
	return std::unexpected(RpcError{methodNotFound, "Method not found", nullptr});
}

std::expected<json, RpcError> encodeCommandResult(const CommandResult& result) {
	if (const auto* view = std::get_if<GraphView>(&result)) return encodeGraph(*view);
	if (const auto* file = std::get_if<GraphFile>(&result)) return encodeGraphFile(*file);
	if (const auto* refusal = std::get_if<FileRefusal>(&result)) return std::unexpected(encodeFileRefusal(*refusal));
	const auto& edit = std::get<std::expected<void, CommandError>>(result);
	if (edit) return json::object();
	return std::unexpected(
		RpcError{commandRefused, describe(edit.error()), {{"code", std::string(errorCode(edit.error()))}}});
}

json encodeGraphFile(const GraphFile& file) {
	json out = {{"version", file.version}, {"text", file.text}, {"diagnostics", encodeDiagnostics(file.diagnostics)}};
	if (file.running) out["running"] = *file.running;
	if (file.refusal) {
		json refusal = {{"code", std::string(errorCode(file.refusal->error))}, {"message", describe(file.refusal->error)}};
		if (file.refusal->place) addPlace(refusal, *file.refusal->place);
		out["refusal"] = std::move(refusal);
	}
	json pending = json::array();
	for (const FormatKey key : file.pendingRestart) pending.push_back(formatKeyName(key));
	out["pendingRestart"] = std::move(pending);
	if (file.writeFailed) out["writeFailed"] = *file.writeFailed;
	return out;
}

} // namespace ear::control
