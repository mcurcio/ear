// The graph read (docs/architecture.md#packages, graph_view.hpp): the join
// of the real desired graph, node framework and reconciler, wired as main.cpp wires them, with
// the engine faked behind their factory interfaces.
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include "catch2/catch.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/lifecycle.hpp"

using namespace ear;

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace graph_status_test {

struct SilentCode final : AudioCode {
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const GraphCycle&) noexcept override {}
	void release() noexcept override {}
};

struct NullConsumer final : SinkConsumer {
	void consume(const Chunk&) override {}
	void finish() override {}
};

constexpr auto seedRow = setting::optional("seed", setting::number(0, 100));
constexpr auto noise = bank::output<"noise">;
constexpr auto record = bank::input<"record">;
constexpr auto tap = bank::insideOutput<"tap">;
constexpr auto play = bank::input<"play">;
constexpr auto capture = bank::output<"capture">;
constexpr auto fromCard = bank::insideInput<"from_card">;
constexpr auto toCard = bank::insideOutput<"to_card">;

/// "source": output noise.mono, and a silent code.
constexpr TypeSpec sourceSpec{.settings = SettingsTable<seedRow>{"source"}, .banks = Banks<noise>{}};
struct SourceType {
	static Members<sourceSpec> ports(const CheckedOf<sourceSpec>&) { return {noise(members("mono"))}; }
	Configured configure(CreateOf<sourceSpec>& cx, const CheckedOf<sourceSpec>&) {
		cx.use(std::make_unique<SilentCode>());
		return Configured::applied();
	}
};

/// "recorder": inputs record.left and record.right, and a sink on its inside bank tap, so a removed one stays stopping
/// until the sink closes.
constexpr TypeSpec recorderSpec{.settings = SettingsTable<seedRow>{"recorder"}, .banks = Banks<record, tap>{}};
struct RecorderType {
	static Members<recorderSpec> ports(const CheckedOf<recorderSpec>&) {
		return {record(members("left", "right")), tap(members("left", "right"))};
	}
	Configured configure(CreateOf<recorderSpec>& cx, const CheckedOf<recorderSpec>&) {
		cx.sink(tap, SinkShape{.frames = 64}).consumedBy(std::make_unique<NullConsumer>());
		cx.use(std::make_unique<SilentCode>());
		return Configured::applied();
	}
};

/// "card": public play.1 and capture.1, and inside banks a card's companions would join (none is asked for).
constexpr TypeSpec cardSpec{.settings = SettingsTable<seedRow>{"card"},
                            .banks = Banks<play, capture, fromCard, toCard>{}};
struct CardType {
	static Members<cardSpec> ports(const CheckedOf<cardSpec>&) {
		return {play(members(1)), capture(members(1)), fromCard(members(1)), toCard(members(1))};
	}
	Configured configure(CreateOf<cardSpec>& cx, const CheckedOf<cardSpec>&) {
		cx.use(std::make_unique<SilentCode>());
		return Configured::applied();
	}
};

std::vector<std::unique_ptr<const NodeType>> types() {
	std::vector<std::unique_ptr<const NodeType>> all;
	all.push_back(makeNodeType<sourceSpec, SourceType>());
	all.push_back(makeNodeType<recorderSpec, RecorderType>());
	all.push_back(makeNodeType<cardSpec, CardType>());
	return all;
}

/// Completions wait in the factory until the test runs them, as the engine runs them on the main loop later
/// (docs/engine.md#drive-and-fade).
using Completions = std::vector<std::move_only_function<void()>>;

struct Block final : PortBlock {
	Completions& completions;
	NodeCounts counted; // what counts() answers: the test sets it, as a real block's cycles would
	explicit Block(Completions& c) : completions(c) {}
	void drive(DriveRequest, std::move_only_function<void(std::uint64_t)> done) override {
		completions.push_back([done = std::move(done)]() mutable { done(0); });
	}
	void fadeOut(std::move_only_function<void()> done) override { completions.push_back(std::move(done)); }
	void settle() override {}
	std::uint64_t takenAt() const noexcept override { return 0; }
	std::uint32_t blockId() const noexcept override { return 0xFFFFFFFFU; }
	NodeCounts counts() const noexcept override { return counted; }
};

