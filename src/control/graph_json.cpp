// The one home of node and route JSON (docs/design/batch2.md section 4.5, graph_json.hpp): the
// graph file (graph_file.cpp) and the control port's commands (commands.cpp) decode with it, so
// the same bad JSON gives the same text through either. Every error names its JSON pointer.
#include "ear/control/graph_json.hpp"

#include <format>
#include <utility>

namespace ear::control {
namespace {

using nlohmann::json;

std::unexpected<JsonError> fail(const JsonPointer& at, std::string_view problem) {
	const std::string where = at.empty() ? std::string("the document") : at.to_string();
	return std::unexpected(JsonError{std::format("{}: {}", where, problem)});
}

std::expected<PortRef, JsonError> decodePortRef(const json& route, const JsonPointer& at, const std::string& key) {
	auto value = requireMember(route, at, key);
	if (!value) return std::unexpected(value.error());
	const JsonPointer here = at / key;
	if (auto checked = requireObject(**value, here, {"node", "port"}); !checked)
		return std::unexpected(checked.error());
	auto node = requireName(**value, here, "node");
	if (!node) return std::unexpected(node.error());
	auto port = requireName(**value, here, "port");
	if (!port) return std::unexpected(port.error());
	return PortRef{std::move(*node), std::move(*port)};
}

} // namespace

std::expected<void, JsonError> requireObject(const json& value, const JsonPointer& at,
                                             std::initializer_list<std::string_view> keys) {
	if (!value.is_object()) return fail(at, "must be an object");
	for (const auto& [key, member] : value.items()) {
		bool known = false;
		for (const std::string_view allowed : keys) known = known || key == allowed;
		if (!known) return fail(at / key, "unknown key");
	}
	return {};
}

std::expected<const json*, JsonError> requireMember(const json& object, const JsonPointer& at,
                                                    const std::string& key) {
	const auto found = object.find(key);
	if (found == object.end()) return fail(at / key, "missing");
	return &*found;
}

std::expected<std::string, JsonError> requireName(const json& object, const JsonPointer& at, const std::string& key) {
	auto value = requireMember(object, at, key);
	if (!value) return std::unexpected(value.error());
	if (!(*value)->is_string() || (*value)->get_ref<const std::string&>().empty())
		return fail(at / key, "must be a non-empty string");
	return (*value)->get<std::string>();
}

std::expected<NodeSpec, JsonError> decodeNodeSpec(const json& value, const JsonPointer& at) {
	if (auto checked = requireObject(value, at, {"name", "type", "settings"}); !checked)
		return std::unexpected(checked.error());
	auto name = requireName(value, at, "name");
	if (!name) return std::unexpected(name.error());
	auto type = requireName(value, at, "type");
	if (!type) return std::unexpected(type.error());
	const auto settings = value.find("settings");
	return NodeSpec{std::move(*name), std::move(*type), settings == value.end() ? json::object() : *settings};
}

std::expected<Route, JsonError> decodeRoute(const json& value, const JsonPointer& at) {
	if (auto checked = requireObject(value, at, {"from", "to"}); !checked) return std::unexpected(checked.error());
	auto from = decodePortRef(value, at, "from");
	if (!from) return std::unexpected(from.error());
	auto to = decodePortRef(value, at, "to");
	if (!to) return std::unexpected(to.error());
	return Route{std::move(*from), std::move(*to)};
}

json encodePortRef(const PortRef& port) { return {{"node", port.node}, {"port", port.port}}; }

json encodeRoute(const Route& route) {
	return {{"from", encodePortRef(route.from)}, {"to", encodePortRef(route.to)}};
}

} // namespace ear::control
