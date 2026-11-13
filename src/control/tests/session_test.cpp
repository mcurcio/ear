// ControlSession and ControlHub (docs/control-port.md), through the real MainLoopGateway on two
// queue executors the test runs by hand, with a scripted graph file keeper that answers each batch
// on the main loop and, like the keeper's main-loop side, DesiredGraph, NodeHost and the Reconciler,
// tells the gateway each file change and graph event first. Every fixture in test/protocol is
// replayed request by request and the session's output must equal its messages exactly.
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/rpc.hpp"
#include "ear/control/session.hpp"
#include "graph_file_fixture.hpp"

using namespace ear::control;
using nlohmann::json;
using Result = std::expected<void, ear::CommandError>;

namespace {

json fixture(const std::string& name) {
	std::ifstream file(std::string(EAR_PROTOCOL_DIR) + "/" + name + ".json");
	REQUIRE(file);
	return json::parse(file);
}

using ear::control::testing::health;

class QueueExecutor final : public ear::Executor {
public:
	explicit QueueExecutor(const QueueExecutor*& running) : running_(running) {}
	void post(std::move_only_function<void()> work) override { queue_.push_back(std::move(work)); }
	bool empty() const { return queue_.empty(); }
	void runOne() {
		auto work = std::move(queue_.front());
		queue_.pop_front();
		running_ = this;
		work();
		running_ = nullptr;
	}

private:
	const QueueExecutor*& running_;
	std::deque<std::move_only_function<void()>> queue_;
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

std::string text(const ear::Route& r) { return r.from.node + ":" + r.from.port + "->" + r.to.node + ":" + r.to.port; }

/// A command as the script names it.
std::string describe(const Command& command) {
	return std::visit(
		[](const auto& c) -> std::string {
			using C = std::decay_t<decltype(c)>;
			if constexpr (std::is_same_v<C, GetGraph>) return "read";
			else if constexpr (std::is_same_v<C, GetGraphFile>) return "readFile";
			else if constexpr (std::is_same_v<C, AddNode>)
				return "addNode " + c.node.name + " " + c.node.type + " " + c.node.settings.dump() +
			           (c.node.disabled ? " disabled" : "");
			else if constexpr (std::is_same_v<C, UpdateNode>) return "updateNode " + c.node + " " + c.settings.dump();
			else if constexpr (std::is_same_v<C, RemoveNode>) return "removeNode " + c.node;
			else if constexpr (std::is_same_v<C, AddRoute>) return "addRoute " + text(c.route);
			else if constexpr (std::is_same_v<C, RemoveRoute>) return "removeRoute " + text(c.route);
			else if constexpr (std::is_same_v<C, StartNode>) return "startNode " + c.node;
			else if constexpr (std::is_same_v<C, StopNode>) return "stopNode " + c.node;
			else if constexpr (std::is_same_v<C, RestartNode>) return "restartNode " + c.node;
			else if constexpr (std::is_same_v<C, DisableNode>) return "disableNode " + c.node;
			else if constexpr (std::is_same_v<C, EnableNode>) return "enableNode " + c.node;
			else {
				static_assert(std::is_same_v<C, SetGraphFile>);
				return "setGraphFile " + c.version;
			}
		},
		command);
}

/// The graph file keeper, scripted: each batch is one main-loop step. Reads answer `view` and
/// `file`; every other command must be the next one scripted, and its handler runs inside the
/// step, on the main loop, telling the gateway what the command causes and returning its result.
class ScriptedRequests final : public GraphRequests {
public:
	struct Step {
		std::string call;
		std::function<CommandResult()> handler;
	};
	std::deque<Step> script;
	std::vector<std::string> calls;
	ear::GraphView view{};
	GraphFile file{};
	const QueueExecutor* const* running = nullptr;
	const SnapshotStamp* stamp = nullptr; // the gateway: each read's since, filled at the read
	QueueExecutor* mainLoop = nullptr;
	int offLoop = 0;

	void submit(std::vector<Command> commands, BatchReply reply) override {
		mainLoop->post([this, commands = std::move(commands), reply = std::move(reply)]() mutable {
			std::vector<CommandResult> results;
			for (const Command& command : commands) results.push_back(run(command));
			reply(std::move(results));
		});
	}

private:
	CommandResult run(const Command& command) {
		std::string call = describe(command);
		if (*running != mainLoop) ++offLoop;
		calls.push_back(call);
		if (std::holds_alternative<GetGraph>(command)) {
			ear::GraphView read = view;
			if (stamp != nullptr) stamp->stamp(read);
			return read;
		}
		if (std::holds_alternative<GetGraphFile>(command)) return file;
		REQUIRE_FALSE(script.empty());
		Step step = std::move(script.front());
		script.pop_front();
		CHECK(step.call == call);
		return step.handler ? step.handler() : CommandResult{Result{}};
	}
};

class FakeLevels final : public ear::LevelReader {
public:
	std::vector<ear::NodeLevels> latest;
	int reads = 0;
	std::vector<ear::NodeLevels> latestLevels() override {
		++reads;
		return latest;
	}
};

/// Records what the session queues; replacing an unsent levels snapshot is the transport's job.
class RecordingOutbox final : public Outbox {
public:
	std::vector<std::string> all;    // responses, notifications and levels, in order
	std::vector<std::string> levels; // what came through sendLevels
	int sends = 0;
	void send(Message message) override {
		++sends;
		all.push_back(*message);
	}
	void sendLevels(Message message) override {
		levels.push_back(*message);
		all.push_back(*message);
	}
};

struct World {
	const QueueExecutor* running = nullptr;
	QueueExecutor main{running};
	QueueExecutor control{running};
	ScriptedRequests graph;
	int clockReadings = 0; // the gateway's clock: 10 ms on at each reading, so each since is fixed by the order
	MainLoopGateway gateway{main, control, graph, [this] { return ear::control::testing::reading(clockReadings++); }};
	FakeLevels levels;
	std::unique_ptr<ControlHub> hub;