struct Blocks final : PortBlockFactory {
	std::map<NodeName, PortBlockEvents*> events; // the latest block of each name
	std::map<NodeName, Block*> made;             // the latest block of each name
	Completions completions;
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports&, const AudioFormat&,
	                                PortBlockEvents& e) override {
		events[node] = &e;
		auto block = std::make_unique<Block>(completions);
		made[node] = block.get();
		return block;
	}
	/// Runs every completion, and those they cause, in order.
	void complete() {
		while (!completions.empty()) {
			auto now = std::move(completions);
			completions.clear();
			for (auto& done : now) done();
		}
	}
};

/// A sink whose consumer begins at once and whose close waits until the test closes every sink, as a sink's thread
/// finishes later.
struct PendingSink final : Sink, SinkTap {
	Executor& mainLoop;
	std::unique_ptr<SinkConsumer> consumer;
	std::move_only_function<void(std::uint64_t)> closed;
	explicit PendingSink(Executor& m) : mainLoop(m) {}
	SinkTap& tap() noexcept override { return *this; }
	void begin(std::unique_ptr<SinkConsumer> c, std::move_only_function<void()> begun) override {
		consumer = std::move(c);
		consumer->begin();
		mainLoop.post(std::move(begun));
	}
	void close(std::move_only_function<void(std::uint64_t)> c) override { closed = std::move(c); }
	std::uint64_t droppedFrames() const noexcept override { return 0; }
	void write(std::span<const std::span<const float>>, std::uint32_t, const ChunkStamp&) noexcept override {}
};

struct PendingSinks final : SinkAdapter {
	Executor& mainLoop;
	std::vector<PendingSink*> made; // each sink made; one closed by closeAll may since have been freed
	explicit PendingSinks(Executor& m) : mainLoop(m) {}
	std::unique_ptr<Sink> make(const NodeName&, std::string_view, std::uint32_t, std::uint32_t, SampleRate) override {
		auto sink = std::make_unique<PendingSink>(mainLoop);
		made.push_back(sink.get());
		return sink;
	}
	/// Every sink told to close finishes its consumer and reports closed, on the main thread.
	void closeAll() {
		auto all = std::exchange(made, {});
		for (PendingSink* sink : all) {
			if (!sink->closed) {
				made.push_back(sink); // not told to close yet: still open
				continue;
			}
			sink->consumer->finish();
			std::exchange(sink->closed, nullptr)(0);
		}
	}
};

struct NoCards final : SoundCardCompanions {
	std::unique_ptr<EngineCompanion> make(const CompanionTarget&, const SoundCardShape&,
	                                      EngineCompanionEvents&) override {
		FAIL("a sound card was made");
		return nullptr;
	}
};

struct NoReceivers final : RtpReceiverCompanions {
	std::unique_ptr<EngineCompanion> make(const CompanionTarget&, const RtpReceiverShape&,
	                                      EngineCompanionEvents&) override {
		FAIL("an RTP receiver was made");
		return nullptr;
	}
};

/// The main loop as the test turns it: NodeHost posts its per-turn flush here (docs/architecture.md#isolation).
struct Turns final : Executor {
	std::vector<std::move_only_function<void()>> posted;
	void post(std::move_only_function<void()> work) override { posted.push_back(std::move(work)); }
};

/// Close deadlines that never come due: every run here closes in time.
struct NoTimers final : Timers {
	struct Never final : Timer {};
	std::unique_ptr<Timer> after(std::chrono::milliseconds, std::move_only_function<void()>) override {
		return std::make_unique<Never>();
	}
	std::chrono::steady_clock::time_point now() const override { return {}; }
};

struct Links final : LinkMaker {
	std::vector<LinkId> made;
	void createLink(LinkId link, EnginePortId, EnginePortId) override { made.push_back(link); }
	void destroyLink(LinkId) override {}
};

/// No node here asks for a source.
struct NoSources final : SourceAdapter {
	std::unique_ptr<Source> make(const NodeName&, std::string_view, std::uint32_t, std::uint32_t, SampleRate,
	                             std::unique_ptr<SourceClock>) override {
		throw std::logic_error("no source in these tests");
	}
};

