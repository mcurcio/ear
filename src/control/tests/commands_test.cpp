// The control port's commands (docs/control-port.md): decodeCommand for every method and every missing, extra and
// mistyped key; encodeCommandResult and encodeGraph give exactly the fixtures' texts.
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/commands.hpp"
#include "graph_file_fixture.hpp"

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

/// The message of an invalid params refusal, whose data names the pointer its message starts with.
std::string invalid(std::string_view method, const std::string& params) {
	const RpcError error = refusal(method, params);
	CHECK(error.code == invalidParams);
	const std::string pointer =
		error.message.starts_with("the document") ? "" : error.message.substr(0, error.message.find(':'));
	CHECK(error.data == json{{"pointer", pointer}});
	return error.message;
}

const ear::Route noiseToRecFl{{"noise", "noise.mono"}, {"rec", "record.left"}};
const std::string noiseToRecFlJson =
	R"({"from": {"node": "noise", "port": "noise.mono"}, "to": {"node": "rec", "port": "record.left"}})";

} // namespace

TEST_CASE("Every method decodes into its command", "[commands]") {
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.get", nullptr).value()));
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.get", json::object()).value()));
	CHECK(std::holds_alternative<GetGraph>(decodeCommand("graph.subscribe", nullptr).value()));

	const auto add =
		decodeCommand("node.add", json::parse(R"({"name": "rec", "type": "file", "settings": {"path": "x.wav"}})"));
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

	for (const std::string_view method : {"graphFile.get", "graphFile.subscribe"}) {
		INFO(method);
		CHECK(std::holds_alternative<GetGraphFile>(decodeCommand(method, nullptr).value()));
		CHECK(std::holds_alternative<GetGraphFile>(decodeCommand(method, json::object()).value()));
	}
	const auto set = decodeCommand("graphFile.set", json::parse(R"({"text": "node white n;\n", "version": "a.3"})"));
	CHECK(std::get<SetGraphFile>(set.value()).text == "node white n;\n");
	CHECK(std::get<SetGraphFile>(set.value()).version == "a.3");
	const auto empty = decodeCommand("graphFile.set", json::parse(R"({"text": "", "version": ""})"));
	CHECK(std::get<SetGraphFile>(empty.value()).text.empty()); // an empty file is a text

	CHECK(std::get<StartNode>(decodeCommand("node.start", json::parse(R"({"name": "porch"})")).value()).node ==
	      "porch");
	CHECK(std::get<StopNode>(decodeCommand("node.stop", json::parse(R"({"name": "porch"})")).value()).node == "porch");
	CHECK(std::get<RestartNode>(decodeCommand("node.restart", json::parse(R"({"name": "desk"})")).value()).node ==
	      "desk");
	CHECK(std::get<DisableNode>(decodeCommand("node.disable", json::parse(R"({"name": "porch"})")).value()).node ==
	      "porch");
	CHECK(std::get<EnableNode>(decodeCommand("node.enable", json::parse(R"({"name": "porch"})")).value()).node ==
	      "porch");
	const auto disabled = decodeCommand("node.add", json::parse(R"({"name": "q", "type": "white", "disabled": true})"));
	CHECK(std::get<AddNode>(disabled.value()).node.disabled);
	const auto enabled = decodeCommand("node.add", json::parse(R"({"name": "q", "type": "white", "disabled": false})"));
	CHECK_FALSE(std::get<AddNode>(enabled.value()).node.disabled);
	CHECK_FALSE(std::get<AddNode>(addPlain.value()).node.disabled);
}

TEST_CASE("The lifecycle's five methods take a name and nothing else, and node.add's disabled is a boolean",
          "[commands][lifecycle]") {
	for (const std::string_view method : {"node.start", "node.stop", "node.restart", "node.disable", "node.enable"}) {
		INFO(method);
		CHECK(invalid(method, "{}") == "/name: missing");
		CHECK(invalid(method, "null") == "/name: missing");
		CHECK(invalid(method, R"({"name": ""})") == "/name: must be a non-empty string");
		CHECK(invalid(method, R"({"name": "a", "force": true})") == "/force: unknown key");
	}
	CHECK(invalid("node.add", R"({"name": "q", "type": "white", "disabled": "yes"})") ==
	      "/disabled: must be a boolean");
	CHECK(invalid("node.add", R"({"name": "q", "type": "white", "colour": "red"})") == "/colour: unknown key");
}