	World() {
		graph.running = &running;
		graph.mainLoop = &main;
		graph.stamp = &gateway;
		onControl([&] { hub = std::make_unique<ControlHub>(gateway, levels); });
		pump();
	}
	~World() {
		onControl([&] { hub.reset(); });
	}
	/// Runs both loops until neither has work, main loop first.
	void pump() {
		while (!main.empty() || !control.empty()) {
			while (!main.empty()) main.runOne();
			while (!control.empty()) control.runOne();
		}
	}
	void onMain(std::function<void()> work) {
		main.post([work = std::move(work)] { work(); });
		pump();
	}
	void onControl(const std::function<void()>& work) {
		running = &control;
		work();
		running = nullptr;
	}
	/// A message received on the control thread; done must run once its responses are queued.
	void receive(ControlSession& session, const std::string& text, int* doneCount = nullptr) {
		onControl([&] {
			session.received(text, [doneCount] {
				if (doneCount) ++*doneCount;
			});
		});
		pump();
	}
	// The listener calls the keeper's main-loop side and the core make on the main loop.
	void fileChanged(GraphFile file, FileChangeCause cause, std::uint32_t dropped = 0) {
		gateway.graphFileChanged(GraphFileChanged{std::move(file), cause, dropped});
	}
	void nodeAdded(const ear::NodeSpec& node, ear::Ports ports) {
		gateway.nodeAdded(node, std::make_shared<Config>(std::move(ports)));
	}
	/// A health as NodeHost tells it (testing::health: the state's phase, configuring for starting).
	void state(const std::string& node, ear::NodeState now, std::optional<ear::FailureReason> reason = {}) {
		gateway.nodeChanged(health(node, now, std::move(reason)));
	}
	void told(const ear::NodeHealth& health) { gateway.nodeChanged(health); }
	void route(const ear::Route& route, ear::RouteMatch match, std::string detail = "") {
		gateway.routeChanged({route, match, std::move(detail)});
	}
};

std::vector<json> parsed(const std::vector<std::string>& texts) {
	std::vector<json> out;
	for (const auto& text : texts) out.push_back(json::parse(text));
	return out;
}

void checkMessages(const RecordingOutbox& out, const json& expected) {
	const std::vector<json> got = parsed(out.all);
	REQUIRE(got.size() == expected.size());
	for (std::size_t i = 0; i < got.size(); ++i) {
		INFO("message " << i);
		CHECK(got[i] == expected[i]);
	}
}

using ear::NodeState;
using Code = ear::FailureReason::Code;

ear::GraphView baseView() {
	ear::GraphView view{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "file", "white"}, {}, {}};
	view.nodes.push_back({{"noise", "white", json::object()},
	                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}},
	                      health("noise", NodeState::running)});
	view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}},
	                      ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                      health("rec", NodeState::running)});
	view.routes.push_back({{{"noise", "noise.mono"}, {"rec", "record.left"}}, ear::RouteMatch::linked, ""});
	return view;
}

std::vector<std::string> requestsOf(const json& document) {
	return document["requests"].get<std::vector<std::string>>();
}

} // namespace

TEST_CASE("snapshot.json: graph.get and graph.subscribe answer with the whole graph", "[session][protocol]") {
	const json document = fixture("snapshot");
	World w;
	w.graph.view = ear::control::testing::snapshotView();
	for (ear::NodeView& node : w.graph.view.nodes) node.since.reset(); // the gateway fills it
	w.onMain([&] { w.state("noise", NodeState::running); });           // told: the snapshot carries its since
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	checkMessages(out, document["messages"]);
	CHECK(w.graph.offLoop == 0);
}

/// A node's counters by name, as NodeHost answers them.
struct HandCounts final : ear::NodeCounterSource {
	std::map<ear::NodeName, ear::NodeCounts> nodes;
	std::optional<ear::NodeCounts> counts(const ear::NodeName& node) const override {
		const auto it = nodes.find(node);
		if (it == nodes.end()) return std::nullopt;
		return it->second;
	}
	std::optional<ear::NodeCounts> spanCounts(std::uint64_t) const override { return std::nullopt; }
	std::vector<ear::CompanionView> spanCompanions(std::uint64_t) const override { return {}; }
};

TEST_CASE("counters-changed.json: the snapshot's counters, counters.changed replacing what it names, and a port-caused "
          "replace's node.added carrying the counts it carries on",
          "[session][protocol][counters]") {
	using ear::NamedCounts;
	using ear::NodeCounts;
	const json document = fixture("counters-changed");
	const auto requests = requestsOf(document);
	World w;
	w.graph.view = ear::GraphView{{{48000, 256}, true, "SCHED_FIFO 88"}, {"card", "file", "white"}, {}, {}};
	const auto cardPorts = [](std::uint32_t playback) {
		return ear::Ports{{{ear::Direction::input, "play", ear::numbered(playback)}}};
	};
	w.graph.view.nodes.push_back(
		{{"desk", "card", {{"card", "MOTU"}, {"playback", 2}}}, cardPorts(2), health("desk", NodeState::running)});
	w.graph.view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}},
	                              ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}},
	                              health("rec", NodeState::running),
	                              std::nullopt,
	                              NodeCounts{.missedCycles = 33, .overruns = 2}});
	w.graph.view.counts = ear::GraphCounts{.cycles = 18750, .cutCycles = 39, .stalls = 1};
	HandCounts counts;
	w.gateway.readCounts(counts);
	RecordingOutbox out;
	ControlSession session(*w.hub, out);

	w.receive(session, requests[0]);
	w.onMain([&] {
		const std::vector<NamedCounts> rec{{"rec", {.missedCycles = 33, .overruns = 3}}};
		w.gateway.countersChanged({.cycles = 37500, .cutCycles = 41, .stalls = 2}, 218, rec);
	});
	w.onMain([&] {
		const std::vector<NamedCounts> two{{"desk", {.missedCycles = 12}}, {"ghost", {.missedCycles = 5}}};
		w.gateway.countersChanged({.cycles = 56250, .cutCycles = 44, .stalls = 2}, 234, two);
	});
	// The channel count changes: a port-caused replace. The gateway hears it before NodeHost does, so the name's counts
	// are still the older node's, which the newer carries on.
	counts.nodes["desk"] = NodeCounts{.missedCycles = 12};
	w.graph.script.push_back({R"(updateNode desk {"card":"MOTU","playback":4})", [&] {
								  const ear::NodeChange ports{ear::NodeOutcome::restart, {"playback"}, true};
								  w.gateway.nodeReplacing("desk", ports);
								  w.gateway.nodeReplaced({"desk", "card", {{"card", "MOTU"}, {"playback", 4}}},
		                                                 std::make_shared<Config>(cardPorts(4)), ports);
								  return Result{};
							  }});
	w.receive(session, requests[1]);
	w.onMain([&] {
		const std::vector<NamedCounts> desk{{"desk", {.missedCycles = 12, .overruns = 1}}};
		w.gateway.countersChanged({.cycles = 75000, .cutCycles = 44, .stalls = 2}, 234, desk);
	});
	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
}

