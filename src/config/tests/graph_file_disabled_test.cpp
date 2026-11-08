// The framework's reserved key `disabled` in ear.conf (docs/design/node-lifecycle.md section 8; node-framework.md 6.3,
// D-F8 to D-F12, R29, R34): read beside a node's settings and never in them, every refusal with its code and place,
// and node.disable and node.enable editing only the node's own line, with knobly's layout pinned first (8.3). Pure:
// text in, read and candidates out, against the test types.
#include <string>
#include <tuple>
#include <vector>

#include "catch2/catch.hpp"
#include "graph_file_model.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using json = nlohmann::json;

namespace {

const std::string formatLines = "graph_frequency 48000;\nblock_size 256;\n";

struct Fixture {
	NodeCatalogue catalogue{test::testTypes()};
	GraphFileModel load(const std::string& text) const { return GraphFileModel::load(DiskText{text}, catalogue); }
};

using Placed = std::tuple<std::string, std::uint32_t, std::uint32_t>; // code, line, column

std::vector<Placed> placed(const GraphFileRead& read) {
	std::vector<Placed> all;
	for (const auto& d : read.diagnostics) all.emplace_back(d.code, d.place.line, d.place.column);
	return all;
}

const NodeSpec& nodeOf(const GraphFileModel& model, const NodeName& name) {
	REQUIRE(model.read().wanted.has_value());
	for (const auto& node : model.read().wanted->nodes)
		if (node.name == name) return node;
	FAIL("no node " << name);
	throw std::logic_error("unreachable");
}

GraphFileModel candidate(EditOutcome outcome) {
	REQUIRE(outcome.has_value());
	REQUIRE(outcome->has_value());
	return std::move(**outcome);
}

/// The text an edit gives, after checking it reads back with no diagnostic.
std::string edited(EditOutcome outcome) {
	const GraphFileModel model = candidate(std::move(outcome));
	CHECK(model.read().diagnostics.empty());
	return model.text();
}

bool unchanged(const EditOutcome& outcome) { return outcome.has_value() && !outcome->has_value(); }

std::optional<control::TextPlace> sharedAt(const EditOutcome& outcome) {
	REQUIRE_FALSE(outcome.has_value());
	const auto* file = std::get_if<control::FileRefusal>(&outcome.error());
	if (file == nullptr) return std::nullopt;
	const auto* shared = std::get_if<control::SharedDefinition>(file);
	if (shared == nullptr) return std::nullopt;
	return shared->place;
}

std::string invalidSettingsDetail(const EditOutcome& outcome) {
	REQUIRE_FALSE(outcome.has_value());
	const auto* error = std::get_if<CommandError>(&outcome.error());
	REQUIRE(error != nullptr);
	const auto* invalid = std::get_if<InvalidSettings>(error);
	REQUIRE(invalid != nullptr);
	return invalid->detail;
}

} // namespace

// 8.3: what knobly's insert_statements gives at index 0 of each kind of block, before anything relies on it.
TEST_CASE("Disable's insert into a one-line block, a block that starts with @use or a comment, and a node with none "
          "keeps every comment",
          "[graph_file_disabled]") {
	const Fixture f;
	const auto disable = [&](const std::string& nodes, const NodeName& name) {
		return edited(f.load(formatLines + nodes).disableNode(name));
	};

	SECTION("a multi-line block") {
		CHECK(disable("node card porch {   # the porch\n    card USB2;\n    playback 2; # stereo\n}\n", "porch") ==
		      formatLines + "node card porch {   # the porch\n    disabled true;\n    card USB2;\n    playback 2; # "
		                    "stereo\n}\n");
	}
	SECTION("a one-line block") {
		CHECK(disable("node card porch { card USB2; playback 2; } # outside\n", "porch") ==
		      formatLines + "node card porch { disabled true; card USB2; playback 2; } # outside\n");
	}
	SECTION("a block that starts with @use") {
		const std::string snippet = "@snippet out $id { card $id; playback 2; }\n";
		CHECK(disable(snippet + "node card porch { @use out USB2; }\n", "porch") ==
		      formatLines + snippet + "node card porch { disabled true; @use out USB2; }\n");
	}
	SECTION("a block whose first statement has a comment of its own above it") {
		CHECK(disable("node file rec {\n    # where it goes\n    path \"a.wav\";\n}\n", "rec") ==
		      formatLines + "node file rec {\n    disabled true;\n    # where it goes\n    path \"a.wav\";\n}\n");
	}
	SECTION("a node with no block, a comment after it") {
		CHECK(disable("node white noise; # the hiss\n", "noise") ==
		      formatLines + "node white noise { disabled true; } # the hiss\n");
	}
	SECTION("an empty block, on one line and on two") {
		CHECK(disable("node white noise { }\n", "noise") == formatLines + "node white noise { disabled true; }\n");
		CHECK(disable("node white noise {}\n", "noise") == formatLines + "node white noise { disabled true; }\n");
		CHECK(disable("node white noise {\n}\n", "noise") ==
		      formatLines + "node white noise {\n    disabled true;\n}\n");
	}
	SECTION("a one-line block nested in a snippet's node") {
		const std::string text = "@snippet one $n {\n    node white $n { }\n}\n@use one solo;\n";
		CHECK(disable(text, "solo") == formatLines + "@snippet one $n {\n    node white $n { disabled true; }\n}\n"
		                                             "@use one solo;\n");
	}
}

