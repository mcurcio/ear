// ControlSession and ControlHub (docs/design/batch2.md sections 4.5 and 6, R1 criteria 1, 3, 5,
// 6 and 7), through the real MainLoopGateway on two queue executors the test runs by hand, with
// a scripted graph that, like DesiredGraph, NodeHost and the Reconciler, tells the gateway each
// change. Every fixture in test/protocol is replayed request by request and the session's
// output must equal its messages exactly.
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/control/session.hpp"

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

/// Each control call must be the next one scripted; its handler runs inside the call, on the
/// main loop, and returns the call's result.
class ScriptedGraph final : public ear::GraphControl, public ear::GraphReader {
public:
	struct Step { std::string call; std::function<Result()> handler; };
	std::deque<Step> script;
	std::vector<std::string> calls;
	ear::GraphView view{};
	const QueueExecutor* const* running = nullptr;
	const QueueExecutor* mainLoop = nullptr;
	int offLoop = 0;

	Result addNode(const ear::NodeSpec& node) override {
		return next("addNode " + node.name + " " + node.type + " " + node.settings.dump());
	}
	Result updateNode(const ear::NodeName& node, const ear::NodeSettings& settings) override {
		return next("updateNode " + node + " " + settings.dump());
	}
	Result removeNode(const ear::NodeName& node) override { return next("removeNode " + node); }
	Result addRoute(const ear::Route& route) override { return next("addRoute " + text(route)); }
	Result removeRoute(const ear::Route& route) override { return next("removeRoute " + text(route)); }
	ear::GraphView read() const override {
		const_cast<ScriptedGraph*>(this)->note("read");
		return view;
	}

private:
	static std::string text(const ear::Route& r) {
		return r.from.node + ":" + r.from.port + "->" + r.to.node + ":" + r.to.port;
	}
	void note(std::string call) {
		if (*running != mainLoop) ++offLoop;
		calls.push_back(std::move(call));
	}
	Result next(std::string call) {
		note(call);
		REQUIRE_FALSE(script.empty());
		Step step = std::move(script.front());
		script.pop_front();
		CHECK(step.call == call);
		return step.handler ? step.handler() : Result{};
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
	ScriptedGraph graph;
	MainLoopGateway gateway{main, control, graph, graph};
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
	// The listener calls the core makes on the main loop.
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

ear::GraphView baseView(bool motu) {
	ear::GraphView view{{{48000, 256}, false, "SCHED_OTHER"}, {"card", "file", "white"}, {}, {}};
	if (motu)
		view.nodes.push_back({{"motu", "card", {{"card", "Pro24"}, {"playback", 2}}},
		                      {{"playback_1", "playback_2"}, {}}, NodeState::running, std::nullopt});
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
	w.graph.view = baseView(false);
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
	w.graph.view = baseView(false);
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
	w.graph.view = baseView(true);
	const std::vector<std::pair<std::string, ear::CommandError>> refusals{
		{"addNode a.b white {}", ear::InvalidName{"a.b"}},
		{"addNode pinky pink {}", ear::UnknownNodeType{"pink"}},
		{"addNode tape file {}", ear::InvalidSettings{"tape", "path: missing"}},
		{R"(addNode noise file {"path":"z.wav"})", ear::DuplicateNode{"noise"}},
		{"updateNode ghost {}", ear::UnknownNode{"ghost"}},
		{R"(updateNode motu {"card":"Pro24","playback":4})", ear::PortsChanged{"motu"}},
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
	w.graph.view = baseView(false);
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

TEST_CASE("Responses keep request order; notifications get none; done follows the responses", "[session]") {
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
