// The graph file model: ear.conf loaded with knobly and read with ear's directives into a wanted graph, with knobly's
// diagnostics and ear's own at their places (docs/graph-file.md#edits-at-the-source). Pure: text in, read out, against
// test types that declare their kinds.
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

/// The record's file format example (docs/graph-file.md#edits-at-the-source).
const std::string recordExample = R"(graph {
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
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
)";

const std::string formatLines = "graph {\n    rate 48khz;\n    block_size 256;\n}\n";

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

bool sameFormat(const std::optional<AudioFormat>& format, std::uint32_t rate, std::uint32_t block) {
	return format && format->rate.hz() == rate && format->maxBlock == block;
}

} // namespace

TEST_CASE("The record's example loads with no diagnostic into its wanted graph", "[graph_file_model]") {
	const Fixture f;
	const auto model = f.load(recordExample);
	const auto& read = model.read();

	CHECK(model.text() == recordExample);
	CHECK(read.diagnostics.empty());
	CHECK_FALSE(read.hasErrors());
	CHECK(sameFormat(read.format, 48000, 256));
	REQUIRE(read.wanted.has_value());
	CHECK(read.wanted->graphFrequency.hz() == 48000);
	CHECK(read.wanted->blockSize == 256);
	CHECK(read.wanted->nodes == std::vector<NodeSpec>{
									{"noise", "white", json::object()},
									{"rec", "file", {{"path", "take1.wav"}}},
									{"desk", "card", {{"card", "MOTU"}, {"playback", 2}}},
									{"porch", "card", {{"card", "USB2"}, {"playback", 2}}},
								});
	CHECK(read.wanted->nodes[2].settings["playback"].is_number_integer()); // its declared kind, not text
	CHECK(read.wanted->routes == std::vector<Route>{{{"noise", "noise.mono"}, {"rec", "record.left"}},
	                                                {{"noise", "noise.mono"}, {"desk", "play.1"}}});
	REQUIRE(read.placements.nodes.size() == 4);
	CHECK(read.placements.nodes[1] == control::TextPlace{11, 1});
	CHECK(read.placements.nodes[2] == control::TextPlace{12, 1});
	REQUIRE(read.placements.routes.size() == 2);
	CHECK(read.placements.routes[1] == control::TextPlace{15, 1});
}

