// The desired graph's plan() and commit(), a whole wanted graph at a time, checked against a
// catalogue of test node types: the errors a plan returns, the events a commit sends and their
// order, and the catalogue's own boot-time check (docs/architecture.md#how-a-change-moves: adding, removing and
// updating, the name rule, observers first, close()). The old control calls' cases are kept as plans.
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/settings_table.hpp"

using namespace ear;
using json = nlohmann::json;

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace desired_graph_test {

/// A node whose ports are fixed.
class TestConfig final : public NodeConfig {
public:
	explicit TestConfig(Ports ports) : ports_(std::move(ports)) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override {
		return std::unexpected(FailureReason{FailureReason::Code::startFailed, "not started in these tests"});
	}

private:
	Ports ports_;
};

constexpr auto levelRow = setting::optional("level", setting::number(0, 100));
constexpr SettingsTable<levelRow> sourceTable{"source"};
constexpr SettingsTable<levelRow> sinkTable{"sink"};
constexpr SettingsTable<> dashedTable{"send-spin"};
constexpr SettingsTable<> upperTable{"Sink"};

/// A node type whose nodes have fixed ports; settings {"bad": ...} are invalid: its table declares no such key.
template <const auto& Table> class TestType final : public NodeTypeOf<Table> {
public:
	explicit TestType(Ports ports) : ports_(std::move(ports)) {}

private:
	MadeConfig make(const NodeName&, const typename NodeTypeOf<Table>::Checked&) const override {
		return std::make_unique<TestConfig>(ports_);
	}

	Ports ports_;
};

/// Any text at all.
std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr auto cardRow = setting::optional("card", setting::text<anyText>("text"));
constexpr auto playbackRow = setting::defaulted("playback", setting::number(0, 64), 0);
constexpr SettingsTable<cardRow, playbackRow> cardTable{"card"};

/// A sound-card-like type: {"card": <text>, "playback": <n>} declares inputs play.1..play.<n>,
/// so a new channel count changes its ports.
class CardType final : public NodeTypeOf<cardTable> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		return std::make_unique<TestConfig>(Ports{{{Direction::input, "play", numbered(settings.get<playbackRow>())}}});
	}
};

/// Text that throws when it reads "boom", as a type's parse with a bug.
std::optional<std::string> throwingText(std::string_view text) {
	if (text == "boom") throw std::runtime_error("parse blew up");
	return std::string(text);
}

constexpr auto modeRow = setting::optional("mode", setting::text<throwingText>("text"));
constexpr SettingsTable<modeRow> faultyTable{"faulty"};

/// A type with bugs: its row's parse throws for "boom", and its make for {"mode": "make"}.
class FaultyType final : public NodeTypeOf<faultyTable> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		if (const auto mode = settings.get<modeRow>(); mode && *mode == "make")
			throw std::logic_error("no ports for it");
		return std::make_unique<TestConfig>(Ports{{{Direction::output, "out", {"1"}}}});
	}
};

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(std::make_unique<CardType>());
	types.push_back(std::make_unique<TestType<sourceTable>>(Ports{{{Direction::output, "noise", {"mono"}}}}));
	types.push_back(std::make_unique<TestType<sinkTable>>(Ports{{{Direction::input, "record", {"left", "right"}}}}));
	return types;
}

using Log = std::vector<std::string>;

std::string text(const Route& route) {
	return route.from.node + ":" + route.from.port + " -> " + route.to.node + ":" + route.to.port;
}

/// Records every event, and writes each one to `log` as "<tag> <event> <what>".
struct Recorder final : DesiredNodeListener, DesiredRouteListener {
	Log& log;
	std::string tag;
	std::vector<std::pair<NodeName, std::shared_ptr<const NodeConfig>>> nodes;
	std::vector<std::pair<NodeSpec, std::shared_ptr<const NodeConfig>>> updates;
	std::vector<Route> routes;
	Recorder(Log& l, std::string t) : log(l), tag(std::move(t)) {}
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override {
		log.push_back(tag + " nodeAdded " + node.name);
		nodes.emplace_back(node.name, std::move(config));
	}
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override {
		log.push_back(tag + " nodeUpdated " + node.name);
		updates.emplace_back(node, std::move(config));
	}
	void nodeRemoved(const NodeName& node) override { log.push_back(tag + " nodeRemoved " + node); }
	void routeAdded(const Route& route) override {
		log.push_back(tag + " routeAdded " + text(route));
		routes.push_back(route);
	}
	void routeRemoved(const Route& route) override { log.push_back(tag + " routeRemoved " + text(route)); }
};

