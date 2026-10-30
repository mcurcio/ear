// The graph read (docs/design/batch2.md section 4.2, graph_view.hpp; C1 criterion 14): the join
// of the real desired graph, node framework and reconciler, wired as main.cpp wires them, with
// the engine faked behind their factory interfaces.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/graph_view.hpp"

using namespace ear;

namespace {

struct SilentCode final : AudioCode {
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>,
	             std::uint32_t) noexcept override {}
	void release() noexcept override {}
};

struct NullConsumer final : HandoffConsumer {
	void consume(std::span<const float>, std::uint32_t) override {}
	void finish() override {}
};

struct Insides final : NodeInsides {
	SilentCode code;
	NullConsumer consumer;
	AudioCode& audioCode() override { return code; }
};

/// Nodes of "recorder" ask for one handoff, so a removed one stays stopping until it closes.
struct Config final : NodeConfig {
	Ports ports_;
	bool handoff;
	Config(Ports p, bool h) : ports_(std::move(p)), handoff(h) {}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		auto insides = std::make_unique<Insides>();
		if (handoff) grant.handoff(1, 64, insides->consumer);
		return insides;
	}
};

struct Type final : NodeType {
	std::string name_;
	Ports ports;
	bool handoff;
	Type(std::string n, Ports p, bool h = false) : name_(std::move(n)), ports(std::move(p)), handoff(h) {}
	std::string_view name() const override { return name_; }
	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings> parse(const NodeName&,
	                                                                        const NodeSettings&) const override {
		return std::make_unique<Config>(ports, handoff);
	}
};

std::vector<std::unique_ptr<const NodeType>> types() {
	std::vector<std::unique_ptr<const NodeType>> all;
	all.push_back(std::make_unique<Type>("source", Ports{{}, {"MONO"}}));
	all.push_back(std::make_unique<Type>("recorder", Ports{{"FL", "FR"}, {}}, true));
	all.push_back(std::make_unique<Type>("card", Ports{{"OUT1"}, {"IN1"}, {"PB1"}, {"CAP1"}}));
	return all;
}

struct Block final : PortBlock {
	void drive(AudioCode*) override {}
};

struct Blocks final : PortBlockFactory {
	std::map<NodeName, PortBlockEvents*> events;   // the latest block of each name
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports&, const AudioFormat&,
	                                PortBlockEvents& e) override {
		events[node] = &e;
		return std::make_unique<Block>();
	}
};

struct Writer final : RtWriter {
	std::uint32_t write(std::span<const std::span<const float>>, std::uint32_t frames) noexcept override {
		return frames;
	}
};

struct PendingHandoff final : Handoff {
	std::vector<std::move_only_function<void(std::uint64_t)>>& pending;
	Writer writer_;
	explicit PendingHandoff(std::vector<std::move_only_function<void(std::uint64_t)>>& p) : pending(p) {}
	RtWriter& writer() noexcept override { return writer_; }
	void close(std::move_only_function<void(std::uint64_t)> closed) override { pending.push_back(std::move(closed)); }
};

struct Handoffs final : HandoffFactory {
	std::vector<std::move_only_function<void(std::uint64_t)>> pending;
	std::unique_ptr<Handoff> make(const NodeName&, std::uint32_t, std::uint32_t, HandoffConsumer&) override {
		return std::make_unique<PendingHandoff>(pending);
	}
	void closeAll() {
		auto all = std::move(pending);
		for (auto& closed : all) closed(0);
	}
};

struct NoCards final : SoundCardFactory {
	std::unique_ptr<SoundCard> make(const NodeName&, const NodeName&, const SoundCardRequest&, const AudioFormat&,
	                                SoundCardEvents&) override {
		FAIL("a sound card was made");
		return nullptr;
	}
};

struct Links final : LinkMaker {
	std::vector<LinkId> made;
	void createLink(LinkId link, EnginePortId, EnginePortId) override { made.push_back(link); }
	void destroyLink(LinkId) override {}
};

struct Fixture {
	NodeCatalogue catalogue{types()};
	DesiredGraph graph{catalogue};
	Blocks blocks;
	Handoffs handoffs;
	NoCards cards;
	Links links;
	NodeHost nodes{AudioFormat{48000, 256}, blocks, handoffs, cards};
	Reconciler routes{links};
	GraphStatus status{EngineView{AudioFormat{48000, 256}, true, "SCHED_FIFO 88"}, catalogue, graph, nodes, routes};
	Fixture() {
		graph.subscribe(static_cast<DesiredNodeListener&>(nodes));
		graph.subscribe(static_cast<DesiredRouteListener&>(routes));
	}
	~Fixture() {
		nodes.stopAll(graph.close(), [] {});
		handoffs.closeAll();
	}
};

