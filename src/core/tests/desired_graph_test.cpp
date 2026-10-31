// The desired graph's plan() and commit(), a whole wanted graph at a time, checked against a
// catalogue of test node types: the errors a plan returns, the events a commit sends and their
// order, and the catalogue's own boot-time check (docs/design/m1-white-to-file.md section 3, S2
// criteria 1-3; docs/design/batch2.md section 4.1, C1 criteria 1-6: adding, removing and updating,
// the name rule, observers first, close(); docs/design/knobly-config.md, S1 criterion 8). The old
// control calls' cases are kept as plans (slice S5).
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/desired_graph.hpp"

using namespace ear;
using json = nlohmann::json;

namespace {

/// A node type whose nodes have fixed ports; settings {"bad": ...} are invalid.
class TestConfig final : public NodeConfig {
public:
	explicit TestConfig(Ports ports) : ports_(std::move(ports)) {}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant&) const override {
		return std::unexpected(FailureReason{FailureReason::Code::startFailed, "not started in these tests"});
	}

private:
	Ports ports_;
};

class TestType final : public NodeType {
public:
	TestType(std::string name, Ports ports) : name_(std::move(name)), ports_(std::move(ports)) {}
	std::string_view name() const override { return name_; }
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName& node,
	                                                                        const NodeSettings& settings) const override {
		if (settings.contains("bad")) return std::unexpected(InvalidSettings{node, "bad is not a setting"});
		Ports ports = ports_;   // {"extra": "<name>"} adds an input port of that name
		if (settings.contains("extra")) ports.inputs.push_back(settings["extra"].get<std::string>());
		return std::make_unique<TestConfig>(std::move(ports));
	}

private:
	static constexpr SettingDeclaration kinds[] = {{"level", SettingKind::wholeNumber}, {"extra", SettingKind::text}};
	std::string name_;
	Ports ports_;
};

/// A sound-card-like type: {"card": <text>, "playback": <n>} declares inputs playback_1..playback_n,
/// so a new channel count changes its ports.
class CardType final : public NodeType {
public:
	std::string_view name() const override { return "card"; }
	std::span<const SettingDeclaration> settingKinds() const override { return kinds; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName&,
	                                                                        const NodeSettings& settings) const override {
		Ports ports;
		const int playback = settings.value("playback", 0);
		for (int i = 1; i <= playback; ++i) ports.inputs.push_back("playback_" + std::to_string(i));
		return std::make_unique<TestConfig>(std::move(ports));
	}

private:
	static constexpr SettingDeclaration kinds[] = {{"card", SettingKind::text}, {"playback", SettingKind::wholeNumber}};
};

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(std::make_unique<CardType>());
	types.push_back(std::make_unique<TestType>("source", Ports{{}, {"MONO"}}));
	types.push_back(std::make_unique<TestType>("sink", Ports{{"FL", "FR"}, {}}));
	types.push_back(std::make_unique<TestType>("broken", Ports{{"X"}, {"X"}}));
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

const Route noiseToFl{{"noise", "MONO"}, {"rec", "FL"}};
const Route noiseToFr{{"noise", "MONO"}, {"rec", "FR"}};
const Route hissToFl{{"hiss", "MONO"}, {"rec", "FL"}};

const NodeSpec noiseSpec{"noise", "source", json::object()};
const NodeSpec recSpec{"rec", "sink", json::object()};
NodeSpec deskSpec(int playback) { return {"desk", "card", {{"card", "MOTU"}, {"playback", playback}}}; }
const Route noiseToDesk1{{"noise", "MONO"}, {"desk", "playback_1"}};

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

} // namespace