struct Fixture {
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	Log log;
	Recorder recorder{log, "sub"};
	Fixture() {
		graph.subscribe(static_cast<DesiredNodeListener&>(recorder));
		graph.subscribe(static_cast<DesiredRouteListener&>(recorder));
	}
};

const Route noiseToFl{{"noise", "noise.mono"}, {"rec", "record.left"}};
const Route noiseToFr{{"noise", "noise.mono"}, {"rec", "record.right"}};
const Route hissToFl{{"hiss", "noise.mono"}, {"rec", "record.left"}};

const NodeSpec noiseSpec{"noise", "source", json::object()};
const NodeSpec recSpec{"rec", "sink", json::object()};
NodeSpec deskSpec(int playback) { return {"desk", "card", {{"card", "MOTU"}, {"playback", playback}}}; }
const Route noiseToDesk1{{"noise", "noise.mono"}, {"desk", "play.1"}};

/// Plans `nodes` and `routes` and commits the plan, which must be accepted.
void apply(DesiredGraph& graph, const std::vector<NodeSpec>& nodes, const std::vector<Route>& routes) {
	auto plan = graph.plan(nodes, routes);
	REQUIRE(plan.has_value());
	REQUIRE(graph.commit(std::move(*plan)).has_value());
}

template <class Error> const Error& planErrorOf(const std::expected<Plan, CommandError>& result) {
	REQUIRE_FALSE(result.has_value());
	REQUIRE(std::holds_alternative<Error>(result.error()));
	return std::get<Error>(result.error());
}

} // namespace desired_graph_test
using namespace desired_graph_test;

TEST_CASE("A wanted node of a known type is added once and told once", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec}, {});
	REQUIRE(f.recorder.nodes.size() == 1);
	CHECK(f.recorder.nodes[0].first == "noise");
	CHECK(f.recorder.nodes[0].second->ports().outputs() == std::vector<PortName>{"noise.mono"});

	SECTION("the same spec again is an empty plan") {
		auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
		REQUIRE(plan.has_value());
		CHECK(plan->empty());
		CHECK(f.recorder.nodes.size() == 1);
	}
	SECTION("a node that is no type's, or with invalid settings, is refused and changes nothing") {
		CHECK(planErrorOf<UnknownNodeType>(f.graph.plan(std::vector<NodeSpec>{{"noise", "pink", {}}}, {})).type ==
		      "pink");
		const auto result = f.graph.plan(std::vector<NodeSpec>{{"noise", "source", {{"bad", true}}}}, {});
		const auto& invalid = planErrorOf<InvalidSettings>(result);
		CHECK(invalid.node == "noise");
		CHECK(invalid.detail == "unknown key bad");
		CHECK(f.recorder.nodes.size() == 1);
		CHECK(f.graph.nodes().at(0).spec == noiseSpec);
	}
}

TEST_CASE("A type that throws at parse refuses the edit as invalid settings, and the graph runs on as it was",
          "[desired_graph][type-throw]") {
	std::vector<std::unique_ptr<const NodeType>> types = testTypes();
	types.push_back(std::make_unique<FaultyType>());
	NodeCatalogue catalogue{std::move(types)};
	DesiredGraph graph{catalogue};
	apply(graph, {noiseSpec}, {});
	for (const auto& [mode, detail] : std::vector<std::pair<std::string, std::string>>{
			 {"boom", "faulty threw: parse blew up"}, {"make", "faulty threw: no ports for it"}}) {
		INFO(mode);
		const auto result = graph.plan(std::vector<NodeSpec>{noiseSpec, {"bad", "faulty", {{"mode", mode}}}}, {});
		const auto& invalid = planErrorOf<InvalidSettings>(result);
		CHECK(invalid.node == "bad");
		CHECK(invalid.detail == detail);
		REQUIRE(graph.nodes().size() == 1);
		CHECK(graph.nodes().at(0).spec == noiseSpec);
	}
	apply(graph, {noiseSpec, {"good", "faulty", {{"mode", "quiet"}}}}, {}); // the type is refused only where it threw
	CHECK(graph.nodes().size() == 2);
}

