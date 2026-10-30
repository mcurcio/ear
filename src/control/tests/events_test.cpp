// The graph notifications and levels (docs/design/batch2.md section 6, R1 criterion 3): each
// encoder gives exactly the fixtures' texts.
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/events.hpp"

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
	const json m = fixture("edits")["messages"];
	const ear::Route n2ToRecFr{{"n2", "MONO"}, {"rec", "FR"}};
	const ear::Route n2ToTapeFl{{"n2", "MONO"}, {"tape", "FL"}};
	CHECK(encoded(NodeAdded{{"n2", "white", json::object()}, {{}, {"MONO"}}}) == m[1]);
	CHECK(encoded(NodeChanged{{"n2", NodeState::starting, std::nullopt}}) == m[2]);
	CHECK(encoded(NodeChanged{{"n2", NodeState::running, std::nullopt}}) == m[4]);
	CHECK(encoded(RouteAdded{n2ToRecFr}) == m[5]);
	CHECK(encoded(RouteChanged{{n2ToRecFr, ear::RouteMatch::pending, ""}}) == m[6]);
	CHECK(encoded(RouteChanged{{n2ToRecFr, ear::RouteMatch::linked, ""}}) == m[8]);
	CHECK(encoded(NodeAdded{{"tape", "file", {{"path", "nope/t.wav"}}}, {{"FL", "FR"}, {}}}) == m[9]);
	CHECK(encoded(NodeChanged{{"tape", NodeState::failed,
	                           ear::FailureReason{ear::FailureReason::Code::startFailed,
	                                              "cannot open nope/t.wav: No such file or directory"}}}) == m[11]);
	CHECK(encoded(RouteChanged{{n2ToTapeFl, ear::RouteMatch::refused, "the engine removed the link before it was active"}}) ==
	      m[16]);
	CHECK(encoded(NodeUpdated{"rec", {{"path", "y.wav"}}}) == m[17]);
	CHECK(encoded(RouteRemoved{n2ToRecFr}) == m[21]);
	CHECK(encoded(NodeRemoved{"noise"}) == m[24]);
	CHECK(encoded(NodeChanged{{"noise", NodeState::stopping, std::nullopt}}) == m[25]);
	CHECK(encoded(NodeStopped{"noise", 0}) == m[27]);
}

TEST_CASE("A levels notification holds every node given, linear to 4 decimals", "[events][levels][protocol]") {
	const json m = fixture("levels")["messages"];
	const std::vector<ear::NodeLevels> both{
		{"noise", 1600, {{"MONO", {0.99981F, 0.57712F}}}},
		{"rec", 1600, {{"FL", {0.99978F, 0.57709F}}, {"FR", {0.0F, 0.0F}}}},
	};
	CHECK(json::parse(encodeLevels(both)) == m[2]);
	const std::vector<ear::NodeLevels> recOnly{{"rec", 3200, {{"FL", {0.9991F, 0.5779F}}, {"FR", {0.0F, 0.0F}}}}};
	CHECK(json::parse(encodeLevels(recOnly)) == m[3]);
	CHECK(json::parse(encodeLevels({})) == json::parse(R"({"jsonrpc": "2.0", "method": "levels", "params": {"nodes": {}}})"));
}