TEST_CASE("The frame: graph.get says protocol 1 and the lifecycle, counters and companions features",
          "[commands][lifecycle]") {
	const json graph =
		encodeGraph(ear::GraphView{{{ear::SampleRate::fixed<48000>(), 256}, false, "SCHED_OTHER"}, {}, {}, {}});
	CHECK(graph["protocol"] == 1);
	CHECK(graph["features"] == json::array({"lifecycle", "counters", "companions"}));
}

TEST_CASE("A snapshot's node carries its companion list in request order, each counter only on its kind, and [] for "
          "none",
          "[commands][companions]") {
	ear::GraphView view{{{ear::SampleRate::fixed<48000>(), 256}, false, "SCHED_OTHER"}, {}, {}, {}};
	view.nodes.push_back({{"n", "white", json::object()}, ear::Ports{}, ear::NodeHealth{"n"}});
	view.nodes.push_back(
		{{"x", "test", json::object()},
	     ear::Ports{},
	     ear::NodeHealth{"x"},
	     std::nullopt,
	     {},
	     {{.name = "x.playback", .kind = ear::CompanionKind::cardPlayback, .bank = "to_card", .ready = true},
	      {.name = "x.rtp", .kind = ear::CompanionKind::rtpReceiver, .bank = "from_rtp"},
	      {.name = "x.out", .kind = ear::CompanionKind::sink, .bank = "out", .ready = true, .dropped = 4096},
	      {.name = "x.in", .kind = ear::CompanionKind::source, .bank = "in", .ready = true, .shortfall = 7}}});
	const json graph = encodeGraph(view);
	CHECK(graph["nodes"][0].value("companions", json()) == json::array());
	CHECK(graph["nodes"][1].value("companions", json()) == json::parse(R"([
		{"name": "x.playback", "kind": "card-playback", "bank": "to_card", "ready": true},
		{"name": "x.rtp", "kind": "rtp-receiver", "bank": "from_rtp", "ready": false},
		{"name": "x.out", "kind": "sink", "bank": "out", "ready": true, "dropped": 4096},
		{"name": "x.in", "kind": "source", "bank": "in", "ready": true, "shortfall": 7}])"));
}

TEST_CASE("A snapshot's counters: the graph's with lostMs at its own rate and block size, and every node's",
          "[commands][counters]") {
	ear::GraphView view{{{ear::SampleRate::fixed<44100>(), 512}, false, "SCHED_OTHER"}, {}, {}, {}};
	view.counts = ear::GraphCounts{.cycles = 900, .cutCycles = 10, .stalls = 3};
	view.nodes.push_back({{"n", "white", json::object()},
	                      ear::Ports{},
	                      ear::NodeHealth{"n"},
	                      std::nullopt,
	                      ear::NodeCounts{.missedCycles = 1, .overruns = 2, .repositions = 3, .driverChanges = 4}});
	const json graph = encodeGraph(view);
	CHECK(graph["counters"] == json::parse(R"({"cycles":900,"cutCycles":10,"stalls":3,"lostMs":116})"));
	CHECK(graph["nodes"][0]["counters"] ==
	      json::parse(R"({"missedCycles":1,"overruns":2,"repositions":3,"driverChanges":4})"));
}