TEST_CASE("A wanted route is checked at both ends: node, port and direction", "[desired_graph]") {
	Fixture f;
	const std::vector<NodeSpec> both{noiseSpec, recSpec};
	const auto refused = [&](const Route& route) { return f.graph.plan(both, std::vector<Route>{route}); };

	CHECK(planErrorOf<UnknownNode>(refused({{"hiss", "noise.mono"}, {"rec", "record.left"}})).node == "hiss");
	CHECK(planErrorOf<UnknownNode>(refused({{"noise", "noise.mono"}, {"tape", "record.left"}})).node == "tape");
	CHECK(planErrorOf<UnknownPort>(refused({{"noise", "noise.mono"}, {"rec", "FX"}})).port == PortRef{"rec", "FX"});
	CHECK(planErrorOf<UnknownPort>(refused({{"noise", "L"}, {"rec", "record.left"}})).port == PortRef{"noise", "L"});

	const Route backwards{{"rec", "record.left"}, {"noise", "noise.mono"}};
	CHECK(planErrorOf<WrongDirection>(refused(backwards)).route == backwards);
	const Route inputToInput{{"rec", "record.left"}, {"rec", "record.right"}};
	CHECK(planErrorOf<WrongDirection>(refused(inputToInput)).route == inputToInput);
	const Route outputToOutput{{"noise", "noise.mono"}, {"noise", "noise.mono"}};
	CHECK(planErrorOf<WrongDirection>(refused(outputToOutput)).route == outputToOutput);

	CHECK(f.log.empty());
}

TEST_CASE("The catalogue finds types by name and refuses two types with one name", "[catalogue]") {
	const NodeCatalogue catalogue{testTypes()};
	REQUIRE(catalogue.find("sink") != nullptr);
	CHECK(catalogue.find("sink")->name() == "sink");
	CHECK(catalogue.find("pink") == nullptr);

	auto types = testTypes();
	types.push_back(std::make_unique<TestType<sinkTable>>(Ports{{{Direction::input, "in", {"a"}}}}));
	CHECK_THROWS_AS(NodeCatalogue{std::move(types)}, std::invalid_argument);
}

TEST_CASE("The catalogue refuses a type whose name is not 1 or more of [a-z0-9]", "[catalogue]") {
	// "send-spin" with node "x" and "send" with node "spin-x" would name one store file, send-spin-x.<suffix>.
	const auto refused = [](std::unique_ptr<const NodeType> made) {
		auto types = testTypes();
		const std::string name(made->name());
		types.push_back(std::move(made));
		CHECK_THROWS_WITH(NodeCatalogue{std::move(types)},
		                  "node catalogue: a node type's name is 1 or more of [a-z0-9], not \"" + name + "\"");
	};
	refused(std::make_unique<TestType<dashedTable>>(Ports{}));
	refused(std::make_unique<TestType<upperTable>>(Ports{}));
}

TEST_CASE("Command errors have one code each and describe what they name", "[command_error]") {
	const Route route{{"rec", "record.left"}, {"noise", "noise.mono"}};
	const std::vector<std::pair<CommandError, std::string>> cases{
		{UnknownNodeType{"pink"}, "unknown-node-type pink"},
		{InvalidSettings{"rec", "path: missing"}, "invalid-settings rec: path: missing"},
		{DuplicateNode{"rec"}, "duplicate-node rec"},
		{UnknownNode{"tape"}, "unknown-node tape"},
		{UnknownPort{{"rec", "FX"}}, "unknown-port rec:FX"},
		{WrongDirection{route}, "wrong-direction rec:record.left -> noise:noise.mono"},
		{InvalidName{"a.b"}, "invalid-name a.b"},
		{UnknownRoute{{{"noise", "noise.mono"}, {"rec", "record.right"}}},
	     "unknown-route noise:noise.mono -> rec:record.right"},
		{Stopping{}, "stopping"},
	};
	for (const auto& [error, text] : cases) {
		CHECK(describe(error) == text);
		const std::string code(errorCode(error));
		CHECK((text == code || text.starts_with(code + " ")));
	}
}

TEST_CASE("A route no longer wanted is removed alone, and it can be wanted again", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl, noiseToFr});
	f.log.clear();

	apply(f.graph, {noiseSpec, recSpec}, {noiseToFr});
	CHECK(f.log == Log{"sub routeRemoved noise:noise.mono -> rec:record.left"});
	CHECK(f.graph.routes() == std::vector<Route>{noiseToFr});

	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl, noiseToFr});
	CHECK(f.log.back() == "sub routeAdded noise:noise.mono -> rec:record.left");
}

