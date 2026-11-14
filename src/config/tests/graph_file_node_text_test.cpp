// A node's own knobly text (docs/control-port.md#a-nodes-own-text): where each node's statement is written
// (nodeTexts), and node.setText, which replaces only that statement, checked by the file's own reader in context, with
// diagnostics placed in the text sent. Pure: text in, candidates out, against the test types.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "graph_file_model.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using json = nlohmann::json;

namespace {

/// The record example (docs/graph-file.md#an-example) with a comment owned by desk's statement.
const std::string example = R"(graph {
    rate 48khz;
    block_size 256;
}
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
# the desk's card
node card desk { @use card_out MOTU; }   # the big one
node card porch { @use card_out USB2; }
node rtp fpp { rate 44.1khz; }
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
)";

/// A snippet that makes a node, used twice, and one used once: their node lines are not any node's own.
const std::string snippetNodes = R"(graph {
    rate 48khz;
    block_size 256;
}
@snippet take $n {
    node file $n { path "a.wav"; }
}
@snippet one {
    node white solo;
}
@use take t1;
@use take t2;
@use one;
node white noise;
)";

struct Fixture {
	NodeCatalogue catalogue{test::testTypes()};
	GraphFileModel load(const std::string& text) const { return GraphFileModel::load(DiskText{text}, catalogue); }
};

/// The text from `start` to `end` (wire places: lines from 1, UTF-16 columns from 1; ASCII here).
std::string slice(const std::string& text, control::TextPlace start, control::TextPlace end) {
	const auto offset = [&](control::TextPlace place) {
		std::size_t at = 0;
		for (std::uint32_t line = 1; line < place.line; ++line) at = text.find('\n', at) + 1;
		return at + place.column - 1;
	};
	return text.substr(offset(start), offset(end) - offset(start));
}

const control::NodeText& textOf(const GraphFileModel& model, const std::string& name) {
	REQUIRE(model.read().nodeTexts.has_value());
	for (const auto& node : *model.read().nodeTexts)
		if (node.name == name) return node;
	FAIL("no node text for " << name);
	throw std::logic_error("unreachable");
}

GraphFileModel candidate(EditOutcome outcome) {
	REQUIRE(outcome.has_value());
	REQUIRE(outcome->has_value());
	return std::move(**outcome);
}

std::vector<control::Diagnostic> invalid(const EditOutcome& outcome) {
	REQUIRE_FALSE(outcome.has_value());
	const auto* file = std::get_if<control::FileRefusal>(&outcome.error());
	REQUIRE(file != nullptr);
	const auto* text = std::get_if<control::InvalidText>(file);
	REQUIRE(text != nullptr);
	return text->diagnostics;
}

