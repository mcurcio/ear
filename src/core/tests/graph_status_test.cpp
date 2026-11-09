// The graph read (docs/design/batch2.md section 4.2, graph_view.hpp; C1 criterion 14): the join
// of the real desired graph, node framework and reconciler, wired as main.cpp wires them, with
// the engine faked behind their factory interfaces.
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/settings_table.hpp"

using namespace ear;

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace graph_status_test {

struct SilentCode final : AudioCode {
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const GraphCycle&) noexcept override {}
	void release() noexcept override {}
};

struct NullConsumer final : HandoffConsumer {
	void consume(std::span<const float>, std::uint32_t) override {}
	void finish() override {}
};

struct Run final : RunObject {
	NullConsumer consumer;
};

/// Nodes of "recorder" ask for one handoff, so a removed one stays stopping until it closes.
struct Config final : NodeConfig {
	Ports ports_;
	bool handoff;
	Config(Ports p, bool h) : ports_(std::move(p)), handoff(h) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& context) const override {
		auto run = std::make_unique<Run>();
		if (handoff) context.acquire(HandoffRequest{"record", 64, &run->consumer});
		return StartResult{
			std::make_unique<SilentCode>(),
			[run = std::move(run)](Ready&) mutable -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
				return std::move(run);
			}};
	}
};

constexpr auto seedRow = setting::optional("seed", setting::number(0, 100));
constexpr SettingsTable<seedRow> sourceTable{"source"};
constexpr SettingsTable<seedRow> recorderTable{"recorder"};
constexpr SettingsTable<seedRow> cardTable{"card"};

template <const auto& Table> class Type final : public NodeTypeOf<Table> {
public:
	explicit Type(Ports ports, bool handoff = false) : ports_(std::move(ports)), handoff_(handoff) {}

private:
	MadeConfig make(const NodeName&, const typename NodeTypeOf<Table>::Checked&) const override {
		return std::make_unique<Config>(ports_, handoff_);
	}

	Ports ports_;
	bool handoff_;
};

std::vector<std::unique_ptr<const NodeType>> types() {
	std::vector<std::unique_ptr<const NodeType>> all;
	all.push_back(std::make_unique<Type<sourceTable>>(Ports{{{Direction::output, "noise", {"mono"}}}}));
	all.push_back(
		std::make_unique<Type<recorderTable>>(Ports{{{Direction::input, "record", {"left", "right"}}}}, true));
	all.push_back(
		std::make_unique<Type<cardTable>>(Ports{{{Direction::input, "play", {"1"}},
	                                             {Direction::output, "capture", {"1"}},
	                                             {Direction::input, "from_card", {"1"}, PortBank::Reach::inside},
	                                             {Direction::output, "to_card", {"1"}, PortBank::Reach::inside}}}));
	return all;
}

/// Completions wait in the factory until the test runs them, as the engine runs them on the main loop later
/// (node-lifecycle.md 5.3).
using Completions = std::vector<std::move_only_function<void()>>;

struct Block final : PortBlock {
	Completions& completions;
	explicit Block(Completions& c) : completions(c) {}
	void drive(DriveRequest, std::move_only_function<void(std::uint64_t)> done) override {
		completions.push_back([done = std::move(done)]() mutable { done(0); });
	}
	void fadeOut(std::move_only_function<void()> done) override { completions.push_back(std::move(done)); }
	void settle() override {}
	std::uint64_t takenAt() const noexcept override { return 0; }
	std::uint32_t blockId() const noexcept override { return 0xFFFFFFFFU; }
};

