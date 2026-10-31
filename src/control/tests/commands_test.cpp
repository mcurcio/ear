// The control port's commands (docs/design/batch2.md sections 4.5 and 6): decodeCommand for every
// method and every missing, extra and mistyped key (R1 criterion 2); runCommand calls the one
// control call each command names; encodeCommandResult and encodeGraph give exactly the
// fixtures' texts (R1 criterion 3).
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/commands.hpp"
#include "ear/control/graph_file.hpp"

using namespace ear::control;
using nlohmann::json;

namespace {

json fixture(const std::string& name) {
	std::ifstream file(std::string(EAR_PROTOCOL_DIR) + "/" + name + ".json");
	REQUIRE(file);
	return json::parse(file);
}

RpcError refusal(std::string_view method, const std::string& params) {
	const auto decoded = decodeCommand(method, json::parse(params));
	REQUIRE_FALSE(decoded.has_value());
	return decoded.error();
}

std::string invalid(std::string_view method, const std::string& params) {
	const RpcError error = refusal(method, params);
	CHECK(error.code == invalidParams);
	CHECK(error.data.is_null());
	return error.message;
}

const ear::Route noiseToRecFl{{"noise", "MONO"}, {"rec", "FL"}};
const std::string noiseToRecFlJson =
	R"({"from": {"node": "noise", "port": "MONO"}, "to": {"node": "rec", "port": "FL"}})";

/// Records each control call by name; answers with `answer`.
class RecordingControl final : public ear::GraphControl {
public:
	std::vector<std::string> calls;
	std::expected<void, ear::CommandError> answer;
	std::expected<void, ear::CommandError> addNode(const ear::NodeSpec& node) override {
		calls.push_back("addNode " + node.name);
		return answer;
	}
	std::expected<void, ear::CommandError> updateNode(const ear::NodeName& node, const ear::NodeSettings& s) override {
		calls.push_back("updateNode " + node + " " + s.dump());
		return answer;
	}
	std::expected<void, ear::CommandError> removeNode(const ear::NodeName& node) override {
		calls.push_back("removeNode " + node);
		return answer;
	}
	std::expected<void, ear::CommandError> addRoute(const ear::Route& route) override {
		calls.push_back("addRoute " + route.to.port);
		return answer;
	}
	std::expected<void, ear::CommandError> removeRoute(const ear::Route& route) override {
		calls.push_back("removeRoute " + route.to.port);
		return answer;
	}
};

class FixedReader final : public ear::GraphReader {
public:
	ear::GraphView view;
	mutable int reads = 0;
	ear::GraphView read() const override {
		++reads;
		return view;
	}
};

/// snapshot.json's graph, as GraphStatus would read it.
ear::GraphView snapshotView() {
	using ear::NodeState;
	using Code = ear::FailureReason::Code;
	ear::GraphView view{{{48000, 256}, true, "SCHED_FIFO 88"}, {"card", "file", "white"}, {}, {}};
	view.nodes.push_back({{"motu", "card", {{"card", "Pro24"}, {"playback", 2}}},
	                      {{"playback_1", "playback_2"}, {}},
	                      NodeState::failed,
	                      ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=Pro24,DEV=0: No such device"}});
	view.nodes.push_back({{"noise", "white", json::object()}, {{}, {"MONO"}}, NodeState::running, std::nullopt});
	view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}}, {{"FL", "FR"}, {}}, NodeState::running, std::nullopt});
	view.nodes.push_back({{"tape", "file", {{"path", "nope/t.wav"}}},
	                      {{"FL", "FR"}, {}},
	                      NodeState::failed,
	                      ear::FailureReason{Code::startFailed, "cannot open nope/t.wav: No such file or directory"}});
	view.routes.push_back({{{"noise", "MONO"}, {"motu", "playback_1"}}, ear::RouteMatch::pending, ""});
	view.routes.push_back({noiseToRecFl, ear::RouteMatch::linked, ""});
	view.routes.push_back({{{"noise", "MONO"}, {"tape", "FL"}},
	                       ear::RouteMatch::refused,
	                       "the engine removed the link before it was active"});
	return view;
}

} // namespace

