// graph_json: the one decoder and encoder of node and route JSON (docs/design/batch2.md section
// 4.5, R1 criterion 2): every missing, extra and mistyped key is named by its JSON pointer.
#include <string>

#include "catch2/catch.hpp"
#include "ear/control/graph_json.hpp"

using ear::control::JsonPointer;
using nlohmann::json;

namespace {

std::string nodeError(const std::string& text, const JsonPointer& at = JsonPointer{}) {
	const auto decoded = ear::control::decodeNodeSpec(json::parse(text), at);
	REQUIRE_FALSE(decoded.has_value());
	return decoded.error().detail;
}

std::string routeError(const std::string& text, const JsonPointer& at = JsonPointer{}) {
	const auto decoded = ear::control::decodeRoute(json::parse(text), at);
	REQUIRE_FALSE(decoded.has_value());
	return decoded.error().detail;
}

} // namespace

TEST_CASE("requireObject accepts only an object whose keys are all listed", "[graph-json]") {
	const JsonPointer at("/x");
	CHECK(ear::control::requireObject(json::parse(R"({"a": 1})"), at, {"a", "b"}).has_value());
	CHECK(ear::control::requireObject(json::parse("[]"), at, {"a"}).error().detail == "/x: must be an object");
	CHECK(ear::control::requireObject(json::parse("[]"), JsonPointer{}, {}).error().detail ==
	      "the document: must be an object");
	CHECK(ear::control::requireObject(json::parse(R"({"a": 1, "c": 2})"), at, {"a"}).error().detail ==
	      "/x/c: unknown key");
}

TEST_CASE("requireMember and requireName name the member's pointer", "[graph-json]") {
	const JsonPointer at("/y");
	const json object = json::parse(R"({"n": "noise", "e": "", "i": 3})");
	CHECK(*ear::control::requireMember(object, at, "i").value() == 3);
	CHECK(ear::control::requireMember(object, at, "m").error().detail == "/y/m: missing");
	CHECK(ear::control::requireName(object, at, "n").value() == "noise");
	CHECK(ear::control::requireName(object, at, "m").error().detail == "/y/m: missing");
	CHECK(ear::control::requireName(object, at, "e").error().detail == "/y/e: must be a non-empty string");
	CHECK(ear::control::requireName(object, at, "i").error().detail == "/y/i: must be a non-empty string");
}

TEST_CASE("A node decodes with its settings, which default to {}", "[graph-json]") {
	const auto plain = ear::control::decodeNodeSpec(json::parse(R"({"name": "noise", "type": "white"})"), JsonPointer{});
	REQUIRE(plain.has_value());
	CHECK(*plain == ear::NodeSpec{"noise", "white", json::object()});
	const auto set = ear::control::decodeNodeSpec(
		json::parse(R"({"name": "rec", "type": "file", "settings": {"path": "/x.wav"}})"), JsonPointer{});
	REQUIRE(set.has_value());
	CHECK(*set == ear::NodeSpec{"rec", "file", {{"path", "/x.wav"}}});
}

TEST_CASE("Every missing, extra and mistyped node key is named by its pointer", "[graph-json]") {
	CHECK(nodeError(R"({"type": "white"})") == "/name: missing");
	CHECK(nodeError(R"({"name": "a"})") == "/type: missing");
	CHECK(nodeError(R"({"name": "a", "type": "white", "ports": []})") == "/ports: unknown key");
	CHECK(nodeError(R"({"name": 1, "type": "white"})") == "/name: must be a non-empty string");
	CHECK(nodeError(R"({"name": "a", "type": ""})") == "/type: must be a non-empty string");
	CHECK(nodeError(R"("a")") == "the document: must be an object");
	CHECK(nodeError(R"({"type": "white"})", JsonPointer("/nodes/2")) == "/nodes/2/name: missing");
}

TEST_CASE("A route decodes from and to", "[graph-json]") {
	const auto route = ear::control::decodeRoute(
		json::parse(R"({"from": {"node": "a", "port": "MONO"}, "to": {"node": "b", "port": "FL"}})"), JsonPointer{});
	REQUIRE(route.has_value());
	CHECK(*route == ear::Route{{"a", "MONO"}, {"b", "FL"}});
}

TEST_CASE("Every missing, extra and mistyped route key is named by its pointer", "[graph-json]") {
	CHECK(routeError(R"({"to": {"node": "b", "port": "FL"}})") == "/from: missing");
	CHECK(routeError(R"({"from": {"node": "a", "port": "MONO"}})") == "/to: missing");
	CHECK(routeError(R"({"from": {"node": "a"}, "to": {"node": "b", "port": "FL"}})") == "/from/port: missing");
	CHECK(routeError(R"({"from": {"port": "MONO"}, "to": {"node": "b", "port": "FL"}})") == "/from/node: missing");
	CHECK(routeError(R"({"from": {"node": "a", "port": "MONO"}, "to": {"node": "b", "port": 2}})") ==
	      "/to/port: must be a non-empty string");
	CHECK(routeError(R"({"from": "a:MONO", "to": {"node": "b", "port": "FL"}})") == "/from: must be an object");
	CHECK(routeError(R"({"from": {"node": "a", "port": "MONO"}, "to": {"node": "b", "port": "FL"}, "gain": 1})") ==
	      "/gain: unknown key");
	CHECK(routeError(R"({"from": {"node": "a", "port": "MONO", "ch": 0}, "to": {"node": "b", "port": "FL"}})") ==
	      "/from/ch: unknown key");
}

TEST_CASE("Ports and routes encode as the wire format names them", "[graph-json]") {
	CHECK(ear::control::encodePortRef({"a", "MONO"}) == json::parse(R"({"node": "a", "port": "MONO"})"));
	CHECK(ear::control::encodeRoute({{"a", "MONO"}, {"b", "FL"}}) ==
	      json::parse(R"({"from": {"node": "a", "port": "MONO"}, "to": {"node": "b", "port": "FL"}})"));
}
