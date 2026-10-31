// The graph edits made at the source (docs/design/knobly-config.md, "File format"; S3 criteria 1-8):
// each edit on the record's example gives a candidate whose text changed only where the rule says and
// which reads back as the edited graph, or the refusal the rule names; the model it is called on never
// changes. Pure: text in, candidates out, against the test types.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "graph_file_model.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using json = nlohmann::json;

namespace {

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

/// A snippet that makes a node, used twice: its node line makes two rendered nodes.
const std::string twiceUsed = R"(graph_frequency 48000;
block_size 256;
@snippet take $n {
    node file $n { path "a.wav"; }
}
@use take t1;
@use take t2;
node white noise;
)";

struct Fixture {
	NodeCatalogue catalogue{test::testTypes()};
	GraphFileModel load(const std::string& text) const { return GraphFileModel::load(DiskText{text}, catalogue); }
};

std::string replaced(std::string text, std::string_view from, std::string_view to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	return text.replace(at, from.size(), to);
}

/// The candidate an edit gives; the test fails when it is a refusal or nothing.
GraphFileModel candidate(EditOutcome outcome) {
	REQUIRE(outcome.has_value());
	REQUIRE(outcome->has_value());
	return std::move(**outcome);
}

EditRefusal refusal(const EditOutcome& outcome) {
	REQUIRE_FALSE(outcome.has_value());
	return outcome.error();
}

std::optional<control::TextPlace> sharedAt(const EditOutcome& outcome) {
	const EditRefusal refused = refusal(outcome);
	const auto* file = std::get_if<control::FileRefusal>(&refused);
	if (file == nullptr) return std::nullopt;
	const auto* shared = std::get_if<control::SharedDefinition>(file);
	if (shared == nullptr) return std::nullopt;
	return shared->place;
}

std::string commandCode(const EditOutcome& outcome) {
	const EditRefusal refused = refusal(outcome);
	const auto* error = std::get_if<CommandError>(&refused);
	REQUIRE(error != nullptr);
	return std::string(errorCode(*error));
}

std::string commandDetail(const EditOutcome& outcome) {
	const EditRefusal refused = refusal(outcome);
	const auto* error = std::get_if<CommandError>(&refused);
	REQUIRE(error != nullptr);
	return describe(*error);
}

const NodeSpec& nodeIn(const GraphFileModel& model, const NodeName& name) {
	REQUIRE(model.read().wanted.has_value());
	for (const auto& node : model.read().wanted->nodes)
		if (node.name == name) return node;
	FAIL("no node " << name);
	throw std::logic_error("unreachable");
}

std::size_t count(const std::string& text, std::string_view what) {
	std::size_t n = 0;
	for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
	return n;
}

} // namespace

TEST_CASE("A changed path changes only that value's characters", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);
	const auto edited = candidate(model.updateNode("rec", {{"path", "take2.wav"}}));
	CHECK(edited.text() == replaced(recordExample, "take1.wav", "take2.wav"));
	CHECK(nodeIn(edited, "rec").settings == json{{"path", "take2.wav"}});
	CHECK(edited.read().diagnostics.empty());
}

TEST_CASE("A changed value sets its definition, so every node using it changes", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	SECTION("playback through @set $ch") {
		const auto edited = candidate(model.updateNode("desk", {{"card", "MOTU"}, {"playback", 4}}));
		CHECK(edited.text() == replaced(recordExample, "@set $ch 2;", "@set $ch 4;"));
		CHECK(nodeIn(edited, "desk").settings == json{{"card", "MOTU"}, {"playback", 4}});
		CHECK(nodeIn(edited, "porch").settings == json{{"card", "USB2"}, {"playback", 4}});
	}
	SECTION("card through desk's own @use argument") {
		const auto edited = candidate(model.updateNode("desk", {{"card", "USB9"}, {"playback", 2}}));
		CHECK(edited.text() == replaced(recordExample, "@use card_out MOTU;", "@use card_out \"USB9\";"));
		CHECK(nodeIn(edited, "desk").settings == json{{"card", "USB9"}, {"playback", 2}});
		CHECK(nodeIn(edited, "porch").settings == json{{"card", "USB2"}, {"playback", 2}});
	}
}

