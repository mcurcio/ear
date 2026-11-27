// The reach index (docs/graph-file.md#reaches): where each node's key is defined, its defining lines from the
// statement's tokens, the pairs sharing the definition, and the related-change rule over two versions.
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "graph_file_model.hpp"
#include "reach_index.hpp"
#include "test_types.hpp"

namespace {

using namespace ear;
using namespace ear::config;

const std::string formatLines = "graph {\n    rate 48khz;\n    block_size 256;\n}\n";

struct Fixture {
	NodeCatalogue catalogue{test::testTypes()};
	std::shared_ptr<const ReachIndex> index(const std::string& body) const {
		const auto model = GraphFileModel::load(DiskText{formatLines + body}, catalogue);
		REQUIRE_FALSE(model.read().hasErrors());
		auto made = model.reachIndex();
		REQUIRE(made);
		return made;
	}
};

std::vector<GestureKey> sorted(std::vector<GestureKey> keys) {
	std::ranges::sort(keys, [](const GestureKey& a, const GestureKey& b) {
		return a.node != b.node ? a.node < b.node : a.key < b.key;
	});
	return keys;
}

} // namespace

TEST_CASE("A @set used twice reaches every user, a key the file does not give reaches only its node", "[reach_index]") {
	Fixture f;
	const auto index = f.index("@set $trim 1.5;\n"
	                           "node strip a { trimDb.1 $trim; trimDb.2 $trim; }\n"
	                           "node strip b { trimDb.1 $trim; gainDb -6; }\n");
	CHECK(sorted(index->reachOf({"a", "trimDb.1"})) ==
	      std::vector<GestureKey>{{"a", "trimDb.1"}, {"a", "trimDb.2"}, {"b", "trimDb.1"}});
	CHECK(index->reachOf({"b", "gainDb"}) == std::vector<GestureKey>{{"b", "gainDb"}});
	const ReachEntry* given = index->find({"a", "trimDb.1"});
	REQUIRE(given);
	CHECK(given->value == 1.5);
	CHECK(given->place.line == 5); // the @set's value
	CHECK(given->row.member);
	CHECK(given->row.ramped);
	CHECK(given->row.unit == "dB");
	CHECK(given->row.decimals == 1);
	const ReachEntry* absent = index->find({"a", "gainDb"});
	REQUIRE(absent);
	CHECK_FALSE(absent->definition);
	CHECK(absent->value == 0.0); // the row's default
	CHECK(index->reachOf({"a", "gainDb"}) == std::vector<GestureKey>{{"a", "gainDb"}});
	CHECK(index->find({"a", "trimDb.3"})->row.active == false); // above channels' default of 2
	CHECK(index->find({"a", "nope"}) == nullptr);
}

TEST_CASE("A snippet's user and a direct user of its variable share one reach", "[reach_index]") {
	Fixture f;
	const auto index = f.index("@set $trim 0;\n"
	                           "@snippet trims { trimDb.1 $trim; }\n"
	                           "node strip a { @use trims; }\n"
	                           "node strip b { trimDb.1 $trim; }\n");
	CHECK(sorted(index->reachOf({"a", "trimDb.1"})) == std::vector<GestureKey>{{"a", "trimDb.1"}, {"b", "trimDb.1"}});
}

TEST_CASE("A comment added above a definition, or a route, is not related, and a changed definition is",
          "[reach_index]") {
	Fixture f;
	const std::string nodes = "node strip a { trimDb.1 $trim; trimDb.2 $trim; }\nnode strip b { gainDb -6; }\n";
	const auto before = f.index("@set $trim 0;\n" + nodes);
	const std::vector<GestureKey> reach{{"a", "trimDb.1"}, {"a", "trimDb.2"}};
	CHECK(relationOf(*before, *f.index("# trims for a\n@set $trim 0;\n" + nodes), reach) == Relation::none);
	CHECK(relationOf(*before, *f.index("@set $trim 0;\n" + nodes + "node strip c;\n"), reach) == Relation::none);
	CHECK(relationOf(*before, *f.index("@set $trim 2;\n" + nodes), reach) == Relation::value);
	CHECK(relationOf(*before,
	                 *f.index("@set $trim 0;\nnode strip a { trimDb.1 $trim; trimDb.2 $trim; }\n"
	                          "node strip b { gainDb -6; trimDb.1 $trim; }\n"),
	                 reach) == Relation::reach); // another user joined
	CHECK(relationOf(*before, *f.index("@set $trim 0;\nnode strip b { gainDb -6; }\n"), reach) ==
	      Relation::nodeRemoved);
	CHECK(relationOf(*before,
	                 *f.index("@set $trim 0;\nnode strip a { channels 1; trimDb.1 $trim; trimDb.2 $trim; }\n"
	                          "node strip b { gainDb -6; }\n"),
	                 reach) == Relation::keyRemoved); // trimDb.2 inactive at one channel
	// A member inactive already (trimDb.3 at channels' default of 2) is not removed by a change elsewhere, and its value
	// changing is a value's change.
	const std::vector<GestureKey> third{{"a", "trimDb.3"}};
	CHECK(relationOf(*before, *f.index("@set $trim 0;\n" + nodes + "route a:out.1 b:in.1;\n"), third) ==
	      Relation::none);
	CHECK(relationOf(*before, *f.index("@set $trim 0;\n" + nodes + "node strip c;\n"), third) == Relation::none);
	CHECK(relationOf(*before,
	                 *f.index("@set $trim 0;\nnode strip a { trimDb.1 $trim; trimDb.2 $trim; trimDb.3 -6; }\n"
	                          "node strip b { gainDb -6; }\n"),
	                 third) == Relation::value);
	// A key the file does not give: related once the file gives it, or its node line changes.
	const std::vector<GestureKey> gain{{"a", "gainDb"}};
	CHECK(relationOf(*before, *f.index("@set $trim 0;\n" + nodes + "route a:out.1 b:in.1;\n"), gain) == Relation::none);
	CHECK(relationOf(*before,
	                 *f.index("@set $trim 0;\nnode strip a { gainDb 0; trimDb.1 $trim; trimDb.2 $trim; }\n"
	                          "node strip b { gainDb -6; }\n"),
	                 gain) == Relation::reach);
}
