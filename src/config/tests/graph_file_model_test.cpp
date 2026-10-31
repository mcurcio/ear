// The graph file model: ear.conf loaded with knobly and read with ear's directives into a wanted graph,
// with knobly's diagnostics and ear's own at their places (docs/design/knobly-config.md, "File
// format"; S1 criteria 2-6). Pure: text in, read out, against test types that declare their kinds.
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

const std::string formatLines = "graph_frequency 48000;\nblock_size 256;\n";

struct Fixture {
	NodeCatalogue catalogue{test::testTypes()};
	GraphFileModel load(const std::string& text) const { return GraphFileModel::load(DiskText{text}, catalogue); }
};

using Placed = std::tuple<std::string, std::uint32_t, std::uint32_t>;   // code, line, column

std::vector<Placed> placed(const GraphFileRead& read) {
	std::vector<Placed> all;
	for (const auto& d : read.diagnostics) all.emplace_back(d.code, d.place.line, d.place.column);
	return all;
}

bool sameFormat(const std::optional<AudioFormat>& format, std::uint32_t rate, std::uint32_t block) {
	return format && format->rate == rate && format->maxBlock == block;
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
	CHECK(read.wanted->graphFrequency == 48000);
	CHECK(read.wanted->blockSize == 256);
	CHECK(read.wanted->nodes == std::vector<NodeSpec>{
	                                {"noise", "white", json::object()},
	                                {"rec", "file", {{"path", "take1.wav"}}},
	                                {"desk", "card", {{"card", "MOTU"}, {"playback", 2}}},
	                                {"porch", "card", {{"card", "USB2"}, {"playback", 2}}},
	                            });
	CHECK(read.wanted->nodes[2].settings["playback"].is_number_integer());   // its declared kind, not text
	CHECK(read.wanted->routes == std::vector<Route>{{{"noise", "MONO"}, {"rec", "FL"}},
	                                                {{"noise", "MONO"}, {"desk", "playback_1"}}});
	REQUIRE(read.placements.nodes.size() == 4);
	CHECK(read.placements.nodes[1] == control::TextPlace{9, 1});
	CHECK(read.placements.nodes[2] == control::TextPlace{10, 1});
	REQUIRE(read.placements.routes.size() == 2);
	CHECK(read.placements.routes[1] == control::TextPlace{13, 1});
}

TEST_CASE("Each of ear's codes is placed where the problem is written, and any error leaves no wanted graph",
          "[graph_file_model]") {
	const Fixture f;

	SECTION("a directive ear does not take") {
		const auto model = f.load(formatLines + "volume 3;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/unknown-directive", 3, 1}});
		CHECK_FALSE(model.read().wanted.has_value());
		CHECK(sameFormat(model.read().format, 48000, 256));
	}
	SECTION("a route end that is not <node>:<port>") {
		const auto model = f.load(formatLines + "node white noise;\nroute noise rec:FL;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 4, 7}});
		CHECK_FALSE(model.read().wanted.has_value());
		const auto empty = f.load(formatLines + "node white noise;\nroute \":MONO\" \"rec:\";\n");   // an empty part
		CHECK(placed(empty.read()) == std::vector<Placed>{{"ear/bad-directive", 4, 7}, {"ear/bad-directive", 4, 15}});
	}
	SECTION("a node without its name") {
		const auto model = f.load(formatLines + "node white;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 1}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a setting given twice, and a setting with a block") {
		const auto model = f.load(formatLines + "node file rec { path \"a\"; path \"b\"; }\nnode file r2 { path { x; } }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 3, 27}, {"ear/bad-directive", 4, 16}});
	}
	SECTION("graph_frequency left out") {
		const auto model = f.load("block_size 256;\nnode white noise;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/missing-directive", 1, 1}});
		CHECK_FALSE(model.read().format.has_value());
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a format line given twice, out of range, or with two values") {
		const auto model = f.load("graph_frequency 48000;\ngraph_frequency 44100;\nblock_size 0;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"ear/bad-directive", 2, 1}, {"ear/bad-directive", 3, 12}});
		CHECK_FALSE(model.read().format.has_value());
		const auto big = f.load("graph_frequency 4294967296;\nblock_size 256 512;\n");
		CHECK(placed(big.read()) == std::vector<Placed>{{"ear/bad-directive", 1, 17}, {"ear/bad-directive", 2, 1}});
	}
	SECTION("a word on a whole-number setting gets knobly's integer reader code at the value") {
		const auto model = f.load(formatLines + "node card desk { card MOTU; playback two; }\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E4005", 3, 38}});
		CHECK_FALSE(model.read().wanted.has_value());
	}
	SECTION("a typo in a node line with both format lines fine: a format, but no wanted graph") {
		const auto model = f.load(formatLines + "node white \"noise;\n");
		const auto& read = model.read();
		REQUIRE_FALSE(read.diagnostics.empty());
		CHECK(read.diagnostics[0].code.starts_with("E"));
		CHECK(read.diagnostics[0].place.line == 3);
		CHECK(read.hasErrors());
		CHECK(sameFormat(read.format, 48000, 256));
		CHECK_FALSE(read.wanted.has_value());
		CHECK(read.placements.nodes.empty());
	}
	SECTION("a problem on a format line takes the format away") {
		const auto model = f.load("graph_frequency 48000 }\nblock_size 256;\nnode white noise;\n");
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
	const std::string head = "graph_frequency 48000;\nblock_size 256;\n#";
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
		CHECK(read.diagnostics[0].place == control::TextPlace{3, 1});
		CHECK_FALSE(read.wanted.has_value());
	}
	SECTION("@frob") {
		const auto model = f.load(formatLines + "@frob 1;\n");
		CHECK(placed(model.read()) == std::vector<Placed>{{"E3008", 3, 1}});
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
	CHECK(read.diagnostics[0].place.line == 3);
	CHECK_FALSE(read.wanted.has_value());
}

TEST_CASE("A model moves, and keeps its text and read", "[graph_file_model]") {
	const Fixture f;
	auto model = f.load(recordExample);
	GraphFileModel moved = std::move(model);
	CHECK(moved.text() == recordExample);
	CHECK(moved.read().wanted.has_value());
}