const NodeSpec& nodeIn(const GraphFileModel& model, const std::string& name) {
	REQUIRE(model.read().wanted.has_value());
	for (const auto& node : model.read().wanted->nodes)
		if (node.name == name) return node;
	FAIL("no node " << name);
	throw std::logic_error("unreachable");
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
	const auto at = text.find(from);
	REQUIRE(at != std::string::npos);
	return text.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("nodeTexts say where each node's own statement is, the comments it owns included", "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);
	REQUIRE(model.read().nodeTexts.has_value());
	REQUIRE(model.read().nodeTexts->size() == 5); // parallel to the wanted nodes
	for (std::size_t i = 0; i < 5; ++i) CHECK((*model.read().nodeTexts)[i].name == model.read().wanted->nodes[i].name);

	const auto& noise = textOf(model, "noise");
	CHECK(noise == control::NodeText{"noise", {10, 1}, {10, 18}, false});
	CHECK(slice(example, noise.start, noise.end) == "node white noise;");
	CHECK(slice(example, textOf(model, "rec").start, textOf(model, "rec").end) ==
	      R"(node file rec { path "take1.wav"; })");
	// The comment line above it and the one after it on its line are desk's.
	const auto& desk = textOf(model, "desk");
	CHECK_FALSE(desk.shared);
	CHECK(slice(example, desk.start, desk.end) ==
	      "# the desk's card\nnode card desk { @use card_out MOTU; }   # the big one");
	// A rate keeps its unit: the text is as written.
	CHECK(slice(example, textOf(model, "fpp").start, textOf(model, "fpp").end) == "node rtp fpp { rate 44.1khz; }");
}

TEST_CASE("A node made in a snippet is shared, however many use it, and a file with errors has no nodeTexts",
          "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(snippetNodes);
	const auto& t1 = textOf(model, "t1");
	const auto& t2 = textOf(model, "t2");
	CHECK(t1.shared);
	CHECK(t2.shared);
	CHECK(t1.start == t2.start); // one statement makes both
	CHECK(slice(snippetNodes, t1.start, t1.end) == R"(node file $n { path "a.wav"; })");
	CHECK(textOf(model, "solo").shared); // used once, still written in a snippet
	CHECK_FALSE(textOf(model, "noise").shared);

	CHECK_FALSE(f.load(example + "@include \"x.conf\";\n").read().nodeTexts.has_value());
}

TEST_CASE("node.setText replaces only the node's statement, and every other byte is kept", "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);

	SECTION("a setting, its spelling as written") {
		const GraphFileModel after = candidate(model.setNodeText("rec", R"(node file rec { path "take2.wav"; })"));
		CHECK(after.text() == replaced(example, "take1.wav", "take2.wav"));
		CHECK(nodeIn(after, "rec").settings == json{{"path", "take2.wav"}});
	}
	SECTION("a rate with its unit") {
		const GraphFileModel after = candidate(model.setNodeText("fpp", "node rtp fpp { rate 48khz; }"));
		CHECK(after.text() == replaced(example, "rate 44.1khz;", "rate 48khz;"));
		CHECK(nodeIn(after, "fpp").settings == json{{"rate", 48000}});
	}
	SECTION("the file's @set and @snippet apply in the text, and are never changed by it") {
		const GraphFileModel after = candidate(model.setNodeText(
			"desk", "# the desk's card\nnode card desk {\n    card MOTU;\n    playback $ch;   # its own now\n}"));
		CHECK(after.text() == replaced(example, "node card desk { @use card_out MOTU; }   # the big one",
		                               "node card desk {\n    card MOTU;\n    playback $ch;   # its own now\n}"));
		CHECK(nodeIn(after, "desk").settings == json{{"card", "MOTU"}, {"playback", 2}});
		CHECK(nodeIn(after, "porch").settings == json{{"card", "USB2"}, {"playback", 2}}); // porch untouched
	}
	SECTION("disabled true; and a comment") {
		const GraphFileModel after =
			candidate(model.setNodeText("noise", "node white noise { disabled true; } # hush"));
		CHECK(after.text() == replaced(example, "node white noise;", "node white noise { disabled true; } # hush"));
		CHECK(nodeIn(after, "noise").disabled);
	}
	SECTION("the type") {
		const GraphFileModel after = candidate(model.setNodeText("noise", "node rtp noise { rate 48khz; }"));
		CHECK(nodeIn(after, "noise").type == "rtp");
	}
	SECTION("whitespace around the text is dropped") {
		const GraphFileModel after = candidate(model.setNodeText("rec", "\n  node file rec { path \"b.wav\"; }\n\n"));
		CHECK(after.text() == replaced(example, "take1.wav", "b.wav"));
	}
	SECTION("its own text again changes nothing") {
		const auto same = model.setNodeText("noise", "node white noise;\n");
		REQUIRE(same.has_value());
		CHECK_FALSE(same->has_value());
	}
	CHECK(model.text() == example); // the model it is called on never changes
}

TEST_CASE("node.setText refuses an unknown node, and a shared one at its statement", "[graph_file_node_text]") {
	const Fixture f;
	const auto unknown = f.load(example).setNodeText("nope", "node white nope;");
	REQUIRE_FALSE(unknown.has_value());
	CHECK(std::holds_alternative<CommandError>(unknown.error()));

	const auto shared = f.load(snippetNodes).setNodeText("t1", R"(node file t1 { path "b.wav"; })");
	REQUIRE_FALSE(shared.has_value());
	const auto* file = std::get_if<control::FileRefusal>(&shared.error());
	REQUIRE(file != nullptr);
	REQUIRE(std::holds_alternative<control::SharedDefinition>(*file));
	CHECK(std::get<control::SharedDefinition>(*file).place == control::TextPlace{6, 5});
	const auto once = f.load(snippetNodes).setNodeText("solo", "node white solo;");
	REQUIRE_FALSE(once.has_value());
	CHECK(std::holds_alternative<control::FileRefusal>(once.error()));
}

TEST_CASE("node.setText's diagnostics are placed in the text sent", "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);

	SECTION("ear's, on the text's second line") {
		const auto d =
			invalid(model.setNodeText("rec", "node file rec {\n    path take1.wav;\n    path take2.wav;\n}"));
		REQUIRE(d.size() == 1);
		CHECK(d[0].code == "ear/bad-directive");
		CHECK(d[0].place == control::TextPlace{3, 5});
	}
	SECTION("a bad rate, after whitespace dropped before the text") {
		const auto d = invalid(model.setNodeText("fpp", "\n  node rtp fpp { rate 44100; }"));
		REQUIRE(d.size() == 1);
		CHECK(d[0].code == "ear/bad-rate");
		INFO(d[0].message << " at " << d[0].place.line << ":" << d[0].place.column);
		CHECK(d[0].place == control::TextPlace{2, 23}); // in the text as sent, its leading line and spaces included
	}
	SECTION("knobly's, on the first line") {
		const auto d = invalid(model.setNodeText("rec", R"(node file rec { path "take2.wav" })"));
		REQUIRE_FALSE(d.empty());
		CHECK(d[0].code.starts_with("E"));
		CHECK(d[0].place.line == 1);
	}
	SECTION("one the text causes in a snippet it uses is placed at its start, saying where it is in ear.conf") {
		// desk's own card line and the snippet's card $id line both give card: the second is the snippet's (7:5).
		const auto d = invalid(model.setNodeText("desk", "node card desk { card X; @use card_out MOTU; }"));
		REQUIRE_FALSE(d.empty());
		bool elsewhere = false;
		for (const auto& diagnostic : d) {
			INFO(diagnostic.code << " " << diagnostic.message << " at " << diagnostic.place.line << ":"
			                     << diagnostic.place.column);
			if (diagnostic.message.starts_with("ear.conf ")) {
				elsewhere = true;
				CHECK(diagnostic.place == control::TextPlace{1, 1});
				CHECK(diagnostic.message == "ear.conf 7:5: setting card is given twice");
			}
		}
		CHECK(elsewhere);
	}
}

TEST_CASE("node.setText's text must be one node statement of the same name", "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);
	const auto only = [](const std::vector<control::Diagnostic>& d) {
		REQUIRE(d.size() == 1);
		return d[0];
	};
	CHECK(only(invalid(model.setNodeText("noise", "  \n"))).code == notOneNodeCode);
	CHECK(only(invalid(model.setNodeText("noise", "node white noise; node white more;"))).code == notOneNodeCode);
	CHECK(only(invalid(model.setNodeText("noise", "route noise:noise.mono rec:record.right;"))).code == notOneNodeCode);
	CHECK(only(invalid(model.setNodeText("noise", "@set $x 1;"))).code == notOneNodeCode);

	const auto renamed = only(invalid(model.setNodeText("noise", "# hiss\nnode white hiss;")));
	CHECK(renamed.code == nodeRenamedCode);
	CHECK(renamed.message == "this text edits node noise: rename it in ear.conf, where its routes are renamed with it");
	CHECK(renamed.place == control::TextPlace{2, 12});
}

TEST_CASE("node.setText's statement must end where its text ends: a { left open never takes what follows",
          "[graph_file_node_text]") {
	const Fixture f;
	// rec's block left open: what follows in the file (desk, with its own block) would be read into it.
	const std::string open = R"(node file rec { path "take2.wav";)";
	const auto d = invalid(f.load(example).setNodeText("rec", open));
	REQUIRE(d.size() == 1);
	CHECK(d[0].code == notOneNodeCode);
	CHECK(d[0].message == "the text's statement runs on past its end into ear.conf: close every { with } so the text "
	                      "is one whole node statement");
	CHECK(d[0].place == control::TextPlace{1, static_cast<std::uint32_t>(open.size() + 1)}); // at the text's end
}

TEST_CASE("node.setText writes the file's own line ending", "[graph_file_node_text]") {
	const Fixture f;
	const auto crlf = [](std::string text) {
		for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2))
			text.replace(at, 1, "\r\n");
		return text;
	};
	const auto loneNewlines = [](const std::string& text) {
		std::size_t lone = 0;
		for (std::size_t at = 0; at < text.size(); ++at) lone += text[at] == '\n' && (at == 0 || text[at - 1] != '\r');
		return lone;
	};
	const std::string windows = crlf(example);
	const GraphFileModel model = f.load(windows);
	REQUIRE(model.read().nodeTexts.has_value());
	CHECK(*model.read().nodeTexts == *f.load(example).read().nodeTexts); // the \r ends each line, after the statement

	// What a textarea gives back for desk's text, unchanged: "\n" between its lines. Nothing changes.
	const auto same =
		model.setNodeText("desk", "# the desk's card\nnode card desk { @use card_out MOTU; }   # the big one");
	REQUIRE(same.has_value());
	CHECK_FALSE(same->has_value());

	const GraphFileModel after =
		candidate(model.setNodeText("desk", "# the desk's card\nnode card desk {\n    card MOTU;\n    playback 4;\n}"));
	CHECK(loneNewlines(after.text()) == 0);
	CHECK(after.text() == crlf(replaced(example, "node card desk { @use card_out MOTU; }   # the big one",
	                                    "node card desk {\n    card MOTU;\n    playback 4;\n}")));

	// A text sent with CRLF into an LF file is written with LF.
	const GraphFileModel lf = candidate(f.load(example).setNodeText("rec", "node file rec {\r\n    path b.wav;\r\n}"));
	CHECK(lf.text().find('\r') == std::string::npos);
}

