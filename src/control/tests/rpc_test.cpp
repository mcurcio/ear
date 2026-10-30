// The JSON-RPC 2.0 envelope (docs/design/batch2.md section 4.5, R1 criterion 1), against the
// specification's section 7 examples (https://www.jsonrpc.org/specification#examples). EAR has
// none of the examples' methods, so these check the envelope: what each text parses into and
// that the encoders give the example's response. Methods and notifications through a session
// are in session_test.cpp.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/rpc.hpp"

using namespace ear::control;
using nlohmann::json;

namespace {

RpcRequest onlyRequest(const RpcMessage& message) {
	REQUIRE_FALSE(message.batch);
	REQUIRE(message.entries.size() == 1);
	REQUIRE(std::holds_alternative<RpcRequest>(message.entries[0]));
	return std::get<RpcRequest>(message.entries[0]);
}

json onlyError(const RpcMessage& message) {
	REQUIRE_FALSE(message.batch);
	REQUIRE(message.entries.size() == 1);
	REQUIRE(std::holds_alternative<std::string>(message.entries[0]));
	return json::parse(std::get<std::string>(message.entries[0]));
}

} // namespace

TEST_CASE("Named params: a request parses and its result encodes as the spec's example", "[rpc]") {
	const RpcRequest request = onlyRequest(
		parseMessage(R"({"jsonrpc": "2.0", "method": "subtract", "params": {"subtrahend": 23, "minuend": 42}, "id": 3})"));
	CHECK(request.id == json(3));
	CHECK(request.method == "subtract");
	CHECK(request.params == json::parse(R"({"subtrahend": 23, "minuend": 42})"));
	CHECK(json::parse(encodeResult(*request.id, 19)) == json::parse(R"({"jsonrpc": "2.0", "result": 19, "id": 3})"));

	const RpcRequest swapped = onlyRequest(
		parseMessage(R"({"jsonrpc": "2.0", "method": "subtract", "params": {"minuend": 42, "subtrahend": 23}, "id": 4})"));
	CHECK(swapped.id == json(4));
	CHECK(json::parse(encodeResult(*swapped.id, 19)) == json::parse(R"({"jsonrpc": "2.0", "result": 19, "id": 4})"));
}

TEST_CASE("A request without id is a notification; string ids and absent params are kept", "[rpc]") {
	const RpcRequest update = onlyRequest(parseMessage(R"({"jsonrpc": "2.0", "method": "update", "params": [1,2,3,4,5]})"));
	CHECK_FALSE(update.id.has_value());
	CHECK(update.params == json::array({1, 2, 3, 4, 5}));
	const RpcRequest foobar = onlyRequest(parseMessage(R"({"jsonrpc": "2.0", "method": "foobar"})"));
	CHECK_FALSE(foobar.id.has_value());
	CHECK(foobar.params.is_null());
	const RpcRequest named = onlyRequest(parseMessage(R"({"jsonrpc": "2.0", "method": "foobar", "id": "1"})"));
	CHECK(named.id == json("1"));
}

TEST_CASE("Errors encode as the spec's examples", "[rpc]") {
	CHECK(json::parse(encodeError("1", {methodNotFound, "Method not found", nullptr})) ==
	      json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32601, "message": "Method not found"}, "id": "1"})"));
	CHECK(json::parse(encodeError(7, {commandRefused, "stopping", {{"code", "stopping"}}})) ==
	      json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32000, "message": "stopping", "data": {"code": "stopping"}}, "id": 7})"));
}

TEST_CASE("Text that is not JSON is one parse error with id null", "[rpc]") {
	const json parse = json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32700, "message": "Parse error"}, "id": null})");
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "2.0", "method": "foobar, "params": "bar", "baz])")) == parse);
	CHECK(onlyError(parseMessage(R"([
  {"jsonrpc": "2.0", "method": "sum", "params": [1,2,4], "id": "1"},
  {"jsonrpc": "2.0", "method"
])")) == parse);
}

TEST_CASE("An invalid request object is an invalid request; its id is kept when readable", "[rpc]") {
	const json invalidNull =
		json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null})");
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "2.0", "method": 1, "params": "bar"})")) == invalidNull);
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "1.0", "method": "graph.get"})")) == invalidNull);
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "2.0", "method": "graph.get", "id": [1]})")) == invalidNull);
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "2.0", "method": "graph.get", "params": null, "id": 5})"))["id"] == 5);
	CHECK(onlyError(parseMessage(R"({"jsonrpc": "2.0", "method": "graph.get", "params": 3, "id": 5})"))["id"] == 5);
}

TEST_CASE("An empty batch is one invalid request, not a batch", "[rpc]") {
	CHECK(onlyError(parseMessage("[]")) ==
	      json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null})"));
}

TEST_CASE("A batch of non-requests answers each with an invalid request", "[rpc]") {
	const RpcMessage one = parseMessage("[1]");
	CHECK(one.batch);
	REQUIRE(one.entries.size() == 1);
	const RpcMessage three = parseMessage("[1,2,3]");
	CHECK(three.batch);
	REQUIRE(three.entries.size() == 3);
	std::vector<std::string> responses;
	for (const auto& entry : three.entries) responses.push_back(std::get<std::string>(entry));
	CHECK(json::parse(encodeBatch(responses)) == json::parse(R"([
  {"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null},
  {"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null},
  {"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null}
])"));
}

TEST_CASE("A mixed batch keeps its entries in order", "[rpc]") {
	const RpcMessage message = parseMessage(R"([
  {"jsonrpc": "2.0", "method": "sum", "params": [1,2,4], "id": "1"},
  {"jsonrpc": "2.0", "method": "notify_hello", "params": [7]},
  {"jsonrpc": "2.0", "method": "subtract", "params": [42,23], "id": "2"},
  {"foo": "boo"},
  {"jsonrpc": "2.0", "method": "foo.get", "params": {"name": "myself"}, "id": "5"},
  {"jsonrpc": "2.0", "method": "get_data", "id": "9"}
])");
	CHECK(message.batch);
	REQUIRE(message.entries.size() == 6);
	CHECK(std::get<RpcRequest>(message.entries[0]).id == json("1"));
	CHECK_FALSE(std::get<RpcRequest>(message.entries[1]).id.has_value());
	CHECK(std::get<RpcRequest>(message.entries[2]).method == "subtract");
	CHECK(json::parse(std::get<std::string>(message.entries[3])) ==
	      json::parse(R"({"jsonrpc": "2.0", "error": {"code": -32600, "message": "Invalid Request"}, "id": null})"));
	CHECK(std::get<RpcRequest>(message.entries[4]).params == json::parse(R"({"name": "myself"})"));
	CHECK(std::get<RpcRequest>(message.entries[5]).method == "get_data");
}

TEST_CASE("A batch with no responses sends nothing", "[rpc]") {
	CHECK(encodeBatch({}).empty());
}

TEST_CASE("A notification encodes with method and params", "[rpc]") {
	CHECK(json::parse(encodeNotification("node.removed", {{"name", "noise"}})) ==
	      json::parse(R"({"jsonrpc": "2.0", "method": "node.removed", "params": {"name": "noise"}})"));
}