TEST_CASE("Each of ear's codes is placed where the problem is written, and any error leaves no "
          "wanted graph",
          "[graph_file_model]") {
	const Fixture f;

	SECTION("a directive ear does not take") {
		const auto model = f.load(formatLines + "volume 3;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/unknown-directive", 5, 1}});
		CHECK_FALSE(model.read().wanted.has_value());
		CHECK(sameFormat(model.read().format, 48000, 256));
	}
	SECTION("a route end that is not <node>:<port>") {
		const auto model = f.load(formatLines + "node white noise;\nroute noise rec:record.left;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 6, 7}});
		CHECK_FALSE(model.read().wanted.has_value());
		const auto empty =
			f.load(formatLines + "node white noise;\nroute \":noise.mono\" \"rec:\";\n"); // an empty part
		CHECK(placed(empty.read()) == std::vector<Placed>{{"ear/bad-directive", 6, 7}, {"ear/bad-directive", 6, 21}});
	}
	SECTION("a node without its name") {
		const auto model = f.load(formatLines + "node white;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 5, 1}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a setting given twice, and a setting with a block") {
		const auto model =
			f.load(formatLines + "node file rec { path \"a\"; path \"b\"; }\nnode file r2 { path { x; } }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 5, 27}, {"ear/bad-directive", 6, 16}});
	}
	SECTION("a word on a whole-number setting gets knobly's integer reader code at the value") {
		const auto model = f.load(formatLines + "node card desk { card MOTU; playback two; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E4005", 5, 38}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a typo in a node line with both format lines fine: a format, but no wanted graph") {
		const auto model = f.load(formatLines + "node white \"noise;\n");
		const auto& read = model.read();
		REQUIRE_FALSE(read.diagnostics.empty());
		CHECK(read.diagnostics[0].code.starts_with("E"));
		CHECK(read.diagnostics[0].place.line == 5);
		CHECK(read.hasErrors());
		CHECK(sameFormat(read.format, 48000, 256));
		CHECK_FALSE(read.wanted.has_value());
		CHECK(read.placements.nodes.empty());
	}
	SECTION("a problem on a graph line takes the format away") {
		const auto model = f.load("graph {\n    rate 48khz }\n    block_size 256;\n}\nnode white noise;\n");
		CHECK(model.read().hasErrors());
		CHECK_FALSE(model.read().format.has_value());
	}
}

TEST_CASE("A missing or unreadable ear.conf is one error and an empty text", "[graph_file_model]") {
	const Fixture f;

	SECTION("missing") {
		const auto model = GraphFileModel::load(DiskText{std::nullopt}, f.catalogue);
		CHECK(model.text().empty());
		CHECK(model.read().diagnostics ==
		      std::vector<control::Diagnostic>{
				  {control::Severity::error, "ear/missing-file", "ear.conf is not there", control::TextPlace{1, 1}}});
		CHECK_FALSE(model.read().format.has_value());
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("unreadable") {
		const auto model = GraphFileModel::load(DiskText{std::unexpected(std::string("x"))}, f.catalogue);
		CHECK(model.text().empty());
		REQUIRE(model.read().diagnostics.size() == 1);
		const auto& only = model.read().diagnostics[0];
		CHECK(only.code == "ear/unreadable-file");
		CHECK(only.message == "ear.conf cannot be read: x");
		CHECK(only.place == control::TextPlace{1, 1});
		CHECK_FALSE(model.read().wanted.has_value());
	}
}

TEST_CASE("A text over 1 MiB is one error and an empty text, and one at 1 MiB loads", "[graph_file_model]") {
	const Fixture f;
	const std::string head = formatLines + "#";
	const std::string atMost = head + std::string(maxDocumentBytes - head.size() - 1, 'x') + "\n";
	REQUIRE(atMost.size() == maxDocumentBytes);
	CHECK(GraphFileModel::load(DiskText{atMost}, f.catalogue).read().diagnostics.empty());

	const auto model = GraphFileModel::load(DiskText{atMost + "\n"}, f.catalogue);
	CHECK(model.text().empty());
	CHECK(model.read().diagnostics ==
	      std::vector<control::Diagnostic>{{control::Severity::error, "ear/too-large",
	                                        "ear.conf would be larger than 1048576 bytes, the most ear reads",
	                                        control::TextPlace{1, 1}}});
	CHECK_FALSE(model.read().wanted.has_value());
}

TEST_CASE("@include is refused with ear's own error, and another unknown @name keeps knobly's", "[graph_file_model]") {
	const Fixture f;

	SECTION("@include") {
		const auto model = f.load(formatLines + "@include \"x.conf\";\n");
		const auto& read = model.read();
		REQUIRE(read.diagnostics.size() == 1);
		CHECK(read.diagnostics[0].code == "ear/include-unsupported");
		CHECK(read.diagnostics[0].message == "@include is not supported yet: write the whole graph in ear.conf");
		CHECK(read.diagnostics[0].place == control::TextPlace{5, 1});
		CHECK_FALSE(read.wanted.has_value());
	}
	SECTION("@frob") {
		const auto model = f.load(formatLines + "@frob 1;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E3008", 5, 1}});
	}
}

TEST_CASE("A text nested past knobly's limit loads with knobly's nesting error", "[graph_file_model]") {
	const Fixture f;
	std::string deep = formatLines + "node white noise ";
	for (int i = 0; i < 120; ++i) deep += "{ a ";
	deep += ";";
	for (int i = 0; i < 120; ++i) deep += "}";
	deep += "\n";

	const auto model = f.load(deep);
	const auto& read = model.read();
	REQUIRE_FALSE(read.diagnostics.empty());
	CHECK(read.diagnostics[0].code == "E2010");
	CHECK(read.diagnostics[0].place.line == 5);
	CHECK_FALSE(read.wanted.has_value());
}

TEST_CASE("A model moves, and keeps its text and read", "[graph_file_model]") {
	const Fixture f;
	auto model = f.load(recordExample);
	GraphFileModel moved = std::move(model);
	CHECK(moved.text() == recordExample);
	CHECK(moved.read().wanted.has_value());
}

namespace {

using Said = std::tuple<std::string, std::string, std::uint32_t, std::uint32_t>; // code, message, line, column

std::vector<Said> said(const GraphFileRead& read) {
	std::vector<Said> all;
	for (const auto& d : read.diagnostics) all.emplace_back(d.code, d.message, d.place.line, d.place.column);
	return all;
}

} // namespace

TEST_CASE("The graph block gives the format: rate with its unit and block_size, written any way knobly allows",
          "[graph_file_model][graph-block]") {
	const Fixture f;
	SECTION("on one line, with a decimal rate in any case") {
		const auto model = f.load("graph { rate 44.1KHz; block_size 512; }\nnode white noise;\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(sameFormat(model.read().format, 44100, 512));
		REQUIRE(model.read().wanted.has_value());
		CHECK(model.read().wanted->graphFrequency.hz() == 44100);
		CHECK(model.read().wanted->blockSize == 512);
	}
	SECTION("in hertz, the keys in either order") {
		const auto model = f.load("graph { block_size 128; rate 96000hz; }\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(sameFormat(model.read().format, 96000, 128));
	}
	SECTION("through a variable and a snippet, as in a node's block") {
		const auto model =
			f.load("@set $r 48khz;\n@snippet frames $n { block_size $n; }\ngraph { rate $r; @use frames 256; }\n");
		CHECK(model.read().diagnostics.empty());
		CHECK(sameFormat(model.read().format, 48000, 256));
	}
	SECTION("at both ends of its range") {
		CHECK(sameFormat(f.load("graph { rate 8khz; block_size 1; }\n").read().format, 8000, 1));
		CHECK(sameFormat(f.load("graph { rate 768khz; block_size 4294967295; }\n").read().format, 768000, 4294967295));
	}
}

TEST_CASE("Each problem with the graph block is placed and said", "[graph_file_model][graph-block]") {
	const Fixture f;
	// Whether the read still has a format, after checking what it says and that no graph is wanted.
	const auto saysOnly = [&](const std::string& text, const std::vector<Said>& expected) {
		INFO(text);
		const auto model = f.load(text);
		CHECK(said(model.read()) == expected);
		CHECK_FALSE(model.read().wanted.has_value());
		return model.read().format.has_value();
	};
	const std::string example = "graph { rate 48khz; block_size 256; }";
	SECTION("no graph block") {
		CHECK_FALSE(
			saysOnly("node white noise;\n", {{"ear/missing-directive", "graph is missing: write " + example, 1, 1}}));
	}
	SECTION("a second graph block") {
		CHECK(saysOnly(formatLines + "graph { rate 44.1khz; block_size 512; }\n",
		               {{"ear/bad-directive", "graph is given twice", 5, 1}}));
	}
	SECTION("a value, or no block") {
		CHECK_FALSE(saysOnly("graph 48khz { rate 48khz; block_size 256; }\n",
		                     {{"ear/bad-directive", "graph takes no value and a block: " + example, 1, 1}}));
		CHECK_FALSE(
			saysOnly("graph;\n", {{"ear/bad-directive", "graph takes no value and a block: " + example, 1, 1}}));
	}
	SECTION("an unknown key") {
		CHECK(saysOnly("graph { rate 48khz; block_size 256; clock card; }\n",
		               {{"ear/bad-directive", "graph has no setting clock: it takes rate and block_size", 1, 37}}));
		CHECK(saysOnly("graph { rate 48khz; block_size 256; disabled true; }\n",
		               {{"ear/bad-directive", "graph has no setting disabled: it takes rate and block_size", 1, 37}}));
	}
	SECTION("a key twice") {
		CHECK(saysOnly("graph { rate 48khz; rate 44.1khz; block_size 256; }\n",
		               {{"ear/bad-directive", "graph's rate is given twice", 1, 21}}));
		CHECK(saysOnly("graph { rate 48khz; block_size 256; block_size 512; }\n",
		               {{"ear/bad-directive", "graph's block_size is given twice", 1, 37}}));
	}
	SECTION("a key missing") {
		CHECK_FALSE(
			saysOnly("graph { block_size 256; }\n",
		             {{"ear/missing-directive", "graph is missing rate: write rate 48khz; inside graph { }", 1, 1}}));
		CHECK_FALSE(saysOnly(
			"graph { rate 48khz; }\n",
			{{"ear/missing-directive", "graph is missing block_size: write block_size 256; inside graph { }", 1, 1}}));
	}
	SECTION("a rate with no unit, a bad unit, not whole, out of range, or its unit apart") {
		CHECK_FALSE(saysOnly("graph { rate 48000; block_size 256; }\n",
		                     {{"ear/bad-rate", "rate: 48000 has no unit: write 48000hz or 48khz", 1, 14}}));
		CHECK_FALSE(saysOnly("graph { rate 48mhz; block_size 256; }\n",
		                     {{"ear/bad-rate",
		                       "rate: 48mhz is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or "
		                       "48000hz",
		                       1, 14}}));
		CHECK_FALSE(saysOnly("graph { rate 44.1003khz; block_size 256; }\n",
		                     {{"ear/bad-rate", "rate: 44.1003khz is not a whole number of hertz", 1, 14}}));
		CHECK_FALSE(saysOnly("graph { rate 4khz; block_size 256; }\n",
		                     {{"ear/bad-rate", "rate: 4khz is outside 8khz to 768khz", 1, 14}}));
		CHECK_FALSE(saysOnly("graph { rate 769khz; block_size 256; }\n",
		                     {{"ear/bad-rate", "rate: 769khz is outside 8khz to 768khz", 1, 14}}));
		CHECK_FALSE(
			saysOnly("graph { rate 48 khz; block_size 256; }\n",
		             {{"ear/bad-rate", "rate takes one value: write the unit with no space, such as 48khz", 1, 9}}));
	}
	SECTION("a quoted rate is knobly's wrong kind, as a quoted number is") {
		const auto model = f.load("graph { rate \"48khz\"; block_size 256; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E4002", 1, 14}});
		CHECK_FALSE(model.read().format.has_value());
	}
	SECTION("a bad block_size") {
		CHECK_FALSE(saysOnly("graph { rate 48khz; block_size 0; }\n",
		                     {{"ear/bad-directive", "block_size takes a whole number from 1 to 4294967295", 1, 32}}));
		CHECK_FALSE(saysOnly("graph { rate 48khz; block_size 256 512; }\n",
		                     {{"ear/bad-directive", "block_size takes one value and no block", 1, 21}}));
	}
}

TEST_CASE("An old file's graph_frequency and block_size are refused with what to write, from its own values",
          "[graph_file_model][graph-block]") {
	const Fixture f;
	SECTION("both, with their values: no missing-directive besides") {
		const auto model = f.load("graph_frequency 44100;\nblock_size 512;\nnode white noise;\n");
		const std::string hint = "graph settings moved: write graph { rate 44.1khz; block_size 512; }";
		CHECK(said(model.read()) ==
		      std::vector<Said>{{"ear/unknown-directive", "graph_frequency is no longer a directive: " + hint, 1, 1},
		                        {"ear/unknown-directive", "block_size is no longer a directive: " + hint, 2, 1}});
		CHECK_FALSE(model.read().format.has_value());
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("one, or a value that does not read: the example's values") {
		const auto model = f.load("graph_frequency many;\n");
		CHECK(said(model.read()) ==
		      std::vector<Said>{{"ear/unknown-directive",
		                         "graph_frequency is no longer a directive: graph settings moved: "
		                         "write graph { rate 48khz; block_size 256; }",
		                         1, 1}});
	}
	SECTION("beside a graph block: still refused") {
		const auto model = f.load(formatLines + "block_size 512;\n");
		CHECK(said(model.read()) == std::vector<Said>{{"ear/unknown-directive",
		                                               "block_size is no longer a directive: graph settings moved: "
		                                               "write graph { rate 48khz; block_size 512; }",
		                                               5, 1}});
		CHECK(sameFormat(model.read().format, 48000, 256));
	}
	SECTION("another unknown directive names the three") {
		const auto model = f.load(formatLines + "volume 3;\n");
		CHECK(said(model.read()) ==
		      std::vector<Said>{{"ear/unknown-directive",
		                         "volume is not one of ear's directives: ear.conf takes graph, node and route", 5, 1}});
	}
}

TEST_CASE("A rate setting is read with its unit into a JSON number of Hz, and its range is the plan's",
          "[graph_file_model][graph-block]") {
	const Fixture f;
	SECTION("written in khz or hz, through a variable") {
		const auto model = f.load(formatLines + "@set $r 96khz;\nnode rtp a { rate 44.1khz; }\n"
		                                        "node rtp b { rate 48000HZ; }\nnode rtp c { rate $r; }\n");
		CHECK(model.read().diagnostics.empty());
		REQUIRE(model.read().wanted.has_value());
		const auto& nodes = model.read().wanted->nodes;
		CHECK(nodes[0].settings == json{{"rate", 44100}});
		CHECK(nodes[0].settings["rate"].is_number_integer());
		CHECK(nodes[1].settings == json{{"rate", 48000}});
		CHECK(nodes[2].settings == json{{"rate", 96000}});
	}
	SECTION("out of the row's range: read, for the plan to refuse") {
		const auto model = f.load(formatLines + "node rtp a { rate 4khz; }\n");
		CHECK(model.read().diagnostics.empty());
		REQUIRE(model.read().wanted.has_value());
		CHECK(model.read().wanted->nodes[0].settings == json{{"rate", 4000}});
	}
	SECTION("a bare number, a bad unit or not whole: ear/bad-rate at the value") {
		const auto model = f.load(formatLines + "node rtp a { rate 48000; }\nnode rtp b { rate 44.1003khz; }\n");
		CHECK(said(model.read()) ==
		      std::vector<Said>{{"ear/bad-rate", "rate: 48000 has no unit: write 48000hz or 48khz", 5, 19},
		                        {"ear/bad-rate", "rate: 44.1003khz is not a whole number of hertz", 6, 19}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
}
