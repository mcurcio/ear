// The desired graph's control calls, checked against a catalogue of test node types: the errors
// each call returns, the events it sends, and the catalogue's own boot-time check
// (docs/design/m1-white-to-file.md section 3, S2 criteria 1-3; docs/design/batch2.md section 4.1,
// C1 criteria 1-6: removing and updating, the name rule, observers first, close()).
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/desired_graph.hpp"

using namespace ear;

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
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName& node,
	                                                                        const NodeSettings& settings) const override {
		if (settings.contains("bad")) return std::unexpected(InvalidSettings{node, "bad is not a setting"});
		Ports ports = ports_;   // {"extra": "<name>"} adds an input port of that name
		if (settings.contains("extra")) ports.inputs.push_back(settings["extra"].get<std::string>());
		return std::make_unique<TestConfig>(std::move(ports));
	}

private:
	std::string name_;
	Ports ports_;
};

std::vector<std::unique_ptr<const NodeType>> testTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
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

template <class Error> const Error& errorOf(const std::expected<void, CommandError>& result) {
	REQUIRE_FALSE(result.has_value());
	REQUIRE(std::holds_alternative<Error>(result.error()));
	return std::get<Error>(result.error());
}

} // namespace

TEST_CASE("addNode records a node of a known type and tells listeners once", "[desired_graph]") {
	Fixture f;
	const NodeSpec noise{"noise", "source", {}};

	REQUIRE(f.graph.addNode(noise).has_value());
	REQUIRE(f.recorder.nodes.size() == 1);
	CHECK(f.recorder.nodes[0].first == "noise");
	CHECK(f.recorder.nodes[0].second->ports().outputs == std::vector<PortName>{"MONO"});

	SECTION("the same spec again succeeds with no event") {
		REQUIRE(f.graph.addNode(noise).has_value());
		CHECK(f.recorder.nodes.size() == 1);
	}
	SECTION("the same name with different settings is a duplicate node") {
		CHECK(errorOf<DuplicateNode>(f.graph.addNode({"noise", "source", {{"level", 1}}})).node == "noise");
		CHECK(f.recorder.nodes.size() == 1);
	}
	SECTION("the same name with a different type is a duplicate node") {
		CHECK(errorOf<DuplicateNode>(f.graph.addNode({"noise", "sink", {}})).node == "noise");
	}
}

TEST_CASE("addNode refuses an unknown type and invalid settings without an event", "[desired_graph]") {
	Fixture f;

	CHECK(errorOf<UnknownNodeType>(f.graph.addNode({"noise", "pink", {}})).type == "pink");
	const auto result = f.graph.addNode({"noise", "source", {{"bad", true}}});
	const auto& invalid = errorOf<InvalidSettings>(result);
	CHECK(invalid.node == "noise");
	CHECK(invalid.detail == "bad is not a setting");
	CHECK(f.recorder.nodes.empty());

	// Neither attempt recorded the name.
	CHECK(f.graph.addNode({"noise", "source", {}}).has_value());
}

TEST_CASE("addNode throws a logic error for a type that declares a port name twice", "[desired_graph]") {
	Fixture f;
	CHECK_THROWS_AS(f.graph.addNode({"odd", "broken", {}}), std::logic_error);
	CHECK(f.recorder.nodes.empty());
}

TEST_CASE("addRoute checks both ends' nodes, ports and directions", "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());

	CHECK(errorOf<UnknownNode>(f.graph.addRoute({{"hiss", "MONO"}, {"rec", "FL"}})).node == "hiss");
	CHECK(errorOf<UnknownNode>(f.graph.addRoute({{"noise", "MONO"}, {"tape", "FL"}})).node == "tape");
	CHECK(errorOf<UnknownPort>(f.graph.addRoute({{"noise", "MONO"}, {"rec", "FX"}})).port == PortRef{"rec", "FX"});
	CHECK(errorOf<UnknownPort>(f.graph.addRoute({{"noise", "L"}, {"rec", "FL"}})).port == PortRef{"noise", "L"});

	const Route backwards{{"rec", "FL"}, {"noise", "MONO"}};
	CHECK(errorOf<WrongDirection>(f.graph.addRoute(backwards)).route == backwards);
	const Route inputToInput{{"rec", "FL"}, {"rec", "FR"}};
	CHECK(errorOf<WrongDirection>(f.graph.addRoute(inputToInput)).route == inputToInput);
	const Route outputToOutput{{"noise", "MONO"}, {"noise", "MONO"}};
	CHECK(errorOf<WrongDirection>(f.graph.addRoute(outputToOutput)).route == outputToOutput);

	CHECK(f.recorder.routes.empty());
}