TEST_CASE("A node no longer wanted goes after each of its routes, and the name can come back", "[desired_graph]") {
	Fixture f;
	const NodeSpec hissSpec{"hiss", "source", json::object()};
	apply(f.graph, {noiseSpec, hissSpec, recSpec}, {noiseToFl, noiseToFr, hissToFl});
	f.log.clear();

	apply(f.graph, {hissSpec, recSpec}, {hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:noise.mono -> rec:record.left",
	                   "sub routeRemoved noise:noise.mono -> rec:record.right", "sub nodeRemoved noise"});
	CHECK(f.graph.routes() == std::vector<Route>{hissToFl});
	REQUIRE(f.graph.nodes().size() == 2);
	CHECK(f.graph.nodes()[0].spec.name == "hiss");
	CHECK(f.graph.nodes()[1].spec.name == "rec");

	apply(f.graph, {{"noise", "source", {{"level", 2}}}, hissSpec, recSpec}, {noiseToFl, hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:noise.mono -> rec:record.left",
	                   "sub routeRemoved noise:noise.mono -> rec:record.right", "sub nodeRemoved noise",
	                   "sub nodeAdded noise", "sub routeAdded noise:noise.mono -> rec:record.left"});

	SECTION("a node with no routes is removed alone") {
		apply(f.graph, {{"noise", "source", {{"level", 2}}}, hissSpec, recSpec, {"lone", "source", json::object()}},
		      {noiseToFl, hissToFl});
		f.log.clear();
		apply(f.graph, {{"noise", "source", {{"level", 2}}}, hissSpec, recSpec}, {noiseToFl, hissToFl});
		CHECK(f.log == Log{"sub nodeRemoved lone"});
	}
}

TEST_CASE("New settings with the same ports update the node, which records the new spec", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {{"rec", "sink", {{"level", 1}}}}, {});
	f.log.clear();

	SECTION("invalid settings change nothing") {
		CHECK(planErrorOf<InvalidSettings>(f.graph.plan(std::vector<NodeSpec>{{"rec", "sink", {{"bad", true}}}}, {}))
		          .node == "rec");
		CHECK(f.log.empty());
		CHECK(f.graph.nodes().at(0).spec.settings == NodeSettings{{"level", 1}});
	}
	SECTION("new settings with the same ports") {
		apply(f.graph, {{"rec", "sink", {{"level", 2}}}}, {});
		CHECK(f.log == Log{"sub nodeUpdated rec"});
		REQUIRE(f.recorder.updates.size() == 1);
		CHECK(f.recorder.updates[0].first == NodeSpec{"rec", "sink", {{"level", 2}}});
		CHECK(f.recorder.updates[0].second != f.recorder.nodes.at(0).second); // a new config
		CHECK(f.recorder.updates[0].second->ports().inputs() == std::vector<PortName>{"record.left", "record.right"});
		CHECK(f.graph.nodes().at(0).spec.settings == NodeSettings{{"level", 2}});
		// The recorded spec is the new one: wanting it again is an empty plan.
		auto again = f.graph.plan(std::vector<NodeSpec>{{"rec", "sink", {{"level", 2}}}}, {});
		REQUIRE(again.has_value());
		CHECK(again->empty());
	}
}

TEST_CASE("A node name is 1-64 characters of letters, digits, '_' and '-'", "[desired_graph]") {
	Fixture f;
	for (const std::string& name : {std::string(), std::string(65, 'a'), std::string("a.b"), std::string("a b"),
	                                std::string("a:b"), std::string("rec/1")}) {
		INFO(name);
		CHECK(planErrorOf<InvalidName>(f.graph.plan(std::vector<NodeSpec>{{name, "source", {}}}, {})).node == name);
	}
	CHECK(f.log.empty());
	apply(f.graph, {{"noise-1_A", "source", json::object()}, {std::string(64, 'z'), "source", json::object()}}, {});
	CHECK(f.log.size() == 2);
}

