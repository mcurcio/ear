// ControlSession and ControlHub (docs/design/batch2.md sections 4.5 and 6, R1 criteria 1, 3, 5,
// 6 and 7; docs/design/knobly-config.md, S5 criterion T10), through the real MainLoopGateway on two
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
	std::expected<std::unique_ptr<ear::NodeInsides>, ear::FailureReason> start(ear::NodeGrant&) const override {
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
				return "addNode " + c.node.name + " " + c.node.type + " " + c.node.settings.dump();
			else if constexpr (std::is_same_v<C, UpdateNode>) return "updateNode " + c.node + " " + c.settings.dump();
			else if constexpr (std::is_same_v<C, RemoveNode>) return "removeNode " + c.node;
			else if constexpr (std::is_same_v<C, AddRoute>) return "addRoute " + text(c.route);
			else if constexpr (std::is_same_v<C, RemoveRoute>) return "removeRoute " + text(c.route);
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
	struct Step { std::string call; std::function<CommandResult()> handler; };
	std::deque<Step> script;
	std::vector<std::string> calls;
	ear::GraphView view{};
	GraphFile file{};
	const QueueExecutor* const* running = nullptr;
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
		if (std::holds_alternative<GetGraph>(command)) return view;
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
	std::vector<std::string> all;      // responses, notifications and levels, in order
	std::vector<std::string> levels;   // what came through sendLevels
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
	MainLoopGateway gateway{main, control, graph};
	FakeLevels levels;
	std::unique_ptr<ControlHub> hub;

	World() {
		graph.running = &running;
		graph.mainLoop = &main;
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
		onControl([&] { session.received(text, [doneCount] { if (doneCount) ++*doneCount; }); });
		pump();
	}
	// The listener calls the keeper's main-loop side and the core make on the main loop.
	void fileChanged(GraphFile file, FileChangeCause cause, std::uint32_t dropped = 0) {
		gateway.graphFileChanged(GraphFileChanged{std::move(file), cause, dropped});
	}
	void nodeAdded(const ear::NodeSpec& node, ear::Ports ports) {
		gateway.nodeAdded(node, std::make_shared<Config>(std::move(ports)));
	}
	void state(const std::string& node, ear::NodeState now, std::optional<ear::FailureReason> reason = {}) {
		gateway.nodeChanged({node, now, std::move(reason)});
	}
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
	view.nodes.push_back({{"noise", "white", json::object()}, {{}, {"MONO"}}, NodeState::running, std::nullopt});
	view.nodes.push_back({{"rec", "file", {{"path", "x.wav"}}}, {{"FL", "FR"}, {}}, NodeState::running, std::nullopt});
	view.routes.push_back({{{"noise", "MONO"}, {"rec", "FL"}}, ear::RouteMatch::linked, ""});
	return view;
}

std::vector<std::string> requestsOf(const json& document) {
	return document["requests"].get<std::vector<std::string>>();
}

} // namespace