TEST_CASE("disabled is read as true or false beside the settings, from the node's line, a variable or a snippet",
          "[graph_file_disabled]") {
	const Fixture f;

	SECTION("true, false and absent") {
		const auto model = f.load(formatLines + "node card a { disabled true; card USB2; playback 2; }\n"
		                                        "node card b { disabled false; card USB2; playback 2; }\n"
		                                        "node white c;\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(nodeOf(model, "a") == NodeSpec{"a", "card", {{"card", "USB2"}, {"playback", 2}}, true});
		CHECK(nodeOf(model, "b") == NodeSpec{"b", "card", {{"card", "USB2"}, {"playback", 2}}, false});
		CHECK(nodeOf(model, "c") == NodeSpec{"c", "white", json::object(), false});
	}
	SECTION("a node with no settings but disabled") {
		const auto model = f.load(formatLines + "node white noise { disabled true; }\n");
		CHECK(nodeOf(model, "noise") == NodeSpec{"noise", "white", json::object(), true});
	}
	SECTION("from a @set variable") {
		const auto model = f.load(formatLines + "@set $quiet true;\nnode white a { disabled $quiet; }\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(nodeOf(model, "a").disabled);
		CHECK(nodeOf(model, "a").settings == json::object());
	}
	SECTION("from a @snippet") {
		const auto model = f.load(formatLines + "@snippet off { disabled true; }\nnode white a { @use off; }\n"
		                                        "node white b;\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(nodeOf(model, "a").disabled);
		CHECK_FALSE(nodeOf(model, "b").disabled);
	}
}

TEST_CASE("Every refusal of disabled has its code and place, and leaves no wanted graph", "[graph_file_disabled]") {
	const Fixture f;
	const auto messageOf = [](const GraphFileModel& model) {
		REQUIRE(model.read().diagnostics.size() == 1);
		return model.read().diagnostics[0].message;
	};

	SECTION("a bare disabled") {
		const auto model = f.load(formatLines + "node white noise { disabled; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 20}});
		CHECK(messageOf(model) == "disabled takes true or false: write disabled true;");
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("two values") {
		const auto model = f.load(formatLines + "node white noise { disabled true false; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 20}});
		CHECK(messageOf(model) == "disabled takes true or false: write disabled true;");
	}
	SECTION("a block") {
		const auto model = f.load(formatLines + "node white noise { disabled true { x; } }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 20}});
		CHECK(messageOf(model) == "disabled takes true or false: write disabled true;");
	}
	SECTION("given twice") {
		const auto model = f.load(formatLines + "node white noise { disabled false; disabled true; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 36}});
		CHECK(messageOf(model) == "setting disabled is given twice");
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a word other than true or false: knobly's E4006 on the value") {
		const auto model = f.load(formatLines + "node white noise { disabled yes; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E4006", 3, 29}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a quoted value or a list: knobly's E4002 on the value") {
		const auto quoted = f.load(formatLines + "node white noise { disabled \"true\"; }\n");
		CHECK(placed(quoted.read()) == std::vector<Placed>{{"E4002", 3, 29}});
		const auto list = f.load(formatLines + "node white noise { disabled [true]; }\n");
		CHECK(placed(list.read()) == std::vector<Placed>{{"E4002", 3, 29}});
	}
	SECTION("the old spellings on the node's line, with the hint") {
		for (const std::string line : {"node card porch disabled { card USB2; playback 2; }\n",
		                               "node disabled card porch { card USB2; playback 2; }\n"}) {
			const auto model = f.load(formatLines + line);
			CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 1}});
			CHECK(messageOf(model) == "node takes a type and a name: node <type> <name> [{ <setting> <value>; ... }]; "
			                          "to disable a node, write disabled true; inside its braces");
		}
	}
	SECTION("three values without the word keep the plain message") {
		const auto model = f.load(formatLines + "node card porch extra;\n");
		CHECK(messageOf(model) == "node takes a type and a name: node <type> <name> [{ <setting> <value>; ... }]");
	}
}

TEST_CASE("node.disable edits only the node's own line, or puts disabled true first in its block",
          "[graph_file_disabled]") {
	const Fixture f;

	SECTION("a node with no block gets one") {
		const auto model = f.load(formatLines + "node white noise;\n");
		const auto after = candidate(model.disableNode("noise"));
		CHECK(after.text() == formatLines + "node white noise { disabled true; }\n");
		CHECK(nodeOf(after, "noise").disabled);
	}
	SECTION("disabled false; becomes disabled true;, its comment kept") {
		const auto model = f.load(formatLines + "node white noise { disabled false; } # off for now\n");
		CHECK(edited(model.disableNode("noise")) ==
		      formatLines + "node white noise { disabled true; } # off for now\n");
	}
	SECTION("disabled $quiet; becomes disabled true;: the @set and its other users stay as they are (R34)") {
		const std::string text = formatLines + "@set $quiet false;\n"
		                                       "node white a { disabled $quiet; }\n"
		                                       "node white b { disabled $quiet; }\n";
		const auto after = candidate(f.load(text).disableNode("a"));
		CHECK(after.text() == formatLines + "@set $quiet false;\n"
		                                    "node white a { disabled true; }\n"
		                                    "node white b { disabled $quiet; }\n");
		CHECK(nodeOf(after, "a").disabled);
		CHECK_FALSE(nodeOf(after, "b").disabled);
	}
	SECTION("a snippet used by this node alone is this node's own line") {
		const std::string text = formatLines + "@snippet on { disabled false; }\nnode white a { @use on; }\n";
		CHECK(edited(f.load(text).disableNode("a")) ==
		      formatLines + "@snippet on { disabled true; }\nnode white a { @use on; }\n");
	}
	SECTION("refused shared-definition when the line sits in a snippet another node uses") {
		const std::string text =
			formatLines + "@snippet on { disabled false; }\nnode white a { @use on; }\nnode white b { @use on; }\n";
		CHECK(sharedAt(f.load(text).disableNode("a")) == control::TextPlace{3, 15});
	}
	SECTION("refused shared-definition when the node's own line makes two nodes") {
		const std::string text = formatLines + "@snippet take $n { node white $n; }\n@use take t1;\n@use take t2;\n";
		CHECK(sharedAt(f.load(text).disableNode("t1")) == control::TextPlace{3, 20});
	}
	SECTION("already disabled: unchanged, from any source") {
		CHECK(unchanged(f.load(formatLines + "node white a { disabled true; }\n").disableNode("a")));
		CHECK(unchanged(f.load(formatLines + "@set $q true;\nnode white a { disabled $q; }\n").disableNode("a")));
	}
	SECTION("an unknown node") {
		const auto outcome = f.load(formatLines + "node white a;\n").disableNode("b");
		REQUIRE_FALSE(outcome.has_value());
		const auto* error = std::get_if<CommandError>(&outcome.error());
		REQUIRE(error != nullptr);
		CHECK(std::holds_alternative<UnknownNode>(*error));
	}
}

TEST_CASE("node.enable removes the node's own line, and drops a block it empties keeping a comment outside it",
          "[graph_file_disabled]") {
	const Fixture f;

	SECTION("the line and the block it empties go") {
		const auto model = f.load(formatLines + "node white noise { disabled true; } # the hiss\n");
		const auto after = candidate(model.enableNode("noise"));
		CHECK(after.text() == formatLines + "node white noise; # the hiss\n");
		CHECK_FALSE(nodeOf(after, "noise").disabled);
	}
	SECTION("disable then enable gives the text back") {
		const std::string text = formatLines + "node white noise;\n";
		const auto disabled = candidate(f.load(text).disableNode("noise"));
		CHECK(candidate(disabled.enableNode("noise")).text() == text);
	}
	SECTION("a block with settings keeps them, on lines of their own") {
		CHECK(edited(f.load(formatLines + "node card porch {\n    disabled true;\n    card USB2;\n    playback 2;\n}\n")
		                 .enableNode("porch")) ==
		      formatLines + "node card porch {\n    card USB2;\n    playback 2;\n}\n");
		CHECK(edited(f.load(formatLines + "node card porch { disabled true; card USB2; playback 2; }\n")
		                 .enableNode("porch")) == formatLines + "node card porch { card USB2; playback 2; }\n");
	}
	SECTION("a block left holding only a comment is kept") {
		CHECK(edited(f.load(formatLines + "node white noise { # kept\n    disabled true;\n}\n").enableNode("noise")) ==
		      formatLines + "node white noise { # kept\n}\n");
	}
	SECTION("a comment the line owns (on the lines above it) goes with it, as any removed statement's does") {
		CHECK(edited(f.load(formatLines + "node white noise {\n    # off for now\n    disabled true;\n}\n")
		                 .enableNode("noise")) == formatLines + "node white noise;\n");
	}
	SECTION("disabled $quiet; goes, and its @set stays") {
		const std::string text = formatLines + "@set $quiet true;\n"
		                                       "node white a { disabled $quiet; }\n"
		                                       "node white b { disabled $quiet; }\n";
		const auto after = candidate(f.load(text).enableNode("a"));
		CHECK(after.text() == formatLines + "@set $quiet true;\nnode white a;\nnode white b { disabled $quiet; }\n");
		CHECK_FALSE(nodeOf(after, "a").disabled);
		CHECK(nodeOf(after, "b").disabled);
	}
	SECTION("refused shared-definition when the line sits in a snippet another node uses") {
		const std::string text =
			formatLines + "@snippet off { disabled true; }\nnode white a { @use off; }\nnode white b { @use off; }\n";
		CHECK(sharedAt(f.load(text).enableNode("b")) == control::TextPlace{3, 16});
	}
	SECTION("not disabled, or disabled false;: unchanged") {
		CHECK(unchanged(f.load(formatLines + "node white a;\n").enableNode("a")));
		CHECK(unchanged(f.load(formatLines + "node white a { disabled false; }\n").enableNode("a")));
	}
}

TEST_CASE("node.update leaves a disabled line alone", "[graph_file_disabled]") {
	const Fixture f;
	const auto model = f.load(formatLines + "node card porch { disabled true; card USB2; playback 2; }\n");
	SECTION("a removed and a changed key") {
		const auto after = candidate(model.updateNode("porch", {{"card", "MOTU"}}));
		CHECK(after.text() == formatLines + "node card porch { disabled true; card \"MOTU\"; }\n");
		CHECK(nodeOf(after, "porch") == NodeSpec{"porch", "card", {{"card", "MOTU"}}, true});
	}
	SECTION("all keys removed") {
		const auto after = candidate(model.updateNode("porch", json::object()));
		CHECK(after.text() == formatLines + "node card porch { disabled true; }\n");
		CHECK(nodeOf(after, "porch").disabled);
	}
}

TEST_CASE("node.update with a disabled key in its settings is refused invalid-settings naming node.disable",
          "[graph_file_disabled]") {
	const Fixture f;
	const auto model = f.load(formatLines + "node white noise;\n");
	const std::string detail =
		"disabled is not a setting: use node.disable and node.enable, or \"disabled\": true on node.add";
	CHECK(invalidSettingsDetail(model.updateNode("noise", {{"disabled", true}})) == detail);
	CHECK(invalidSettingsDetail(model.addNode(NodeSpec{"n2", "white", {{"disabled", true}}})) == detail);
}

TEST_CASE("node.add with disabled writes disabled true first in the block", "[graph_file_disabled]") {
	const Fixture f;
	const auto model = f.load(formatLines + "node white noise;\n");
	const auto bare = candidate(model.addNode(NodeSpec{"n2", "white", json::object(), true}));
	CHECK(bare.text() == formatLines + "node white noise;\nnode white n2 { disabled true; }\n");
	CHECK(nodeOf(bare, "n2").disabled);
	const auto card = candidate(model.addNode(NodeSpec{"p", "card", {{"card", "USB2"}, {"playback", 2}}, true}));
	CHECK(card.text() ==
	      formatLines + "node white noise;\nnode card p { disabled true; card \"USB2\"; playback 2; }\n");
	CHECK(unchanged(bare.addNode(NodeSpec{"n2", "white", json::object(), true})));
}
