// MainLoopGateway (docs/design/batch2.md sections 2 and 4.5, R1 criterion 4; docs/design/knobly-config.md,
// crossings C1 and C4, S5 criterion T10), with two queue executors the test runs by hand: a batch goes
// whole to the graph file keeper (here a fake that answers on the main loop, telling the gateway the
// file change and graph events its batch causes first, as the keeper's main-loop side does), and the
// reply, file changes and events come back on the control executor in main-loop order.
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/gateway.hpp"

using namespace ear::control;
using nlohmann::json;

namespace {

/// Posted work waits until the test runs it; `running` tells the fakes whose turn it is.
class QueueExecutor final : public ear::Executor {
public:
	explicit QueueExecutor(const QueueExecutor*& running) : running_(running) {}
	void post(std::move_only_function<void()> work) override { queue_.push_back(std::move(work)); }
	std::size_t size() const { return queue_.size(); }
	/// One item (one turn).
	void runOne() {
		REQUIRE_FALSE(queue_.empty());
		auto work = std::move(queue_.front());
		queue_.pop_front();
		running_ = this;
		work();
		running_ = nullptr;
	}
	void runAll() {
		while (!queue_.empty()) runOne();
	}

private:
	const QueueExecutor*& running_;
	std::deque<std::move_only_function<void()>> queue_;
};

struct Loops {
	const QueueExecutor* running = nullptr;
	QueueExecutor main{running};
	QueueExecutor control{running};
};

class Config final : public ear::NodeConfig {
public:
	explicit Config(ear::Ports ports) : ports_(std::move(ports)) {}
	const ear::Ports& ports() const override { return ports_; }
	ear::StartResult start(ear::StartContext&) const override {
		return std::unexpected(ear::FailureReason{ear::FailureReason::Code::startFailed, "test config"});
	}

private:
	ear::Ports ports_;
};

GraphFile file(std::string version) {
	return GraphFile{std::move(version), "node white a;\n", {}, std::nullopt, std::nullopt, {}, std::nullopt};
}

/// The keeper as the gateway sees it: submit() posts one main-loop step that tells the gateway what
/// `step` makes it tell (a file change, graph events), then calls the reply with `results`.
class FakeRequests final : public GraphRequests {
public:
	explicit FakeRequests(Loops& loops) : loops_(loops) {}
	MainLoopGateway* gateway = nullptr;
	std::vector<std::vector<Command>> submitted;
	std::function<void(MainLoopGateway&)> step;
	std::vector<CommandResult> results;
	int submittedOffControl = 0;
	int repliesOffMain = 0;

