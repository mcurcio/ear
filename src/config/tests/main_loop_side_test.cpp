// The keeper's main-loop side (main_loop_side.hpp): where each kind of refusal points (placeOf, one
// row per CommandError alternative), and a refusal shown only for the version it was recorded for.
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "main_loop_side.hpp"
#include "test_types.hpp"

using namespace ear;
using namespace ear::config;
using control::TextPlace;
using json = nlohmann::json;

namespace {

/// A wanted graph whose items sit on lines 1..8, in order: four nodes, then four routes.
GraphSpec wanted() {
	return GraphSpec{48000, 256,
	                 {{"noise", "white", json::object()},
	                  {"rec", "file", {{"path", "a.wav"}}},
	                  {"rec", "file", {{"path", "b.wav"}}},
	                  {"x", "bogus", json::object()}},
	                 {{{"noise", "MONO"}, {"rec", "FL"}},
	                  {{"noise", "MONO"}, {"ghost", "FL"}},
	                  {{"noise", "MONO"}, {"rec", "FX"}},
	                  {{"rec", "FL"}, {"noise", "MONO"}}}};
}

Placements placements() {
	return {{{1, 1}, {2, 1}, {3, 1}, {4, 1}}, {{5, 1}, {6, 1}, {7, 1}, {8, 1}}};
}

} // namespace

TEST_CASE("placeOf points each refusal at the line that causes it", "[main_loop_side]") {
	const auto spec = wanted();
	const auto places = placements();
	const auto line = [&](const CommandError& error) -> std::optional<std::uint32_t> {
		const auto place = placeOf(error, spec, places);
		if (!place) return std::nullopt;
		CHECK(place->column == 1);
		return place->line;
	};
	CHECK(line(UnknownNodeType{"bogus"}) == 4u);                              // the first node of that type
	CHECK(line(InvalidSettings{"rec", "file needs a path"}) == 2u);           // the named node
	CHECK(line(DuplicateNode{"rec"}) == 3u);                                  // the second of the two
	CHECK(line(UnknownNode{"ghost"}) == 6u);                                  // the first route naming it
	CHECK(line(UnknownPort{{"rec", "FX"}}) == 7u);                            // the first route naming it
	CHECK(line(WrongDirection{{{"rec", "FL"}, {"noise", "MONO"}}}) == 8u);    // the route refused
	CHECK(line(InvalidName{"noise"}) == 1u);                                  // the named node
	CHECK(line(UnknownRoute{{{"noise", "MONO"}, {"rec", "FL"}}}) == 5u);      // the route refused
	CHECK(line(Stopping{}) == std::nullopt);
	CHECK(line(UnknownNode{"nobody"}) == std::nullopt);                       // nothing names it
	CHECK(line(UnknownNodeType{"other"}) == std::nullopt);
}

namespace {

class NoReader final : public GraphReader {
public:
	GraphView read() const override { return {}; }
};

class Listener final : public control::GraphFileListener {
public:
	std::vector<control::GraphFileChanged> changes;
	void graphFileChanged(const control::GraphFileChanged& change) override { changes.push_back(change); }
	void graphFileWritten(const std::string&) override {}
};

FileState state(std::string version) { return FileState{std::move(version), "text", {}, {}, std::nullopt}; }

} // namespace

TEST_CASE("A disk offer the graph refuses is recorded for its version only, and nothing runs", "[main_loop_side]") {
	NodeCatalogue catalogue{test::testTypes()};
	DesiredGraph graph{catalogue};
	NoReader reader;
	MainLoopSide side{graph, reader};
	Listener listener;
	side.subscribe(listener);

	auto spec = wanted();
	spec.nodes.erase(spec.nodes.begin() + 2, spec.nodes.end());   // noise and rec: only routes refuse
	const auto judged = side.offer(Offer{state("v1"), spec, placements(), control::FileChangeCause::disk, 2});
	CHECK(judged.verdict == Verdict::refused);
	REQUIRE_FALSE(judged.result.has_value());
	CHECK(errorCode(judged.result.error()) == "unknown-node");
	REQUIRE(listener.changes.size() == 1);
	const auto& told = listener.changes[0];
	CHECK(told.cause == control::FileChangeCause::disk);
	CHECK(told.droppedEdits == 2);
	CHECK_FALSE(told.file.running.has_value());
	REQUIRE(told.file.refusal.has_value());
	CHECK(told.file.refusal->place == TextPlace{6, 1});

	CHECK(side.graphFile(state("v1")).refusal.has_value());
	CHECK_FALSE(side.graphFile(state("v2")).refusal.has_value());
	CHECK(graph.nodes().empty());

	// A refused edit tells nothing and records nothing.
	const auto edit = side.offer(Offer{state("v3"), spec, placements(), control::FileChangeCause::edit, 0});
	CHECK(edit.verdict == Verdict::refused);
	CHECK(listener.changes.size() == 1);
	CHECK_FALSE(side.graphFile(state("v3")).refusal.has_value());
}