TEST_CASE("node.addText appends a node as node.add does, and keeps every other byte", "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);
	const GraphFileModel byJson = candidate(model.addNode(NodeSpec{"n2", "white", json::object()}));
	const GraphFileModel byText = candidate(model.addNodeText("node white n2;\n"));
	CHECK(byText.text() == byJson.text()); // the same place, the same layout

	// A comment it owns, a block on lines of its own and a value from the file's @set, read in context.
	const std::string added = "# a third card\nnode card attic {\n    card USB3;\n    playback $ch;\n}";
	const GraphFileModel after = candidate(model.addNodeText("\n" + added + "\n\n"));
	CHECK(after.text() == example + added + "\n");
	CHECK(nodeIn(after, "attic").settings == json{{"card", "USB3"}, {"playback", 2}});
	CHECK(model.text() == example); // the model it is called on never changes

	// A CRLF file gets CRLF.
	std::string windows = example;
	for (std::size_t at = windows.find('\n'); at != std::string::npos; at = windows.find('\n', at + 2))
		windows.replace(at, 1, "\r\n");
	const GraphFileModel crlf = candidate(f.load(windows).addNodeText("node card attic {\n    card USB3;\n}"));
	CHECK(crlf.text() == windows + "node card attic {\r\n    card USB3;\r\n}\r\n");
	// A text sent with CRLF into an LF file is written with LF.
	const GraphFileModel lf = candidate(model.addNodeText("node card attic {\r\n    card USB3;\r\n}"));
	CHECK(lf.text().find('\r') == std::string::npos);
	// On a CRLF file a diagnostic is still placed in the text as sent (with "\n").
	const auto d = invalid(f.load(windows).addNodeText("node file take {\n    path a.wav;\n    path b.wav;\n}"));
	REQUIRE(d.size() == 1);
	CHECK(d[0].place == control::TextPlace{3, 5});
	CHECK(d[0].message == "setting path is given twice");
}