TEST_CASE("A wanted node of a known type is added once and told once", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec}, {});
	REQUIRE(f.recorder.nodes.size() == 1);
	CHECK(f.recorder.nodes[0].first == "noise");
	CHECK(f.recorder.nodes[0].second->ports().outputs == std::vector<PortName>{"MONO"});

	SECTION("the same spec again is an empty plan") {
		auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
		REQUIRE(plan.has_value());
		CHECK(plan->empty());
		CHECK(f.recorder.nodes.size() == 1);
	}
	SECTION("a node that is no type's, or with invalid settings, is refused and changes nothing") {
		CHECK(planErrorOf<UnknownNodeType>(f.graph.plan(std::vector<NodeSpec>{{"noise", "pink", {}}}, {})).type == "pink");
		const auto result = f.graph.plan(std::vector<NodeSpec>{{"noise", "source", {{"bad", true}}}}, {});
		const auto& invalid = planErrorOf<InvalidSettings>(result);
		CHECK(invalid.node == "noise");
		CHECK(invalid.detail == "bad is not a setting");
		CHECK(f.recorder.nodes.size() == 1);
		CHECK(f.graph.nodes().at(0).spec == noiseSpec);
	}
}

TEST_CASE("plan throws a logic error for a type that declares a port name twice", "[desired_graph]") {
	Fixture f;
	CHECK_THROWS_AS(f.graph.plan(std::vector<NodeSpec>{{"odd", "broken", {}}}, {}), std::logic_error);
	CHECK(f.recorder.nodes.empty());
}

TEST_CASE("A wanted route is checked at both ends: node, port and direction", "[desired_graph]") {
	Fixture f;
	const std::vector<NodeSpec> both{noiseSpec, recSpec};
	const auto refused = [&](const Route& route) { return f.graph.plan(both, std::vector<Route>{route}); };

	CHECK(planErrorOf<UnknownNode>(refused({{"hiss", "MONO"}, {"rec", "FL"}})).node == "hiss");
	CHECK(planErrorOf<UnknownNode>(refused({{"noise", "MONO"}, {"tape", "FL"}})).node == "tape");
	CHECK(planErrorOf<UnknownPort>(refused({{"noise", "MONO"}, {"rec", "FX"}})).port == PortRef{"rec", "FX"});
	CHECK(planErrorOf<UnknownPort>(refused({{"noise", "L"}, {"rec", "FL"}})).port == PortRef{"noise", "L"});

	const Route backwards{{"rec", "FL"}, {"noise", "MONO"}};
	CHECK(planErrorOf<WrongDirection>(refused(backwards)).route == backwards);
	const Route inputToInput{{"rec", "FL"}, {"rec", "FR"}};
	CHECK(planErrorOf<WrongDirection>(refused(inputToInput)).route == inputToInput);
	const Route outputToOutput{{"noise", "MONO"}, {"noise", "MONO"}};
	CHECK(planErrorOf<WrongDirection>(refused(outputToOutput)).route == outputToOutput);

	CHECK(f.log.empty());
}

TEST_CASE("The catalogue finds types by name and refuses two types with one name", "[catalogue]") {
	const NodeCatalogue catalogue{testTypes()};
	REQUIRE(catalogue.find("sink") != nullptr);
	CHECK(catalogue.find("sink")->name() == "sink");
	CHECK(catalogue.find("pink") == nullptr);

	auto types = testTypes();
	types.push_back(std::make_unique<TestType>("sink", Ports{{"IN"}, {}}));
	CHECK_THROWS_AS(NodeCatalogue{std::move(types)}, std::invalid_argument);
}

TEST_CASE("Command errors have one code each and describe what they name", "[command_error]") {
	const Route route{{"rec", "FL"}, {"noise", "MONO"}};
	const std::vector<std::pair<CommandError, std::string>> cases{
		{UnknownNodeType{"pink"}, "unknown-node-type pink"},
		{InvalidSettings{"rec", "path: missing"}, "invalid-settings rec: path: missing"},
		{DuplicateNode{"rec"}, "duplicate-node rec"},
		{UnknownNode{"tape"}, "unknown-node tape"},
		{UnknownPort{{"rec", "FX"}}, "unknown-port rec:FX"},
		{WrongDirection{route}, "wrong-direction rec:FL -> noise:MONO"},
		{InvalidName{"a.b"}, "invalid-name a.b"},
		{PortsChanged{"motu"}, "ports-changed motu"},
		{UnknownRoute{{{"noise", "MONO"}, {"rec", "FR"}}}, "unknown-route noise:MONO -> rec:FR"},
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
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL"});
	CHECK(f.graph.routes() == std::vector<Route>{noiseToFr});

	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl, noiseToFr});
	CHECK(f.log.back() == "sub routeAdded noise:MONO -> rec:FL");
}

