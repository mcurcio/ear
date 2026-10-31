// ear.conf's state on the wire (docs/design/knobly-config.md, "Wire additions"; S3 criterion 9): the
// names the wire carries, value equality of places and diagnostics, and every message of
// test/protocol/graph-file.json built here from typed values and encoded exactly as the fixture has it.
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/commands.hpp"
#include "ear/control/events.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/control/rpc.hpp"

using namespace ear::control;
using nlohmann::json;

namespace {

json fixture(const std::string& name) {
	std::ifstream file(std::string(EAR_PROTOCOL_DIR) + "/" + name + ".json");
	REQUIRE(file);
	return json::parse(file);
}

/// The record's file format example (docs/design/knobly-config.md, "File format").
const std::string recordExample = R"(graph_frequency 48000;
block_size 256;
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:MONO rec:FL;
route noise:MONO desk:playback_1;
)";

std::string replaced(std::string text, std::string_view from, std::string_view to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	return text.replace(at, from.size(), to);
}

/// The texts of the session in graph-file.json, in order.
struct Texts {
	std::string t1 = recordExample;
	std::string t2 = replaced(t1, "take1.wav", "take2.wav");
	std::string t3 = t2 + "@include \"more.conf\";\n";
	std::string t5 = replaced(t2, "take2.wav", "take3.wav");
	std::string t7 = replaced(t2, "@set $ch 2;", "@set $ch 4;");
	std::string t8 = replaced(t7, "take2.wav", "take3.wav");
	std::string t9 = t7 + "route noise:MONO rec:FX;\n";
	std::string t10 = replaced(t7, "graph_frequency 48000;", "graph_frequency 44100;");
	std::string t11 = replaced(t10, "take2.wav", "take4.wav");
};

GraphFile file(std::string version, std::string text, std::optional<std::string> running) {
	return GraphFile{std::move(version), std::move(text), {}, std::move(running), std::nullopt, {}, std::nullopt};
}

json changed(GraphFile f, FileChangeCause cause, std::uint32_t dropped = 0) {
	return json::parse(encodeGraphFileChanged(GraphFileChanged{std::move(f), cause, dropped}));
}

json resultOf(const json& id, const CommandResult& result) {
	const auto encoded = encodeCommandResult(result);
	return json::parse(encoded ? encodeResult(id, *encoded) : encodeError(id, encoded.error()));
}

const Diagnostic includeUnsupported{Severity::error, "ear/include-unsupported",
                                    "@include is not supported yet: write the whole graph in ear.conf", TextPlace{14, 1}};