TEST_CASE("node.addText's text is parsed alone, must be one node statement, and is read in context, its diagnostics "
          "placed in it",
          "[graph_file_node_text]") {
	const Fixture f;
	const GraphFileModel model = f.load(example);
	SECTION("a { left open is knobly's error in the text, never a read of what follows") {
		const auto d = invalid(model.addNodeText("node file take {\n    path \"t.wav\";\n"));
		REQUIRE_FALSE(d.empty());
		CHECK(d[0].code.starts_with("E"));
		CHECK(d[0].place.line <= 3);
	}
	SECTION("knobly's syntax error after whitespace dropped before the text") {
		const auto d = invalid(model.addNodeText("\n  node file take { path \"t.wav\" }"));
		REQUIRE_FALSE(d.empty());
		CHECK(d[0].code.starts_with("E"));
		CHECK(d[0].place.line == 2); // in the text as sent
	}
	SECTION("the reader's, on the text's third line") {
		const auto d = invalid(model.addNodeText("node file take {\n    path a.wav;\n    path b.wav;\n}"));
		REQUIRE(d.size() == 1);
		CHECK(d[0].code == "ear/bad-directive");
		CHECK(d[0].place == control::TextPlace{3, 5});
	}
	SECTION("not one node statement") {
		for (const std::string text :
		     {"  ", "node white a; node white b;", "@set $x 1;", "route noise:noise.mono rec:record.right;"}) {
			INFO(text);
			const auto d = invalid(model.addNodeText(text));
			REQUIRE(d.size() == 1);
			CHECK(d[0].code == notOneNodeCode);
		}
	}
}

TEST_CASE("node.setText on a file with errors is a programming error", "[graph_file_node_text]") {
	const Fixture f;
	CHECK_THROWS_AS(f.load(example + "oops;\n").setNodeText("noise", "node white noise;"), std::logic_error);
}
