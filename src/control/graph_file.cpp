// The graph file (ear.json v2): read, checked and replayed as control calls at boot
// (docs/design/m1-white-to-file.md section 2, graph_file.hpp). Every error names the JSON
// pointer of what is wrong; readGraphFile adds the file's path.
#include "ear/control/graph_file.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace ear::control {
namespace {

using nlohmann::json;
using Pointer = json::json_pointer;

std::unexpected<GraphFileError> fail(const Pointer& at, std::string_view problem) {
	const std::string where = at.empty() ? std::string("the document") : at.to_string();
	return std::unexpected(GraphFileError{std::format("{}: {}", where, problem)});
}

std::expected<void, GraphFileError> object(const json& value, const Pointer& at,
                                           std::initializer_list<std::string_view> keys) {
	if (!value.is_object()) return fail(at, "must be an object");
	for (const auto& [key, member] : value.items()) {
		bool known = false;
		for (const std::string_view allowed : keys) known = known || key == allowed;
		if (!known) return fail(at / key, "unknown key");
	}
	return {};
}

std::expected<const json*, GraphFileError> required(const json& object, const Pointer& at,
                                                    const std::string& key) {
	const auto found = object.find(key);
	if (found == object.end()) return fail(at / key, "missing");
	return &*found;
}

std::expected<std::uint32_t, GraphFileError> positive(const json& object, const Pointer& at,
                                                      const std::string& key) {
	auto value = required(object, at, key);
	if (!value) return std::unexpected(value.error());
	const json& number = **value;
	if (!number.is_number_unsigned() || number.get<std::uint64_t>() == 0 ||
	    number.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
		return fail(at / key, "must be a whole number from 1 to 4294967295");
	return static_cast<std::uint32_t>(number.get<std::uint64_t>());
}

std::expected<std::string, GraphFileError> name(const json& object, const Pointer& at,
                                                const std::string& key) {
	auto value = required(object, at, key);
	if (!value) return std::unexpected(value.error());
	if (!(*value)->is_string() || (*value)->get_ref<const std::string&>().empty())
		return fail(at / key, "must be a non-empty string");
	return (*value)->get<std::string>();
}

/// An optional array member; absent reads as empty.
std::expected<const json*, GraphFileError> list(const json& object, const Pointer& at,
                                                const std::string& key) {
	static const json empty = json::array();
	const auto found = object.find(key);
	if (found == object.end()) return &empty;
	if (!found->is_array()) return fail(at / key, "must be an array");
	return &*found;
}

std::expected<PortRef, GraphFileError> port(const json& route, const Pointer& at, const std::string& key) {
	auto value = required(route, at, key);
	if (!value) return std::unexpected(value.error());
	const Pointer here = at / key;
	if (auto checked = object(**value, here, {"node", "port"}); !checked) return std::unexpected(checked.error());
	auto node = name(**value, here, "node");
	if (!node) return std::unexpected(node.error());
	auto portName = name(**value, here, "port");
	if (!portName) return std::unexpected(portName.error());
	return PortRef{std::move(*node), std::move(*portName)};
}

std::expected<NodeSpec, GraphFileError> node(const json& value, const Pointer& at) {
	if (auto checked = object(value, at, {"name", "type", "settings"}); !checked)
		return std::unexpected(checked.error());
	auto nodeName = name(value, at, "name");
	if (!nodeName) return std::unexpected(nodeName.error());
	auto type = name(value, at, "type");
	if (!type) return std::unexpected(type.error());
	const auto settings = value.find("settings");
	return NodeSpec{std::move(*nodeName), std::move(*type),
	                settings == value.end() ? json::object() : *settings};
}

std::expected<Route, GraphFileError> route(const json& value, const Pointer& at) {
	if (auto checked = object(value, at, {"from", "to"}); !checked) return std::unexpected(checked.error());
	auto from = port(value, at, "from");
	if (!from) return std::unexpected(from.error());
	auto to = port(value, at, "to");
	if (!to) return std::unexpected(to.error());
	return Route{std::move(*from), std::move(*to)};
}

} // namespace

std::expected<GraphSpec, GraphFileError> parseGraphFile(std::string_view text) {
	json document;
	try {
		document = json::parse(text);
	} catch (const json::parse_error& error) {
		return std::unexpected(GraphFileError{std::format("not JSON: {}", error.what())});
	}
	const Pointer root;
	if (auto checked = object(document, root, {"graphFrequency", "blockSize", "nodes", "routes"}); !checked)
		return std::unexpected(checked.error());

	GraphSpec spec{};
	auto frequency = positive(document, root, "graphFrequency");
	if (!frequency) return std::unexpected(frequency.error());
	spec.graphFrequency = *frequency;
	auto block = positive(document, root, "blockSize");
	if (!block) return std::unexpected(block.error());
	spec.blockSize = *block;

	auto nodes = list(document, root, "nodes");
	if (!nodes) return std::unexpected(nodes.error());
	for (std::size_t index = 0; index < (*nodes)->size(); ++index) {
		auto parsed = node((**nodes)[index], root / "nodes" / index);
		if (!parsed) return std::unexpected(parsed.error());
		spec.nodes.push_back(std::move(*parsed));
	}
	auto routes = list(document, root, "routes");
	if (!routes) return std::unexpected(routes.error());
	for (std::size_t index = 0; index < (*routes)->size(); ++index) {
		auto parsed = route((**routes)[index], root / "routes" / index);
		if (!parsed) return std::unexpected(parsed.error());
		spec.routes.push_back(std::move(*parsed));
	}
	return spec;
}

std::expected<GraphSpec, GraphFileError> readGraphFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return std::unexpected(GraphFileError{std::format("{}: cannot read: {}", path.string(), std::strerror(errno))});
	const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	if (file.bad()) return std::unexpected(GraphFileError{std::format("{}: cannot read", path.string())});
	auto spec = parseGraphFile(text);
	if (!spec) return std::unexpected(GraphFileError{std::format("{}: {}", path.string(), spec.error().detail)});
	return spec;
}

std::expected<void, CommandError> replay(const GraphSpec& spec, GraphControl& control) {
	for (const NodeSpec& node : spec.nodes)
		if (auto added = control.addNode(node); !added) return added;
	for (const Route& route : spec.routes)
		if (auto added = control.addRoute(route); !added) return added;
	return {};
}

} // namespace ear::control