struct Blocks final : PortBlockFactory {
	std::map<NodeName, PortBlockEvents*> events; // the latest block of each name
	Completions completions;
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports&, const AudioFormat&,
	                                PortBlockEvents& e) override {
		events[node] = &e;
		return std::make_unique<Block>(completions);
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

struct Handoffs final : HandoffAdapter {
	std::vector<std::move_only_function<void(std::uint64_t)>> pending;
	std::unique_ptr<Handoff> make(const NodeName&, std::uint32_t, std::uint32_t, HandoffConsumer&) override {
		return std::make_unique<PendingHandoff>(pending);
	}
	void closeAll() {
		auto all = std::move(pending);
		for (auto& closed : all) closed(0);
	}
};

struct NoCards final : SoundCardAdapter {
	std::unique_ptr<OutsideNode> make(const OutsideTarget&, const SoundCardRequest&, OutsideNodeEvents&) override {
		FAIL("a sound card was made");
		return nullptr;
	}
};

struct NoReceivers final : RtpReceiverAdapter {
	std::unique_ptr<OutsideNode> make(const OutsideTarget&, const RtpReceiverRequest&, OutsideNodeEvents&) override {
		FAIL("an RTP receiver was made");
		return nullptr;
	}
};

/// The main loop as the test turns it: NodeHost posts its per-turn flush here (node-lifecycle.md 5.5).
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

struct Fixture {
	NodeCatalogue catalogue{types()};
	DesiredGraph graph{catalogue};
	Blocks blocks;
	Handoffs handoffs;
	NoCards cards;
	NoReceivers receivers;
	Links links;
	Turns mainLoop;
	NoTimers timers;
	ResourceRegistry resources{cards, receivers, handoffs};
	NodeHost nodes{AudioFormat{48000, 256}, blocks, resources, mainLoop, timers};
	Reconciler routes{links};
	GraphStatus status{EngineView{AudioFormat{48000, 256}, true, "SCHED_FIFO 88"}, catalogue, graph, nodes, routes};
	Fixture() {
		graph.subscribe(static_cast<DesiredNodeListener&>(nodes));
		graph.subscribe(static_cast<DesiredRouteListener&>(routes));
	}
	~Fixture() {
		nodes.stopAll(graph.close(), [] {});
		settle();
		handoffs.closeAll();
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

	CHECK(view.engine.format.rate == 48000);
	CHECK(view.engine.format.maxBlock == 256);
	CHECK(view.engine.realtime);
	CHECK(view.engine.scheduling == "SCHED_FIFO 88");
	CHECK(view.types == std::vector<std::string>{"card", "recorder", "source"});

	REQUIRE(view.nodes.size() == 3);
	CHECK(view.nodes[0].spec == NodeSpec{"motu", "card", noSettings});
	CHECK(view.nodes[0].state == NodeState::failed);
	REQUIRE(view.nodes[0].reason.has_value());
	CHECK(view.nodes[0].reason->detail == "no driver");
	// Public ports only: inside ports are the card's own.
	CHECK(view.nodes[0].ports.inputs() == std::vector<PortName>{"play.1"});
	CHECK(view.nodes[0].ports.outputs() == std::vector<PortName>{"capture.1"});
	CHECK(view.nodes[0].ports.insideInputs().empty());
	CHECK(view.nodes[0].ports.insideOutputs().empty());
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
	apply(f.graph, {{"rec", "recorder", noSettings}, {"noise", "source", noSettings}}, {noiseToFl});
	f.settle();
	f.blocks.events.at("rec")->ready();
	f.settle();
	REQUIRE(f.nodes.health().at(1).name == "rec"); // a commit adds in name order
	REQUIRE(f.nodes.health().at(1).state == NodeState::running);

	apply(f.graph, {{"noise", "source", noSettings}}, {});
	f.settle();                            // rec's silence, then its block destroyed
	REQUIRE(f.nodes.health().size() == 2); // rec waits for its handoff to close
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
	CHECK(view.nodes[1].state == NodeState::starting);

	f.settle(); // the new rec's block
	f.blocks.events.at("rec")->ready();
	view = f.status.read();
	CHECK(view.nodes[1].state == NodeState::starting); // its insides wait for the old rec

	f.handoffs.closeAll();
	f.settle(); // the new rec's drive
	REQUIRE(f.nodes.health().size() == 2);
	view = f.status.read();
	CHECK(view.nodes[1].state == NodeState::running);
}
