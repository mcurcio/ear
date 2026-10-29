// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/graph_spec.hpp"
namespace ear {
struct UnknownNodeType { std::string type; };
struct InvalidSettings { NodeName node; std::string detail; };
struct DuplicateNode   { NodeName node; };   // name in use with different settings
struct UnknownNode     { NodeName node; };   // a route names a node not in the graph
struct UnknownPort     { PortRef port; };    // node exists, port not declared
struct WrongDirection  { Route route; };     // from is not an output, or to is not an input
using CommandError = std::variant<UnknownNodeType, InvalidSettings, DuplicateNode,
                                  UnknownNode, UnknownPort, WrongDirection>;
/// "unknown-node-type" | "invalid-settings" | "duplicate-node" | "unknown-node" |
/// "unknown-port" | "wrong-direction". The one home of these strings.
std::string_view errorCode(const CommandError& error) noexcept;
/// Code plus what it names, e.g. "unknown-port rec:FX".
std::string describe(const CommandError& error);
}
