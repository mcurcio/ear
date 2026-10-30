// The graph file (ear.json v2): what parseGraphFile accepts, the JSON pointer each error names,
// readGraphFile's path in its errors, and replay's call order (docs/design/m1-white-to-file.md
// section 3, S6 criterion 5).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/control/graph_file.hpp"

using ear::control::parseGraphFile;

namespace {

/// The error parseGraphFile gives for `text`; fails the test if it parses.
std::string errorOf(const std::string& text) {
	const auto parsed = parseGraphFile(text);
	REQUIRE_FALSE(parsed.has_value());
	return parsed.error().detail;
}

/// A control surface that records each call and refuses the ones named.
class RecordingControl final : public ear::GraphControl {
public:
	std::vector<std::string> calls;
	std::string refuseNode;
	std::string refuseRouteTo;

	std::expected<void, ear::CommandError> addNode(const ear::NodeSpec& node) override {
		calls.push_back("node " + node.name);
		if (node.name == refuseNode) return std::unexpected(ear::DuplicateNode{node.name});
		return {};
	}
	std::expected<void, ear::CommandError> addRoute(const ear::Route& route) override {
		calls.push_back("route " + route.to.node + ":" + route.to.port);
		if (route.to.port == refuseRouteTo) return std::unexpected(ear::UnknownPort{route.to});
		return {};
	}
	// replay only adds; the other edits are recorded so a stray call fails the order checks.
	std::expected<void, ear::CommandError> updateNode(const ear::NodeName& node,
	                                                  const ear::NodeSettings&) override {
		calls.push_back("update " + node);
		return {};
	}
	std::expected<void, ear::CommandError> removeNode(const ear::NodeName& node) override {
		calls.push_back("remove node " + node);
		return {};
	}
	std::expected<void, ear::CommandError> removeRoute(const ear::Route& route) override {
		calls.push_back("remove route " + route.to.node + ":" + route.to.port);
		return {};
	}
};

const std::string twoNodes = R"({
	"graphFrequency": 48000, "blockSize": 256,
	"nodes": [{"name": "noise", "type": "white"},
	          {"name": "rec", "type": "file", "settings": {"path": "x.wav"}}],
	"routes": [{"from": {"node": "noise", "port": "MONO"}, "to": {"node": "rec", "port": "FL"}},
	           {"from": {"node": "noise", "port": "MONO"}, "to": {"node": "rec", "port": "FR"}}]
})";

} // namespace

TEST_CASE("A graph file parses into its frequency, block size, nodes and routes", "[graph-file]") {
	const auto spec = parseGraphFile(twoNodes);
	REQUIRE(spec.has_value());
	CHECK(spec->graphFrequency == 48000);
	CHECK(spec->blockSize == 256);
	REQUIRE(spec->nodes.size() == 2);
	CHECK(spec->nodes[0] == ear::NodeSpec{"noise", "white", nlohmann::json::object()});
	CHECK(spec->nodes[1] == ear::NodeSpec{"rec", "file", {{"path", "x.wav"}}});
	REQUIRE(spec->routes.size() == 2);
	CHECK(spec->routes[0] == ear::Route{{"noise", "MONO"}, {"rec", "FL"}});
	CHECK(spec->routes[1] == ear::Route{{"noise", "MONO"}, {"rec", "FR"}});
}

TEST_CASE("A graph file without nodes or routes is an empty graph", "[graph-file]") {
	const auto spec = parseGraphFile(R"({"graphFrequency": 44100, "blockSize": 512})");
	REQUIRE(spec.has_value());
	CHECK(spec->nodes.empty());
	CHECK(spec->routes.empty());
}

TEST_CASE("The graph frequency and block size are required, with no default", "[graph-file]") {
	CHECK(errorOf(R"({"blockSize": 256})") == "/graphFrequency: missing");
	CHECK(errorOf(R"({"graphFrequency": 48000})") == "/blockSize: missing");
}

TEST_CASE("The graph frequency and block size are whole numbers above zero", "[graph-file]") {
	for (const std::string bad : {"0", "-1", "48000.5", "\"48000\"", "4294967296", "null"}) {
		INFO(bad);
		CHECK(errorOf(R"({"blockSize": 256, "graphFrequency": )" + bad + "}") ==
		      "/graphFrequency: must be a whole number from 1 to 4294967295");
		CHECK(errorOf(R"({"graphFrequency": 48000, "blockSize": )" + bad + "}") ==
		      "/blockSize: must be a whole number from 1 to 4294967295");
	}
	const auto largest = parseGraphFile(R"({"graphFrequency": 4294967295, "blockSize": 1})");
	REQUIRE(largest.has_value());
	CHECK(largest->graphFrequency == 4294967295U);
}

