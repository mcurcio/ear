// The JSON-RPC 2.0 envelope (docs/design/batch2.md section 4.5, rpc.hpp): the specification's
// four shapes (https://www.jsonrpc.org/specification sections 4, 5 and 6), nothing of EAR's
// methods. Error messages for -32700 and -32600 are the specification's own (section 5.1).
#include "ear/control/rpc.hpp"

#include <utility>

namespace ear::control {
namespace {

using nlohmann::json;

/// Every text sent is valid UTF-8 even if a detail carried bytes that are not.
std::string dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string invalid(const RpcId& id) { return encodeError(id, {invalidRequest, "Invalid Request", nullptr}); }

/// A request (section 4), or the encoded error response for an entry that is not one. The id is
/// echoed when it is readable (a string, a number or null), else null.
std::variant<RpcRequest, std::string> parseEntry(const json& entry) {
	if (!entry.is_object()) return invalid(nullptr);
	std::optional<RpcId> id;
	if (const auto found = entry.find("id"); found != entry.end()) {
		if (!found->is_string() && !found->is_number() && !found->is_null()) return invalid(nullptr);
		id = *found;
	}
	const RpcId replyTo = id.value_or(nullptr);
	for (const auto& [key, member] : entry.items())
		if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") return invalid(replyTo);
	const auto version = entry.find("jsonrpc");
	if (version == entry.end() || *version != "2.0") return invalid(replyTo);
	const auto method = entry.find("method");
	if (method == entry.end() || !method->is_string()) return invalid(replyTo);
	json params;   // null: absent
	if (const auto found = entry.find("params"); found != entry.end()) {
		if (!found->is_object() && !found->is_array()) return invalid(replyTo);
		params = *found;
	}
	return RpcRequest{std::move(id), method->get<std::string>(), std::move(params)};
}

} // namespace

RpcMessage parseMessage(std::string_view text) {
	json document;
	try {
		document = json::parse(text);
	} catch (const json::parse_error&) {
		return {false, {encodeError(nullptr, {parseError, "Parse error", nullptr})}};
	}
	if (!document.is_array()) return {false, {parseEntry(document)}};
	// An empty batch is one invalid request, answered alone (specification section 6).
	if (document.empty()) return {false, {invalid(nullptr)}};
	RpcMessage message{true, {}};
	message.entries.reserve(document.size());
	for (const json& entry : document) message.entries.push_back(parseEntry(entry));
	return message;
}

std::string encodeResult(const RpcId& id, const json& result) {
	return dump({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

std::string encodeError(const RpcId& id, const RpcError& error) {
	json body = {{"code", error.code}, {"message", error.message}};
	if (!error.data.is_null()) body["data"] = error.data;
	return dump({{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(body)}});
}

std::string encodeNotification(std::string_view method, const json& params) {
	return dump({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

std::string encodeBatch(std::span<const std::string> responses) {
	if (responses.empty()) return {};
	std::string batch = "[";
	for (const std::string& response : responses) {
		if (batch.size() > 1) batch += ',';
		batch += response;
	}
	batch += ']';
	return batch;
}

} // namespace ear::control