TEST_CASE("A node no longer wanted goes after each of its routes, and the name can come back",
          "[desired_graph]") {
	Fixture f;
	const NodeSpec hissSpec{"hiss", "source", json::object()};
	apply(f.graph, {noiseSpec, hissSpec, recSpec}, {noiseToFl, noiseToFr, hissToFl});
	f.log.clear();

	apply(f.graph, {hissSpec, recSpec}, {hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FR",
	                   "sub nodeRemoved noise"});
	CHECK(f.graph.routes() == std::vector<Route>{hissToFl});
	REQUIRE(f.graph.nodes().size() == 2);
	CHECK(f.graph.nodes()[0].spec.name == "hiss");
	CHECK(f.graph.nodes()[1].spec.name == "rec");

	apply(f.graph, {{"noise", "source", {{"level", 2}}}, hissSpec, recSpec}, {noiseToFl, hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FR",
	                   "sub nodeRemoved noise", "sub nodeAdded noise", "sub routeAdded noise:MONO -> rec:FL"});

	SECTION("a node with no routes is removed alone") {
		apply(f.graph, {{"noise", "source", {{"level", 2}}}, hissSpec, recSpec, {"lone", "source", {}}},
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
		CHECK(f.recorder.updates[0].second != f.recorder.nodes.at(0).second);   // a new config
		CHECK(f.recorder.updates[0].second->ports().inputs == std::vector<PortName>{"FL", "FR"});
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
	apply(f.graph, {{"noise-1_A", "source", {}}, {std::string(64, 'z'), "source", {}}}, {});
	CHECK(f.log.size() == 2);
}

TEST_CASE("Observers hear every change before any subscriber, whatever the registration order",
          "[desired_graph]") {
	Fixture f;   // its recorder subscribed first
	Recorder observer{f.log, "obs"};
	f.graph.observe(static_cast<DesiredNodeListener&>(observer));
	f.graph.observe(static_cast<DesiredRouteListener&>(observer));

	apply(f.graph, {noiseSpec, recSpec}, {});
	apply(f.graph, {noiseSpec, {"rec", "sink", {{"level", 2}}}}, {noiseToFl});
	apply(f.graph, {noiseSpec}, {});

	CHECK(f.log == Log{
		"obs nodeAdded noise", "sub nodeAdded noise",
		"obs nodeAdded rec", "sub nodeAdded rec",
		"obs nodeUpdated rec", "sub nodeUpdated rec",
		"obs routeAdded noise:MONO -> rec:FL", "sub routeAdded noise:MONO -> rec:FL",
		"obs routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FL",
		"obs nodeRemoved rec", "sub nodeRemoved rec",
	});
}

TEST_CASE("After close() plan and commit return stopping, and reads still answer", "[desired_graph]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl});
	auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec}, std::vector<Route>{});
	REQUIRE(plan.has_value());
	f.log.clear();

	(void)f.graph.close();
	(void)f.graph.close();   // again: another proof, nothing changes

	CHECK(std::holds_alternative<Stopping>(f.graph.plan(std::vector<NodeSpec>{noiseSpec, recSpec},
	                                                   std::vector<Route>{noiseToFl}).error()));   // even the same graph
	const auto committed = f.graph.commit(std::move(*plan));
	REQUIRE_FALSE(committed.has_value());
	CHECK(std::holds_alternative<Stopping>(committed.error()));
	CHECK(f.log.empty());

	const auto nodes = f.graph.nodes();
	REQUIRE(nodes.size() == 2);
	CHECK(nodes[0].spec == noiseSpec);
	CHECK(nodes[0].ports.outputs == std::vector<PortName>{"MONO"});
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
	CHECK(f.log.empty());   // plan changes nothing
	CHECK(f.graph.nodes().empty());
	REQUIRE(f.graph.commit(std::move(*plan)).has_value());

	CHECK(f.log == Log{"obs nodeAdded desk", "sub nodeAdded desk", "obs nodeAdded noise", "sub nodeAdded noise",
	                   "obs nodeAdded rec", "sub nodeAdded rec", "obs routeAdded noise:MONO -> desk:playback_1",
	                   "sub routeAdded noise:MONO -> desk:playback_1", "obs routeAdded noise:MONO -> rec:FL",
	                   "sub routeAdded noise:MONO -> rec:FL"});
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

TEST_CASE("plan with new settings and the same ports updates the node and keeps its routes",
          "[desired_graph][plan]") {
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
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> desk:playback_1", "sub nodeRemoved desk", "sub nodeAdded desk",
	                   "sub routeAdded noise:MONO -> desk:playback_1"});
	REQUIRE(f.recorder.nodes.size() == 4);
	CHECK(f.recorder.nodes[3].second->ports().inputs.size() == 4);

	SECTION("another type under the same name is a replacement too") {
		f.log.clear();
		apply(f.graph, {noiseSpec, recSpec, {"desk", "sink", json::object()}}, {noiseToFl});
		CHECK(f.log == Log{"sub routeRemoved noise:MONO -> desk:playback_1", "sub nodeRemoved desk",
		                   "sub nodeAdded desk"});
	}
}

