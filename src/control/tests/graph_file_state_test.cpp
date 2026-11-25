// ear.conf's state on the wire (docs/control-port.md#the-graph-file-over-the-port): the
// names the wire carries, value equality of places and diagnostics, and every message of
// test/protocol/graph-file.json built here from typed values (graph_file_fixture.hpp) and encoded
// exactly as the fixture has it.
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/control/rpc.hpp"
#include "graph_file_fixture.hpp"

using namespace ear::control;
using namespace ear::control::testing;
using nlohmann::json;

namespace {

json fixture(const std::string& name) {
	std::ifstream file(std::string(EAR_PROTOCOL_DIR) + "/" + name + ".json");
	REQUIRE(file);
	return json::parse(file);
}

json changed(GraphFile f, FileChangeCause cause, std::uint32_t dropped = 0) {
	return json::parse(encodeGraphFileChanged(GraphFileChanged{std::move(f), cause, dropped}));
}

json resultOf(const json& id, const CommandResult& result) {
	const auto encoded = encodeCommandResult(result);
	return json::parse(encoded ? encodeResult(id, *encoded) : encodeError(id, encoded.error()));
}

json event(const GraphEvent& e) { return json::parse(encodeEvent(e)); }

} // namespace

TEST_CASE("Severities, format keys and causes are named as the wire writes them", "[graph_file_state]") {
	CHECK(severityName(Severity::error) == "error");
	CHECK(severityName(Severity::warning) == "warning");
	CHECK(formatKeyName(FormatKey::graphFrequency) == "graphFrequency");
	CHECK(formatKeyName(FormatKey::blockSize) == "blockSize");
	CHECK(causeName(FileChangeCause::edit) == "edit");
	CHECK(causeName(FileChangeCause::disk) == "disk");
	CHECK(causeName(FileChangeCause::write) == "write");
}

TEST_CASE("Each file refusal has its code, and describe adds what it names", "[graph_file_state]") {
	CHECK(fileRefusalCode(FileChanged{}) == "file-changed");
	CHECK(fileRefusalCode(InvalidText{}) == "invalid-text");
	CHECK(fileRefusalCode(FileInvalid{}) == "file-invalid");
	CHECK(fileRefusalCode(SharedDefinition{{6, 5}}) == "shared-definition");
	CHECK(describe(FileRefusal{FileChanged{}}) == "file-changed");
	CHECK(describe(FileRefusal{InvalidText{{includeUnsupported}}}) == "invalid-text");
	CHECK(describe(FileRefusal{FileInvalid{}}) == "file-invalid");
	CHECK(describe(FileRefusal{SharedDefinition{{6, 5}}}) == "shared-definition 6:5");
}

TEST_CASE("Places and diagnostics compare by value", "[graph_file_state]") {
	const Diagnostic typo{Severity::error, "E2001", "unexpected token", TextPlace{3, 7}};
	CHECK(typo == Diagnostic{Severity::error, "E2001", "unexpected token", TextPlace{3, 7}});
	CHECK_FALSE(typo == Diagnostic{Severity::error, "E2001", "unexpected token", TextPlace{3, 8}});
	CHECK_FALSE(typo == Diagnostic{Severity::warning, "E2001", "unexpected token", TextPlace{3, 7}});
	CHECK_FALSE(typo == Diagnostic{Severity::error, "ear/bad-directive", "unexpected token", TextPlace{3, 7}});
}