TEST_CASE("Unknown keys are errors at every level", "[graph-file]") {
	CHECK(errorOf(R"({"graphFrequency": 48000, "blockSize": 256, "device": "0"})") == "/device: unknown key");
	CHECK(errorOf(R"({"graphFrequency": 48000, "blockSize": 256,
		"nodes": [{"name": "a", "type": "white", "connect": []}]})") == "/nodes/0/connect: unknown key");
	CHECK(errorOf(R"({"graphFrequency": 48000, "blockSize": 256,
		"routes": [{"from": {"node": "a", "port": "MONO"}, "to": {"node": "b", "port": "FL"}, "gain": 1}]})") ==
	      "/routes/0/gain: unknown key");
	CHECK(errorOf(R"({"graphFrequency": 48000, "blockSize": 256,
		"routes": [{"from": {"node": "a", "port": "MONO", "channel": 0}, "to": {"node": "b", "port": "FL"}}]})") ==
	      "/routes/0/from/channel: unknown key");
}

TEST_CASE("Each missing or malformed member is named by its JSON pointer", "[graph-file]") {
	const std::string head = R"({"graphFrequency": 48000, "blockSize": 256, )";
	CHECK(errorOf(head + R"("routes": [{"from": {"node": "a", "port": "MONO"}, "to": {"node": "b"}}]})") ==
	      "/routes/0/to/port: missing");
	CHECK(errorOf(head + R"("routes": [{"from": {"node": "a", "port": "MONO"}}]})") == "/routes/0/to: missing");
	CHECK(errorOf(head + R"("routes": [{"from": "a:MONO", "to": {"node": "b", "port": "FL"}}]})") ==
	      "/routes/0/from: must be an object");
	CHECK(errorOf(head + R"("nodes": [{"name": "a", "type": "white"}, {"type": "white"}]})") ==
	      "/nodes/1/name: missing");
	CHECK(errorOf(head + R"("nodes": [{"name": "", "type": "white"}]})") == "/nodes/0/name: must be a non-empty string");
	CHECK(errorOf(head + R"("nodes": [{"name": "a", "type": 3}]})") == "/nodes/0/type: must be a non-empty string");
	CHECK(errorOf(head + R"("nodes": {}})") == "/nodes: must be an array");
	CHECK(errorOf("[]") == "the document: must be an object");
}

TEST_CASE("Node settings pass through unchecked", "[graph-file]") {
	const auto spec = parseGraphFile(R"({"graphFrequency": 48000, "blockSize": 256,
		"nodes": [{"name": "a", "type": "anything", "settings": [1, "two"]}]})");
	REQUIRE(spec.has_value());
	CHECK(spec->nodes[0].settings == nlohmann::json::array({1, "two"}));
}

TEST_CASE("Text that is not JSON is an error", "[graph-file]") {
	CHECK_THAT(errorOf("{\"graphFrequency\": "), Catch::StartsWith("not JSON: "));
}

TEST_CASE("readGraphFile names the file in its errors", "[graph-file]") {
	std::string pattern = (std::filesystem::temp_directory_path() / "ear-graph-file-XXXXXX").string();
	REQUIRE(::mkdtemp(pattern.data()) != nullptr);
	const std::filesystem::path directory = pattern;
	const std::filesystem::path path = directory / "ear.json";

	const auto missing = ear::control::readGraphFile(path);
	REQUIRE_FALSE(missing.has_value());
	CHECK_THAT(missing.error().detail, Catch::StartsWith(path.string() + ": cannot read: "));

	std::ofstream(path) << R"({"graphFrequency": 48000})";
	const auto incomplete = ear::control::readGraphFile(path);
	REQUIRE_FALSE(incomplete.has_value());
	CHECK(incomplete.error().detail == path.string() + ": /blockSize: missing");

	std::ofstream(path) << twoNodes;
	const auto read = ear::control::readGraphFile(path);
	REQUIRE(read.has_value());
	CHECK(read->nodes.size() == 2);
	std::filesystem::remove_all(directory);
}

TEST_CASE("replay adds the nodes in file order, then the routes in file order", "[graph-file][replay]") {
	const auto spec = parseGraphFile(twoNodes);
	REQUIRE(spec.has_value());
	RecordingControl control;
	CHECK(ear::control::replay(*spec, control).has_value());
	CHECK(control.calls == std::vector<std::string>{"node noise", "node rec", "route rec:FL", "route rec:FR"});
}

TEST_CASE("replay stops at the first error and returns it", "[graph-file][replay]") {
	const auto spec = parseGraphFile(twoNodes);
	REQUIRE(spec.has_value());

	RecordingControl refusesRoute;
	refusesRoute.refuseRouteTo = "FL";
	const auto routeError = ear::control::replay(*spec, refusesRoute);
	REQUIRE_FALSE(routeError.has_value());
	CHECK(ear::describe(routeError.error()) == "unknown-port rec:FL");
	CHECK(refusesRoute.calls == std::vector<std::string>{"node noise", "node rec", "route rec:FL"});

	RecordingControl refusesNode;
	refusesNode.refuseNode = "noise";
	const auto nodeError = ear::control::replay(*spec, refusesNode);
	REQUIRE_FALSE(nodeError.has_value());
	CHECK(ear::describe(nodeError.error()) == "duplicate-node noise");
	CHECK(refusesNode.calls == std::vector<std::string>{"node noise"});
}