const Route noiseToFl{{"noise", "MONO"}, {"rec", "FL"}};
const Route noiseToFr{{"noise", "MONO"}, {"rec", "FR"}};

} // namespace

TEST_CASE("The graph read joins the wanted graph with node and route status", "[graph_status]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"rec", "recorder", {}}).has_value());
	REQUIRE(f.graph.addNode({"noise", "source", {{"seed", 1}}}).has_value());
	REQUIRE(f.graph.addNode({"motu", "card", {}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	REQUIRE(f.graph.addRoute(noiseToFr).has_value());
	f.blocks.events.at("noise")->ready();
	f.blocks.events.at("motu")->lost("no driver");
	f.routes.portAppeared({{"noise", "MONO"}, Direction::output, 1});
	f.routes.portAppeared({{"rec", "FL"}, Direction::input, 2});
	REQUIRE(f.links.made.size() == 1);
	f.routes.linkActive(f.links.made.front());

	const GraphView view = f.status.read();

	CHECK(view.engine.format.rate == 48000);
	CHECK(view.engine.format.maxBlock == 256);
	CHECK(view.engine.realtime);
	CHECK(view.engine.scheduling == "SCHED_FIFO 88");
	CHECK(view.types == std::vector<std::string>{"card", "recorder", "source"});

	REQUIRE(view.nodes.size() == 3);
	CHECK(view.nodes[0].spec == NodeSpec{"motu", "card", {}});
	CHECK(view.nodes[0].state == NodeState::failed);
	REQUIRE(view.nodes[0].reason.has_value());
	CHECK(view.nodes[0].reason->detail == "no driver");
	// Public ports only: inside ports are the card's own.
	CHECK(view.nodes[0].ports.inputs == std::vector<PortName>{"OUT1"});
	CHECK(view.nodes[0].ports.outputs == std::vector<PortName>{"IN1"});
	CHECK(view.nodes[0].ports.insideInputs.empty());
	CHECK(view.nodes[0].ports.insideOutputs.empty());
	CHECK(view.nodes[1].spec == NodeSpec{"noise", "source", {{"seed", 1}}});
	CHECK(view.nodes[1].state == NodeState::running);
	CHECK_FALSE(view.nodes[1].reason.has_value());
	CHECK(view.nodes[2].spec.name == "rec");
	CHECK(view.nodes[2].state == NodeState::starting);

	REQUIRE(view.routes.size() == 2);
	CHECK(view.routes[0].route == noiseToFl);
	CHECK(view.routes[0].match == RouteMatch::linked);
	CHECK(view.routes[1].route == noiseToFr);
	CHECK(view.routes[1].match == RouteMatch::pending);
}

TEST_CASE("A removed node still stopping is absent, and a re-added name shows the new node", "[graph_status]") {
	Fixture f;
	REQUIRE(f.graph.addNode({"rec", "recorder", {}}).has_value());
	REQUIRE(f.graph.addNode({"noise", "source", {}}).has_value());
	REQUIRE(f.graph.addRoute(noiseToFl).has_value());
	f.blocks.events.at("rec")->ready();
	REQUIRE(f.nodes.status().at(0).state == NodeState::running);

	REQUIRE(f.graph.removeNode("rec").has_value());
	REQUIRE(f.nodes.status().size() == 2);   // rec waits for its handoff to close
	CHECK(f.nodes.status().at(0).state == NodeState::stopping);

	GraphView view = f.status.read();
	REQUIRE(view.nodes.size() == 1);
	CHECK(view.nodes[0].spec.name == "noise");
	CHECK(view.routes.empty());

	REQUIRE(f.graph.addNode({"rec", "recorder", {}}).has_value());
	REQUIRE(f.nodes.status().size() == 3);   // the old rec, still stopping, comes first
	view = f.status.read();
	REQUIRE(view.nodes.size() == 2);
	CHECK(view.nodes[1].spec.name == "rec");
	CHECK(view.nodes[1].state == NodeState::starting);

	f.blocks.events.at("rec")->ready();
	view = f.status.read();
	CHECK(view.nodes[1].state == NodeState::running);
}