TEST_CASE("A graph file's JSON leaves out each absent optional and holds each present one", "[graph_file_state]") {
	const json bare = encodeGraphFile(file("v1", "", std::nullopt));
	CHECK(bare ==
	      json{{"version", "v1"}, {"text", ""}, {"diagnostics", json::array()}, {"pendingRestart", json::array()}});

	GraphFile full = file("v2", "x", "v1");
	full.diagnostics = {Diagnostic{Severity::warning, "W3005", "nothing matched", TextPlace{2, 3}}};
	full.refusal = Refusal{ear::Stopping{}, std::nullopt};
	full.pendingRestart = {FormatKey::graphFrequency, FormatKey::blockSize};
	full.writeFailed = "no space";
	full.nodeTexts = {{"a", {1, 1}, {1, 15}}, {"t1", {3, 5}, {3, 20}, true}};
	CHECK(encodeGraphFile(full) ==
	      json{{"version", "v2"},
	           {"text", "x"},
	           {"diagnostics", json::array({{{"severity", "warning"},
	                                         {"code", "W3005"},
	                                         {"message", "nothing matched"},
	                                         {"line", 2},
	                                         {"column", 3}}})},
	           {"running", "v1"},
	           {"refusal", {{"code", "stopping"}, {"message", "stopping"}}},
	           {"pendingRestart", json::array({"graphFrequency", "blockSize"})},
	           {"writeFailed", "no space"},
	           {"nodeTexts",
	            json::array(
					{{{"name", "a"}, {"start", {{"line", 1}, {"column", 1}}}, {"end", {{"line", 1}, {"column", 15}}}},
	                 {{"name", "t1"},
	                  {"start", {{"line", 3}, {"column", 5}}},
	                  {"end", {{"line", 3}, {"column", 20}}},
	                  {"shared", true}}})}});
	full.nodeTexts = std::vector<NodeText>{};
	CHECK(encodeGraphFile(full)["nodeTexts"] == json::array()); // a file that reads with no nodes says so
}