TEST_CASE("addRoute records a route once and tells listeners once", "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());
	const Route left{{"noise", "MONO"}, {"rec", "FL"}};
	const Route right{{"noise", "MONO"}, {"rec", "FR"}};

	REQUIRE(f.graph.addRoute(left).has_value());
	REQUIRE(f.graph.addRoute(left).has_value());
	REQUIRE(f.graph.addRoute(right).has_value());

	CHECK(f.recorder.routes == std::vector<Route>{left, right});
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

TEST_CASE("removeRoute removes a wanted route once and refuses one not wanted", "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	REQUIRE(f.graph.addRoute(noiseToFr).has_value());
	f.log.clear();

	REQUIRE(f.graph.removeRoute(noiseToFl).has_value());
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL"});
	CHECK(f.graph.routes() == std::vector<Route>{noiseToFr});

	CHECK(errorOf<UnknownRoute>(f.graph.removeRoute(noiseToFl)).route == noiseToFl);
	CHECK(f.log.size() == 1);

	// It can be wanted again.
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	CHECK(f.log.back() == "sub routeAdded noise:MONO -> rec:FL");
}

TEST_CASE("removeNode removes each of its routes before the node, and the name can be added again",
          "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"hiss", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	REQUIRE(f.graph.addRoute(noiseToFr).has_value());
	REQUIRE(f.graph.addRoute(hissToFl).has_value());
	f.log.clear();

	REQUIRE(f.graph.removeNode("noise").has_value());
	CHECK(f.log == Log{"sub routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FR",
	                   "sub nodeRemoved noise"});
	CHECK(f.graph.routes() == std::vector<Route>{hissToFl});
	REQUIRE(f.graph.nodes().size() == 2);
	CHECK(f.graph.nodes()[0].spec.name == "hiss");
	CHECK(f.graph.nodes()[1].spec.name == "rec");

	CHECK(errorOf<UnknownNode>(f.graph.removeNode("noise")).node == "noise");
	CHECK(errorOf<UnknownNode>(f.graph.addRoute(noiseToFl)).node == "noise");
	CHECK(f.log.size() == 3);

	REQUIRE(f.graph.addNode({"noise", "source", {{"level", 2}}}).has_value());
	CHECK(f.log.back() == "sub nodeAdded noise");
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());

	SECTION("a node with no routes is removed alone") {
		REQUIRE(f.graph.addNode({"lone", "source", {}}).has_value());
		f.log.clear();
		REQUIRE(f.graph.removeNode("lone").has_value());
		CHECK(f.log == Log{"sub nodeRemoved lone"});
	}
}

TEST_CASE("updateNode records new settings with the same ports and tells nodeUpdated", "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"rec", "sink", {{"level", 1}}}).has_value());
	f.log.clear();

	SECTION("the same settings succeed with no event") {
		REQUIRE(f.graph.updateNode("rec", {{"level", 1}}).has_value());
		CHECK(f.log.empty());
	}
	SECTION("an unknown node") {
		CHECK(errorOf<UnknownNode>(f.graph.updateNode("tape", {})).node == "tape");
		CHECK(f.log.empty());
	}
	SECTION("invalid settings change nothing") {
		CHECK(errorOf<InvalidSettings>(f.graph.updateNode("rec", {{"bad", true}})).node == "rec");
		CHECK(f.log.empty());
		CHECK(f.graph.nodes().at(0).spec.settings == NodeSettings{{"level", 1}});
	}
	SECTION("settings that change the ports are refused") {
		CHECK(errorOf<PortsChanged>(f.graph.updateNode("rec", {{"extra", "FC"}})).node == "rec");
		CHECK(f.log.empty());
		CHECK(f.graph.nodes().at(0).spec.settings == NodeSettings{{"level", 1}});
		CHECK(f.graph.nodes().at(0).ports.inputs == std::vector<PortName>{"FL", "FR"});
	}
	SECTION("new settings with the same ports") {
		REQUIRE(f.graph.updateNode("rec", {{"level", 2}}).has_value());
		CHECK(f.log == Log{"sub nodeUpdated rec"});
		REQUIRE(f.recorder.updates.size() == 1);
		CHECK(f.recorder.updates[0].first == NodeSpec{"rec", "sink", {{"level", 2}}});
		CHECK(f.recorder.updates[0].second != f.recorder.nodes.at(0).second);   // a new config
		CHECK(f.recorder.updates[0].second->ports().inputs == std::vector<PortName>{"FL", "FR"});
		CHECK(f.graph.nodes().at(0).spec.settings == NodeSettings{{"level", 2}});
		// The recorded spec is the new one: adding the old spec again is a duplicate.
		CHECK(errorOf<DuplicateNode>(f.graph.addNode({"rec", "sink", {{"level", 1}}})).node == "rec");
		CHECK(f.graph.addNode({"rec", "sink", {{"level", 2}}}).has_value());
	}
}