TEST_CASE("Observers hear every change before any subscriber, whatever the registration order", "[desired_graph]") {
	Fixture f; // its recorder subscribed first
	Recorder observer{f.log, "obs"};
	f.graph.observe(static_cast<DesiredNodeListener&>(observer));
	f.graph.observe(static_cast<DesiredRouteListener&>(observer));

	apply(f.graph, {noiseSpec, recSpec}, {});
	apply(f.graph, {noiseSpec, {"rec", "sink", {{"level", 2}}}}, {noiseToFl});
	apply(f.graph, {noiseSpec}, {});

	CHECK(f.log == Log{
					   "obs nodeAdded noise",
					   "sub nodeAdded noise",
					   "obs nodeAdded rec",
					   "sub nodeAdded rec",
					   "obs nodeUpdated rec",
					   "sub nodeUpdated rec",
					   "obs routeAdded noise:noise.mono -> rec:record.left",
					   "sub routeAdded noise:noise.mono -> rec:record.left",
					   "obs routeRemoved noise:noise.mono -> rec:record.left",
					   "sub routeRemoved noise:noise.mono -> rec:record.left",
					   "obs nodeRemoved rec",
					   "sub nodeRemoved rec",
				   });
}

TEST_CASE("After close() plan and commit return stopping, and reads still answer", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl});
	auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
	REQUIRE(plan.has_value());
	f.log.clear();

	(void)f.graph.close();
	(void)f.graph.close(); // again: another proof, nothing changes

	CHECK(std::holds_alternative<Stopping>(
		f.graph.plan(std::vector<NodeSpec>{noiseSpec, recSpec}, std::vector<Route>{noiseToFl})
			.error())); // even the same graph
	const auto committed = f.graph.commit(std::move(*plan));
	REQUIRE_FALSE(committed.has_value());
	CHECK(std::holds_alternative<Stopping>(committed.error()));
	CHECK(f.log.empty());

	const auto nodes = f.graph.nodes();
	REQUIRE(nodes.size() == 2);
	CHECK(nodes[0].spec == noiseSpec);
	CHECK(nodes[0].ports.outputs() == std::vector<PortName>{"noise.mono"});
	CHECK(nodes[1].spec == recSpec);
	CHECK(f.graph.routes() == std::vector<Route>{noiseToFl});
}

TEST_CASE("plan from empty to a full graph commits every change in the fixed order", "[desired_graph][plan]") {
	Fixture f;
	Recorder observer{f.log, "obs"};
	f.graph.observe(static_cast<DesiredNodeListener&>(observer));
	f.graph.observe(static_cast<DesiredRouteListener&>(observer));
	const std::vector<NodeSpec> nodes{recSpec, noiseSpec, deskSpec(2)};
	const std::vector<Route> routes{noiseToDesk1, noiseToFl};

	auto plan = f.graph.plan(nodes, routes);
	REQUIRE(plan.has_value());
	CHECK_FALSE(plan->empty());
	CHECK(f.log.empty()); // plan changes nothing
	CHECK(f.graph.nodes().empty());
	REQUIRE(f.graph.commit(std::move(*plan)).has_value());

	CHECK(f.log == Log{"obs nodeAdded desk", "sub nodeAdded desk", "obs nodeAdded noise", "sub nodeAdded noise",
	                   "obs nodeAdded rec", "sub nodeAdded rec", "obs routeAdded noise:noise.mono -> desk:play.1",
	                   "sub routeAdded noise:noise.mono -> desk:play.1",
	                   "obs routeAdded noise:noise.mono -> rec:record.left",
	                   "sub routeAdded noise:noise.mono -> rec:record.left"});
	CHECK(f.graph.nodes().size() == 3);
	CHECK(f.graph.routes() == std::vector<Route>{noiseToDesk1, noiseToFl});
}

TEST_CASE("plan of what the graph holds is empty, and its commit tells nobody", "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl});
	f.log.clear();

	SECTION("the same spec") {
		auto plan = f.graph.plan(std::vector<NodeSpec>{recSpec, noiseSpec}, std::vector<Route>{noiseToFl});
		REQUIRE(plan.has_value());
		CHECK(plan->empty());
		REQUIRE(f.graph.commit(std::move(*plan)).has_value());
		CHECK(f.log.empty());
	}
	SECTION("identical routes are one route") {
		auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec, recSpec}, std::vector<Route>{noiseToFl, noiseToFl});
		REQUIRE(plan.has_value());
		CHECK(plan->empty());
	}
}

