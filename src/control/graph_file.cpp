// The graph file (ear.json v2): read, checked and replayed as control calls at boot
// (docs/design/m1-white-to-file.md section 2, graph_file.hpp). Every error names the JSON
// pointer of what is wrong; readGraphFile adds the file's path. Nodes and routes decode through
// graph_json (docs/design/batch2.md section 4.5), shared with the control port's commands.
#include "ear/control/graph_file.hpp"
#include "ear/control/graph_json.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace ear::control {
namespace {

using nlohmann::json;

/// Node, route and member checks come from graph_json, the one home of their texts.
std::unexpected<GraphFileError> fail(const JsonError& error) {
	return std::unexpected(GraphFileError{error.detail});
}

std::unexpected<GraphFileError> fail(const JsonPointer& at, std::string_view problem) {
	return std::unexpected(GraphFileError{std::format("{}: {}", at.to_string(), problem)});
}

std::expected<std::uint32_t, GraphFileError> positive(const json& object, const JsonPointer& at,
                                                      const std::string& key) {
	auto value = requireMember(object, at, key);
	if (!value) return fail(value.error());
	const json& number = **value;
	if (!number.is_number_unsigned() || number.get<std::uint64_t>() == 0 ||
	    number.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
		return fail(at / key, "must be a whole number from 1 to 4294967295");
	return static_cast<std::uint32_t>(number.get<std::uint64_t>());
}

/// An optional array member; absent reads as empty.
std::expected<const json*, GraphFileError> list(const json& object, const JsonPointer& at,
                                                const std::string& key) {
	static const json empty = json::array();
	const auto found = object.find(key);
	if (found == object.end()) return &empty;
	if (!found->is_array()) return fail(at / key, "must be an array");
	return &*found;
}

} // namespace

std::expected<GraphSpec, GraphFileError> parseGraphFile(std::string_view text) {
	json document;
	try {
		document = json::parse(text);
	} catch (const json::parse_error& error) {
		return std::unexpected(GraphFileError{std::format("not JSON: {}", error.what())});
	}
	const JsonPointer root;
	if (auto checked = requireObject(document, root, {"graphFrequency", "blockSize", "nodes", "routes"}); !checked)
		return fail(checked.error());

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
		auto parsed = decodeNodeSpec((**nodes)[index], root / "nodes" / index);
		if (!parsed) return fail(parsed.error());
		spec.nodes.push_back(std::move(*parsed));
	}
	auto routes = list(document, root, "routes");
	if (!routes) return std::unexpected(routes.error());
	for (std::size_t index = 0; index < (*routes)->size(); ++index) {
		auto parsed = decodeRoute((**routes)[index], root / "routes" / index);
		if (!parsed) return fail(parsed.error());
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
