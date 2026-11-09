// The graph notifications and levels (docs/design/batch2.md section 6, R1 criterion 3): each
// encoder gives exactly the fixtures' texts.
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/events.hpp"
#include "graph_file_fixture.hpp"

using namespace ear::control;
using nlohmann::json;

namespace {

json fixture(const std::string& name) {
	std::ifstream file(std::string(EAR_PROTOCOL_DIR) + "/" + name + ".json");
	REQUIRE(file);
	return json::parse(file);
}

json encoded(const GraphEvent& event) { return json::parse(encodeEvent(event)); }

} // namespace

TEST_CASE("Every graph notification encodes as edits.json has it", "[events][protocol]") {
	using ear::NodeState;
	using ear::control::testing::health;
	using ear::control::testing::reading;
	using Kind = ear::NodeCause::Kind;
	const json m = fixture("edits")["messages"];
	const ear::Route n2ToRecFr{{"n2", "noise.mono"}, {"rec", "record.right"}};
	const ear::Route n2ToTapeFl{{"n2", "noise.mono"}, {"tape", "record.left"}};
	const ear::FailureReason cannotOpen{ear::FailureReason::Code::startFailed,
	                                    "cannot open nope/t.wav: No such file or directory"};
	CHECK(encoded(NodeAdded{{"n2", "white", json::object()},
	                        ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}},
	                        ear::NodeHealth{"n2"},
	                        reading(0)}) == m[1]);
	CHECK(encoded(NodeChanged{health("n2", NodeState::starting), reading(1)}) == m[2]);
	CHECK(encoded(NodeChanged{health("n2", NodeState::running), reading(2)}) == m[4]);
	CHECK(encoded(RouteAdded{n2ToRecFr}) == m[5]);
	CHECK(encoded(RouteChanged{{n2ToRecFr, ear::RouteMatch::pending, ""}}) == m[6]);
	CHECK(encoded(RouteChanged{{n2ToRecFr, ear::RouteMatch::linked, ""}}) == m[8]);
	CHECK(encoded(NodeAdded{{"tape", "file", {{"path", "nope/t.wav"}}},
	                        ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                        ear::NodeHealth{"tape"},
	                        reading(3)}) == m[9]);
	CHECK(encoded(NodeChanged{health("tape", NodeState::failed, cannotOpen), reading(5)}) == m[11]);
	CHECK(encoded(RouteChanged{
			  {n2ToTapeFl, ear::RouteMatch::refused, "the engine removed the link before it was active"}}) == m[16]);
	CHECK(encoded(NodeUpdated{"rec", {{"path", "y.wav"}}, false, ear::NodeOutcome::restart}) == m[17]);
	auto restarting = health("rec", NodeState::stopping);
	restarting.cause = ear::NodeCause{Kind::restarting, Kind::settings, {"path"}};
	CHECK(encoded(NodeChanged{restarting, reading(6)}) == m[18]);
	auto configuring = health("rec", NodeState::starting);
	configuring.cause = ear::NodeCause{Kind::settings, std::nullopt, {"path"}};
	CHECK(encoded(NodeChanged{configuring, reading(7)}) == m[19]);
	CHECK(encoded(RouteRemoved{n2ToRecFr}) == m[22]);
	CHECK(encoded(NodeRemoved{"noise"}) == m[25]);
	auto removing = health("noise", NodeState::stopping);
	removing.cause = ear::NodeCause{Kind::removing, std::nullopt, {}};
	CHECK(encoded(NodeChanged{removing, reading(9)}) == m[26]);
	CHECK(encoded(NodeStopped{"noise", 0}) == m[28]);
	CHECK(encoded(NodeGone{"noise", 0}) == m[29]);
}

TEST_CASE("node.updated says disabled only when true, and each outcome by its name", "[events][lifecycle]") {
	const json disabled = encoded(NodeUpdated{"porch", json::object(), true, ear::NodeOutcome::disable});
	CHECK(disabled["params"] == json::parse(R"({"name":"porch","settings":{},"applied":"disable","disabled":true})"));
	CHECK(encoded(NodeUpdated{"porch", json::object(), false, ear::NodeOutcome::enable})["params"] ==
	      json::parse(R"({"name":"porch","settings":{},"applied":"enable"})"));
	CHECK(outcomeName(ear::NodeOutcome::restart) == "restart");
	CHECK(outcomeName(ear::NodeOutcome::stored) == "stored");
}

TEST_CASE("Every lifecycle.json notification is one of these encoders' and every reply one of encodeCommandResult's",
          "[events][protocol][lifecycle]") {
	// The session replay (session_test.cpp) makes the fixture through the gateway; here each method it holds is one
	// ear sends, with the members control-plane.md 4.8 gives it.
	const json document = fixture("lifecycle");
	for (const json& text : document["messages"]) {
		for (const json& message : text.is_array() ? text : json::array({text})) {
			if (!message.contains("method")) continue;
			const std::string method = message["method"];
			INFO(message.dump());
			CHECK((method == "node.added" || method == "node.state" || method == "node.updated" ||
			       method == "node.removed" || method == "node.stopped" || method == "node.gone"));
			if (method == "node.state" || method == "node.added")
				for (const char* member : {"state", "phase", "since", "wanted", "blockLost"})
					CHECK(message["params"].contains(member));
		}
	}
}

TEST_CASE("A levels notification holds every node given, linear to 4 decimals", "[events][levels][protocol]") {
	const json m = fixture("levels")["messages"];
	const std::vector<ear::NodeLevels> both{
		{"noise", 1600, {{"noise.mono", {0.99981F, 0.57712F}}}},
		{"rec", 1600, {{"record.left", {0.99978F, 0.57709F}}, {"record.right", {0.0F, 0.0F}}}},
	};
	CHECK(json::parse(encodeLevels(both)) == m[2]);
	const std::vector<ear::NodeLevels> recOnly{
		{"rec", 3200, {{"record.left", {0.9991F, 0.5779F}}, {"record.right", {0.0F, 0.0F}}}}};
	CHECK(json::parse(encodeLevels(recOnly)) == m[3]);
	CHECK(json::parse(encodeLevels({})) ==
	      json::parse(R"({"jsonrpc": "2.0", "method": "levels", "params": {"nodes": {}}})"));
}
