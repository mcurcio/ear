// The desired graph's control calls, checked against a catalogue of test node types: the errors
// each call returns, the events it sends, and the catalogue's own boot-time check
// (docs/design/m1-white-to-file.md section 3, S2 criteria 1-3).
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
		return std::make_unique<TestConfig>(ports_);
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

struct Recorder final : DesiredNodeListener, DesiredRouteListener {
	std::vector<std::pair<NodeName, std::shared_ptr<const NodeConfig>>> nodes;
	std::vector<Route> routes;
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override {
		nodes.emplace_back(node.name, std::move(config));
	}
	void nodeUpdated(const NodeSpec&, std::shared_ptr<const NodeConfig>) override {}
	void nodeRemoved(const NodeName&) override {}
	void routeAdded(const Route& route) override { routes.push_back(route); }
	void routeRemoved(const Route&) override {}
};

struct Fixture {
	NodeCatalogue catalogue{testTypes()};
	DesiredGraph graph{catalogue};
	Recorder recorder;
	Fixture() {
		graph.subscribe(static_cast<DesiredNodeListener&>(recorder));
		graph.subscribe(static_cast<DesiredRouteListener&>(recorder));
	}
};

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
