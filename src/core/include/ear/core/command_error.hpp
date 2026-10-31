// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
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
struct InvalidName     { NodeName node; };   // not 1-64 characters of [A-Za-z0-9_-]
/// Made by nothing: a port change replaces the node (docs/design/knobly-config.md). Kept only until
/// src/config/tests/main_loop_side_test.cpp drops its row (S5 erratum; S5 may not edit src/config).
struct PortsChanged    { NodeName node; };
struct UnknownRoute    { Route route; };     // removeRoute: the route is not wanted
struct Stopping        {};                   // EAR is stopping: the graph takes no more edits
using CommandError = std::variant<UnknownNodeType, InvalidSettings, DuplicateNode, UnknownNode,
                                  UnknownPort, WrongDirection, InvalidName, PortsChanged,
                                  UnknownRoute, Stopping>;
/// "unknown-node-type" | "invalid-settings" | "duplicate-node" | "unknown-node" |
/// "unknown-port" | "wrong-direction" | "invalid-name" | "ports-changed" | "unknown-route" |
/// "stopping". The one home of these strings.
std::string_view errorCode(const CommandError& error) noexcept;
/// Code plus what it names, e.g. "unknown-port rec:FX", "invalid-name <n>",
/// "ports-changed <n>", "unknown-route <a>:<p> -> <b>:<q>", "stopping".
std::string describe(const CommandError& error);
}