TEST_CASE("An added key goes in the node's own block; a key made by a shared line is refused", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	SECTION("desk capture 2 in desk's block") {
		const auto edited = candidate(model.updateNode("desk", {{"card", "MOTU"}, {"playback", 2}, {"capture", 2}}));
		CHECK(nodeIn(edited, "desk").settings == json{{"card", "MOTU"}, {"playback", 2}, {"capture", 2}});
		CHECK(nodeIn(edited, "porch").settings == json{{"card", "USB2"}, {"playback", 2}});
		const std::string& text = edited.text();
		const auto desk = text.find("node card desk {");
		const auto porch = text.find("node card porch");
		const auto capture = text.find("capture 2;");
		REQUIRE(capture != std::string::npos);
		CHECK(desk < capture);
		CHECK(capture < porch);
		CHECK(count(text, "capture") == 1);
		CHECK(text.starts_with(recordExample.substr(0, recordExample.find("node card desk"))));
	}
	SECTION("removing playback from desk: the snippet's playback line makes porch's too") {
		CHECK(sharedAt(model.updateNode("desk", {{"card", "MOTU"}})) == control::TextPlace{6, 5});
	}
	SECTION("adding a key to a node written in a snippet used twice") {
		const auto twice = f.load(twiceUsed);
		CHECK(sharedAt(twice.updateNode("t1", {{"path", "a.wav"}, {"gain", 3}})) == control::TextPlace{4, 5});
	}
	SECTION("a node written without a block is written with one") {
		const auto edited = candidate(model.updateNode("noise", {{"level", "low"}}));
		CHECK(edited.text() == replaced(recordExample, "node white noise;", "node white noise { level \"low\"; }"));
		CHECK(nodeIn(edited, "noise").settings == json{{"level", "low"}});
	}
	SECTION("a removed key's own line goes") {
		const auto edited = candidate(model.updateNode("rec", json::object()));
		CHECK(count(edited.text(), "take1.wav") == 0);
		CHECK(nodeIn(edited, "rec").settings == json::object());
	}
}

TEST_CASE("Removing a node removes its line and every route line naming it", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	SECTION("noise and both its routes") {
		const auto edited = candidate(model.removeNode("noise"));
		std::string expected = replaced(recordExample, "node white noise;\n", "");
		expected = replaced(expected, "route noise:MONO rec:FL;\n", "");
		expected = replaced(expected, "route noise:MONO desk:playback_1;\n", "");
		CHECK(edited.text() == expected);
		REQUIRE(edited.read().wanted.has_value());
		CHECK(edited.read().wanted->routes.empty());
		CHECK(edited.read().wanted->nodes.size() == 3);
	}
	SECTION("a node made by a snippet used twice, with the text unchanged") {
		const auto twice = f.load(twiceUsed);
		CHECK(sharedAt(twice.removeNode("t2")) == control::TextPlace{4, 5});
		CHECK(twice.text() == twiceUsed);
	}
}

TEST_CASE("Added nodes and routes are appended; an edit that changes nothing gives nothing", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	SECTION("a file node with its settings") {
		const auto edited = candidate(model.addNode({"tape", "file", {{"path", "t.wav"}}}));
		CHECK(edited.text() == recordExample + "node file tape { path \"t.wav\"; }\n");
		CHECK(nodeIn(edited, "tape").settings == json{{"path", "t.wav"}});
	}
	SECTION("a white node with none") {
		const auto edited = candidate(model.addNode({"n2", "white", json::object()}));
		CHECK(edited.text() == recordExample + "node white n2;\n");
		CHECK(nodeIn(edited, "n2").type == "white");
	}
	SECTION("a route") {
		const auto edited = candidate(model.addRoute({{"noise", "MONO"}, {"rec", "FR"}}));
		CHECK(edited.text() == recordExample + "route noise:MONO rec:FR;\n");
		REQUIRE(edited.read().wanted.has_value());
		CHECK(edited.read().wanted->routes.size() == 3);
	}
	SECTION("a removed route") {
		const auto edited = candidate(model.removeRoute({{"noise", "MONO"}, {"rec", "FL"}}));
		CHECK(edited.text() == replaced(recordExample, "route noise:MONO rec:FL;\n", ""));
	}
	SECTION("nothing changes") {
		CHECK(model.addNode({"rec", "file", {{"path", "take1.wav"}}}).value() == std::nullopt);
		CHECK(model.addRoute({{"noise", "MONO"}, {"rec", "FL"}}).value() == std::nullopt);
		CHECK(model.updateNode("desk", {{"card", "MOTU"}, {"playback", 2}}).value() == std::nullopt);
	}
	SECTION("a node of a held name with other settings is still written: the plan refuses it") {
		const auto edited = candidate(model.addNode({"rec", "file", {{"path", "other.wav"}}}));
		CHECK(edited.text() == recordExample + "node file rec { path \"other.wav\"; }\n");
	}
}