	void submit(std::vector<Command> commands, BatchReply reply) override {
		if (loops_.running != &loops_.control) ++submittedOffControl;
		submitted.push_back(std::move(commands));
		loops_.main.post([this, reply = std::move(reply)]() mutable {
			if (step) step(*gateway);
			if (loops_.running != &loops_.main) ++repliesOffMain;
			reply(results);
		});
	}

private:
	Loops& loops_;
};

class Sink final : public EventSink {
public:
	explicit Sink(Loops& loops) : loops_(loops) {}
	std::vector<std::string> seen; // the stream: events, file changes and replies, in arrival order
	int offControl = 0;
	void graphChanged(GraphEvent event) override {
		if (loops_.running != &loops_.control) ++offControl;
		seen.push_back(encodeEvent(event));
	}
	void graphFileChanged(GraphFileChanged change) override {
		if (loops_.running != &loops_.control) ++offControl;
		seen.push_back(encodeGraphFileChanged(change));
	}

private:
	Loops& loops_;
};

/// 2026-11-09T10:00:00Z, the gateway's clock until a test moves it.
constexpr std::chrono::system_clock::time_point testEpoch{std::chrono::seconds{1'794'218'400}};

struct Fixture {
	Loops loops;
	FakeRequests requests{loops};
	std::chrono::system_clock::time_point clockAt = testEpoch;
	MainLoopGateway gateway{loops.main, loops.control, requests, [this] { return clockAt; }};
	Sink sink{loops};
	Fixture() { requests.gateway = &gateway; }
	/// run() from the control thread, as a session calls it.
	void run(std::vector<Command> commands, std::move_only_function<void(std::vector<CommandResult>)> done) {
		loops.running = &loops.control;
		gateway.run(std::move(commands), std::move(done));
		loops.running = nullptr;
	}
};

std::string method(const std::string& text) { return json::parse(text)["method"].get<std::string>(); }

} // namespace

TEST_CASE("run() sends the batch whole to the keeper and answers on the control executor", "[gateway]") {
	Fixture f;
	f.requests.results = {ear::GraphView{{{48000, 256}, false, "SCHED_OTHER"}, {"white"}, {}, {}},
	                      std::expected<void, ear::CommandError>{}, file("b.1")};
	std::vector<CommandResult> results;
	int answered = 0;
	int answeredOffControl = 0;
	f.run({GetGraph{}, AddNode{{"a", "white", json::object()}}, GetGraphFile{}}, [&](std::vector<CommandResult> r) {
		++answered;
		if (f.loops.running != &f.loops.control) ++answeredOffControl;
		results = std::move(r);
	});
	REQUIRE(f.requests.submitted.size() == 1); // one batch, whole
	CHECK(f.requests.submitted[0].size() == 3);
	CHECK(std::holds_alternative<AddNode>(f.requests.submitted[0][1]));
	CHECK(f.loops.control.size() == 0); // nothing inline
	f.loops.main.runAll();
	CHECK(answered == 0);
	f.loops.control.runAll();
	CHECK(answered == 1);
	CHECK(answeredOffControl == 0);
	CHECK(f.requests.repliesOffMain == 0);
	REQUIRE(results.size() == 3);
	CHECK(std::get<ear::GraphView>(results[0]).engine.scheduling == "SCHED_OTHER");
	CHECK(std::get<std::expected<void, ear::CommandError>>(results[1]).has_value());
	CHECK(std::get<GraphFile>(results[2]).version == "b.1");
}

TEST_CASE("The reply follows the file change and every event its batch caused", "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll(); // watch takes effect in a main-loop turn
	f.requests.results = {std::expected<void, ear::CommandError>{}};
	f.requests.step = [&](MainLoopGateway& gateway) {
		gateway.graphFileChanged(GraphFileChanged{file("b.2"), FileChangeCause::edit, 0});
		gateway.nodeAdded({"a", "white", json::object()},
		                  std::make_shared<Config>(ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}}));
		gateway.nodeChanged({"a", ear::NodeState::starting, std::nullopt, ear::NodePhase::configuring});
	};
	f.run({AddNode{{"a", "white", json::object()}}}, [&](std::vector<CommandResult>) {
		if (f.loops.running != &f.loops.control) ++f.sink.offControl;
		f.sink.seen.push_back("reply");
	});
	f.loops.main.runAll();
	CHECK(f.sink.seen.empty()); // everything waits for the control executor
	f.loops.control.runAll();
	CHECK(f.sink.offControl == 0);
	REQUIRE(f.sink.seen.size() == 4);
	CHECK(method(f.sink.seen[0]) == "graphFile.changed");
	CHECK(json::parse(f.sink.seen[0])["params"]["cause"] == "edit");
	CHECK(method(f.sink.seen[1]) == "node.added");
	CHECK(method(f.sink.seen[2]) == "node.state");
	CHECK(f.sink.seen[3] == "reply");
}

