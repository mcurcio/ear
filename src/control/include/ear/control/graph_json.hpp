#pragma once
#include <chrono>
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
struct JsonError {
	std::string detail;  // "<JSON pointer>: <problem>"; "the document" at root
	std::string pointer; // the JSON pointer alone ("" at root): -32602's data.pointer
};
/// An object whose keys are all in `keys`.
std::expected<void, JsonError> requireObject(const nlohmann::json& value, const JsonPointer& at,
                                             std::initializer_list<std::string_view> keys);
std::expected<const nlohmann::json*, JsonError> requireMember(const nlohmann::json& object, const JsonPointer& at,
                                                              const std::string& key); // "<at>/<key>: missing"
std::expected<std::string, JsonError> requireName(const nlohmann::json& object, const JsonPointer& at,
                                                  const std::string& key); // a non-empty string
/// {"name", "type", "settings"?, "disabled"?}; settings defaults to {}, disabled (a boolean) to false.
std::expected<NodeSpec, JsonError> decodeNodeSpec(const nlohmann::json& value, const JsonPointer& at);
/// {"from": {"node", "port"}, "to": {"node", "port"}}.
std::expected<Route, JsonError> decodeRoute(const nlohmann::json& value, const JsonPointer& at);
nlohmann::json encodePortRef(const PortRef& port); // {"node", "port"}
nlohmann::json encodeRoute(const Route& route);    // {"from", "to"}
/// An instant as the wire's `since` and the records' `t` write it: UTC to the microsecond,
/// "2026-11-09T10:05:12.770000Z". The one home of that text.
std::string utcMicros(std::chrono::system_clock::time_point at);
} // namespace ear::control