TEST_CASE("Unknown items and values that cannot be written are refused", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	CHECK(commandCode(model.updateNode("ghost", json::object())) == "unknown-node");
	CHECK(commandCode(model.removeNode("ghost")) == "unknown-node");
	CHECK(commandCode(model.removeRoute({{"noise", "MONO"}, {"rec", "FR"}})) == "unknown-route");
	CHECK(commandDetail(model.updateNode("desk", {{"card", "MOTU"}, {"playback", "two"}})) ==
	      "invalid-settings desk: playback: expected a whole number");
	CHECK(commandDetail(model.updateNode("rec", {{"path", 3}})) == "invalid-settings rec: path: expected text");
	CHECK(commandDetail(model.updateNode("rec", {{"path", "a.wav"}, {"gains", json::array({1, 2})}})) ==
	      "invalid-settings rec: gains: cannot be written");
	CHECK(commandDetail(model.addNode({"n3", "pink", {{"hue", 0.5}}})) == "invalid-settings n3: hue: cannot be written");

	SECTION("an undeclared string key is written and read back") {
		const auto edited = candidate(model.updateNode("rec", {{"path", "take1.wav"}, {"label", "front"}}));
		CHECK(nodeIn(edited, "rec").settings == json{{"path", "take1.wav"}, {"label", "front"}});
		const auto pink = candidate(model.addNode({"n3", "pink", {{"hue", "red"}, {"depth", 3}}}));
		CHECK(nodeIn(pink, "n3").settings == json{{"depth", "3"}, {"hue", "red"}});   // read back as text: undeclared
	}
}

TEST_CASE("Names that are not bare words are quoted, and text is never read as a reference", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);

	const auto spaced = candidate(model.addNode({"my tape", "file", {{"path", "$x"}}}));
	CHECK(spaced.text().ends_with("node file \"my tape\" { path \"$x\"; }\n"));
	CHECK(nodeIn(spaced, "my tape").settings == json{{"path", "$x"}});

	const auto routed = candidate(spaced.addRoute({{"noise", "MONO"}, {"my tape", "FL"}}));
	REQUIRE(routed.read().wanted.has_value());
	CHECK(routed.read().wanted->routes.back() == Route{{"noise", "MONO"}, {"my tape", "FL"}});
}

TEST_CASE("An edit never changes the model it is called on", "[graph_file_edit]") {
	const Fixture f;
	const auto model = f.load(recordExample);
	const GraphFileRead before = model.read();
	const auto same = [&] {
		CHECK(model.text() == recordExample);
		CHECK(model.read().diagnostics == before.diagnostics);
		REQUIRE(model.read().wanted.has_value());
		CHECK(model.read().wanted->nodes == before.wanted->nodes);
		CHECK(model.read().wanted->routes == before.wanted->routes);
	};
	(void)model.updateNode("rec", {{"path", "take2.wav"}});
	same();
	(void)model.updateNode("desk", {{"card", "MOTU"}, {"playback", 4}, {"capture", 2}});
	same();
	(void)model.updateNode("noise", {{"level", "low"}});
	same();
	(void)model.removeNode("noise");
	same();
	(void)model.addNode({"tape", "file", {{"path", "t.wav"}}});
	same();
	(void)model.addRoute({{"noise", "MONO"}, {"rec", "FR"}});
	same();
	(void)model.removeRoute({{"noise", "MONO"}, {"rec", "FL"}});
	same();
}

TEST_CASE("An edit on a file with errors is a programming error", "[graph_file_edit]") {
	const Fixture f;
	const auto broken = f.load("graph_frequency 48000;\nblock_size 256;\nvolume 3;\n");
	CHECK_THROWS_AS(broken.updateNode("rec", json::object()), std::logic_error);
	CHECK_THROWS_AS(broken.addRoute({{"noise", "MONO"}, {"rec", "FL"}}), std::logic_error);
}