TEST_CASE("Every method decodes into its command", "[commands]") {
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.get", nullptr).value()));
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.get", json::object()).value()));
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.subscribe", nullptr).value()));

	const auto add = decodeCommand("node.add", json::parse(R"({"name": "rec", "type": "file", "settings": {"path": "x.wav"}})"));
	CHECK(std::get<AddNode>(add.value()).node == ear::NodeSpec{"rec", "file", {{"path", "x.wav"}}});
	const auto addPlain = decodeCommand("node.add", json::parse(R"({"name": "noise", "type": "white"})"));
	CHECK(std::get<AddNode>(addPlain.value()).node.settings == json::object());

	const auto update = decodeCommand("node.update", json::parse(R"({"name": "rec", "settings": {"path": "y.wav"}})"));
	CHECK(std::get<UpdateNode>(update.value()).node == "rec");
	CHECK(std::get<UpdateNode>(update.value()).settings == json{{"path", "y.wav"}});

	CHECK(std::get<RemoveNode>(decodeCommand("node.remove", json::parse(R"({"name": "rec"})")).value()).node == "rec");
	CHECK(std::get<AddRoute>(decodeCommand("route.add", json::parse(noiseToRecFlJson)).value()).route == noiseToRecFl);
	CHECK(std::get<RemoveRoute>(decodeCommand("route.remove", json::parse(noiseToRecFlJson)).value()).route ==
	      noiseToRecFl);
}

TEST_CASE("Every missing, extra and mistyped key is invalid params naming its pointer", "[commands]") {
	CHECK(invalid("graph.get", R"({"x": 1})") == "/x: unknown key");
	CHECK(invalid("graph.subscribe", R"({"x": 1})") == "/x: unknown key");

	CHECK(invalid("node.add", R"({"type": "white"})") == "/name: missing");
	CHECK(invalid("node.add", R"({"name": "a"})") == "/type: missing");
	CHECK(invalid("node.add", R"({"name": "a", "type": "white", "x": 1})") == "/x: unknown key");
	CHECK(invalid("node.add", R"({"name": 2, "type": "white"})") == "/name: must be a non-empty string");
	CHECK(invalid("node.add", R"({"name": "a", "type": ["white"]})") == "/type: must be a non-empty string");
	CHECK(invalid("node.add", "null") == "/name: missing");   // null: params absent
	CHECK(invalid("node.add", R"("noise")") == "the document: must be an object");

	CHECK(invalid("node.update", R"({"settings": {}})") == "/name: missing");
	CHECK(invalid("node.update", R"({"name": "a"})") == "/settings: missing");
	CHECK(invalid("node.update", R"({"name": "a", "settings": {}, "type": "white"})") == "/type: unknown key");
	CHECK(invalid("node.update", R"({"name": "", "settings": {}})") == "/name: must be a non-empty string");

	CHECK(invalid("node.remove", "{}") == "/name: missing");
	CHECK(invalid("node.remove", R"({"name": "a", "force": true})") == "/force: unknown key");
	CHECK(invalid("node.remove", R"({"name": null})") == "/name: must be a non-empty string");

	for (const std::string_view method : {"route.add", "route.remove"}) {
		INFO(method);
		CHECK(invalid(method, R"({"from": {"node": "noise"}, "to": {"node": "rec", "port": "FR"}})") ==
		      "/from/port: missing");
		CHECK(invalid(method, R"({"from": {"node": "noise", "port": "MONO"}})") == "/to: missing");
		CHECK(invalid(method, noiseToRecFlJson.substr(0, noiseToRecFlJson.size() - 1) + R"(, "gain": 1})") ==
		      "/gain: unknown key");
		CHECK(invalid(method, R"({"from": {"node": "noise", "port": "MONO", "x": 0}, "to": {"node": "rec", "port": "FR"}})") ==
		      "/from/x: unknown key");
		CHECK(invalid(method, R"({"from": {"node": "noise", "port": "MONO"}, "to": {"node": 1, "port": "FR"}})") ==
		      "/to/node: must be a non-empty string");
	}
}

TEST_CASE("Positional params are invalid params whatever the method", "[commands]") {
	CHECK(invalid("node.add", R"(["noise", "white"])") == "the document: must be an object");
	CHECK(invalid("subtract", "[42, 23]") == "the document: must be an object");
}