TEST_CASE("A replace that is not port-caused, and a node added, carry zero counters whatever the name counted",
          "[session][counters]") {
	World w;
	w.graph.view = ear::GraphView{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "white"}, {}, {}};
	HandCounts counts;
	counts.nodes["x"] = ear::NodeCounts{.overruns = 9};
	w.gateway.readCounts(counts);
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.receive(session, R"({"jsonrpc":"2.0","id":1,"method":"graph.subscribe"})");
	w.onMain([&] {
		const ear::NodeChange type{ear::NodeOutcome::restart, {}, false};
		w.gateway.nodeReplacing("x", type);
		w.gateway.nodeReplaced({"x", "white", json::object()},
		                       std::make_shared<Config>(ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}}),
		                       type);
		w.nodeAdded({"y", "white", json::object()}, ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}});
	});
	const std::vector<json> got = parsed(out.all);
	REQUIRE(got.size() == 4); // the snapshot, node.removed, node.added x, node.added y
	const json zero = json::parse(R"({"missedCycles":0,"overruns":0,"repositions":0,"driverChanges":0})");
	CHECK(got[2]["params"]["counters"] == zero);
	CHECK(got[3]["params"]["counters"] == zero);
}

TEST_CASE("edits.json: each change's notification comes before its statuses and the response", "[session][protocol]") {
	const json document = fixture("edits");
	const auto requests = requestsOf(document);
	World w;
	w.graph.view = baseView();
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	const ear::Route n2ToRecFr{{"n2", "noise.mono"}, {"rec", "record.right"}};
	const ear::Route n2ToTapeFl{{"n2", "noise.mono"}, {"tape", "record.left"}};
	const ear::Route noiseToRecFl{{"noise", "noise.mono"}, {"rec", "record.left"}};

	w.receive(session, requests[0]);

	w.graph.script.push_back({R"(addNode n2 white {})", [&] {
								  w.nodeAdded({"n2", "white", json::object()},
		                                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}});
								  w.state("n2", NodeState::starting);
								  return Result{};
							  }});
	w.receive(session, requests[1]);
	w.onMain([&] { w.state("n2", NodeState::running); });

	w.graph.script.push_back({"addRoute n2:noise.mono->rec:record.right", [&] {
								  w.gateway.routeAdded(n2ToRecFr);
								  w.route(n2ToRecFr, ear::RouteMatch::pending);
								  return Result{};
							  }});
	w.receive(session, requests[2]);
	w.onMain([&] { w.route(n2ToRecFr, ear::RouteMatch::linked); });

	w.graph.script.push_back(
		{R"(addNode tape file {"path":"nope/t.wav"})", [&] {
			 w.nodeAdded({"tape", "file", {{"path", "nope/t.wav"}}},
		                 ear::Ports{{{ear::Direction::input, "record", {"left", "right"}}}});
			 w.state("tape", NodeState::starting);
			 w.state("tape", NodeState::failed,
		             ear::FailureReason{Code::startFailed, "cannot open nope/t.wav: No such file or directory"});
			 return Result{};
		 }});
	w.receive(session, requests[3]);

	w.graph.script.push_back({"addRoute n2:noise.mono->tape:record.left", [&] {
								  w.gateway.routeAdded(n2ToTapeFl);
								  w.route(n2ToTapeFl, ear::RouteMatch::pending);
								  return Result{};
							  }});
	w.receive(session, requests[4]);
	w.onMain(
		[&] { w.route(n2ToTapeFl, ear::RouteMatch::refused, "the engine removed the link before it was active"); });

	w.graph.script.push_back({R"(updateNode rec {"path":"y.wav"})", [&] {
								  w.gateway.nodeChanged({"rec", "file", {{"path", "y.wav"}}},
		                                                std::make_shared<Config>(ear::Ports{
															{{ear::Direction::input, "record", {"left", "right"}}}}),
		                                                ear::NodeChange{ear::NodeOutcome::restart, {"path"}});
								  using Kind = ear::NodeCause::Kind;
								  auto stopping = health("rec", NodeState::stopping);
								  stopping.cause = ear::NodeCause{Kind::restarting, Kind::settings, {"path"}};
								  w.told(stopping);
								  auto configuring = health("rec", NodeState::starting);
								  configuring.cause = ear::NodeCause{Kind::settings, std::nullopt, {"path"}};
								  w.told(configuring);
								  return Result{};
							  }});
	w.receive(session, requests[5]);
	w.onMain([&] { w.state("rec", NodeState::running); });

	w.graph.script.push_back({"removeRoute n2:noise.mono->rec:record.right", [&] {
								  w.gateway.routeRemoved(n2ToRecFr);
								  return Result{};
							  }});
	w.receive(session, requests[6]);

	w.graph.script.push_back({"removeNode noise", [&] {
								  w.gateway.routeRemoved(noiseToRecFl);
								  w.gateway.nodeRemoved("noise");
								  auto stopping = health("noise", NodeState::stopping);
								  stopping.cause = ear::NodeCause{ear::NodeCause::Kind::removing, std::nullopt, {}};
								  w.told(stopping);
								  return Result{};
							  }});
	w.receive(session, requests[7]);
	w.onMain([&] {
		w.gateway.nodeGone("noise", 0);
		w.state("ghost", NodeState::running);
	});

	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
	CHECK(w.graph.offLoop == 0);
}

TEST_CASE("errors.json: refusals, stopping and envelope errors", "[session][protocol]") {
	const json document = fixture("errors");
	World w;
	w.graph.view = baseView();
	const std::vector<std::pair<std::string, ear::CommandError>> refusals{
		{"addNode a.b white {}", ear::InvalidName{"a.b"}},
		{"addNode pinky pink {}", ear::UnknownNodeType{"pink"}},
		{"addNode tape file {}", ear::InvalidSettings{"tape", "path: missing"}},
		{R"(addNode noise file {"path":"z.wav"})", ear::DuplicateNode{"noise"}},
		{"updateNode ghost {}", ear::UnknownNode{"ghost"}},
		{"removeNode ghost", ear::UnknownNode{"ghost"}},
		{"addRoute noise:noise.mono->rec:FX", ear::UnknownPort{{"rec", "FX"}}},
		{"addRoute rec:record.left->noise:noise.mono",
	     ear::WrongDirection{{{"rec", "record.left"}, {"noise", "noise.mono"}}}},
		{"removeRoute noise:noise.mono->rec:record.right",
	     ear::UnknownRoute{{{"noise", "noise.mono"}, {"rec", "record.right"}}}},
		{"addNode n3 white {}", ear::Stopping{}},
	};
	for (const auto& [call, error] : refusals)
		w.graph.script.push_back({call, [error] { return Result(std::unexpected(error)); }});
	// The lifecycle's refusals (docs/control-port.md#error-codes), then one more while stopping.
	const auto refused = [](CommandResult result) { return [result] { return result; }; };
	w.graph.script.push_back({"startNode porch", refused(ControlRefusal{NodeDisabled{"porch"}})});
	w.graph.script.push_back(
		{"restartNode desk", refused(ControlRefusal{NotRestartable{"desk", NotRestartable::Reason::blockLost}})});
	w.graph.script.push_back(
		{"restartNode fpp", refused(ControlRefusal{NotRestartable{"fpp", NotRestartable::Reason::notStarted}})});
	w.graph.script.push_back({"stopNode ghost", refused(Result(std::unexpected(ear::UnknownNode{"ghost"})))});
	w.graph.script.push_back({"startNode noise", refused(Result(std::unexpected(ear::Stopping{})))});
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
}