TEST_CASE("plan with new settings and the same ports updates the node and keeps its routes", "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, deskSpec(2)}, {noiseToDesk1});
	f.log.clear();

	NodeSpec usb = deskSpec(2);
	usb.settings["card"] = "USB2";
	apply(f.graph, {noiseSpec, usb}, {noiseToDesk1});
	CHECK(f.log == Log{"sub nodeUpdated desk"});
	REQUIRE(f.recorder.updates.size() == 1);
	CHECK(f.recorder.updates[0].first == usb);
	CHECK(f.graph.routes() == std::vector<Route>{noiseToDesk1});
}

TEST_CASE("plan with other ports replaces the node and adds its routes again", "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec, deskSpec(2)}, {noiseToDesk1, noiseToFl});
	f.log.clear();

	apply(f.graph, {noiseSpec, recSpec, deskSpec(4)}, {noiseToDesk1, noiseToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:noise.mono -> desk:play.1", "sub nodeRemoved desk", "sub nodeAdded desk",
	                   "sub routeAdded noise:noise.mono -> desk:play.1"});
	REQUIRE(f.recorder.nodes.size() == 4);
	CHECK(f.recorder.nodes[3].second->ports().inputs().size() == 4);

	SECTION("another type under the same name is a replacement too") {
		f.log.clear();
		apply(f.graph, {noiseSpec, recSpec, {"desk", "sink", json::object()}}, {noiseToFl});
		CHECK(f.log ==
		      Log{"sub routeRemoved noise:noise.mono -> desk:play.1", "sub nodeRemoved desk", "sub nodeAdded desk"});
	}
}

TEST_CASE("plan removes what is no longer wanted, routes before nodes", "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl, noiseToFr});
	f.log.clear();

	apply(f.graph, {recSpec, {"hiss", "source", json::object()}}, {hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:noise.mono -> rec:record.left",
	                   "sub routeRemoved noise:noise.mono -> rec:record.right", "sub nodeRemoved noise",
	                   "sub nodeAdded hiss", "sub routeAdded hiss:noise.mono -> rec:record.left"});
}

TEST_CASE("plan refuses a wanted graph the catalogue or the ports refuse, and changes nothing",
          "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, deskSpec(4)}, {{{"noise", "noise.mono"}, {"desk", "play.4"}}});
	f.log.clear();

	SECTION("a wanted route to a port that is gone") {
		const Route toFour{{"noise", "noise.mono"}, {"desk", "play.4"}};
		const auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec, deskSpec(2)}, std::vector<Route>{toFour});
		CHECK(planErrorOf<UnknownPort>(plan).port == toFour.to);
		CHECK(errorCode(plan.error()) == "unknown-port");
	}
	SECTION("two wanted nodes share a name") {
		const auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec, noiseSpec}, std::vector<Route>{});
		CHECK(planErrorOf<DuplicateNode>(plan).node == "noise");
		CHECK(errorCode(plan.error()) == "duplicate-node");
	}
	SECTION("the first error found, nodes before routes") {
		const auto plan =
			f.graph.plan(std::vector<NodeSpec>{noiseSpec, {"x.y", "source", json::object()}, {"z", "nope", {}}},
		                 std::vector<Route>{{{"gone", "noise.mono"}, {"noise", "noise.mono"}}});
		CHECK(planErrorOf<InvalidName>(plan).node == "x.y");
	}
	SECTION("each node and route error") {
		CHECK(planErrorOf<UnknownNodeType>(f.graph.plan(std::vector<NodeSpec>{{"z", "nope", {}}}, {})).type == "nope");
		CHECK(
			planErrorOf<InvalidSettings>(f.graph.plan(std::vector<NodeSpec>{{"z", "source", {{"bad", 1}}}}, {})).node ==
			"z");
		CHECK(planErrorOf<UnknownNode>(f.graph.plan(std::vector<NodeSpec>{recSpec}, std::vector<Route>{noiseToFl}))
		          .node == "noise");
		CHECK(planErrorOf<WrongDirection>(
				  f.graph.plan(std::vector<NodeSpec>{noiseSpec, recSpec},
		                       std::vector<Route>{{{"rec", "record.left"}, {"noise", "noise.mono"}}}))
		          .route.from.node == "rec");
	}
	CHECK(f.log.empty());
	CHECK(f.graph.nodes().size() == 2);
	CHECK(f.graph.nodes()[0].ports.inputs().size() == 4);
}

