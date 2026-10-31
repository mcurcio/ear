// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "ear/core/graph_spec.hpp"
// The one home of node and route JSON: the control port's commands decode with it, and its
// results and events encode with it.
namespace ear::control {
using JsonPointer = nlohmann::json::json_pointer;
struct JsonError { std::string detail; };   // "<JSON pointer>: <problem>"; "the document" at root
/// An object whose keys are all in `keys`.
std::expected<void, JsonError> requireObject(const nlohmann::json& value, const JsonPointer& at,
                                             std::initializer_list<std::string_view> keys);
std::expected<const nlohmann::json*, JsonError> requireMember(const nlohmann::json& object,
    const JsonPointer& at, const std::string& key);   // "<at>/<key>: missing"
std::expected<std::string, JsonError> requireName(const nlohmann::json& object,
    const JsonPointer& at, const std::string& key);   // a non-empty string
/// {"name", "type", "settings"?}; settings defaults to {}.
std::expected<NodeSpec, JsonError> decodeNodeSpec(const nlohmann::json& value, const JsonPointer& at);
/// {"from": {"node", "port"}, "to": {"node", "port"}}.
std::expected<Route, JsonError> decodeRoute(const nlohmann::json& value, const JsonPointer& at);
nlohmann::json encodePortRef(const PortRef& port);   // {"node", "port"}
nlohmann::json encodeRoute(const Route& route);      // {"from", "to"}
}
