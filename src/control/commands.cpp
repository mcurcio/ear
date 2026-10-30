// The control port's methods as typed commands (docs/design/batch2.md sections 4.5 and 6,
// commands.hpp): decoded on the control thread with graph_json, run on the main loop, their
// results encoded back on the control thread. encodeGraph lives in events.cpp beside the event
// encoders, so a node's and a route's status JSON has one home.
#include "ear/control/commands.hpp"

#include <string>
#include <type_traits>
#include <utility>

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
	auto settings = requireMember(params, at, "settings");   // passed through unchecked, as in ear.json
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

using Decoder = std::expected<Command, RpcError> (*)(const json&, const JsonPointer&);
struct Method { std::string_view name; Decoder decode; };

/// The graph methods of section 6 (levels.subscribe is the hub's, not a graph command).
constexpr Method methods[] = {
	{"graph.get", decodeGetGraph},       {"graph.subscribe", decodeGetGraph}, {"node.add", decodeAddNode},
	{"node.update", decodeUpdateNode},   {"node.remove", decodeRemoveNode},   {"route.add", decodeAddRoute},
	{"route.remove", decodeRemoveRoute},
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

CommandResult runCommand(const Command& command, GraphControl& control, const GraphReader& reader) {
	return std::visit(
		[&](const auto& c) -> CommandResult {
			using C = std::decay_t<decltype(c)>;
			if constexpr (std::is_same_v<C, GetGraph>) return reader.read();
			else if constexpr (std::is_same_v<C, AddNode>) return control.addNode(c.node);
			else if constexpr (std::is_same_v<C, UpdateNode>) return control.updateNode(c.node, c.settings);
			else if constexpr (std::is_same_v<C, RemoveNode>) return control.removeNode(c.node);
			else if constexpr (std::is_same_v<C, AddRoute>) return control.addRoute(c.route);
			else {
				static_assert(std::is_same_v<C, RemoveRoute>);
				return control.removeRoute(c.route);
			}
		},
		command);
}

std::expected<json, RpcError> encodeCommandResult(const CommandResult& result) {
	if (const auto* view = std::get_if<GraphView>(&result)) return encodeGraph(*view);
	const auto& edit = std::get<std::expected<void, CommandError>>(result);
	if (edit) return json::object();
	return std::unexpected(
		RpcError{commandRefused, describe(edit.error()), {{"code", std::string(errorCode(edit.error()))}}});
}

} // namespace ear::control