TEST_CASE("plan removes what is no longer wanted, routes before nodes", "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, recSpec}, {noiseToFl, noiseToFr});
	f.log.clear();

	apply(f.graph, {recSpec, {"hiss", "source", json::object()}}, {hissToFl});
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FR",
	                   "sub nodeRemoved noise", "sub nodeAdded hiss", "sub routeAdded hiss:MONO -> rec:FL"});
}

TEST_CASE("plan refuses a wanted graph the catalogue or the ports refuse, and changes nothing",
          "[desired_graph][plan]") {
	Fixture f;
	apply(f.graph, {noiseSpec, deskSpec(4)}, {{{"noise", "MONO"}, {"desk", "playback_4"}}});
	f.log.clear();

	SECTION("a wanted route to a port that is gone") {
		const Route toFour{{"noise", "MONO"}, {"desk", "playback_4"}};
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
		const auto plan = f.graph.plan(std::vector<NodeSpec>{noiseSpec, {"x.y", "source", {}}, {"z", "nope", {}}},
		                               std::vector<Route>{{{"gone", "MONO"}, {"noise", "MONO"}}});
		CHECK(planErrorOf<InvalidName>(plan).node == "x.y");
	}
	SECTION("each node and route error") {
		CHECK(planErrorOf<UnknownNodeType>(f.graph.plan(std::vector<NodeSpec>{{"z", "nope", {}}}, {})).type == "nope");
		CHECK(planErrorOf<InvalidSettings>(
		          f.graph.plan(std::vector<NodeSpec>{{"z", "source", {{"bad", 1}}}}, {})).node == "z");
		CHECK(planErrorOf<UnknownNode>(f.graph.plan(std::vector<NodeSpec>{recSpec},
		                                            std::vector<Route>{noiseToFl})).node == "noise");
		CHECK(planErrorOf<WrongDirection>(
		          f.graph.plan(std::vector<NodeSpec>{noiseSpec, recSpec},
		                       std::vector<Route>{{{"rec", "FL"}, {"noise", "MONO"}}})).route.from.node == "rec");
	}
	CHECK(f.log.empty());
	CHECK(f.graph.nodes().size() == 2);
	CHECK(f.graph.nodes()[0].ports.inputs.size() == 4);
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