TEST_CASE("commit of a stale plan or of another graph's plan is a wiring bug", "[desired_graph][plan]") {
	Fixture f;
	auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
	REQUIRE(plan.has_value());

	SECTION("the graph changed since the plan was made") {
		apply(f.graph, {recSpec}, {});
		f.log.clear();
		CHECK_THROWS_AS(f.graph.commit(std::move(*plan)), std::logic_error);
		CHECK(f.log.empty());
	}
	SECTION("a plan made on another graph") {
		DesiredGraph other{f.catalogue};
		CHECK_THROWS_AS(other.commit(std::move(*plan)), std::logic_error);
		CHECK(other.nodes().empty());
	}
	SECTION("a plan committed already") {
		auto again = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
		REQUIRE(again.has_value());
		REQUIRE(f.graph.commit(std::move(*plan)).has_value());
		CHECK_THROWS_AS(f.graph.commit(std::move(*again)), std::logic_error);
	}
}

namespace desired_graph_test {

/// Records nodeChanged with its outcome and keys, and every other node event, in one log.
struct ChangeRecorder final : DesiredNodeListener, DesiredRouteListener {
	Log log;
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override {
		log.push_back("nodeAdded " + node.name + (node.disabled ? " disabled" : ""));
	}
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override {
		log.push_back("nodeUpdated " + node.name);
	}
	void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig>, const NodeChange& change) override {
		static constexpr const char* names[] = {"restart", "disable", "enable", "stored"};
		std::string keys;
		for (const auto& key : change.keys) keys += " " + key;
		log.push_back("nodeChanged " + node.name + " " + names[static_cast<int>(change.outcome)] +
		              (node.disabled ? " disabled" : "") + keys);
	}
	void nodeRemoved(const NodeName& node) override { log.push_back("nodeRemoved " + node); }
	void routeAdded(const Route& route) override { log.push_back("routeAdded " + text(route)); }
	void routeRemoved(const Route& route) override { log.push_back("routeRemoved " + text(route)); }
};

NodeSpec disabledSpec(NodeSpec spec, bool disabled = true) {
	spec.disabled = disabled;
	return spec;
}

using Outcomes = std::vector<std::pair<NodeName, NodeOutcome>>;

} // namespace desired_graph_test

TEST_CASE("A changed disabled is a disable, an enable or stored, told by nodeChanged in the commit's order",
          "[desired_graph]") {
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	ChangeRecorder recorder;
	graph.subscribe(static_cast<DesiredNodeListener&>(recorder));
	graph.subscribe(static_cast<DesiredRouteListener&>(recorder));
	apply(graph, {noiseSpec, recSpec, deskSpec(2)}, {noiseToFl});
	recorder.log.clear();

	SECTION("false to true: disable, with no key changed; routes kept") {
		auto plan = graph.plan(std::vector<NodeSpec>{noiseSpec, disabledSpec(recSpec), deskSpec(2)},
		                       std::vector<Route>{noiseToFl});
		REQUIRE(plan.has_value());
		CHECK(plan->outcomes() == Outcomes{{"rec", NodeOutcome::disable}});
		REQUIRE(graph.commit(std::move(*plan)).has_value());
		CHECK(recorder.log == Log{"nodeChanged rec disable disabled"});
		CHECK(graph.nodes()[2].spec.disabled);
		CHECK(graph.routes() == std::vector<Route>{noiseToFl});
	}
	SECTION("true to false: enable; true to true with other settings: stored; false to false: restart, with its keys") {
		apply(graph, {disabledSpec(noiseSpec), recSpec, disabledSpec(deskSpec(2))}, {noiseToFl});
		recorder.log.clear();
		NodeSpec desk = disabledSpec(deskSpec(2));
		desk.settings["card"] = "USB2";
		const NodeSpec rec{"rec", "sink", {{"level", 3}}};
		auto plan = graph.plan(std::vector<NodeSpec>{noiseSpec, rec, desk}, std::vector<Route>{noiseToFl});
		REQUIRE(plan.has_value());
		CHECK(plan->outcomes() ==
		      Outcomes{{"desk", NodeOutcome::stored}, {"noise", NodeOutcome::enable}, {"rec", NodeOutcome::restart}});
		REQUIRE(graph.commit(std::move(*plan)).has_value());
		CHECK(recorder.log == Log{"nodeChanged desk stored disabled card", "nodeChanged noise enable",
		                          "nodeChanged rec restart level"});
	}
	SECTION("a disabled node equal to the held one is no change") {
		apply(graph, {noiseSpec, disabledSpec(recSpec), deskSpec(2)}, {noiseToFl});
		recorder.log.clear();
		auto plan = graph.plan(std::vector<NodeSpec>{noiseSpec, disabledSpec(recSpec), deskSpec(2)},
		                       std::vector<Route>{noiseToFl});
		REQUIRE(plan.has_value());
		CHECK(plan->empty());
		CHECK(plan->outcomes().empty());
	}
	SECTION("nodeChanged comes where nodeUpdated came: after removals, before additions and routes") {
		const NodeSpec hiss{"hiss", "source", json::object()};
		auto plan =
			graph.plan(std::vector<NodeSpec>{hiss, disabledSpec(recSpec), deskSpec(2)}, std::vector<Route>{hissToFl});
		REQUIRE(plan.has_value());
		REQUIRE(graph.commit(std::move(*plan)).has_value());
		CHECK(recorder.log == Log{"routeRemoved noise:noise.mono -> rec:record.left", "nodeRemoved noise",
		                          "nodeChanged rec disable disabled", "nodeAdded hiss",
		                          "routeAdded hiss:noise.mono -> rec:record.left"});
	}
}