namespace {

/// A health at `phase` (its state the phase's colour), with what is wanted.
ear::NodeHealth at(const std::string& node, ear::NodePhase phase, ear::Wanted wanted = ear::Wanted::running) {
	ear::NodeHealth h{node};
	h.phase = phase;
	h.state = ear::stateOf(phase);
	h.wanted = wanted;
	return h;
}

ear::NodeHealth because(ear::NodeHealth h, ear::NodeCause::Kind kind,
                        std::optional<ear::NodeCause::Kind> restart = std::nullopt,
                        std::vector<std::string> keys = {}) {
	h.cause = ear::NodeCause{kind, restart, std::move(keys)};
	return h;
}

ear::NodeHealth failedWith(ear::NodeHealth h, ear::FailureReason reason) {
	h.reason = std::move(reason);
	return h;
}

ear::NodeHealth waiting(ear::NodeHealth h, ear::WaitingFor::Kind kind, std::string name) {
	h.waitingFor = {{kind, std::move(name)}};
	return h;
}

} // namespace

TEST_CASE("lifecycle.json: a node's whole life, each health change one node.state, every reply after the node.state "
          "its request caused",
          "[session][protocol][lifecycle]") {
	using ear::NodePhase;
	using ear::Wanted;
	using Kind = ear::NodeCause::Kind;
	using Lifecycle = std::expected<void, ear::CommandError>;
	const json document = fixture("lifecycle");
	const auto requests = requestsOf(document);
	REQUIRE(requests.size() == 26);
	World w;
	const ear::Ports card2{{{ear::Direction::input, "play", {"1", "2"}}}};
	const ear::Ports noisePorts{{{ear::Direction::output, "noise", {"mono"}}}};
	const ear::Ports recPorts{{{ear::Direction::input, "record", {"left", "right"}}}};
	const ear::Ports rtpPorts{{{ear::Direction::output, "stream", {"1", "2"}}}};
	const json deskSettings = {{"card", "MOTU"}, {"playback", 2}};
	const json porchSettings = {{"card", "USB2"}, {"playback", 2}};
	const ear::FailureReason cannotOpen{Code::startFailed, "cannot open nope/t.wav: No such file or directory"};
	w.graph.view = ear::GraphView{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "file", "rtp", "white"}, {}, {}};
	w.graph.view.nodes.push_back({{"desk", "card", deskSettings}, card2, at("desk", NodePhase::running)});
	w.graph.view.nodes.push_back({{"porch", "card", porchSettings}, card2, at("porch", NodePhase::running)});
	w.onMain([&] { // both running before the client subscribes: the snapshot carries their since
		w.told(at("desk", NodePhase::running));
		w.told(at("porch", NodePhase::running));
	});
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	const auto applied = [](AppliedKind kind) { return [kind]() -> CommandResult { return Applied{kind}; }; };
	const auto refused = [](CommandResult result) { return [result] { return result; }; };

	w.receive(session, requests[0]); // graph.subscribe

	// A normal start: added (node.added), configuring, configured, starting, then running after the reply.
	w.graph.script.push_back({"addNode noise white {}", [&]() -> CommandResult {
								  w.nodeAdded({"noise", "white", json::object()}, noisePorts);
								  w.told(at("noise", NodePhase::added)); // what node.added said: nothing sent
								  w.told(at("noise", NodePhase::configuring));
								  w.told(at("noise", NodePhase::configured));
								  w.told(at("noise", NodePhase::starting));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[1]);
	w.onMain([&] { w.told(at("noise", NodePhase::running)); });

	// A stop and a start: no file, no node.updated.
	w.graph.script.push_back({"stopNode noise", [&]() -> CommandResult {
								  w.told(because(at("noise", NodePhase::stopping, Wanted::stopped), Kind::stop));
								  return Applied{AppliedKind::stop};
							  }});
	w.receive(session, requests[2]);
	w.onMain([&] { w.told(at("noise", NodePhase::stopped, Wanted::stopped)); });
	w.graph.script.push_back({"startNode noise", [&]() -> CommandResult {
								  w.told(at("noise", NodePhase::starting));
								  return Applied{AppliedKind::start};
							  }});
	w.receive(session, requests[3]);
	w.onMain([&] { w.told(at("noise", NodePhase::running)); });

	// A restart: stopping (restarting, action), the reply, then configuring (action) to running.
	w.graph.script.push_back({"restartNode desk", [&]() -> CommandResult {
								  w.told(because(at("desk", NodePhase::stopping), Kind::restarting, Kind::action));
								  return Applied{AppliedKind::restart};
							  }});
	w.receive(session, requests[4]);
	w.onMain([&] {
		w.told(because(at("desk", NodePhase::configuring), Kind::action));
		w.told(at("desk", NodePhase::configured));
		w.told(at("desk", NodePhase::starting));
		w.told(at("desk", NodePhase::running));
	});

	// A disable: node.updated with applied and disabled, stopping (disabling), the reply, then disabled.
	ear::NodeSpec porchDisabled{"porch", "card", porchSettings};
	porchDisabled.disabled = true;
	w.graph.script.push_back({"disableNode porch", [&]() -> CommandResult {
								  w.gateway.nodeChanged(porchDisabled, std::make_shared<Config>(card2),
		                                                ear::NodeChange{ear::NodeOutcome::disable, {}});
								  w.told(because(at("porch", NodePhase::stopping, Wanted::disabled), Kind::disabling));
								  return Applied{AppliedKind::disable};
							  }});
	w.receive(session, requests[5]);
	w.onMain([&] { w.told(at("porch", NodePhase::disabled, Wanted::disabled)); });
	w.graph.script.push_back({"startNode porch", refused(ControlRefusal{NodeDisabled{"porch"}})});
	w.receive(session, requests[6]);

	// An enable: node.updated (no disabled), configuring (enabling), the reply, then to running.
	w.graph.script.push_back({"enableNode porch", [&]() -> CommandResult {
								  w.gateway.nodeChanged({"porch", "card", porchSettings},
		                                                std::make_shared<Config>(card2),
		                                                ear::NodeChange{ear::NodeOutcome::enable, {}});
								  w.told(because(at("porch", NodePhase::configuring), Kind::enabling));
								  return Applied{AppliedKind::enable};
							  }});
	w.receive(session, requests[7]);
	w.onMain([&] {
		w.told(at("porch", NodePhase::configured));
		w.told(at("porch", NodePhase::starting));
		w.told(at("porch", NodePhase::running));
	});

	// A node configured while stopped rests stopped again; then a start.
	w.graph.script.push_back({"stopNode porch", [&]() -> CommandResult {
								  w.told(because(at("porch", NodePhase::stopping, Wanted::stopped), Kind::stop));
								  return Applied{AppliedKind::stop};
							  }});
	w.receive(session, requests[8]);
	w.onMain([&] { w.told(at("porch", NodePhase::stopped, Wanted::stopped)); });
	const json usb3 = {{"card", "USB3"}, {"playback", 2}};
	w.graph.script.push_back({R"(updateNode porch {"card":"USB3","playback":2})", [&]() -> CommandResult {
								  w.gateway.nodeChanged({"porch", "card", usb3}, std::make_shared<Config>(card2),
		                                                ear::NodeChange{ear::NodeOutcome::restart, {"card"}});
								  w.told(because(at("porch", NodePhase::stopping, Wanted::stopped), Kind::restarting,
		                                         Kind::settings, {"card"}));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[9]);
	w.onMain([&] {
		w.told(because(at("porch", NodePhase::configuring, Wanted::stopped), Kind::settings, std::nullopt, {"card"}));
		w.told(at("porch", NodePhase::configured, Wanted::stopped));
		w.told(at("porch", NodePhase::stopped, Wanted::stopped));
	});
	w.graph.script.push_back({"startNode porch", [&]() -> CommandResult {
								  w.told(at("porch", NodePhase::starting));
								  return Applied{AppliedKind::start};
							  }});
	w.receive(session, requests[10]);
	w.onMain([&] { w.told(at("porch", NodePhase::running)); });

	// A node added disabled: node.added says disabled, and its first node.state says it again (a protocol 0 client).
	ear::NodeSpec quiet{"quiet", "white", json::object()};
	quiet.disabled = true;
	w.graph.script.push_back({"addNode quiet white {} disabled", [&]() -> CommandResult {
								  w.gateway.nodeAdded(quiet, std::make_shared<Config>(noisePorts));
								  w.told(at("quiet", NodePhase::disabled, Wanted::disabled));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[11]);

	// A failed node: stopped (one node.state, wanted stopped), retried by start and by restart.
	w.graph.script.push_back({R"(addNode tape file {"path":"nope/t.wav"})", [&]() -> CommandResult {
								  w.nodeAdded({"tape", "file", {{"path", "nope/t.wav"}}}, recPorts);
								  w.told(at("tape", NodePhase::configuring));
								  w.told(failedWith(at("tape", NodePhase::failed), cannotOpen));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[12]);
	w.graph.script.push_back({"stopNode tape", [&]() -> CommandResult {
								  w.told(failedWith(at("tape", NodePhase::failed, Wanted::stopped), cannotOpen));
								  return Applied{AppliedKind::stop};
							  }});
	w.receive(session, requests[13]);
	w.graph.script.push_back({"startNode tape", [&]() -> CommandResult {
								  w.told(because(at("tape", NodePhase::configuring), Kind::retry));
								  return Applied{AppliedKind::retry};
							  }});
	w.receive(session, requests[14]);
	w.onMain([&] { w.told(failedWith(at("tape", NodePhase::failed), cannotOpen)); });
	w.graph.script.push_back({"restartNode tape", [&]() -> CommandResult {
								  w.told(because(at("tape", NodePhase::configuring), Kind::retry));
								  return Applied{AppliedKind::retry};
							  }});
	w.receive(session, requests[15]);
	w.onMain([&] { w.told(failedWith(at("tape", NodePhase::failed), cannotOpen)); });

	// A restart during a restart: unchanged.
	w.graph.script.push_back({"restartNode desk", [&]() -> CommandResult {
								  w.told(because(at("desk", NodePhase::stopping), Kind::restarting, Kind::action));
								  return Applied{AppliedKind::restart};
							  }});
	w.graph.script.push_back({"restartNode desk", applied(AppliedKind::unchanged)});
	w.receive(session, requests[16]);
	// The card's block is lost: failed, blockLost; neither a restart nor a start brings it back.
	const ear::FailureReason blockGone{Code::engineRefused, "the engine removed the block"};
	w.onMain([&] {
		auto lost = failedWith(at("desk", NodePhase::failed), blockGone);
		lost.blockLost = true;
		w.told(lost);
	});
	w.graph.script.push_back(
		{"restartNode desk", refused(ControlRefusal{NotRestartable{"desk", NotRestartable::Reason::blockLost}})});
	w.receive(session, requests[17]);
	w.graph.script.push_back(
		{"startNode desk", refused(ControlRefusal{NotRestartable{"desk", NotRestartable::Reason::blockLost}})});
	w.receive(session, requests[18]);

	// A name added back waits for its older node, and cannot restart before it was started.
	w.graph.script.push_back({"removeNode tape", [&]() -> CommandResult {
								  w.gateway.nodeRemoved("tape");
								  w.told(because(at("tape", NodePhase::stopping), Kind::removing));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[19]);
	w.graph.script.push_back({R"(addNode tape file {"path":"t.wav"})", [&]() -> CommandResult {
								  w.nodeAdded({"tape", "file", {{"path", "t.wav"}}}, recPorts); // the older one is
								  w.told(at("tape", NodePhase::added)); // superseded: the gateway hears none of it
								  return Lifecycle{};
							  }});
	w.receive(session, requests[20]);
	w.onMain([&] { w.told(waiting(at("tape", NodePhase::added), ear::WaitingFor::Kind::olderNode, "tape")); });
	w.graph.script.push_back(
		{"restartNode tape", refused(ControlRefusal{NotRestartable{"tape", NotRestartable::Reason::notStarted}})});
	w.receive(session, requests[21]);
	w.onMain([&] {
		w.told(at("tape", NodePhase::configuring));
		w.told(at("tape", NodePhase::configured));
		w.told(at("tape", NodePhase::starting));
		w.told(at("tape", NodePhase::running));
	});

	// A removal: node.removed at the commit, then stopping, node.stopped and node.gone, which the client ignores.
	w.graph.script.push_back({"removeNode noise", [&]() -> CommandResult {
								  w.gateway.nodeRemoved("noise");
								  w.told(because(at("noise", NodePhase::stopping), Kind::removing));
								  return Lifecycle{};
							  }});
	w.receive(session, requests[22]);
	w.onMain([&] { w.gateway.nodeGone("noise", 0); });
	w.graph.script.push_back({"restartNode quiet", refused(ControlRefusal{NodeDisabled{"quiet"}})});
	w.receive(session, requests[23]);

	// A stop while configuring: one node.state, the same phase, wanted stopped; it rests stopped once configured.
	const json fppSettings = {{"address", "0.0.0.0"}, {"port", 5004}, {"format", "L24"}, {"channels", 2}};
	w.graph.script.push_back(
		{R"(addNode fpp rtp {"address":"0.0.0.0","channels":2,"format":"L24","port":5004})", [&]() -> CommandResult {
			 w.nodeAdded({"fpp", "rtp", fppSettings}, rtpPorts);
			 w.told(at("fpp", NodePhase::configuring));
			 return Lifecycle{};
		 }});
	w.receive(session, requests[24]);
	w.onMain([&] { w.told(waiting(at("fpp", NodePhase::configuring), ear::WaitingFor::Kind::companion, "fpp.rtp")); });
	w.graph.script.push_back({"stopNode fpp", [&]() -> CommandResult {
								  w.told(waiting(at("fpp", NodePhase::configuring, Wanted::stopped),
		                                         ear::WaitingFor::Kind::companion, "fpp.rtp"));
								  return Applied{AppliedKind::stop};
							  }});
	w.receive(session, requests[25]);
	w.onMain([&] {
		w.told(at("fpp", NodePhase::configured, Wanted::stopped));
		w.told(at("fpp", NodePhase::stopped, Wanted::stopped));
	});

	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
	CHECK(w.graph.offLoop == 0);
}

TEST_CASE("A snapshot read before a verb in the same batch carries the since of the phase it read",
          "[session][lifecycle]") {
	World w;
	w.graph.view = baseView();
	w.onMain([&] { w.state("noise", NodeState::running); }); // since: reading 0
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.graph.script.push_back({"stopNode noise", [&]() -> CommandResult {
								  auto stopping = health("noise", NodeState::stopping);
								  stopping.cause = ear::NodeCause{ear::NodeCause::Kind::stop, std::nullopt, {}};
								  stopping.wanted = ear::Wanted::stopped;
								  w.told(stopping); // since: reading 1
								  return Applied{AppliedKind::stop};
							  }});
	w.receive(session, R"([{"jsonrpc":"2.0","id":1,"method":"graph.get"},
	                       {"jsonrpc":"2.0","id":2,"method":"node.stop","params":{"name":"noise"}}])");
	REQUIRE(out.all.size() == 1);
	const json batch = json::parse(out.all[0]);
	REQUIRE(batch.size() == 2);
	const json& noise = batch[0]["result"]["nodes"][0];
	CHECK(noise["phase"] == "running");
	CHECK(noise["since"] == "2026-11-09T10:00:00.000000Z");
	CHECK(batch[1]["result"] == json{{"applied", "stop"}});
}

TEST_CASE("levels.json: levels.subscribe, then complete snapshots", "[session][levels][protocol]") {
	const json document = fixture("levels");
	World w;
	w.graph.view = baseView();
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	w.levels.latest = {{"noise", 1600, {{"noise.mono", {0.99981F, 0.57712F}}}},
	                   {"rec", 1600, {{"record.left", {0.99978F, 0.57709F}}, {"record.right", {0.0F, 0.0F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	w.levels.latest = {{"rec", 3200, {{"record.left", {0.9991F, 0.5779F}}, {"record.right", {0.0F, 0.0F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	checkMessages(out, document["messages"]);
	CHECK(out.levels.size() == 2);
}

TEST_CASE("graph-file.json: file changes come before the events they cause, and both before the response",
          "[session][protocol][graph-file]") {
	namespace fx = ear::control::testing;
	using ear::NodeState;
	using Code = ear::FailureReason::Code;
	const json document = fixture("graph-file");
	const auto requests = requestsOf(document);
	REQUIRE(requests.size() == 15);
	const fx::Texts t;
	const ear::Route toDesk{{"noise", "noise.mono"}, {"desk", "play.1"}};
	const ear::Ports card2{{{ear::Direction::input, "play", {"1", "2"}}}};
	const ear::Ports card4{{{ear::Direction::input, "play", ear::numbered(4)}}};
	const auto config = [](ear::Ports ports) { return std::make_shared<Config>(std::move(ports)); };
	const ear::Ports recPorts{{{ear::Direction::input, "record", {"left", "right"}}}};

	World w;
	w.graph.view = fx::graphView(2, "take1.wav");
	w.graph.file = fx::file("v1", t.t1, "v1");
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	// An edit the keeper accepts: its file change, then rec's update.
	const auto recEdit = [&](GraphFile file, const std::string& path) {
		return [&w, &config, &recPorts, file = std::move(file), path]() -> CommandResult {
			w.fileChanged(file, FileChangeCause::edit);
			w.gateway.nodeUpdated({"rec", "file", {{"path", path}}}, config(recPorts));
			return Result{};
		};
	};
	const auto refused = [](CommandResult result) { return [result] { return result; }; };

	w.receive(session, requests[0]); // graphFile.subscribe
	w.receive(session, requests[1]); // graph.subscribe

	w.graph.script.push_back(
		{R"(updateNode rec {"path":"take2.wav"})", recEdit(fx::file("v2", t.t2, "v2"), "take2.wav")});
	w.receive(session, requests[2]);

	w.graph.script.push_back({"setGraphFile v1", refused(FileRefusal{FileChanged{}})});
	w.receive(session, requests[3]);
	w.graph.script.push_back(
		{"setGraphFile v2", refused(FileRefusal{InvalidText{{Diagnostic{
								Severity::error, "E2001", "unexpected token: \"}\"", TextPlace{9, 34}}}}})});
	w.receive(session, requests[4]);
	w.graph.script.push_back(
		{"addRoute noise:noise.mono->rec:FX", refused(Result(std::unexpected(ear::UnknownPort{{"rec", "FX"}})))});
	w.receive(session, requests[5]);

	GraphFile included = fx::file("v3", t.t3, "v2");
	included.diagnostics = {fx::includeUnsupported};
	w.onMain([&] { w.fileChanged(included, FileChangeCause::disk); }); // a hand edit with @include
	w.graph.script.push_back({R"(updateNode rec {"path":"take3.wav"})", refused(FileRefusal{FileInvalid{}})});
	w.receive(session, requests[6]);
	w.onMain([&] { w.fileChanged(fx::file("v4", t.t2, "v4"), FileChangeCause::disk); }); // the hand fix

	w.graph.script.push_back(
		{R"(updateNode rec {"path":"take3.wav"})", recEdit(fx::file("v5", t.t5, "v5"), "take3.wav")});
	w.receive(session, requests[7]);
	w.onMain([&] { // a hand save over it before it was written: the hand edit wins
		w.fileChanged(fx::file("v6", t.t2, "v6"), FileChangeCause::disk, 1);
		w.gateway.nodeUpdated({"rec", "file", {{"path", "take2.wav"}}}, config(recPorts));
	});

	w.graph.script.push_back({R"(updateNode desk {"card":"MOTU"})", refused(FileRefusal{SharedDefinition{{6, 5}}})});
	w.receive(session, requests[8]);

	w.graph.script.push_back({R"(updateNode desk {"card":"MOTU","playback":4})", [&]() -> CommandResult {
								  w.fileChanged(fx::file("v7", t.t7, "v7"), FileChangeCause::edit);
								  w.gateway.routeRemoved(toDesk);
								  w.gateway.nodeRemoved("desk");
								  w.gateway.nodeRemoved("porch");
								  w.nodeAdded({"desk", "card", {{"card", "MOTU"}, {"playback", 4}}}, card4);
								  w.nodeAdded({"porch", "card", {{"card", "USB2"}, {"playback", 4}}}, card4);
								  w.gateway.routeAdded(toDesk);
								  return Result{};
							  }});
	w.receive(session, requests[9]);
	w.onMain([&] {
		w.state("desk", NodeState::failed,
		        ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=MOTU,DEV=0: No such device"});
		w.state("porch", NodeState::failed,
		        ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=USB2,DEV=0: No such device"});
	});
	GraphFile failed = fx::file("v7", t.t7, "v7");
	failed.writeFailed = "ear.conf: No space left on device";
	w.onMain([&] { w.fileChanged(failed, FileChangeCause::write); });
	w.onMain([&] { w.fileChanged(fx::file("v7", t.t7, "v7"), FileChangeCause::write); });

	w.graph.script.push_back(
		{R"(updateNode rec {"path":"take3.wav"})", recEdit(fx::file("v8", t.t8, "v8"), "take3.wav")});
	w.receive(session, requests[10]);
	GraphFile refusedByJudge = fx::file("v9", t.t9, "v8");
	refusedByJudge.refusal = Refusal{ear::UnknownPort{{"rec", "FX"}}, TextPlace{14, 1}};
	w.onMain([&] { w.fileChanged(refusedByJudge, FileChangeCause::disk, 1); });

	GraphFile pending = fx::file("v10", t.t10, "v10");
	pending.pendingRestart = {FormatKey::graphFrequency};
	w.graph.script.push_back({"setGraphFile v9", recEdit(pending, "take2.wav")});
	w.receive(session, requests[11]);

	GraphFile last = fx::file("v11", t.t11, "v11");
	last.pendingRestart = {FormatKey::graphFrequency};
	w.graph.view = fx::graphView(4, "take4.wav");
	w.graph.file = last;
	w.graph.script.push_back({R"(updateNode rec {"path":"take4.wav"})", recEdit(last, "take4.wav")});
	w.receive(session, requests[12]); // [node.update, graph.subscribe]
	w.receive(session, requests[13]); // [graph.subscribe, graphFile.subscribe]
	w.receive(session, requests[14]); // graphFile.get

	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
	CHECK(w.graph.offLoop == 0);
	// Each batch's snapshots ran after its other commands, in request order; graphFile.get read the file.
	const std::vector<std::string> tail(w.graph.calls.end() - 5, w.graph.calls.end());
	CHECK(tail ==
	      std::vector<std::string>{R"(updateNode rec {"path":"take4.wav"})", "read", "read", "readFile", "readFile"});
}

TEST_CASE("graph.subscribe gets graph events only, graphFile.subscribe file changes only", "[session][graph-file]") {
	World w;
	RecordingOutbox graphOnly, fileOnly, both;
	ControlSession graphSession(*w.hub, graphOnly), fileSession(*w.hub, fileOnly), bothSession(*w.hub, both);
	w.receive(graphSession, R"({"jsonrpc":"2.0","id":1,"method":"graph.subscribe"})");
	w.receive(fileSession, R"({"jsonrpc":"2.0","id":1,"method":"graphFile.subscribe","params":{}})");
	w.receive(bothSession, R"([{"jsonrpc":"2.0","id":1,"method":"graphFile.subscribe"},
	                          {"jsonrpc":"2.0","id":2,"method":"graph.subscribe"}])");
	w.onMain([&] {
		w.fileChanged(ear::control::testing::file("a.2", "node white n;\n", "a.2"), FileChangeCause::edit);
		w.gateway.nodeRemoved("n");
	});
	const auto methods = [](const RecordingOutbox& out) {
		std::vector<std::string> seen;
		for (const json& message : parsed(out.all))
			seen.push_back(message.is_array() || message.contains("result") ? "snapshot"
			                                                                : message["method"].get<std::string>());
		return seen;
	};
	CHECK(methods(graphOnly) == std::vector<std::string>{"snapshot", "node.removed"});
	CHECK(methods(fileOnly) == std::vector<std::string>{"snapshot", "graphFile.changed"});
	CHECK(methods(both) == std::vector<std::string>{"snapshot", "graphFile.changed", "node.removed"});
	CHECK(w.graph.calls == std::vector<std::string>{"read", "readFile", "readFile", "read"});
}

TEST_CASE("A session gets no file change from before its graphFile snapshot and every one after",
          "[session][graph-file]") {
	World w;
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.main.post([&] { w.fileChanged(ear::control::testing::file("a.1", "", std::nullopt), FileChangeCause::disk); });
	w.onControl([&] { session.received(R"({"jsonrpc":"2.0","id":1,"method":"graphFile.subscribe"})", [] {}); });
	w.main.post([&] { w.fileChanged(ear::control::testing::file("a.2", "", std::nullopt), FileChangeCause::write); });
	w.pump();
	const std::vector<json> got = parsed(out.all);
	REQUIRE(got.size() == 2);
	CHECK(got[0].contains("result"));
	CHECK(got[1]["params"]["version"] == "a.2");
}

TEST_CASE("A session gets no event from before its snapshot and every one after, in order", "[session]") {
	World w;
	RecordingOutbox earlyOut;
	RecordingOutbox lateOut;
	ControlSession early(*w.hub, earlyOut);
	ControlSession late(*w.hub, lateOut);
	w.receive(early, R"({"jsonrpc":"2.0","id":1,"method":"graph.subscribe"})");

	// On the main loop: "before" happens, then late's snapshot is read, then "after" happens.
	w.main.post([&] { w.gateway.nodeRemoved("before"); });
	w.onControl([&] { late.received(R"({"jsonrpc":"2.0","id":1,"method":"graph.subscribe"})", [] {}); });
	w.main.post([&] { w.gateway.nodeRemoved("after"); });
	w.main.post([&] { w.gateway.nodeRemoved("later"); });
	w.pump();

	const auto names = [](const RecordingOutbox& out) {
		std::vector<std::string> seen;
		for (const json& message : parsed(out.all))
			seen.push_back(message.contains("result") ? "snapshot" : message["params"]["name"].get<std::string>());
		return seen;
	};
	CHECK(names(earlyOut) == std::vector<std::string>{"snapshot", "before", "after", "later"});
	CHECK(names(lateOut) == std::vector<std::string>{"snapshot", "after", "later"});
}

TEST_CASE("A batch's subscribe snapshot holds the batch's own changes", "[session]") {
	World w;
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.graph.script.push_back({"addNode x white {}", [&] {
								  w.nodeAdded({"x", "white", json::object()},
		                                      ear::Ports{{{ear::Direction::output, "noise", {"mono"}}}});
								  return Result{};
							  }});
	w.receive(session, R"([{"jsonrpc":"2.0","id":1,"method":"graph.subscribe"},
	                      {"jsonrpc":"2.0","id":2,"method":"node.add","params":{"name":"x","type":"white"}}])");
	CHECK(w.graph.calls == std::vector<std::string>{"addNode x white {}", "read"});
	REQUIRE(out.all.size() == 1); // the batch's responses; node.added x is in the snapshot
	const json batch = json::parse(out.all[0]);
	REQUIRE(batch.size() == 2);
	CHECK(batch[0]["id"] == 1);
	CHECK(batch[0]["result"].contains("nodes"));
	CHECK(batch[1] == json::parse(R"({"jsonrpc":"2.0","id":2,"result":{}})"));
	w.onMain([&] { w.state("x", NodeState::running); }); // later events reach it
	REQUIRE(out.all.size() == 2);
	CHECK(json::parse(out.all[1])["method"] == "node.state");
}

TEST_CASE("Responses keep request order, notifications get none, and done follows the responses", "[session]") {
	World w;
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.graph.script.push_back({"removeNode a", {}});
	w.graph.script.push_back({"addNode b white {}", {}});
	std::size_t queuedAtDone = 0;
	int done = 0;
	w.onControl([&] {
		session.received(R"([
		  {"jsonrpc":"2.0","method":"sum","params":[1,2,4],"id":"1"},
		  {"jsonrpc":"2.0","method":"notify_hello","params":[7]},
		  {"jsonrpc":"2.0","method":"node.remove","params":{"name":"a"},"id":"r"},
		  {"foo":"boo"},
		  {"jsonrpc":"2.0","method":"foo.get","params":{"name":"myself"},"id":"5"},
		  {"jsonrpc":"2.0","method":"node.add","params":{"name":"b","type":"white"}},
		  {"jsonrpc":"2.0","method":"levels.subscribe","id":"l"},
		  {"jsonrpc":"2.0","method":"get_data","id":"9"}
		])",
		                 [&] {
							 ++done;
							 queuedAtDone = out.all.size();
						 });
	});
	CHECK(done == 0); // its commands are on the main loop
	w.pump();
	CHECK(done == 1);
	CHECK(queuedAtDone == 1);
	CHECK(w.graph.script.empty()); // the notification node.add ran
	REQUIRE(out.all.size() == 1);
	CHECK(json::parse(out.all[0]) == json::parse(R"([
	  {"jsonrpc":"2.0","id":"1","error":{"code":-32602,"message":"the document: must be an object","data":{"pointer":""}}},
	  {"jsonrpc":"2.0","id":"r","result":{}},
	  {"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}},
	  {"jsonrpc":"2.0","id":"5","error":{"code":-32601,"message":"Method not found"}},
	  {"jsonrpc":"2.0","id":"l","result":{"windowsPerSecond":30}},
	  {"jsonrpc":"2.0","id":"9","error":{"code":-32601,"message":"Method not found"}}
	])"));
}

TEST_CASE("A message of notifications only sends nothing and still calls done", "[session]") {
	World w;
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	w.graph.script.push_back({"removeNode a", {}});
	int done = 0;
	w.receive(session, R"({"jsonrpc":"2.0","method":"node.remove","params":{"name":"a"}})", &done);
	w.receive(session, R"([{"jsonrpc":"2.0","method":"graph.get"},{"jsonrpc":"2.0","method":"nope"}])", &done);
	w.receive(session, R"({"jsonrpc":"2.0","method":"update","params":[1,2,3,4,5]})", &done);
	CHECK(done == 3);
	CHECK(out.all.empty());
	CHECK(w.graph.script.empty());
}

TEST_CASE("Levels: one complete snapshot per levels-subscribed session per bell", "[session][levels]") {
	World w;
	RecordingOutbox a, b, quiet;
	ControlSession sa(*w.hub, a), sb(*w.hub, b), sq(*w.hub, quiet);
	w.receive(sa, R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
	w.receive(sb, R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe","params":{}})");
	w.receive(sq, R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe","params":{"rate":60}})");
	CHECK(json::parse(quiet.all.at(0))["error"]["message"] == "/rate: unknown key");
	const int sendsBefore = a.sends + b.sends;
	w.levels.latest = {{"n1", 1600, {{"out.a", {0.5F, 0.25F}}}}, {"n2", 1600, {{"in.a", {0.125F, 0.0625F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	CHECK(w.levels.reads == 1); // read and encoded once for every session
	REQUIRE(a.levels.size() == 1);
	REQUIRE(b.levels.size() == 1);
	CHECK(quiet.levels.empty());
	CHECK(a.sends + b.sends == sendsBefore); // levels never queue behind responses
	const json nodes = json::parse(a.levels[0])["params"]["nodes"];
	CHECK(nodes.size() == 2);
	CHECK(nodes["n1"]["out.a"] == json::array({0.5, 0.25}));
	CHECK(nodes["n2"]["in.a"] == json::array({0.125, 0.0625}));
	CHECK(a.levels[0] == b.levels[0]);

	w.levels.latest = {{"n2", 3200, {{"in.a", {0.0F, 0.0F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	REQUIRE(a.levels.size() == 2);
	CHECK(json::parse(a.levels[1])["params"]["nodes"].size() == 1); // n1 gone: absent
}

TEST_CASE("A session closed while its command is on the main loop drops the reply", "[session]") {
	World w;
	RecordingOutbox out;
	auto session = std::make_unique<ControlSession>(*w.hub, out);
	int done = 0;
	w.graph.script.push_back({"removeNode a", {}});
	w.onControl([&] {
		session->received(R"({"jsonrpc":"2.0","id":1,"method":"node.remove","params":{"name":"a"}})", [&] { ++done; });
		session.reset();
	});
	w.pump();
	CHECK(w.graph.script.empty()); // the command still ran
	CHECK(out.all.empty());
	CHECK(done == 0);
}