struct Fixture {
	NodeCatalogue catalogue{types()};
	DesiredGraph graph{catalogue};
	Blocks blocks;
	NoCards cards;
	NoReceivers receivers;
	Links links;
	Turns mainLoop;
	NoTimers timers;
	PendingSinks sinks{mainLoop};
	NoSources sources;
	ResourceRegistry resources{
		ResourceAdapters{.cards = &cards, .receivers = &receivers, .sinks = &sinks, .sources = &sources}};
	NodeHost nodes{AudioFormat{ear::SampleRate::fixed<48000>(), 256}, blocks, resources, mainLoop, timers};
	Reconciler routes{links};
	GraphStatus status{EngineView{AudioFormat{ear::SampleRate::fixed<48000>(), 256}, true, "SCHED_FIFO 88"}, catalogue,
	                   graph, nodes, routes};
	Fixture() {
		graph.subscribe(static_cast<DesiredNodeListener&>(nodes));
		graph.subscribe(static_cast<DesiredRouteListener&>(routes));
	}
	~Fixture() {
		nodes.stopAll(graph.close(), [] {});
		settle();
		sinks.closeAll();
		settle();
	}
	/// The main loop turns until nothing is left: the flushes NodeHost posted and every completion, in order.
	void settle() {
		while (!mainLoop.posted.empty() || !blocks.completions.empty()) {
			auto now = std::move(mainLoop.posted);
			mainLoop.posted.clear();
			for (auto& work : now) work();
			blocks.complete();
		}
	}
};

const NodeSettings noSettings = NodeSettings::object();
const Route noiseToFl{{"noise", "noise.mono"}, {"rec", "record.left"}};
const Route noiseToFr{{"noise", "noise.mono"}, {"rec", "record.right"}};

/// Plans `nodes` and `routes` and commits the plan, which must be accepted.
void apply(DesiredGraph& graph, const std::vector<NodeSpec>& nodes, const std::vector<Route>& routes) {
	auto plan = graph.plan(nodes, routes);
	REQUIRE(plan.has_value());
	REQUIRE(graph.commit(std::move(*plan)).has_value());
}

} // namespace graph_status_test
using namespace graph_status_test;

TEST_CASE("The graph read joins the wanted graph with node and route status", "[graph_status]") {
	Fixture f;
	apply(f.graph, {{"rec", "recorder", noSettings}, {"noise", "source", {{"seed", 1}}}, {"motu", "card", noSettings}},
	      {noiseToFl, noiseToFr});
	f.settle(); // the turn's flush makes the blocks
	f.blocks.events.at("noise")->ready();
	f.blocks.events.at("motu")->lost("no driver");
	f.settle(); // noise's drive
	f.routes.portAppeared({{"noise", "noise.mono"}, Direction::output, 1});
	f.routes.portAppeared({{"rec", "record.left"}, Direction::input, 2});
	REQUIRE(f.links.made.size() == 1);
	f.routes.linkActive(f.links.made.front());

	const GraphView view = f.status.read();

	CHECK(view.engine.format.rate.hz() == 48000);
	CHECK(view.engine.format.maxBlock == 256);
	CHECK(view.engine.realtime);
	CHECK(view.engine.scheduling == "SCHED_FIFO 88");
	CHECK(view.types == std::vector<std::string>{"card", "recorder", "source"});

	REQUIRE(view.nodes.size() == 3);
	CHECK(view.nodes[0].spec == NodeSpec{"motu", "card", noSettings});
	CHECK(view.nodes[0].health.state == NodeState::failed);
	REQUIRE(view.nodes[0].health.reason.has_value());
	CHECK(view.nodes[0].health.reason->detail == "no driver");
	// Public ports only: inside ports are the card's own.
	CHECK(view.nodes[0].ports.inputs() == std::vector<PortName>{"play.1"});
	CHECK(view.nodes[0].ports.outputs() == std::vector<PortName>{"capture.1"});
	CHECK(view.nodes[0].ports.insideInputs().empty());
	CHECK(view.nodes[0].ports.insideOutputs().empty());
	CHECK(view.nodes[1].spec == NodeSpec{"noise", "source", {{"seed", 1}}});
	CHECK(view.nodes[1].health.state == NodeState::running);
	CHECK_FALSE(view.nodes[1].health.reason.has_value());
	CHECK(view.nodes[2].spec.name == "rec");
	CHECK(view.nodes[2].health.state == NodeState::starting);

	REQUIRE(view.routes.size() == 2);
	CHECK(view.routes[0].route == noiseToFl);
	CHECK(view.routes[0].match == RouteMatch::linked);
	CHECK(view.routes[1].route == noiseToFr);
	CHECK(view.routes[1].match == RouteMatch::pending);
}

/// The graph's counters, set by hand.
struct HandGraphCounts final : GraphCounterSource {
	GraphCounts counts;
	GraphCounts graphCounts() const noexcept override { return counts; }
};

