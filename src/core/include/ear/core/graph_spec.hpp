// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace ear {
using NodeName = std::string;           // unique in the graph; also the engine node.name
using PortName = std::string;           // declared by the node type: "FL", "FR", "MONO"
using NodeSettings = nlohmann::json;    // unchecked; only NodeType::parse reads it
enum class Direction { input, output };
struct PortRef { NodeName node; PortName port;
  friend auto operator<=>(const PortRef&, const PortRef&) = default; };
/// One output port to one input port.
struct Route { PortRef from; PortRef to;
  friend auto operator<=>(const Route&, const Route&) = default; };
struct NodeSpec { NodeName name; std::string type; NodeSettings settings; // {} when absent
  friend bool operator==(const NodeSpec&, const NodeSpec&) = default; };
struct GraphSpec {
  std::uint32_t graphFrequency;  // Hz, > 0, read at boot (R5)
  std::uint32_t blockSize;       // samples per cycle, > 0, read at boot (R4)
  std::vector<NodeSpec> nodes;
  std::vector<Route> routes;
};
}
