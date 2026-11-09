#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace ear {
using NodeName = std::string; // unique in the graph; also the engine node.name
using PortName = std::string; // a public port is declared in a bank of its node type: "record.left" (node_type.hpp)
using NodeSettings = nlohmann::json; // unchecked; only NodeType::parse reads it
enum class Direction { input, output };
struct PortRef {
	NodeName node;
	PortName port;
	friend auto operator<=>(const PortRef&, const PortRef&) = default;
};
/// One output port to one input port.
struct Route {
	PortRef from;
	PortRef to;
	friend auto operator<=>(const Route&, const Route&) = default;
};
struct NodeSpec {
	NodeName name;
	std::string type;
	NodeSettings settings; // {} when absent
	/// The framework's reserved key, `disabled true;` in the node's block
	/// (docs/architecture.md#phases-states-and-what-is-wanted): beside the settings, never in them, so no type's parse,
	/// check or setting kinds sees it. False when absent.
	bool disabled = false;
	friend bool operator==(const NodeSpec&, const NodeSpec&) = default;
};
struct GraphSpec {
	std::uint32_t graphFrequency; // Hz, > 0, read at boot
	std::uint32_t blockSize;      // samples per cycle, > 0, read at boot
	std::vector<NodeSpec> nodes;
	std::vector<Route> routes;
};
} // namespace ear