TEST_CASE("A node name is 1-64 characters of letters, digits, '_' and '-'", "[desired_graph]") {
	Fixture f;
	for (const std::string& name : {std::string(), std::string(65, 'a'), std::string("a.b"), std::string("a b"),
	                               std::string("a:b"), std::string("rec/1")}) {
		INFO(name);
		CHECK(errorOf<InvalidName>(f.graph.addNode({name, "source", {}})).node == name);
	}
	CHECK(f.log.empty());
	CHECK(f.graph.addNode({"noise-1_A", "source", {}}).has_value());
	CHECK(f.graph.addNode({std::string(64, 'z'), "source", {}}).has_value());
	CHECK(f.log.size() == 2);
}

TEST_CASE("Observers hear every change before any subscriber, whatever the registration order",
          "[desired_graph]") {
	Fixture f;   // its recorder subscribed first
	Recorder observer{f.log, "obs"};
	f.graph.observe(static_cast<DesiredNodeListener&>(observer));
	f.graph.observe(static_cast<DesiredRouteListener&>(observer));

	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());
	REQUIRE(f.graph.updateNode("rec", {{"level", 2}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	REQUIRE(f.graph.removeRoute(noiseToFl).has_value());
	REQUIRE(f.graph.addRoute(noiseToFr).has_value());
	REQUIRE(f.graph.removeNode("rec").has_value());

	CHECK(f.log == Log{
		"obs nodeAdded noise", "sub nodeAdded noise",
		"obs nodeAdded rec", "sub nodeAdded rec",
		"obs nodeUpdated rec", "sub nodeUpdated rec",
		"obs routeAdded noise:MONO -> rec:FL", "sub routeAdded noise:MONO -> rec:FL",
		"obs routeRemoved noise:MONO -> rec:FL", "sub routeRemoved noise:MONO -> rec:FL",
		"obs routeAdded noise:MONO -> rec:FR", "sub routeAdded noise:MONO -> rec:FR",
		"obs routeRemoved noise:MONO -> rec:FR", "sub routeRemoved noise:MONO -> rec:FR",
		"obs nodeRemoved rec", "sub nodeRemoved rec",
	});
}

TEST_CASE("After close() every edit returns stopping and tells nobody, and reads still answer",
          "[desired_graph]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addNode({"rec", "sink", {}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	f.log.clear();

	(void)f.graph.close();
	(void)f.graph.close();   // again: another proof, nothing changes

	const std::vector<std::expected<void, CommandError>> results{
		f.graph.addNode({"hiss", "source", {}}),
		f.graph.addNode({"noise", "source", {}}),   // even the same spec again
		f.graph.updateNode("rec", {{"level", 2}}),
		f.graph.removeNode("noise"),
		f.graph.addRoute(noiseToFr),
		f.graph.removeRoute(noiseToFl),
	};
	for (const auto& result : results) errorOf<Stopping>(result);
	CHECK(f.log.empty());

	const auto nodes = f.graph.nodes();
	REQUIRE(nodes.size() == 2);
	CHECK(nodes[0].spec == NodeSpec{"noise", "source", {}});
	CHECK(nodes[0].ports.outputs == std::vector<PortName>{"MONO"});
	CHECK(nodes[1].spec == NodeSpec{"rec", "sink", {}});
	CHECK(f.graph.routes() == std::vector<Route>{noiseToFl});
}