/// graph-file.json's graph: the cards fail (no such card), noise and rec run.
ear::GraphView graphView(int playback, const std::string& recPath) {
	using ear::NodeState;
	using Code = ear::FailureReason::Code;
	ear::GraphView view{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "file", "white"}, {}, {}};
	ear::Ports cardPorts;
	for (int i = 1; i <= playback; ++i) cardPorts.inputs.push_back("playback_" + std::to_string(i));
	view.nodes.push_back({{"desk", "card", {{"card", "MOTU"}, {"playback", playback}}},
	                      cardPorts,
	                      NodeState::failed,
	                      ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=MOTU,DEV=0: No such device"}});
	view.nodes.push_back({{"noise", "white", json::object()}, {{}, {"MONO"}}, NodeState::running, std::nullopt});
	view.nodes.push_back({{"porch", "card", {{"card", "USB2"}, {"playback", playback}}},
	                      cardPorts,
	                      NodeState::failed,
	                      ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=USB2,DEV=0: No such device"}});
	view.nodes.push_back({{"rec", "file", {{"path", recPath}}}, {{"FL", "FR"}, {}}, NodeState::running, std::nullopt});
	view.routes.push_back({{{"noise", "MONO"}, {"desk", "playback_1"}}, ear::RouteMatch::pending, ""});
	view.routes.push_back({{{"noise", "MONO"}, {"rec", "FL"}}, ear::RouteMatch::linked, ""});
	return view;
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
	CHECK(bare == json{{"version", "v1"}, {"text", ""}, {"diagnostics", json::array()}, {"pendingRestart", json::array()}});

	GraphFile full = file("v2", "x", "v1");
	full.diagnostics = {Diagnostic{Severity::warning, "W3005", "nothing matched", TextPlace{2, 3}}};
	full.refusal = Refusal{ear::Stopping{}, std::nullopt};
	full.pendingRestart = {FormatKey::graphFrequency, FormatKey::blockSize};
	full.writeFailed = "no space";
	CHECK(encodeGraphFile(full) ==
	      json{{"version", "v2"},
	           {"text", "x"},
	           {"diagnostics",
	            json::array({{{"severity", "warning"}, {"code", "W3005"}, {"message", "nothing matched"}, {"line", 2}, {"column", 3}}})},
	           {"running", "v1"},
	           {"refusal", {{"code", "stopping"}, {"message", "stopping"}}},
	           {"pendingRestart", json::array({"graphFrequency", "blockSize"})},
	           {"writeFailed", "no space"}});
}

TEST_CASE("Every graph-file.json message encodes as the fixture has it", "[graph_file_state][protocol]") {
	const json document = fixture("graph-file");
	const json& m = document["messages"];
	REQUIRE(m.size() == 40);
	const Texts t;
	const ear::Route toDesk{{"noise", "MONO"}, {"desk", "playback_1"}};

	SECTION("graphFile.subscribe, graph.subscribe and the first edit") {
		CHECK(resultOf(1, file("v1", t.t1, "v1")) == m[0]);
		CHECK(resultOf(2, graphView(2, "take1.wav")) == m[1]);
		CHECK(changed(file("v2", t.t2, "v2"), FileChangeCause::edit) == m[2]);
		CHECK(event(NodeUpdated{"rec", {{"path", "take2.wav"}}}) == m[3]);
		CHECK(resultOf(3, std::expected<void, ear::CommandError>{}) == m[4]);
	}
	SECTION("the refusals") {
		CHECK(resultOf(4, FileRefusal{FileChanged{}}) == m[5]);
		CHECK(resultOf(5, FileRefusal{InvalidText{{Diagnostic{Severity::error, "E2001", "unexpected token: \"}\"",
		                                                       TextPlace{9, 34}}}}}) == m[6]);
		CHECK(resultOf(6, std::expected<void, ear::CommandError>(std::unexpected(ear::UnknownPort{{"rec", "FX"}}))) == m[7]);
		CHECK(resultOf(7, FileRefusal{FileInvalid{}}) == m[9]);
		CHECK(resultOf(9, FileRefusal{SharedDefinition{{6, 5}}}) == m[16]);
	}
	SECTION("hand edits: an error, the fix, and one that drops an unwritten edit") {
		GraphFile included = file("v3", t.t3, "v2");
		included.diagnostics = {includeUnsupported};
		CHECK(changed(included, FileChangeCause::disk) == m[8]);
		CHECK(changed(file("v4", t.t2, "v4"), FileChangeCause::disk) == m[10]);
		CHECK(changed(file("v5", t.t5, "v5"), FileChangeCause::edit) == m[11]);
		CHECK(changed(file("v6", t.t2, "v6"), FileChangeCause::disk, 1) == m[14]);
		CHECK(event(NodeUpdated{"rec", {{"path", "take2.wav"}}}) == m[15]);
	}
	SECTION("playback 4 through @set $ch replaces both cards") {
		ear::Ports four;
		for (int i = 1; i <= 4; ++i) four.inputs.push_back("playback_" + std::to_string(i));
		CHECK(changed(file("v7", t.t7, "v7"), FileChangeCause::edit) == m[17]);
		CHECK(event(RouteRemoved{toDesk}) == m[18]);
		CHECK(event(NodeRemoved{"desk"}) == m[19]);
		CHECK(event(NodeRemoved{"porch"}) == m[20]);
		CHECK(event(NodeAdded{{"desk", "card", {{"card", "MOTU"}, {"playback", 4}}}, four}) == m[21]);
		CHECK(event(NodeAdded{{"porch", "card", {{"card", "USB2"}, {"playback", 4}}}, four}) == m[22]);
		CHECK(event(RouteAdded{toDesk}) == m[23]);
		CHECK(resultOf(10, std::expected<void, ear::CommandError>{}) == m[24]);
		const auto& view = graphView(4, "take4.wav");
		CHECK(event(NodeChanged{{"desk", view.nodes[0].state, view.nodes[0].reason}}) == m[25]);
		CHECK(event(NodeChanged{{"porch", view.nodes[2].state, view.nodes[2].reason}}) == m[26]);
	}
	SECTION("a write that fails, then succeeds") {
		GraphFile failed = file("v7", t.t7, "v7");
		failed.writeFailed = "ear.conf: No space left on device";
		CHECK(changed(failed, FileChangeCause::write) == m[27]);
		CHECK(changed(file("v7", t.t7, "v7"), FileChangeCause::write) == m[28]);
	}
	SECTION("a hand edit the graph refuses, then graphFile.set with another graph frequency") {
		CHECK(changed(file("v8", t.t8, "v8"), FileChangeCause::edit) == m[29]);
		GraphFile refused = file("v9", t.t9, "v8");
		refused.refusal = Refusal{ear::UnknownPort{{"rec", "FX"}}, TextPlace{14, 1}};
		CHECK(changed(refused, FileChangeCause::disk, 1) == m[32]);
		GraphFile pending = file("v10", t.t10, "v10");
		pending.pendingRestart = {FormatKey::graphFrequency};
		CHECK(changed(pending, FileChangeCause::edit) == m[33]);
	}
	SECTION("the two batches, and what the client holds at the end") {
		GraphFile last = file("v11", t.t11, "v11");
		last.pendingRestart = {FormatKey::graphFrequency};
		CHECK(changed(last, FileChangeCause::edit) == m[36]);
		const ear::GraphView view = graphView(4, "take4.wav");
		const std::vector<std::string> first{encodeResult(13, json::object()),
		                                     encodeResult(14, encodeCommandResult(view).value())};
		CHECK(json::parse(encodeBatch(first)) == m[38]);
		const std::vector<std::string> second{encodeResult(15, encodeCommandResult(view).value()),
		                                      encodeResult(16, encodeCommandResult(last).value())};
		CHECK(json::parse(encodeBatch(second)) == m[39]);
		CHECK(encodeGraph(view) == document["graph"]);
		CHECK(encodeGraphFile(last) == document["file"]);
	}
}