TEST_CASE("Each listener call posts one event, and the sink hears it only on the control executor", "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll(); // watch takes effect in a main-loop turn

	f.loops.running = &f.loops.main; // listener calls come from the main loop
	f.gateway.graphFileChanged(GraphFileChanged{file("b.3"), FileChangeCause::disk, 2});
	f.gateway.graphFileWritten("b.3"); // a status line only: nothing on the wire
	f.gateway.nodeAdded(
		{"n", "white", json::object()},
		std::make_shared<Config>(ear::Ports{{{ear::Direction::input, "in", {"a"}},
	                                         {ear::Direction::output, "out", {"a"}},
	                                         {ear::Direction::input, "i2", {"1"}, ear::PortBank::Reach::inside},
	                                         {ear::Direction::output, "o2", {"1"}, ear::PortBank::Reach::inside}}}));
	f.gateway.nodeUpdated({"n", "white", {{"k", 1}}}, std::make_shared<Config>(ear::Ports{}));
	f.gateway.nodeChanged({"n", ear::NodeState::running, std::nullopt});
	f.gateway.routeAdded({{"n", "out.a"}, {"m", "in.a"}});
	f.gateway.routeChanged({{{"n", "out.a"}, {"m", "in.a"}}, ear::RouteMatch::linked, ""});
	f.gateway.routeRemoved({{"n", "out.a"}, {"m", "in.a"}});
	f.gateway.nodeRemoved("n");
	f.gateway.nodeGone("n", 7);
	f.loops.running = nullptr;
	CHECK(f.sink.seen.empty());          // never inline
	CHECK(f.loops.control.size() == 10); // one post per call, none for the write, two for the node gone
	f.loops.control.runAll();
	CHECK(f.sink.offControl == 0);
	REQUIRE(f.sink.seen.size() == 10);
	std::vector<std::string> methods;
	for (const auto& text : f.sink.seen) methods.push_back(method(text));
	CHECK(methods == std::vector<std::string>{"graphFile.changed", "node.added", "node.updated", "node.state",
	                                          "route.added", "route.status", "route.removed", "node.removed",
	                                          "node.stopped", "node.gone"});
	CHECK(json::parse(f.sink.seen[9])["params"]["droppedFrames"] == 7);
	CHECK(json::parse(f.sink.seen[0])["params"]["droppedEdits"] == 2);
	// node.added carries the public ports only
	CHECK(json::parse(f.sink.seen[1])["params"]["inputs"] == json::array({"in.a"}));
	CHECK(json::parse(f.sink.seen[1])["params"]["outputs"] == json::array({"out.a"}));
	CHECK(json::parse(f.sink.seen[8])["params"]["droppedFrames"] == 7);
}

TEST_CASE("Events and file changes from before watch are not sent", "[gateway]") {
	Fixture f;
	f.loops.running = &f.loops.main;
	f.gateway.nodeRemoved("before"); // watch has not taken effect
	f.gateway.graphFileChanged(GraphFileChanged{file("b.4"), FileChangeCause::disk, 0});
	f.loops.running = nullptr;
	CHECK(f.loops.control.size() == 0);
	f.gateway.watch(f.sink);
	f.requests.results = {std::expected<void, ear::CommandError>{}};
	f.requests.step = [](MainLoopGateway& gateway) { gateway.nodeRemoved("a"); };
	f.run({RemoveNode{"a"}}, [&](std::vector<CommandResult>) { f.sink.seen.push_back("reply"); });
	f.loops.main.runAll();
	f.loops.control.runAll();
	REQUIRE(f.sink.seen.size() == 2);
	CHECK(method(f.sink.seen[0]) == "node.removed");
	CHECK(f.sink.seen[1] == "reply");
}

TEST_CASE("node.state is sent whenever any health member changes, its since moving only with the phase, and never "
          "for a superseded node",
          "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll();
	using ear::NodePhase;
	using ear::NodeState;
	using ear::Wanted;
	const std::vector<ear::WaitingFor> forRtp{{ear::WaitingFor::Kind::companion, "n.rtp"}};
	f.loops.running = &f.loops.main;
	f.gateway.nodeAdded({"n", "white", json::object()},
	                    std::make_shared<Config>(ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}}));
	f.clockAt += std::chrono::milliseconds(5);
	// NodeHost's first tell, `added`, is what node.added carried: nothing more is sent.
	f.gateway.nodeChanged({"n", NodeState::starting, std::nullopt, NodePhase::added});
	f.gateway.nodeChanged({"n", NodeState::starting, std::nullopt, NodePhase::configuring});
	f.clockAt += std::chrono::milliseconds(5);
	f.gateway.nodeChanged(
		{"n", NodeState::starting, std::nullopt, NodePhase::configuring, Wanted::running, std::nullopt, forRtp});
	f.gateway.nodeChanged(
		{"n", NodeState::starting, std::nullopt, NodePhase::configuring, Wanted::stopped, std::nullopt, forRtp});
	f.clockAt += std::chrono::milliseconds(5);
	f.gateway.nodeChanged({"n", NodeState::stopped, std::nullopt, NodePhase::stopped, Wanted::stopped});
	f.gateway.nodeChanged({"n", NodeState::stopped, std::nullopt, NodePhase::stopped, Wanted::stopped}); // the same
	f.gateway.nodeRemoved("n"); // forgotten: a node added back by the same name is sent from its first state
	f.gateway.nodeChanged({"n", NodeState::stopped, std::nullopt, NodePhase::stopped});
	f.loops.running = nullptr;
	f.loops.control.runAll();
	std::vector<json> states;
	for (const auto& text : f.sink.seen)
		if (method(text) == "node.state") states.push_back(json::parse(text)["params"]);
	REQUIRE(states.size() == 5);
	CHECK(states[0]["phase"] == "configuring");
	CHECK(states[0]["since"] == "2026-11-09T10:00:00.005000Z");
	CHECK(states[1]["waitingFor"] == json::parse(R"([{"kind":"companion","name":"n.rtp"}])"));
	CHECK(states[1]["since"] == "2026-11-09T10:00:00.005000Z"); // the phase did not change
	CHECK(states[2]["wanted"] == "stopped");
	CHECK(states[2]["since"] == "2026-11-09T10:00:00.005000Z");
	CHECK(states[3]["phase"] == "stopped");
	CHECK(states[3]["since"] == "2026-11-09T10:00:00.015000Z");
	CHECK_FALSE(states[3].contains("waitingFor")); // an absent member is absent now
	CHECK(states[3]["blockLost"] == false);
	CHECK(states[4]["phase"] == "stopped"); // the name added back, sent from its first
	CHECK_FALSE(f.gateway.hearsSuperseded());
}