TEST_CASE("The graph read carries the graph's counters and each node's, read with the rest",
          "[graph_status][counters]") {
	Fixture f;
	HandGraphCounts engine;
	engine.counts = GraphCounts{.cycles = 900, .cutCycles = 4, .stalls = 1};
	const GraphStatus status{EngineView{AudioFormat{ear::SampleRate::fixed<48000>(), 256}, true, "SCHED_FIFO 88"},
	                         f.catalogue,
	                         f.graph,
	                         f.nodes,
	                         f.routes,
	                         &engine};
	apply(f.graph, {{"rec", "recorder", noSettings}, {"noise", "source", noSettings}}, {});
	f.settle();
	f.blocks.made.at("rec")->counted = NodeCounts{.missedCycles = 3, .overruns = 1};
	const GraphView view = status.read();
	CHECK(view.counts == engine.counts);
	REQUIRE(view.nodes.size() == 2);
	CHECK(view.nodes[0].spec.name == "noise");
	CHECK(view.nodes[0].counts == NodeCounts{});
	CHECK(view.nodes[1].counts == NodeCounts{.missedCycles = 3, .overruns = 1});
	CHECK(f.status.read().counts == GraphCounts{}); // a read with no source of the graph's counters reads none
}

TEST_CASE("A removed node still stopping is absent, and a re-added name shows the new node", "[graph_status]") {
	Fixture f;
	apply(f.graph, {{"rec", "recorder", noSettings}, {"noise", "source", noSettings}}, {noiseToFl});
	f.settle();
	f.blocks.events.at("rec")->ready();
	f.settle();
	REQUIRE(f.nodes.health().at(1).name == "rec"); // a commit adds in name order
	REQUIRE(f.nodes.health().at(1).state == NodeState::running);

	apply(f.graph, {{"noise", "source", noSettings}}, {});
	f.settle();                            // rec's silence, then its block destroyed
	REQUIRE(f.nodes.health().size() == 2); // rec waits for its sink to close
	CHECK(f.nodes.health().at(1).state == NodeState::stopping);

	GraphView view = f.status.read();
	REQUIRE(view.nodes.size() == 1);
	CHECK(view.nodes[0].spec.name == "noise");
	CHECK(view.routes.empty());

	apply(f.graph, {{"noise", "source", noSettings}, {"rec", "recorder", noSettings}}, {});
	REQUIRE(f.nodes.health().size() == 3); // the old rec, still stopping, comes first
	view = f.status.read();
	REQUIRE(view.nodes.size() == 2);
	CHECK(view.nodes[1].spec.name == "rec");
	CHECK(view.nodes[1].health.state == NodeState::starting);

	f.settle(); // the new rec's block
	f.blocks.events.at("rec")->ready();
	view = f.status.read();
	CHECK(view.nodes[1].health.state == NodeState::starting); // its insides wait for the old rec

	f.sinks.closeAll();
	f.settle(); // the new rec's drive
	REQUIRE(f.nodes.health().size() == 2);
	view = f.status.read();
	CHECK(view.nodes[1].health.state == NodeState::running);
}

TEST_CASE("A node NodeHost has not told yet reads its health from its spec, and a disabled one reads disabled",
          "[graph_status]") {
	const NodeHealth added = healthFromSpec(NodeSpec{"noise", "source", noSettings});
	CHECK(added.name == "noise");
	CHECK(added.phase == NodePhase::added);
	CHECK(added.state == NodeState::starting);
	CHECK(added.wanted == Wanted::running);
	CHECK_FALSE(added.blockLost);
	NodeSpec quiet{"quiet", "source", noSettings};
	quiet.disabled = true;
	const NodeHealth disabled = healthFromSpec(quiet);
	CHECK(disabled.phase == NodePhase::disabled);
	CHECK(disabled.state == NodeState::disabled);
	CHECK(disabled.wanted == Wanted::disabled);

	Fixture f;
	apply(f.graph, {{"noise", "source", noSettings}, quiet}, {});
	f.settle();
	f.blocks.events.at("noise")->ready();
	f.settle();
	const GraphView view = f.status.read();
	REQUIRE(view.nodes.size() == 2);
	CHECK(view.nodes[0].health.phase == NodePhase::running);
	CHECK(view.nodes[0].health.wanted == Wanted::running);
	CHECK(view.nodes[1].spec.disabled);
	CHECK(view.nodes[1].health.phase == NodePhase::disabled);
	CHECK(view.nodes[1].health.wanted == Wanted::disabled);
	CHECK_FALSE(view.nodes[0].since.has_value()); // the gateway's to fill
}
