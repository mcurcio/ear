#pragma once
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/graph_spec.hpp"
namespace ear {
struct UnknownNodeType {
	std::string type;
};
/// Settings a node's type refuses. `detail` is the refusal's whole text (describe's); `key` the full key it names when
/// the settings walk refused one, empty for a rule naming none (the type's check, settings that are not an object):
/// data.key on the wire (docs/control-port.md#error-codes).
struct InvalidSettings {
	NodeName node;
	std::string detail;
	std::string key = {};
};
/// The refusal in its own words: `detail` without the "<key>: " it starts with when it names a key (data.detail on
/// the wire, docs/control-port.md#error-codes), else the whole detail.
std::string_view refusalWords(const InvalidSettings& error) noexcept;
struct DuplicateNode {
	NodeName node;
}; // name in use with different settings
struct UnknownNode {
	NodeName node;
}; // a route names a node not in the graph
struct UnknownPort {
	PortRef port;
}; // node exists, port not declared
struct WrongDirection {
	Route route;
}; // from is not an output, or to is not an input
struct InvalidName {
	NodeName node;
}; // not 1-64 characters of [A-Za-z0-9_-]
struct UnknownRoute {
	Route route;
}; // removeRoute: the route is not wanted
struct Stopping {}; // EAR is stopping: the graph takes no more edits
using CommandError = std::variant<UnknownNodeType, InvalidSettings, DuplicateNode, UnknownNode, UnknownPort,
                                  WrongDirection, InvalidName, UnknownRoute, Stopping>;
/// "unknown-node-type" | "invalid-settings" | "duplicate-node" | "unknown-node" |
/// "unknown-port" | "wrong-direction" | "invalid-name" | "unknown-route" | "stopping". The one
/// home of these strings.
std::string_view errorCode(const CommandError& error) noexcept;
/// Code plus what it names, e.g. "unknown-port rec:FX", "invalid-name <n>",
/// "unknown-route <a>:<p> -> <b>:<q>", "stopping".
std::string describe(const CommandError& error);
} // namespace ear