TEST_CASE("node.added carries the spec's health and a disabled node's disabled, and node.updated its outcome",
          "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll();
	f.loops.running = &f.loops.main;
	ear::NodeSpec quiet{"q", "white", json::object()};
	quiet.disabled = true;
	f.gateway.nodeAdded(quiet, std::make_shared<Config>(ear::Ports{}));
	// NodeHost's first tell of a node added disabled is the spec's: nothing more is sent.
	f.gateway.nodeChanged(
		{"q", ear::NodeState::disabled, std::nullopt, ear::NodePhase::disabled, ear::Wanted::disabled});
	f.gateway.nodeChanged(quiet, std::make_shared<Config>(ear::Ports{}),
	                      ear::NodeChange{ear::NodeOutcome::stored, {"k"}});
	const ear::NodeSpec loud{"q", "white", {{"k", 2}}};
	f.gateway.nodeChanged(loud, std::make_shared<Config>(ear::Ports{}), ear::NodeChange{ear::NodeOutcome::enable, {}});
	f.loops.running = nullptr;
	f.loops.control.runAll();
	REQUIRE(f.sink.seen.size() == 3);
	const json added = json::parse(f.sink.seen[0])["params"];
	CHECK(added["state"] == "disabled");
	CHECK(added["phase"] == "disabled");
	CHECK(added["wanted"] == "disabled");
	CHECK(added["disabled"] == true);
	CHECK(added["blockLost"] == false);
	CHECK(added["since"] == "2026-11-09T10:00:00.000000Z");
	CHECK(json::parse(f.sink.seen[1])["params"] ==
	      json::parse(R"({"name":"q","settings":{},"applied":"stored","disabled":true})"));
	CHECK(json::parse(f.sink.seen[2])["params"] ==
	      json::parse(R"({"name":"q","settings":{"k":2},"applied":"enable"})"));
}

TEST_CASE("A snapshot's since is the one its node.state carried, filled on the main loop before the reply crosses",
          "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll();
	ear::GraphView view{{{48000, 256}, false, "SCHED_OTHER"}, {"white"}, {}, {}};
	view.nodes.push_back({{"n", "white", json::object()}, ear::Ports{}, {"n"}, std::nullopt});
	view.nodes.push_back({{"m", "white", json::object()}, ear::Ports{}, {"m"}, std::nullopt});
	f.requests.results = {view};
	f.requests.step = [&](MainLoopGateway& gateway) {
		f.clockAt += std::chrono::milliseconds(7);
		gateway.nodeChanged({"n", ear::NodeState::running, std::nullopt, ear::NodePhase::running});
	};
	std::vector<CommandResult> results;
	f.run({GetGraph{}}, [&](std::vector<CommandResult> r) { results = std::move(r); });
	f.loops.main.runAll();
	f.loops.control.runAll();
	REQUIRE(results.size() == 1);
	const auto& got = std::get<ear::GraphView>(results[0]);
	REQUIRE(got.nodes[0].since.has_value());
	CHECK(encodeGraph(got)["nodes"][0]["since"] == "2026-11-09T10:00:00.007000Z");
	CHECK_FALSE(got.nodes[1].since.has_value()); // never told
}