TEST_CASE("A replace keeps its disabled change's outcome: stored on a node staying disabled, restart on one enabled",
          "[desired_graph]") {
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	ChangeRecorder recorder;
	graph.subscribe(static_cast<DesiredNodeListener&>(recorder));
	graph.subscribe(static_cast<DesiredRouteListener&>(recorder));

	const auto replaceOutcome = [&](bool was, bool now) {
		apply(graph, {disabledSpec(deskSpec(2), was)}, {});
		recorder.log.clear();
		auto plan = graph.plan(std::vector<NodeSpec>{disabledSpec(deskSpec(4), now)}, std::vector<Route>{});
		REQUIRE(plan.has_value());
		const auto outcomes = plan->outcomes();
		REQUIRE(graph.commit(std::move(*plan)).has_value());
		CHECK(recorder.log == Log{"nodeRemoved desk", std::string("nodeAdded desk") + (now ? " disabled" : "")});
		REQUIRE(outcomes.size() == 1);
		return outcomes[0].second;
	};
	CHECK(replaceOutcome(true, true) == NodeOutcome::stored);    // disabled before and after: kept for the enable
	CHECK(replaceOutcome(false, false) == NodeOutcome::restart); // enabled before and after: the new node starts
	CHECK(replaceOutcome(false, true) == NodeOutcome::disable);  // the replace disables it
	CHECK(replaceOutcome(true, false) == NodeOutcome::enable);   // the replace enables it
}

TEST_CASE("nodeChanged's default tells nodeUpdated, so a listener that tells no outcome apart sees an update",
          "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec}, {});
	f.log.clear();
	apply(f.graph, {disabledSpec(noiseSpec)}, {});
	CHECK(f.log == Log{"sub nodeUpdated noise"});
	REQUIRE(f.recorder.updates.size() == 1);
	CHECK(f.recorder.updates[0].first.disabled);
}

TEST_CASE("A replace is told through nodeReplacing and nodeReplaced, port-caused when only the ports changed",
          "[desired_graph]") {
	struct ReplaceRecorder final : DesiredNodeListener {
		Log log;
		void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override {
			log.push_back("nodeAdded " + node.name);
		}
		void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) override {
			log.push_back("nodeUpdated " + node.name);
		}
		void nodeRemoved(const NodeName& node) override { log.push_back("nodeRemoved " + node); }
		void nodeReplacing(const NodeName& node, const NodeChange& change) override {
			log.push_back("nodeReplacing " + node + text(change));
		}
		void nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig>, const NodeChange& change) override {
			log.push_back("nodeReplaced " + node.name + text(change));
		}
		static std::string text(const NodeChange& change) {
			std::string line = change.portCaused ? " ports" : " type";
			for (const auto& key : change.keys) line += " " + key;
			return line;
		}
	};
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	ReplaceRecorder recorder;
	graph.subscribe(recorder);
	apply(graph, {deskSpec(2)}, {});
	recorder.log.clear();
	apply(graph, {deskSpec(4)}, {});
	CHECK(recorder.log == Log{"nodeReplacing desk ports playback", "nodeReplaced desk ports playback"});
	recorder.log.clear();
	apply(graph, {NodeSpec{"desk", "source", json::object()}}, {});
	CHECK(recorder.log == Log{"nodeReplacing desk type card playback", "nodeReplaced desk type card playback"});
}
