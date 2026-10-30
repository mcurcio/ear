// MainLoopGateway (docs/design/batch2.md sections 2 and 4.5, R1 criterion 4), with two queue
// executors the test runs by hand: commands run on the main loop in one turn, results and events
// come back on the control executor in main-loop order, and the graph is touched only on the
// main loop.
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
	std::expected<std::unique_ptr<ear::NodeInsides>, ear::FailureReason> start(ear::NodeGrant&) const override {
		return std::unexpected(ear::FailureReason{ear::FailureReason::Code::startFailed, "test config"});
	}

private:
	ear::Ports ports_;
};

/// A graph that checks every call runs on the main loop and, like DesiredGraph, tells its
/// observer (the gateway) of each change inside the call.
class Graph final : public ear::GraphControl, public ear::GraphReader {
public:
	explicit Graph(Loops& loops) : loops_(loops) {}
	MainLoopGateway* observer = nullptr;
	std::vector<std::string> calls;
	int offLoop = 0;

	std::expected<void, ear::CommandError> addNode(const ear::NodeSpec& node) override {
		record("addNode " + node.name);
		observer->nodeAdded(node, std::make_shared<Config>(ear::Ports{{}, {"MONO"}, {}, {"INSIDE"}}));
		return {};
	}
	std::expected<void, ear::CommandError> updateNode(const ear::NodeName& node, const ear::NodeSettings&) override {
		record("updateNode " + node);
		return std::unexpected(ear::UnknownNode{node});
	}
	std::expected<void, ear::CommandError> removeNode(const ear::NodeName& node) override {
		record("removeNode " + node);
		observer->nodeRemoved(node);
		return {};
	}
	std::expected<void, ear::CommandError> addRoute(const ear::Route& route) override {
		record("addRoute " + route.to.port);
		return {};
	}
	std::expected<void, ear::CommandError> removeRoute(const ear::Route& route) override {
		record("removeRoute " + route.to.port);
		return {};
	}
	ear::GraphView read() const override {
		const_cast<Graph*>(this)->record("read");
		return {{{48000, 256}, false, "SCHED_OTHER"}, {"white"}, {}, {}};
	}

private:
	void record(std::string call) {
		if (loops_.running != &loops_.main) ++offLoop;
		calls.push_back(std::move(call));
	}
	Loops& loops_;
};

class Sink final : public EventSink {
public:
	explicit Sink(Loops& loops) : loops_(loops) {}
	std::vector<std::string> seen;   // the stream: events and replies, in arrival order
	int offControl = 0;
	void graphChanged(GraphEvent event) override {
		if (loops_.running != &loops_.control) ++offControl;
		seen.push_back(encodeEvent(event));
	}

private:
	Loops& loops_;
};

struct Fixture {
	Loops loops;
	Graph graph{loops};
	MainLoopGateway gateway{loops.main, loops.control, graph, graph};
	Sink sink{loops};
	Fixture() { graph.observer = &gateway; }
};

std::string method(const std::string& text) { return json::parse(text)["method"].get<std::string>(); }

} // namespace

TEST_CASE("run() runs its commands in one main-loop turn and answers on the control executor", "[gateway]") {
	Fixture f;
	std::vector<CommandResult> results;
	int answered = 0;
	int answeredOffControl = 0;
	f.gateway.run({GetGraph{}, AddNode{{"a", "white", json::object()}}, UpdateNode{"b", json::object()}},
	              [&](std::vector<CommandResult> r) {
		              ++answered;
		              if (f.loops.running != &f.loops.control) ++answeredOffControl;
		              results = std::move(r);
	              });
	CHECK(f.graph.calls.empty());   // nothing inline
	REQUIRE(f.loops.main.size() == 1);
	f.loops.main.runOne();   // one turn runs every command
	CHECK(f.graph.calls == std::vector<std::string>{"read", "addNode a", "updateNode b"});
	CHECK(f.graph.offLoop == 0);
	CHECK(answered == 0);
	f.loops.control.runAll();
	CHECK(answered == 1);
	CHECK(answeredOffControl == 0);
	REQUIRE(results.size() == 3);
	CHECK(std::get<ear::GraphView>(results[0]).engine.scheduling == "SCHED_OTHER");
	CHECK(std::get<std::expected<void, ear::CommandError>>(results[1]).has_value());
	CHECK(ear::errorCode(std::get<std::expected<void, ear::CommandError>>(results[2]).error()) == "unknown-node");
}

TEST_CASE("Each listener call posts one event; the sink hears it only on the control executor", "[gateway]") {
	Fixture f;
	f.gateway.watch(f.sink);
	f.loops.main.runAll();   // watch takes effect in a main-loop turn

	f.loops.running = &f.loops.main;   // listener calls come from the main loop
	f.gateway.nodeAdded({"n", "white", json::object()}, std::make_shared<Config>(ear::Ports{{"IN"}, {"OUT"}, {"I2"}, {"O2"}}));
	f.gateway.nodeUpdated({"n", "white", {{"k", 1}}}, std::make_shared<Config>(ear::Ports{}));
	f.gateway.nodeChanged({"n", ear::NodeState::running, std::nullopt});
	f.gateway.routeAdded({{"n", "OUT"}, {"m", "IN"}});
	f.gateway.routeChanged({{{"n", "OUT"}, {"m", "IN"}}, ear::RouteMatch::linked, ""});
	f.gateway.routeRemoved({{"n", "OUT"}, {"m", "IN"}});
	f.gateway.nodeRemoved("n");
	f.gateway.nodeGone("n", 7);
	f.loops.running = nullptr;
	CHECK(f.sink.seen.empty());   // never inline
	CHECK(f.loops.control.size() == 8);   // one post per call
	f.loops.control.runAll();
	CHECK(f.sink.offControl == 0);
	REQUIRE(f.sink.seen.size() == 8);
	std::vector<std::string> methods;
	for (const auto& text : f.sink.seen) methods.push_back(method(text));
	CHECK(methods == std::vector<std::string>{"node.added", "node.updated", "node.state", "route.added", "route.status",
	                                          "route.removed", "node.removed", "node.stopped"});
	// node.added carries the public ports only
	CHECK(json::parse(f.sink.seen[0])["params"]["inputs"] == json::array({"IN"}));
	CHECK(json::parse(f.sink.seen[0])["params"]["outputs"] == json::array({"OUT"}));
	CHECK(json::parse(f.sink.seen[7])["params"]["droppedFrames"] == 7);
}

TEST_CASE("Events and replies arrive in main-loop order; events before watch are not sent", "[gateway]") {
	Fixture f;
	f.loops.running = &f.loops.main;
	f.gateway.nodeRemoved("before");   // watch has not taken effect
	f.loops.running = nullptr;
	CHECK(f.loops.control.size() == 0);
	f.gateway.watch(f.sink);
	f.gateway.run({AddNode{{"a", "white", json::object()}}, RemoveNode{"a"}},
	              [&](std::vector<CommandResult>) { f.sink.seen.push_back("reply"); });
	f.loops.main.runAll();
	f.loops.control.runAll();
	REQUIRE(f.sink.seen.size() == 3);
	CHECK(method(f.sink.seen[0]) == "node.added");
	CHECK(method(f.sink.seen[1]) == "node.removed");
	CHECK(f.sink.seen[2] == "reply");
}