TEST_CASE("snapshot.json: graph.get and graph.subscribe answer with the whole graph", "[session][protocol]") {
	const json document = fixture("snapshot");
	World w;
	w.graph.view = baseView();
	w.graph.view.engine = {{48000, 256}, true, "SCHED_FIFO 88"};
	w.graph.view.nodes.insert(w.graph.view.nodes.begin(),
	                          ear::NodeView{{"motu", "card", {{"card", "Pro24"}, {"playback", 2}}},
	                           {{"playback_1", "playback_2"}, {}}, NodeState::failed,
	                           ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=Pro24,DEV=0: No such device"}});
	w.graph.view.nodes.push_back({{"tape", "file", {{"path", "nope/t.wav"}}}, {{"FL", "FR"}, {}}, NodeState::failed,
	                              ear::FailureReason{Code::startFailed, "cannot open nope/t.wav: No such file or directory"}});
	w.graph.view.routes.insert(w.graph.view.routes.begin(),
	                           ear::RouteStatus{{{"noise", "MONO"}, {"motu", "playback_1"}}, ear::RouteMatch::pending, ""});
	w.graph.view.routes.push_back({{{"noise", "MONO"}, {"tape", "FL"}}, ear::RouteMatch::refused,
	                               "the engine removed the link before it was active"});
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	checkMessages(out, document["messages"]);
	CHECK(w.graph.offLoop == 0);
}

TEST_CASE("edits.json: each change's notification comes before its statuses and the response", "[session][protocol]") {
	const json document = fixture("edits");
	const auto requests = requestsOf(document);
	World w;
	w.graph.view = baseView();
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	const ear::Route n2ToRecFr{{"n2", "MONO"}, {"rec", "FR"}};
	const ear::Route n2ToTapeFl{{"n2", "MONO"}, {"tape", "FL"}};
	const ear::Route noiseToRecFl{{"noise", "MONO"}, {"rec", "FL"}};

	w.receive(session, requests[0]);

	w.graph.script.push_back({R"(addNode n2 white {})", [&] {
		                          w.nodeAdded({"n2", "white", json::object()}, {{}, {"MONO"}});
		                          w.state("n2", NodeState::starting);
		                          return Result{};
	                          }});
	w.receive(session, requests[1]);
	w.onMain([&] { w.state("n2", NodeState::running); });

	w.graph.script.push_back({"addRoute n2:MONO->rec:FR", [&] {
		                          w.gateway.routeAdded(n2ToRecFr);
		                          w.route(n2ToRecFr, ear::RouteMatch::pending);
		                          return Result{};
	                          }});
	w.receive(session, requests[2]);
	w.onMain([&] { w.route(n2ToRecFr, ear::RouteMatch::linked); });

	w.graph.script.push_back({R"(addNode tape file {"path":"nope/t.wav"})", [&] {
		                          w.nodeAdded({"tape", "file", {{"path", "nope/t.wav"}}}, {{"FL", "FR"}, {}});
		                          w.state("tape", NodeState::starting);
		                          w.state("tape", NodeState::failed,
		                                  ear::FailureReason{Code::startFailed, "cannot open nope/t.wav: No such file or directory"});
		                          return Result{};
	                          }});
	w.receive(session, requests[3]);

	w.graph.script.push_back({"addRoute n2:MONO->tape:FL", [&] {
		                          w.gateway.routeAdded(n2ToTapeFl);
		                          w.route(n2ToTapeFl, ear::RouteMatch::pending);
		                          return Result{};
	                          }});
	w.receive(session, requests[4]);
	w.onMain([&] { w.route(n2ToTapeFl, ear::RouteMatch::refused, "the engine removed the link before it was active"); });

	w.graph.script.push_back({R"(updateNode rec {"path":"y.wav"})", [&] {
		                          w.gateway.nodeUpdated({"rec", "file", {{"path", "y.wav"}}},
		                                                std::make_shared<Config>(ear::Ports{{"FL", "FR"}, {}}));
		                          w.state("rec", NodeState::starting);
		                          return Result{};
	                          }});
	w.receive(session, requests[5]);
	w.onMain([&] { w.state("rec", NodeState::running); });

	w.graph.script.push_back({"removeRoute n2:MONO->rec:FR", [&] {
		                          w.gateway.routeRemoved(n2ToRecFr);
		                          return Result{};
	                          }});
	w.receive(session, requests[6]);

	w.graph.script.push_back({"removeNode noise", [&] {
		                          w.gateway.routeRemoved(noiseToRecFl);
		                          w.gateway.nodeRemoved("noise");
		                          w.state("noise", NodeState::stopping);
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
		{"addRoute noise:MONO->rec:FX", ear::UnknownPort{{"rec", "FX"}}},
		{"addRoute rec:FL->noise:MONO", ear::WrongDirection{{{"rec", "FL"}, {"noise", "MONO"}}}},
		{"removeRoute noise:MONO->rec:FR", ear::UnknownRoute{{{"noise", "MONO"}, {"rec", "FR"}}}},
		{"addNode n3 white {}", ear::Stopping{}},
	};
	for (const auto& [call, error] : refusals)
		w.graph.script.push_back({call, [error] { return Result(std::unexpected(error)); }});
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
}

TEST_CASE("levels.json: levels.subscribe, then complete snapshots", "[session][levels][protocol]") {
	const json document = fixture("levels");
	World w;
	w.graph.view = baseView();
	RecordingOutbox out;
	ControlSession session(*w.hub, out);
	for (const auto& request : requestsOf(document)) w.receive(session, request);
	w.levels.latest = {{"noise", 1600, {{"MONO", {0.99981F, 0.57712F}}}},
	                   {"rec", 1600, {{"FL", {0.99978F, 0.57709F}}, {"FR", {0.0F, 0.0F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	w.levels.latest = {{"rec", 3200, {{"FL", {0.9991F, 0.5779F}}, {"FR", {0.0F, 0.0F}}}}};
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
	const ear::Route toDesk{{"noise", "MONO"}, {"desk", "playback_1"}};
	const ear::Ports card2{{"playback_1", "playback_2"}, {}};
	ear::Ports card4;
	for (int i = 1; i <= 4; ++i) card4.inputs.push_back("playback_" + std::to_string(i));
	const auto config = [](ear::Ports ports) { return std::make_shared<Config>(std::move(ports)); };
	const ear::Ports recPorts{{"FL", "FR"}, {}};

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

	w.receive(session, requests[0]);   // graphFile.subscribe
	w.receive(session, requests[1]);   // graph.subscribe

	w.graph.script.push_back({R"(updateNode rec {"path":"take2.wav"})", recEdit(fx::file("v2", t.t2, "v2"), "take2.wav")});
	w.receive(session, requests[2]);

	w.graph.script.push_back({"setGraphFile v1", refused(FileRefusal{FileChanged{}})});
	w.receive(session, requests[3]);
	w.graph.script.push_back(
	    {"setGraphFile v2", refused(FileRefusal{InvalidText{{Diagnostic{Severity::error, "E2001", "unexpected token: \"}\"",
	                                                                    TextPlace{9, 34}}}}})});
	w.receive(session, requests[4]);
	w.graph.script.push_back({"addRoute noise:MONO->rec:FX", refused(Result(std::unexpected(ear::UnknownPort{{"rec", "FX"}})))});
	w.receive(session, requests[5]);

	GraphFile included = fx::file("v3", t.t3, "v2");
	included.diagnostics = {fx::includeUnsupported};
	w.onMain([&] { w.fileChanged(included, FileChangeCause::disk); });   // a hand edit with @include
	w.graph.script.push_back({R"(updateNode rec {"path":"take3.wav"})", refused(FileRefusal{FileInvalid{}})});
	w.receive(session, requests[6]);
	w.onMain([&] { w.fileChanged(fx::file("v4", t.t2, "v4"), FileChangeCause::disk); });   // the hand fix

	w.graph.script.push_back({R"(updateNode rec {"path":"take3.wav"})", recEdit(fx::file("v5", t.t5, "v5"), "take3.wav")});
	w.receive(session, requests[7]);
	w.onMain([&] {   // a hand save over it before it was written: the hand edit wins
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
		w.state("desk", NodeState::failed, ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=MOTU,DEV=0: No such device"});
		w.state("porch", NodeState::failed, ear::FailureReason{Code::engineRefused, "cannot open hw:CARD=USB2,DEV=0: No such device"});
	});
	GraphFile failed = fx::file("v7", t.t7, "v7");
	failed.writeFailed = "ear.conf: No space left on device";
	w.onMain([&] { w.fileChanged(failed, FileChangeCause::write); });
	w.onMain([&] { w.fileChanged(fx::file("v7", t.t7, "v7"), FileChangeCause::write); });

	w.graph.script.push_back({R"(updateNode rec {"path":"take3.wav"})", recEdit(fx::file("v8", t.t8, "v8"), "take3.wav")});
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
	w.receive(session, requests[12]);   // [node.update, graph.subscribe]
	w.receive(session, requests[13]);   // [graph.subscribe, graphFile.subscribe]
	w.receive(session, requests[14]);   // graphFile.get

	checkMessages(out, document["messages"]);
	CHECK(w.graph.script.empty());
	CHECK(w.graph.offLoop == 0);
	// Each batch's snapshots ran after its other commands, in request order; graphFile.get read the file.
	const std::vector<std::string> tail(w.graph.calls.end() - 5, w.graph.calls.end());
	CHECK(tail == std::vector<std::string>{R"(updateNode rec {"path":"take4.wav"})", "read", "read", "readFile", "readFile"});
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
			seen.push_back(message.is_array() || message.contains("result") ? "snapshot" : message["method"].get<std::string>());
		return seen;
	};
	CHECK(methods(graphOnly) == std::vector<std::string>{"snapshot", "node.removed"});
	CHECK(methods(fileOnly) == std::vector<std::string>{"snapshot", "graphFile.changed"});
	CHECK(methods(both) == std::vector<std::string>{"snapshot", "graphFile.changed", "node.removed"});
	CHECK(w.graph.calls == std::vector<std::string>{"read", "readFile", "readFile", "read"});
}

TEST_CASE("A session gets no file change from before its graphFile snapshot and every one after", "[session][graph-file]") {
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
		                          w.nodeAdded({"x", "white", json::object()}, {{}, {"MONO"}});
		                          return Result{};
	                          }});
	w.receive(session, R"([{"jsonrpc":"2.0","id":1,"method":"graph.subscribe"},
	                      {"jsonrpc":"2.0","id":2,"method":"node.add","params":{"name":"x","type":"white"}}])");
	CHECK(w.graph.calls == std::vector<std::string>{"addNode x white {}", "read"});
	REQUIRE(out.all.size() == 1);   // the batch's responses; node.added x is in the snapshot
	const json batch = json::parse(out.all[0]);
	REQUIRE(batch.size() == 2);
	CHECK(batch[0]["id"] == 1);
	CHECK(batch[0]["result"].contains("nodes"));
	CHECK(batch[1] == json::parse(R"({"jsonrpc":"2.0","id":2,"result":{}})"));
	w.onMain([&] { w.state("x", NodeState::running); });   // later events reach it
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
		])", [&] {
			++done;
			queuedAtDone = out.all.size();
		});
	});
	CHECK(done == 0);   // its commands are on the main loop
	w.pump();
	CHECK(done == 1);
	CHECK(queuedAtDone == 1);
	CHECK(w.graph.script.empty());   // the notification node.add ran
	REQUIRE(out.all.size() == 1);
	CHECK(json::parse(out.all[0]) == json::parse(R"([
	  {"jsonrpc":"2.0","id":"1","error":{"code":-32602,"message":"the document: must be an object"}},
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
	w.levels.latest = {{"n1", 1600, {{"OUT", {0.5F, 0.25F}}}}, {"n2", 1600, {{"IN", {0.125F, 0.0625F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	CHECK(w.levels.reads == 1);   // read and encoded once for every session
	REQUIRE(a.levels.size() == 1);
	REQUIRE(b.levels.size() == 1);
	CHECK(quiet.levels.empty());
	CHECK(a.sends + b.sends == sendsBefore);   // levels never queue behind responses
	const json nodes = json::parse(a.levels[0])["params"]["nodes"];
	CHECK(nodes.size() == 2);
	CHECK(nodes["n1"]["OUT"] == json::array({0.5, 0.25}));
	CHECK(nodes["n2"]["IN"] == json::array({0.125, 0.0625}));
	CHECK(a.levels[0] == b.levels[0]);

	w.levels.latest = {{"n2", 3200, {{"IN", {0.0F, 0.0F}}}}};
	w.onControl([&] { w.hub->levelsPublished(); });
	REQUIRE(a.levels.size() == 2);
	CHECK(json::parse(a.levels[1])["params"]["nodes"].size() == 1);   // n1 gone: absent
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
	CHECK(w.graph.script.empty());   // the command still ran
	CHECK(out.all.empty());
	CHECK(done == 0);
}