TEST_CASE("Each health member is encoded once for snapshots: state, phase, wanted and blockLost always, since, "
          "cause, waitingFor, reason and disabled when there",
          "[commands][lifecycle]") {
	using ear::NodePhase;
	using Kind = ear::NodeCause::Kind;
	ear::NodeHealth health{"desk"};
	health.phase = NodePhase::stopping;
	health.state = ear::NodeState::stopping;
	health.cause = ear::NodeCause{Kind::restarting, Kind::settings, {"card", "playback"}};
	health.blockLost = true;
	ear::NodeSpec spec{"desk", "card", json::object()};
	spec.disabled = true;
	ear::GraphView view{{{ear::SampleRate::fixed<48000>(), 256}, false, "SCHED_OTHER"}, {}, {}, {}};
	view.nodes.push_back({spec, ear::Ports{}, health, ear::control::testing::reading(1)});
	ear::NodeHealth waits{"fpp"};
	waits.phase = NodePhase::configuring;
	waits.cause = ear::NodeCause{Kind::settings, std::nullopt, {"port"}};
	waits.waitingFor = {{ear::WaitingFor::Kind::companion, "fpp.rtp"}, {ear::WaitingFor::Kind::block}};
	view.nodes.push_back({{"fpp", "rtp", json::object()}, ear::Ports{}, waits});
	const json nodes = encodeGraph(view)["nodes"];
	CHECK(nodes[0] == json::parse(R"({"name":"desk","type":"card","settings":{},"inputs":[],"outputs":[],
		"state":"stopping","phase":"stopping","since":"2026-11-09T10:00:00.010000Z","wanted":"running","blockLost":true,
		"cause":{"kind":"restarting","restart":{"kind":"settings","keys":["card","playback"]}},"disabled":true,
		"counters":{"missedCycles":0,"overruns":0,"repositions":0,"driverChanges":0},"companions":[]})"));
	CHECK(nodes[1] == json::parse(R"({"name":"fpp","type":"rtp","settings":{},"inputs":[],"outputs":[],
		"state":"starting","phase":"configuring","wanted":"running","blockLost":false,
		"cause":{"kind":"settings","keys":["port"]},"waitingFor":[{"kind":"companion","name":"fpp.rtp"},{"kind":"block"}],
		"counters":{"missedCycles":0,"overruns":0,"repositions":0,"driverChanges":0},"companions":[]})"));
}

