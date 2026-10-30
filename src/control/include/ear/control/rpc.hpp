// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
// The JSON-RPC 2.0 envelope (https://www.jsonrpc.org/specification): the spec's four shapes.
namespace ear::control {
using RpcId = nlohmann::json;              // number or string; null when unreadable
struct RpcError { int code; std::string message; nlohmann::json data; };   // data null: absent
inline constexpr int parseError = -32700, invalidRequest = -32600, methodNotFound = -32601,
                     invalidParams = -32602, commandRefused = -32000;
struct RpcRequest { std::optional<RpcId> id; std::string method; nlohmann::json params; };
/// One received text: a request or a batch. Each entry is a request, or the encoded error
/// response for an entry that is not one. A request without id is a notification: no response.
struct RpcMessage { bool batch; std::vector<std::variant<RpcRequest, std::string>> entries; };
RpcMessage parseMessage(std::string_view text);
std::string encodeResult(const RpcId& id, const nlohmann::json& result);
std::string encodeError(const RpcId& id, const RpcError& error);
std::string encodeNotification(std::string_view method, const nlohmann::json& params);
/// A batch's responses as one array; no responses: "" (nothing is sent).
std::string encodeBatch(std::span<const std::string> responses);
}