TEST_CASE("An unknown method is method not found", "[commands]") {
	const RpcError error = refusal("graph.explode", "{}");
	CHECK(error.code == methodNotFound);
	CHECK(error.message == "Method not found");
	CHECK(refusal("levels.subscribe", "{}").code == methodNotFound);   // the hub's, not a graph command
}

TEST_CASE("The same bad node gives the same text in ear.json and through node.add", "[commands][graph-file]") {
	const std::string head = R"({"graphFrequency": 48000, "blockSize": 256, "nodes": [)";
	for (const std::string node : {R"({"type": "white"})", R"({"name": "a", "type": "white", "x": 1})",
	                               R"({"name": "a", "type": 3})", R"("a")"}) {
		INFO(node);
		const auto file = parseGraphFile(head + node + "]}");
		REQUIRE_FALSE(file.has_value());
		const std::string command = invalid("node.add", node);
		const std::string fromCommand = command.starts_with("the document") ? command.substr(12) : command;
		CHECK(file.error().detail == "/nodes/0" + fromCommand);
	}
}

TEST_CASE("runCommand makes the one control call its command names", "[commands]") {
	RecordingControl control;
	FixedReader reader;
	reader.view = snapshotView();
	CHECK(std::holds_alternative<ear::GraphView>(runCommand(GetGraph{}, control, reader)));
	CHECK(reader.reads == 1);
	runCommand(AddNode{{"noise", "white", json::object()}}, control, reader);
	runCommand(UpdateNode{"rec", {{"path", "y.wav"}}}, control, reader);
	runCommand(RemoveNode{"rec"}, control, reader);
	runCommand(AddRoute{noiseToRecFl}, control, reader);
	control.answer = std::unexpected(ear::UnknownRoute{noiseToRecFl});
	const CommandResult refused = runCommand(RemoveRoute{noiseToRecFl}, control, reader);
	CHECK(control.calls == std::vector<std::string>{"addNode noise", R"(updateNode rec {"path":"y.wav"})",
	                                                "removeNode rec", "addRoute FL", "removeRoute FL"});
	CHECK(reader.reads == 1);
	CHECK(ear::errorCode(std::get<std::expected<void, ear::CommandError>>(refused).error()) == "unknown-route");
	// The graph file keeper answers these (docs/design/knobly-config.md); nothing decodes them yet.
	CHECK_THROWS_AS(runCommand(GetGraphFile{}, control, reader), std::logic_error);
	CHECK_THROWS_AS(runCommand(SetGraphFile{"", "v1"}, control, reader), std::logic_error);
	CHECK(control.calls.size() == 5);
	CHECK(reader.reads == 1);
}

TEST_CASE("encodeGraph gives snapshot.json's graph exactly", "[commands][protocol]") {
	const json expected = fixture("snapshot");
	CHECK(encodeGraph(snapshotView()) == expected["graph"]);
	CHECK(json::parse(encodeResult(1, encodeCommandResult(snapshotView()).value())) == expected["messages"][0]);
}

TEST_CASE("Success is {} and each refusal is errors.json's -32000 response", "[commands][protocol]") {
	CHECK(encodeCommandResult(std::expected<void, ear::CommandError>{}).value() == json::object());
	const json messages = fixture("errors")["messages"];
	const std::vector<ear::CommandError> errors{
		ear::InvalidName{"a.b"},
		ear::UnknownNodeType{"pink"},
		ear::InvalidSettings{"tape", "path: missing"},
		ear::DuplicateNode{"noise"},
		ear::UnknownNode{"ghost"},
		ear::PortsChanged{"motu"},
		ear::UnknownNode{"ghost"},
		ear::UnknownPort{{"rec", "FX"}},
		ear::WrongDirection{{{"rec", "FL"}, {"noise", "MONO"}}},
		ear::UnknownRoute{{{"noise", "MONO"}, {"rec", "FR"}}},
		ear::Stopping{},
	};
	for (std::size_t i = 0; i < errors.size(); ++i) {
		INFO(i);
		const auto encoded = encodeCommandResult(std::expected<void, ear::CommandError>(std::unexpected(errors[i])));
		REQUIRE_FALSE(encoded.has_value());
		CHECK(json::parse(encodeError(static_cast<int>(i) + 2, encoded.error())) == messages[i + 1]);
	}
}