TEST_CASE("Every graph-file.json message encodes as the fixture has it", "[graph_file_state][protocol]") {
	const json document = fixture("graph-file");
	const json& m = document["messages"];
	REQUIRE(m.size() == 39);
	const Texts t;

	SECTION("graphFile.subscribe, graph.subscribe and the first edit") {
		CHECK(resultOf(1, recordFile("v1", t.t1, "v1")) == m[0]);
		CHECK(resultOf(2, graphView(2, "take1.wav")) == m[1]);
		CHECK(changed(recordFile("v2", t.t2, "v2"), FileChangeCause::edit) == m[2]);
		CHECK(event(NodeUpdated{"rec", {{"path", "take2.wav"}}, false, ear::NodeOutcome::restart}) == m[3]);
		CHECK(resultOf(3, std::expected<void, ear::CommandError>{}) == m[4]);
	}
	SECTION("the refusals") {
		CHECK(resultOf(4, FileRefusal{FileChanged{}}) == m[5]);
		CHECK(resultOf(5, FileRefusal{InvalidText{{Diagnostic{Severity::error, "E2001", "unexpected token: \"}\"",
		                                                      TextPlace{11, 34}}}}}) == m[6]);
		CHECK(resultOf(6, std::expected<void, ear::CommandError>(std::unexpected(ear::UnknownPort{{"rec", "FX"}}))) ==
		      m[7]);
		CHECK(resultOf(7, FileRefusal{FileInvalid{}}) == m[9]);
		CHECK(resultOf(9, FileRefusal{SharedDefinition{{8, 5}}}) == m[16]);
	}
	SECTION("hand edits: an error, the fix, and one that drops an unwritten edit") {
		GraphFile included = file("v3", t.t3, "v2");
		included.diagnostics = {includeUnsupported};
		CHECK(changed(included, FileChangeCause::disk) == m[8]);
		CHECK(changed(recordFile("v4", t.t2, "v4"), FileChangeCause::disk) == m[10]);
		CHECK(changed(recordFile("v5", t.t5, "v5"), FileChangeCause::edit) == m[11]);
		CHECK(changed(recordFile("v6", t.t2, "v6"), FileChangeCause::disk, 1) == m[14]);
		CHECK(event(NodeUpdated{"rec", {{"path", "take2.wav"}}, false, ear::NodeOutcome::restart}) == m[15]);
	}
	SECTION("playback 4 through @set $ch reshapes both cards") {
		const ear::Ports two{{{ear::Direction::input, "play", ear::numbered(2)}}};
		const ear::Ports four{{{ear::Direction::input, "play", ear::numbered(4)}}};
		CHECK(changed(recordFile("v7", t.t7, "v7"), FileChangeCause::edit) == m[17]);
		const NodeReshaped reshaped{false, ear::portsMoved(two, four), four};
		CHECK(event(NodeUpdated{
				  "desk", {{"card", "MOTU"}, {"playback", 4}}, false, ear::NodeOutcome::reshape, reshaped}) == m[18]);
		CHECK(event(NodeUpdated{
				  "porch", {{"card", "USB2"}, {"playback", 4}}, false, ear::NodeOutcome::reshape, reshaped}) == m[19]);
		// The gateway's clock: one reading for each node.state that changed a phase.
		ear::NodeHealth desk = health("desk", ear::NodeState::starting);
		desk.cause = ear::NodeCause{ear::NodeCause::Kind::reshape, std::nullopt, {"playback"}};
		desk.cause->ports = ear::PortsMoved{{"play.3", "play.4"}, {}};
		CHECK(event(NodeChanged{desk, reading(0)}) == m[20]);
		ear::NodeHealth porch = desk;
		porch.name = "porch";
		CHECK(event(NodeChanged{porch, reading(1)}) == m[21]);
		CHECK(resultOf(10, std::expected<void, ear::CommandError>{}) == m[22]);
		const auto& view = graphView(4, "take4.wav");
		CHECK(event(NodeChanged{view.nodes[0].health, reading(2)}) == m[23]);
		CHECK(event(NodeChanged{view.nodes[2].health, reading(3)}) == m[24]);
	}
	SECTION("a write that fails, then succeeds") {
		GraphFile failed = recordFile("v7", t.t7, "v7");
		failed.writeFailed = "ear.conf: No space left on device";
		CHECK(changed(failed, FileChangeCause::write) == m[25]);
		CHECK(changed(recordFile("v7", t.t7, "v7"), FileChangeCause::write) == m[26]);
	}
	SECTION("a hand edit the graph refuses, then graphFile.set with another graph frequency") {
		CHECK(changed(recordFile("v8", t.t8, "v8"), FileChangeCause::edit) == m[27]);
		GraphFile refused = recordFile("v9", t.t9, "v8");
		refused.refusal = Refusal{ear::UnknownPort{{"rec", "FX"}}, TextPlace{16, 1}};
		CHECK(changed(refused, FileChangeCause::disk, 1) == m[30]);
		GraphFile pending = recordFile("v10", t.t10, "v10");
		pending.pendingRestart = {FormatKey::graphFrequency};
		CHECK(changed(pending, FileChangeCause::edit) == m[31]);
	}
	SECTION("the two batches, graphFile.get, and what the client holds at the end") {
		GraphFile last = recordFile("v11", t.t11, "v11");
		last.pendingRestart = {FormatKey::graphFrequency};
		CHECK(changed(last, FileChangeCause::edit) == m[34]);
		ear::GraphView view = graphView(4, "take4.wav");
		view.nodes[0].since = reading(2); // the gateway fills in when each card's failure began
		view.nodes[2].since = reading(3);
		const std::vector<std::string> first{encodeResult(13, json::object()),
		                                     encodeResult(14, encodeCommandResult(view).value())};
		CHECK(json::parse(encodeBatch(first)) == m[36]);
		const std::vector<std::string> second{encodeResult(15, encodeCommandResult(view).value()),
		                                      encodeResult(16, encodeCommandResult(last).value())};
		CHECK(json::parse(encodeBatch(second)) == m[37]);
		CHECK(resultOf(17, last) == m[38]);
		CHECK(encodeGraph(view) == document["graph"]);
		CHECK(encodeGraphFile(last) == document["file"]);
	}
}