TEST_CASE("Every missing, extra and mistyped key is invalid params naming its pointer", "[commands]") {
	CHECK(invalid("graph.get", R"({"x": 1})") == "/x: unknown key");
	CHECK(invalid("graph.subscribe", R"({"x": 1})") == "/x: unknown key");

	CHECK(invalid("node.add", R"({"type": "white"})") == "/name: missing");
	CHECK(invalid("node.add", R"({"name": "a"})") == "/type: missing");
	CHECK(invalid("node.add", R"({"name": "a", "type": "white", "x": 1})") == "/x: unknown key");
	CHECK(invalid("node.add", R"({"name": 2, "type": "white"})") == "/name: must be a non-empty string");
	CHECK(invalid("node.add", R"({"name": "a", "type": ["white"]})") == "/type: must be a non-empty string");
	CHECK(invalid("node.add", "null") == "/name: missing"); // null: params absent
	CHECK(invalid("node.add", R"("noise")") == "the document: must be an object");

	CHECK(invalid("node.update", R"({"settings": {}})") == "/name: missing");
	CHECK(invalid("node.update", R"({"name": "a"})") == "/settings: missing");
	CHECK(invalid("node.update", R"({"name": "a", "settings": {}, "type": "white"})") == "/type: unknown key");
	CHECK(invalid("node.update", R"({"name": "", "settings": {}})") == "/name: must be a non-empty string");

	CHECK(invalid("node.remove", "{}") == "/name: missing");
	CHECK(invalid("node.remove", R"({"name": "a", "force": true})") == "/force: unknown key");
	CHECK(invalid("node.remove", R"({"name": null})") == "/name: must be a non-empty string");

	CHECK(invalid("graphFile.get", R"({"x": 1})") == "/x: unknown key");
	CHECK(invalid("graphFile.subscribe", R"({"version": "a.1"})") == "/version: unknown key");
	CHECK(invalid("graphFile.set", R"({"version": "a.1"})") == "/text: missing");
	CHECK(invalid("graphFile.set", R"({"text": ""})") == "/version: missing");
	CHECK(invalid("graphFile.set", R"({"text": "", "version": "a.1", "path": "ear.conf"})") == "/path: unknown key");
	CHECK(invalid("graphFile.set", R"({"text": 1, "version": "a.1"})") == "/text: must be a string");
	CHECK(invalid("graphFile.set", R"({"text": "", "version": null})") == "/version: must be a string");
	CHECK(invalid("graphFile.set", "null") == "/text: missing");

	for (const std::string_view method : {"route.add", "route.remove"}) {
		INFO(method);
		CHECK(invalid(method, R"({"from": {"node": "noise"}, "to": {"node": "rec", "port": "record.right"}})") ==
		      "/from/port: missing");
		CHECK(invalid(method, R"({"from": {"node": "noise", "port": "noise.mono"}})") == "/to: missing");
		CHECK(invalid(method, noiseToRecFlJson.substr(0, noiseToRecFlJson.size() - 1) + R"(, "gain": 1})") ==
		      "/gain: unknown key");
		CHECK(
			invalid(
				method,
				R"({"from": {"node": "noise", "port": "noise.mono", "x": 0}, "to": {"node": "rec", "port": "record.right"}})") ==
			"/from/x: unknown key");
		CHECK(invalid(
				  method,
				  R"({"from": {"node": "noise", "port": "noise.mono"}, "to": {"node": 1, "port": "record.right"}})") ==
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
	CHECK(refusal("levels.subscribe", "{}").code == methodNotFound); // the hub's, not a graph command
}

TEST_CASE("encodeGraph gives snapshot.json's graph exactly", "[commands][protocol]") {
	const json expected = fixture("snapshot");
	using ear::control::testing::snapshotView;
	CHECK(encodeGraph(snapshotView()) == expected["graph"]);
	CHECK(json::parse(encodeResult(1, encodeCommandResult(snapshotView()).value())) == expected["messages"][0]);
}

TEST_CASE("Applied is {applied} and the lifecycle's refusals are errors.json's", "[commands][protocol][lifecycle]") {
	CHECK(encodeCommandResult(Applied{AppliedKind::retry}).value() == json{{"applied", "retry"}});
	const json messages = fixture("errors")["messages"];
	const std::vector<CommandResult> results{
		ControlRefusal{NodeDisabled{"porch"}},
		ControlRefusal{NotRestartable{"desk", NotRestartable::Reason::blockLost}},
		ControlRefusal{NotRestartable{"fpp", NotRestartable::Reason::notStarted}},
		std::expected<void, ear::CommandError>(std::unexpected(ear::UnknownNode{"ghost"})),
	};
	for (std::size_t i = 0; i < results.size(); ++i) {
		INFO(i);
		const auto encoded = encodeCommandResult(results[i]);
		REQUIRE_FALSE(encoded.has_value());
		CHECK(json::parse(encodeError(static_cast<int>(i) + 14, encoded.error())) == messages[i + 15]);
	}
	for (const std::size_t at : {14U, 19U, 20U}) { // each -32602 names its pointer in data
		INFO(at);
		CHECK(messages[at]["error"]["code"] == invalidParams);
		CHECK(messages[at]["error"]["data"].contains("pointer"));
	}
	CHECK(appliedName(AppliedKind::stored) == "stored");
	CHECK(verbName(LifecycleVerb::restart) == "restart");
	CHECK(describe(ControlRefusal{NodeDisabled{"porch"}}) == "node-disabled porch");
}

TEST_CASE("unknown-values.json starts from a snapshot ear makes", "[commands][protocol][lifecycle]") {
	using ear::NodeState;
	using ear::control::testing::health;
	const json document = fixture("unknown-values");
	ear::GraphView view{{{ear::SampleRate::fixed<48000>(), 256}, false, "SCHED_OTHER"}, {"file", "white"}, {}, {}};
	view.nodes.push_back({{"noise", "white", json::object()},
	                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}},
	                      health("noise", NodeState::running),
	                      ear::control::testing::reading(0)});
	view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}},
	                      ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                      health("rec", NodeState::running)});
	view.routes.push_back({noiseToRecFl, ear::RouteMatch::linked, ""});
	CHECK(json::parse(encodeResult(1, encodeCommandResult(view).value())) == document["messages"][0]);
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
		ear::UnknownNode{"ghost"},
		ear::UnknownPort{{"rec", "FX"}},
		ear::WrongDirection{{{"rec", "record.left"}, {"noise", "noise.mono"}}},
		ear::UnknownRoute{{{"noise", "noise.mono"}, {"rec", "record.right"}}},
		ear::Stopping{},
	};
	for (std::size_t i = 0; i < errors.size(); ++i) {
		INFO(i);
		const auto encoded = encodeCommandResult(std::expected<void, ear::CommandError>(std::unexpected(errors[i])));
		REQUIRE_FALSE(encoded.has_value());
		CHECK(json::parse(encodeError(static_cast<int>(i) + 2, encoded.error())) == messages[i + 1]);
	}
}
