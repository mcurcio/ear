// The node framework driven through fake engine adapters: how a node starts, what makes it running or failed, and the
// order a run is torn down in (docs/design/m1-white-to-file.md section 3, S2 criterion 5; docs/design/batch2.md section
// 4.1: the port block is made first and the run drives it; C1 criteria 7-11: restart, sound cards, a lost block,
// removal; docs/design/rtp.md R1a criteria 3-8: RTP receivers on the same outside-node path; docs/design/node-framework.md
// slice 1: the resource registry, named inside banks, the framework's pass-through, the two-phase start, and the
// invariants I4 to I7 and I5a). Every step a fake engine would take later (ready, lost, a handoff closing, a card or
// receiver ready or lost, a close deadline passing) is an event the test sends.
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/node_host.hpp"

using namespace ear;

namespace {

using Log = std::vector<std::string>;

struct TestAudioCode final : AudioCode {
	Log& log;
	explicit TestAudioCode(Log& l) : log(l) {}
	void prepare(const AudioFormat& format) override {
		log.push_back("prepare " + std::to_string(format.rate) + "/" + std::to_string(format.maxBlock));
	}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {}
	void release() noexcept override { log.push_back("release"); }
};

struct TestConsumer final : HandoffConsumer {
	void consume(std::span<const float>, std::uint32_t) override {}
	void finish() override {}
};

/// A run object with no threads: it closes at once.
struct TestRun final : RunObject {
	Log& log;
	TestConsumer consumer;
	explicit TestRun(Log& l) : log(l) {}
	~TestRun() override { log.push_back("run destroyed"); }
};

/// A recipe handing over a run object built in start, logging when it runs.
template <typename Run> RunRecipe handOver(Log& log, std::unique_ptr<Run> run) {
	return [&log, run = std::move(run)](Ready&) mutable -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
		log.push_back("run built");
		return std::move(run);
	};
}

/// Routable inputs record.left and record.right; the inside input bank "cap" (one port) and the inside output bank
/// "pb" (two).
Ports testPorts() {
	return Ports{{{Direction::input, "record", {"left", "right"}},
	              {Direction::input, "cap", {"1"}, PortBank::Reach::inside},
	              {Direction::output, "pb", {"1", "2"}, PortBank::Reach::inside}}};
}

/// A node config whose start asks for `handoffs` handoffs, `cards` sound cards and `receivers` RTP receivers, then
/// succeeds or fails. A config with a tag logs its start and its destruction with that tag.
struct TestConfig final : NodeConfig {
	Log& log;
	std::string tag;
	Ports ports_ = testPorts();
	int handoffs = 0;
	std::vector<SoundCardRequest> cards;
	std::vector<RtpReceiverRequest> receivers;
	bool fail = false;
	mutable StartContext* context = nullptr; // the context start() was given, kept to misuse it later
	explicit TestConfig(Log& l, std::string t = {}) : log(l), tag(std::move(t)) {}
	~TestConfig() override {
		if (!tag.empty()) log.push_back("config " + tag + " destroyed");
	}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& c) const override {
		context = &c;
		const AudioFormat format = c.format();
		log.push_back("start " + std::to_string(format.rate) + (tag.empty() ? "" : " " + tag));
		auto run = std::make_unique<TestRun>(log);
		for (int i = 0; i < handoffs; ++i) c.acquire(HandoffRequest{"record", 1024, &run->consumer});
		for (const auto& card : cards) c.acquire(card);
		for (const auto& receiver : receivers) c.acquire(receiver);
		if (fail) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open /nope"});
		return Started{std::make_unique<TestAudioCode>(log), handOver(log, std::move(run))};
	}
};

const SoundCardRequest motuOut{*CardId::parse("Pro24"), CardDirection::playback, "pb"};
const SoundCardRequest motuIn{*CardId::parse("Pro24"), CardDirection::capture, "cap"};

std::shared_ptr<TestConfig> withCards(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->cards = {motuOut, motuIn};
	return config;
}

const RtpReceiverRequest fppStream{*RtpAddress::parse("239.69.0.1"), 5004, RtpEncoding::l24, 44100, 100, "cap"};

std::shared_ptr<TestConfig> withReceiver(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->receivers = {fppStream};
	return config;
}

struct TestBlock final : PortBlock {
	Log& log;
	AudioCode** driven;
	std::vector<LatencyPlan>* plans;
	TestBlock(Log& l, AudioCode** d, std::vector<LatencyPlan>* p) : log(l), driven(d), plans(p) {}
	~TestBlock() override { log.push_back("block destroyed"); }
	void drive(AudioCode* code) override {
		*driven = code;
		log.push_back(code != nullptr ? "drive code" : "drive nothing");
	}
	void carryLatency(const LatencyPlan& plan) override { plans->push_back(plan); }
};

struct TestBlocks final : PortBlockFactory {
	Log& log;
	std::vector<PortBlockEvents*> events;
	std::deque<AudioCode*> driven;              // what each block runs now, in make order
	std::deque<std::vector<LatencyPlan>> plans; // every latency plan each block was given, in make order
	explicit TestBlocks(Log& l) : log(l) {}
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports, const AudioFormat&,
	                                PortBlockEvents& e) override {
		log.push_back("make " + node + " " + std::to_string(ports.inputs().size()) + " inputs");
		events.push_back(&e);
		return std::make_unique<TestBlock>(log, &driven.emplace_back(nullptr), &plans.emplace_back());
	}
};

struct TestWriter final : RtWriter {
	std::uint32_t write(std::span<const std::span<const float>>, std::uint32_t frames) noexcept override {
		return frames;
	}
};

struct TestHandoff final : Handoff {
	Log& log;
	std::vector<std::move_only_function<void(std::uint64_t)>>& pending;
	TestWriter writer_;
	TestHandoff(Log& l, std::vector<std::move_only_function<void(std::uint64_t)>>& p) : log(l), pending(p) {}
	~TestHandoff() override { log.push_back("handoff destroyed"); }
	RtWriter& writer() noexcept override { return writer_; }
	void close(std::move_only_function<void(std::uint64_t)> closed) override {
		log.push_back("handoff close");
		pending.push_back(std::move(closed));
	}
};

/// Handoffs that close only when the test says so, as a real handoff thread finishes later.
struct TestHandoffs final : HandoffAdapter {
	Log& log;
	std::vector<std::move_only_function<void(std::uint64_t)>> pendingCloses;
	explicit TestHandoffs(Log& l) : log(l) {}
	std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels, std::uint32_t,
	                              HandoffConsumer&) override {
		log.push_back("handoff " + node + " " + std::to_string(channels) + " channels");
		return std::make_unique<TestHandoff>(log, pendingCloses);
	}
	/// The handoff thread finished: runs the oldest pending `closed`.
	void finishOne(std::uint64_t dropped) {
		REQUIRE_FALSE(pendingCloses.empty());
		auto closed = std::move(pendingCloses.front());
		pendingCloses.erase(pendingCloses.begin());
		closed(dropped);
	}
};

struct TestOutside final : OutsideNode {
	Log& log;
	std::string what;
	TestOutside(Log& l, std::string w) : log(l), what(std::move(w)) {}
	~TestOutside() override { log.push_back(what + " destroyed"); }
};

std::string joined(const std::vector<PortName>& ports) {
	std::string all;
	for (const auto& port : ports) all += (all.empty() ? "" : ",") + port;
	return all;
}

/// Cards whose readiness and loss the test tells, as a real card's PCM and links report later.
struct TestCards final : SoundCardAdapter {
	Log& log;
	std::vector<OutsideNodeEvents*> events;
	bool readyInMake = false; // tells ready() inside make(), as a card already running might
	explicit TestCards(Log& l) : log(l) {}
	std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const SoundCardRequest& request,
	                                  OutsideNodeEvents& e) override {
		log.push_back("card " + target.engineNode + " of " + target.blockNode + " " + request.card.text() + " " +
		              joined(target.blockPorts) + " at " + std::to_string(target.format.rate));
		events.push_back(&e);
		if (readyInMake) e.ready();
		return std::make_unique<TestOutside>(log, "card " + target.engineNode);
	}
};

/// RTP receivers whose readiness and loss the test tells; each request and target is kept as it was given.
struct TestReceivers final : RtpReceiverAdapter {
	Log& log;
	std::vector<OutsideNodeEvents*> events;
	std::vector<RtpReceiverRequest> requests;
	std::vector<OutsideTarget> targets;
	explicit TestReceivers(Log& l) : log(l) {}
	std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const RtpReceiverRequest& request,
	                                  OutsideNodeEvents& e) override {
		log.push_back("receiver " + target.engineNode + " of " + target.blockNode);
		events.push_back(&e);
		requests.push_back(request);
		targets.push_back(target);
		return std::make_unique<TestOutside>(log, "receiver " + target.engineNode);
	}
};

struct TestListener final : NodeHealthListener {
	Log& log;
	explicit TestListener(Log& l) : log(l) {}
	void nodeChanged(const NodeHealth& health) override {
		std::string line = "node " + health.name + " " + std::string(stateName(health.state));
		if (health.reason) line += " " + std::string(failureCode(health.reason->code)) + ": " + health.reason->detail;
		log.push_back(line);
	}
	void nodeGone(const NodeName& node, std::uint64_t dropped) override {
		log.push_back("node " + node + " gone dropped=" + std::to_string(dropped));
	}
};

/// The main loop as a test turns it: work waits here, from any thread, until the test runs it.
struct QueueExecutor final : Executor {
	void post(std::move_only_function<void()> work) override {
		const std::scoped_lock lock(mutex);
		queue.push_back(std::move(work));
		changed.notify_all();
	}
	/// Runs everything posted so far, in order, on this thread.
	void runPosted() {
		std::deque<std::move_only_function<void()>> batch;
		{
			const std::scoped_lock lock(mutex);
			batch.swap(queue);
		}
		for (auto& work : batch) work();
	}
	/// Waits for `count` items; the deadline only fails a test.
	bool waitFor(std::size_t count) {
		std::unique_lock lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5), [&] { return queue.size() >= count; });
	}
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<std::move_only_function<void()>> queue;
};

/// Main-loop timers on a clock the test moves.
struct FakeTimers final : Timers {
	struct Armed {
		std::chrono::milliseconds at;
		std::move_only_function<void()> due;
		bool cancelled = false;
	};
	struct Handle final : Timer {
		std::shared_ptr<Armed> armed;
		explicit Handle(std::shared_ptr<Armed> a) : armed(std::move(a)) {}
		~Handle() override { armed->cancelled = true; }
	};
	std::chrono::milliseconds now{0};
	std::vector<std::shared_ptr<Armed>> armed;
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		auto timer = std::make_shared<Armed>(Armed{now + delay, std::move(due)});
		armed.push_back(timer);
		return std::make_unique<Handle>(timer);
	}
	/// Timers still armed (not cancelled, not yet due).
	std::size_t live() const {
		return static_cast<std::size_t>(
			std::ranges::count_if(armed, [](const auto& timer) { return !timer->cancelled && timer->due; }));
	}
	/// Moves the clock and runs what came due, in arming order.
	void advance(std::chrono::milliseconds by) {
		now += by;
		auto due = armed; // a due timer may arm or cancel others
		for (const auto& timer : due) {
			if (timer->cancelled || !timer->due || timer->at > now) continue;
			auto call = std::move(timer->due);
			timer->due = nullptr;
			call();
		}
	}
};

struct TestNotes final : NodeNoteListener {
	Log& log;
	explicit TestNotes(Log& l) : log(l) {}
	void nodeSays(const NodeName& node, const std::string& text) override {
		log.push_back("note " + node + " " + text);
	}
};

struct TestDiagnostics final : NodeDiagnosticListener {
	Log& log;
	std::vector<std::string> details;
	explicit TestDiagnostics(Log& l) : log(l) {}
	void nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail) override {
		log.push_back("diagnostic " + node + " " + std::string(code));
		details.push_back(detail);
	}
};

constexpr std::chrono::milliseconds closeDeadline{10'000};

struct Fixture {
	Log log;
	TestBlocks blocks{log};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	ResourceRegistry resources{cards, receivers, handoffs};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue}; // only for close(), the proof stopAll takes
	QueueExecutor mainLoop;
	FakeTimers timers;
	TestNotes notes{log};
	TestDiagnostics diagnostics{log};
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	int allGone = 0; // how often stopAll's callback has run
	Fixture() {
		host.subscribe(listener);
		host.subscribeNotes(notes);
		host.subscribeDiagnostics(diagnostics);
	}
	void add(const NodeName& name, std::shared_ptr<const NodeConfig> config) {
		host.nodeAdded(NodeSpec{name, "test", {}}, std::move(config));
	}
	void update(const NodeName& name, std::shared_ptr<const NodeConfig> config) {
		host.nodeUpdated(NodeSpec{name, "test", {{"new", true}}}, std::move(config));
	}
	void stopAll() {
		host.stopAll(graph.close(), [this] {
			++allGone;
			log.push_back("all gone");
		});
	}
};

std::ptrdiff_t indexOf(const Log& log, const std::string& line) {
	return std::find(log.begin(), log.end(), line) - log.begin();
}

} // namespace

TEST_CASE("A node gets its port block, then starts, prepares its code, drives the block and builds its run",
          "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;

	f.add("rec", rec);

	CHECK(f.log == Log{"node rec starting", "make rec 2 inputs", "start 44100", "handoff rec 2 channels",
	                   "prepare 44100/256", "drive code", "run built"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::starting);

	f.stopAll();
	f.handoffs.finishOne(0);
}

TEST_CASE("A node becomes running when its port block is ready", "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.log.clear();

	f.blocks.events.at(0)->ready();
	f.blocks.events.at(0)->ready(); // at most once; a repeat changes nothing

	CHECK(f.log == Log{"node rec running"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::running);
	f.stopAll();
}

TEST_CASE("A node whose start fails is failed with start-failed and keeps its port block", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;
	rec->fail = true;

	f.add("rec", rec);

	// The handoff it asked for goes at once (nothing ever wrote to it); no prepare, no drive.
	CHECK(f.log == Log{"node rec starting", "make rec 2 inputs", "start 44100", "handoff rec 2 channels",
	                   "run destroyed", "handoff destroyed", "node rec failed start-failed: cannot open /nope"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	REQUIRE(f.host.health()[0].reason.has_value());
	CHECK(f.host.health()[0].reason->code == FailureReason::Code::startFailed);

	// The block becoming ready leaves the node failed.
	f.log.clear();
	f.blocks.events.at(0)->ready();
	CHECK(f.log.empty());

	f.stopAll();
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "node rec gone dropped=0", "all gone"});
}

TEST_CASE("A node whose port block is lost is failed with engine-refused", "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.blocks.events.at(0)->lost("driver gone");

	CHECK(f.log == Log{"node rec failed engine-refused: driver gone"});
	f.log.clear();
	f.stopAll();
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "release", "run destroyed", "node rec gone dropped=0",
	                   "all gone"});
}

TEST_CASE("stopAll takes a node down in order: block, release, handoffs, run, gone", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 2;
	f.add("rec", rec);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.stopAll();

	// Nothing the consumers live in goes until every handoff has closed.
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "release", "handoff close", "handoff close"});
	f.log.clear();

	f.handoffs.finishOne(3);
	CHECK(f.log.empty());
	CHECK(f.allGone == 0);
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::stopping);

	f.handoffs.finishOne(4);
	CHECK(f.log ==
	      Log{"handoff destroyed", "handoff destroyed", "run destroyed", "node rec gone dropped=7", "all gone"});
	CHECK(f.allGone == 1);
	CHECK(f.host.health().empty());
	CHECK(f.timers.live() == 0); // the close deadline went with the run
}

TEST_CASE("allGone runs once, after the last node is gone", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;
	f.add("rec", rec);
	f.add("noise", std::make_shared<TestConfig>(f.log));
	f.log.clear();

	f.stopAll();

	// noise has no handoff, so it is gone at once; rec waits for its handoff.
	CHECK(f.allGone == 0);
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].name == "rec");

	f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
	CHECK(f.log.back() == "all gone");
}

TEST_CASE("stopAll with no node runs allGone at once", "[node_host]") {
	Fixture f;
	f.stopAll();
	CHECK(f.allGone == 1);
	CHECK(f.log == Log{"all gone"});
}

TEST_CASE("An update restarts the run behind the same port block, letting the old config go last", "[node_host]") {
	Fixture f;
	{
		auto a = std::make_shared<TestConfig>(f.log, "A");
		a->handoffs = 1;
		f.add("rec", std::move(a)); // the host holds the only reference
	}
	f.blocks.events.at(0)->ready();
	f.log.clear();

	auto b = std::make_shared<TestConfig>(f.log, "B");
	b->handoffs = 1;
	f.update("rec", std::move(b));

	// The block stops running the old code before release(); the handoff is closing.
	CHECK(f.log == Log{"node rec starting", "drive nothing", "release", "handoff close"});
	f.log.clear();

	SECTION("the old run goes, then its config, then the new config starts") {
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 B",
		                   "handoff rec 2 channels", "prepare 44100/256", "drive code", "run built",
		                   "node rec running"});
	}
	SECTION("two updates during one restart start only the latest config") {
		auto c = std::make_shared<TestConfig>(f.log, "C");
		f.update("rec", std::move(c));
		CHECK(f.log == Log{"config B destroyed"}); // B never started; nothing else happened
		f.log.clear();
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 C",
		                   "prepare 44100/256", "drive code", "run built", "node rec running"});
	}
	SECTION("a removal during a restart stops the node and starts nothing") {
		f.host.nodeRemoved("rec");
		CHECK(f.log == Log{"config B destroyed", "node rec stopping", "block destroyed"});
		f.log.clear();
		f.handoffs.finishOne(2);
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "node rec gone dropped=2"});
		CHECK(f.host.health().empty());
		f.log.clear();
		f.stopAll();
		CHECK(f.log == Log{"all gone"});
		return;
	}
	// The port block was never destroyed or made again.
	CHECK(f.blocks.events.size() == 1);
	f.log.clear();
	f.stopAll();
	CHECK(f.log.at(1) == "block destroyed");
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

TEST_CASE("An update of a node whose start failed starts the new config", "[node_host]") {
	Fixture f;
	auto a = std::make_shared<TestConfig>(f.log, "A");
	a->fail = true;
	f.add("rec", std::move(a));
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.update("rec", std::make_shared<TestConfig>(f.log, "B"));

	CHECK(f.log == Log{"node rec starting", "drive nothing", "config A destroyed", "start 44100 B", "prepare 44100/256",
	                   "drive code", "run built", "node rec running"});
	f.stopAll();
}

TEST_CASE("A node's sound cards are made once its port block is ready, and its run is built and running when "
          "every card is ready",
          "[node_host]") {
	Fixture f;
	f.add("motu", withCards(f.log));

	// No card before the block is ready, whatever the run asked for; the code already drives the block.
	CHECK(f.log == Log{"node motu starting", "make motu 2 inputs", "start 44100", "prepare 44100/256", "drive code"});
	CHECK(f.cards.events.empty());
	f.log.clear();

	f.blocks.events.at(0)->ready();
	// Each card joins the ports of the inside bank it named, under its one name.
	CHECK(f.log ==
	      Log{"card motu.playback of motu Pro24 pb.1,pb.2 at 44100", "card motu.capture of motu Pro24 cap.1 at 44100"});
	REQUIRE(f.cards.events.size() == 2);
	CHECK(f.host.health().at(0).state == NodeState::starting);
	f.log.clear();

	f.cards.events[1]->ready();
	CHECK(f.log.empty()); // the run object waits for every resource
	f.cards.events[0]->ready();
	CHECK(f.log == Log{"run built", "node motu running"});
	f.log.clear();

	SECTION("a lost card fails the node with the card's detail") {
		f.cards.events[0]->lost("unplugged");
		CHECK(f.log == Log{"node motu failed engine-refused: unplugged"});
		f.log.clear();
	}
	SECTION("a restart remakes the cards at once, the block being ready") {
		f.update("motu", withCards(f.log, "B"));
		CHECK(f.log == Log{"node motu starting", "drive nothing", "release", "card motu.playback destroyed",
		                   "card motu.capture destroyed", "run destroyed", "start 44100 B", "prepare 44100/256",
		                   "drive code", "card motu.playback of motu Pro24 pb.1,pb.2 at 44100",
		                   "card motu.capture of motu Pro24 cap.1 at 44100"});
		REQUIRE(f.cards.events.size() == 4);
		f.cards.events[2]->ready();
		f.cards.events[3]->ready();
		CHECK(f.log.back() == "node motu running");
		f.log.clear();
	}

	// Teardown takes the cards before the run.
	f.stopAll();
	CHECK(f.log.front() == "node motu stopping");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "card motu.playback destroyed"));
	CHECK(indexOf(f.log, "card motu.capture destroyed") < indexOf(f.log, "run destroyed"));
	CHECK(f.log.back() == "all gone");
}

TEST_CASE("Cards that say ready inside make() build the run only once every card is made (7.2)", "[node_host]") {
	Fixture f;
	f.cards.readyInMake = true;
	f.add("motu", withCards(f.log));
	f.log.clear();
	f.blocks.events.at(0)->ready();
	CHECK(f.log == Log{"card motu.playback of motu Pro24 pb.1,pb.2 at 44100",
	                   "card motu.capture of motu Pro24 cap.1 at 44100", "run built", "node motu running"});
	f.stopAll();
	CHECK(f.log.back() == "all gone");
}

TEST_CASE("A node acquires resources and its events only inside start", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	f.add("motu", config);
	REQUIRE(config->context != nullptr);
	CHECK_THROWS_AS(config->context->acquire(motuOut), std::logic_error);
	CHECK_THROWS_AS(config->context->acquire(fppStream), std::logic_error);
	TestConsumer consumer;
	CHECK_THROWS_AS(config->context->acquire(HandoffRequest{"record", 64, &consumer}), std::logic_error);
	CHECK_THROWS_AS(config->context->events(), std::logic_error);
	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty());
	CHECK(f.receivers.events.empty());
	f.stopAll();
}

TEST_CASE("A node whose start fails makes none of the cards it asked for", "[node_host]") {
	Fixture f;
	auto config = withCards(f.log);
	config->fail = true;
	f.add("motu", config);
	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty());
	CHECK(f.host.health().at(0).state == NodeState::failed);
	f.stopAll();
}

TEST_CASE("A lost port block is final: the node stays failed through updates and later failures", "[node_host]") {
	Fixture f;
	{
		auto a = withCards(f.log, "A");
		a->handoffs = 1;
		f.add("motu", std::move(a));
	}
	f.blocks.events.at(0)->ready();
	f.cards.events.at(0)->ready();
	f.cards.events.at(1)->ready();
	REQUIRE(f.host.health().at(0).state == NodeState::running);
	f.log.clear();

	f.blocks.events.at(0)->lost("driver gone");
	CHECK(f.log == Log{"node motu failed engine-refused: driver gone"});
	f.log.clear();

	// A card failing afterwards changes nothing: the block's reason stands.
	f.cards.events.at(0)->lost("unplugged");
	CHECK(f.log.empty());

	// An update tears the old run down and starts nothing; no starting health.
	f.update("motu", withCards(f.log, "B"));
	f.handoffs.finishOne(0);
	CHECK(f.log == Log{"drive nothing", "release", "card motu.playback destroyed", "card motu.capture destroyed",
	                   "handoff close", "config B destroyed", "handoff destroyed", "run destroyed",
	                   "config A destroyed"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	REQUIRE(f.host.health()[0].reason.has_value());
	CHECK(f.host.health()[0].reason->detail == "driver gone");
	CHECK(f.cards.events.size() == 2);
	f.log.clear();

	// Removal still ends with nodeGone.
	f.host.nodeRemoved("motu");
	CHECK(f.log == Log{"node motu stopping", "block destroyed", "node motu gone dropped=0"});
	CHECK(f.host.health().empty());
	f.stopAll();
}

TEST_CASE("nodeRemoved stops only that node and ends with nodeGone", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;
	f.add("rec", rec);
	f.add("noise", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.blocks.events.at(1)->ready();
	f.log.clear();

	f.host.nodeRemoved("noise");
	CHECK(f.log ==
	      Log{"node noise stopping", "block destroyed", "release", "run destroyed", "node noise gone dropped=0"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].name == "rec");
	CHECK(f.host.health()[0].state == NodeState::running);
	f.log.clear();

	SECTION("a removed node still stopping comes before a newer node of the same name") {
		f.host.nodeRemoved("rec");
		f.add("rec", std::make_shared<TestConfig>(f.log));
		const auto health = f.host.health();
		REQUIRE(health.size() == 2);
		CHECK(health[0].state == NodeState::stopping);
		CHECK(health[1].state == NodeState::starting);
		// An update names the newer one.
		f.update("rec", std::make_shared<TestConfig>(f.log));
		f.handoffs.finishOne(5);
		CHECK(std::ranges::find(f.log, "node rec gone dropped=5") != f.log.end());
		CHECK(f.log.back() == "run built"); // the newer one waited for it
		REQUIRE(f.host.health().size() == 1);
		CHECK(f.host.health()[0].state == NodeState::starting);
	}
	SECTION("stopAll after a removal still waits for it") {
		f.host.nodeRemoved("rec");
		f.stopAll();
		CHECK(f.allGone == 0);
		f.handoffs.finishOne(0);
		CHECK(f.allGone == 1);
		return;
	}
	f.stopAll();
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

TEST_CASE("After stopAll no node event can reach the node framework", "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.stopAll();
	CHECK_THROWS_AS(f.add("noise", std::make_shared<TestConfig>(f.log)), std::logic_error);
	CHECK_THROWS_AS(f.update("rec", std::make_shared<TestConfig>(f.log)), std::logic_error);
	CHECK_THROWS_AS(f.host.nodeRemoved("rec"), std::logic_error);
}

TEST_CASE("A node's RTP receiver is made once its port block is ready, named <node>.rtp, with the "
          "request unchanged and the ports of the bank it named",
          "[node_host][rtp]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	CHECK(f.receivers.events.empty()); // nothing before the block is ready
	f.log.clear();

	f.blocks.events.at(0)->ready();
	CHECK(f.log == Log{"receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.requests.size() == 1);
	const RtpReceiverRequest& made = f.receivers.requests[0];
	CHECK(made.address == fppStream.address);
	CHECK(made.port == fppStream.port);
	CHECK(made.encoding == fppStream.encoding);
	CHECK(made.rate == fppStream.rate);
	CHECK(made.latencyMs == fppStream.latencyMs);
	CHECK(made.bank == "cap");
	CHECK(f.receivers.targets[0].blockPorts == std::vector<PortName>{"cap.1"});
	CHECK(f.receivers.targets[0].format.rate == 44100);
	CHECK(f.host.health().at(0).state == NodeState::starting);
	f.log.clear();

	f.receivers.events[0]->ready();
	CHECK(f.log == Log{"run built", "node fpp running"});
	f.stopAll();
}

TEST_CASE("A node with a card and a receiver runs only once both are ready, in either order", "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {fppStream};
	f.add("mix", config);
	f.log.clear();
	f.blocks.events.at(0)->ready();
	// Request order: the card first, then the receiver.
	CHECK(f.log == Log{"card mix.playback of mix Pro24 pb.1,pb.2 at 44100", "receiver mix.rtp of mix"});
	REQUIRE(f.cards.events.size() == 1);
	REQUIRE(f.receivers.events.size() == 1);
	f.log.clear();

	SECTION("the receiver first") {
		f.receivers.events[0]->ready();
		CHECK(f.log.empty());
		f.cards.events[0]->ready();
	}
	SECTION("the card first") {
		f.cards.events[0]->ready();
		CHECK(f.log.empty());
		f.receivers.events[0]->ready();
	}
	CHECK(f.log == Log{"run built", "node mix running"});
	f.stopAll();
}

TEST_CASE("A receiver lost after the node ran fails it, and an update makes a new receiver behind the "
          "same block",
          "[node_host][rtp]") {
	Fixture f;
	{
		auto a = withReceiver(f.log, "A");
		a->handoffs = 1;
		f.add("fpp", std::move(a));
	}
	f.blocks.events.at(0)->ready();
	f.receivers.events.at(0)->ready();
	REQUIRE(f.host.health().at(0).state == NodeState::running);
	f.log.clear();

	f.receivers.events[0]->lost("the engine removed the RTP receiver");
	CHECK(f.log == Log{"node fpp failed engine-refused: the engine removed the RTP receiver"});
	f.log.clear();

	f.update("fpp", withReceiver(f.log, "B"));
	CHECK(f.log == Log{"node fpp starting", "drive nothing", "release", "receiver fpp.rtp destroyed", "handoff close"});
	f.log.clear();
	f.handoffs.finishOne(0);
	CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 B", "prepare 44100/256",
	                   "drive code", "receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.events.size() == 2); // made once more, not twice
	CHECK(f.blocks.events.size() == 1);      // the block stayed
	f.log.clear();
	f.receivers.events[1]->ready();
	CHECK(f.log == Log{"run built", "node fpp running"});
	f.stopAll();
}

TEST_CASE("An outside node lost before the node ran fails it with engine-refused and the outside "
          "node's detail, and its run is never built",
          "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {fppStream};
	f.add("mix", config);
	f.blocks.events.at(0)->ready();
	REQUIRE(f.cards.events.size() == 1);
	REQUIRE(f.receivers.events.size() == 1);
	f.log.clear();

	SECTION("the receiver, with the card ready") {
		f.cards.events[0]->ready();
		f.receivers.events[0]->lost("cannot load PipeWire's rtp-source module: No such file or directory");
		CHECK(f.log == Log{"node mix failed engine-refused: cannot load PipeWire's rtp-source module: "
		                   "No such file or directory"});
	}
	SECTION("the card, with the receiver ready") {
		f.receivers.events[0]->ready();
		f.cards.events[0]->lost("the card failed: busy");
		CHECK(f.log == Log{"node mix failed engine-refused: the card failed: busy"});
	}
	SECTION("the receiver, nothing ready") {
		f.receivers.events[0]->lost("the engine removed the RTP receiver's module");
		CHECK(f.log == Log{"node mix failed engine-refused: the engine removed the RTP receiver's module"});
	}
	CHECK(f.host.health().at(0).state == NodeState::failed);
	f.log.clear();
	f.stopAll();
	// The run object was never built, but it goes with the run, after the outside nodes.
	CHECK(std::ranges::count(f.log, std::string("run built")) == 0);
	CHECK(indexOf(f.log, "receiver mix.rtp destroyed") < indexOf(f.log, "run destroyed"));
}

TEST_CASE("One run names each engine object once: a second of a name, or a bank of the wrong kind, is a type bug "
          "(I9)",
          "[node_host][rtp]") {
	Fixture f;
	auto twice = std::make_shared<TestConfig>(f.log);
	twice->receivers = {fppStream, fppStream};
	CHECK_THROWS_AS(f.add("fpp", twice), std::logic_error);

	auto twoPlayback = std::make_shared<TestConfig>(f.log);
	twoPlayback->cards = {motuOut, motuOut};
	CHECK_THROWS_AS(f.add("motu", twoPlayback), std::logic_error);

	auto routable = std::make_shared<TestConfig>(f.log);
	routable->receivers = {RtpReceiverRequest{fppStream.address, 5004, RtpEncoding::l24, 44100, 100, "record"}};
	CHECK_THROWS_AS(f.add("routable", routable), std::logic_error);

	auto wrongWay = std::make_shared<TestConfig>(f.log);
	wrongWay->cards = {SoundCardRequest{*CardId::parse("Pro24"), CardDirection::playback, "cap"}};
	CHECK_THROWS_AS(f.add("wrongway", wrongWay), std::logic_error);

	auto missing = std::make_shared<TestConfig>(f.log);
	missing->cards = {SoundCardRequest{*CardId::parse("Pro24"), CardDirection::capture, "nope"}};
	CHECK_THROWS_AS(f.add("missing", missing), std::logic_error);

	for (auto* events : f.blocks.events) events->ready();
	CHECK(f.receivers.events.empty());
	CHECK(f.cards.events.empty());
}

TEST_CASE("Restart and removal destroy the receiver before the handoffs close and the run goes", "[node_host][rtp]") {
	Fixture f;
	auto a = withReceiver(f.log, "A");
	a->handoffs = 1;
	f.add("fpp", std::move(a));
	f.blocks.events.at(0)->ready();
	f.receivers.events.at(0)->ready();
	f.log.clear();

	SECTION("restart") {
		f.update("fpp", withReceiver(f.log, "B"));
		f.handoffs.finishOne(0);
	}
	SECTION("removal") {
		f.host.nodeRemoved("fpp");
		f.handoffs.finishOne(0);
		CHECK(f.log.back() == "node fpp gone dropped=0");
	}
	const auto destroyed = indexOf(f.log, "receiver fpp.rtp destroyed");
	CHECK(destroyed < static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(indexOf(f.log, "release") < destroyed);
	CHECK(destroyed < indexOf(f.log, "handoff close"));
	CHECK(destroyed < indexOf(f.log, "run destroyed"));
	f.log.clear();
	f.stopAll();
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

namespace {

/// A config whose start returns the framework's pass-through between its banks: routable "play" (2) to inside "out"
/// (2), inside "in" (1) to routable "capture" (1), and an output bank "spare" no pair names.
struct PassConfig final : NodeConfig {
	Ports ports_{{{Direction::input, "play", numbered(2)},
	              {Direction::output, "capture", numbered(1)},
	              {Direction::output, "spare", numbered(1)},
	              {Direction::input, "in", numbered(1), PortBank::Reach::inside},
	              {Direction::output, "out", numbered(2), PortBank::Reach::inside}}};
	std::vector<BankPair> pairs{{"play", "out"}, {"in", "capture"}};
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override { return Started{passThrough(pairs)}; }
};

} // namespace

TEST_CASE("The framework's pass-through copies each bank to its pair and silences every output no pair names",
          "[node_host][pass_through]") {
	Fixture f;
	f.add("card", std::make_shared<PassConfig>());
	f.blocks.events.at(0)->ready();
	CHECK(f.host.health().at(0).state == NodeState::running); // no recipe: ready once the code drives the block
	AudioCode* code = f.blocks.driven.at(0);
	REQUIRE(code != nullptr);

	// Inputs: play.1, play.2, then in.1. Outputs: capture.1, spare.1, then out.1, out.2.
	constexpr std::uint32_t frames = 4;
	std::array<std::array<float, frames>, 3> in{{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}}};
	std::array<std::array<float, frames>, 4> out{};
	for (auto& channel : out) channel.fill(-1.0F);
	const std::array<std::span<const float>, 3> inputs{in[0], in[1], in[2]};
	const std::array<std::span<float>, 4> outputs{out[0], out[1], out[2], out[3]};
	code->prepare({44100, 256});
	code->process(inputs, outputs, frames, GraphCycle{0, 0});

	CHECK(out[0] == in[2]);                                 // in.1 -> capture.1
	CHECK(out[1] == std::array<float, frames>{0, 0, 0, 0}); // spare.1: silent
	CHECK(out[2] == in[0]);                                 // play.1 -> out.1
	CHECK(out[3] == in[1]);                                 // play.2 -> out.2
	f.stopAll();
}

namespace {

/// PassConfig's banks and pairs, with latency declared on "capture" (and `extra`, each by start()).
struct LatencyConfig final : NodeConfig {
	PassConfig pass;
	std::vector<std::pair<std::string, Latency>> extra;
	mutable StartContext* context = nullptr;
	const Ports& ports() const override { return pass.ports_; }
	StartResult start(StartContext& c) const override {
		context = &c;
		c.declareLatency("capture", Latency{100, 200});
		for (const auto& [bank, latency] : extra) c.declareLatency(bank, latency);
		return Started{passThrough(pass.pairs)};
	}
};
} // namespace

TEST_CASE("A run's latency plan reaches its block before its code: the pass-through's pairs and what start declared, "
          "cleared at the run's teardown",
          "[node_host][latency]") {
	Fixture f;
	auto config = std::make_shared<LatencyConfig>();
	f.add("card", config);
	REQUIRE(f.blocks.plans.at(0).size() == 1);
	const LatencyPlan& plan = f.blocks.plans.at(0).at(0);
	CHECK(plan.pairs == config->pass.pairs);
	REQUIRE(plan.internal.size() == 1);
	CHECK(plan.internal[0].first == "capture");
	CHECK(plan.internal[0].second == Latency{100, 200});
	CHECK_THROWS_AS(config->context->declareLatency("spare", Latency{1, 1}), std::logic_error); // outside start()

	f.update("card", std::make_shared<LatencyConfig>());
	REQUIRE(f.blocks.plans.at(0).size() == 3); // cleared by the teardown, then the new run's
	CHECK(f.blocks.plans.at(0).at(1).pairs.empty());
	CHECK(f.blocks.plans.at(0).at(1).internal.empty());
	CHECK(f.blocks.plans.at(0).at(2).internal.size() == 1);
	f.stopAll();
}

TEST_CASE("Latency declared twice for a bank, or for a bank the node lacks, is a type bug", "[node_host][latency]") {
	for (const std::string bank : {"capture", "nowhere"}) {
		Fixture f;
		auto config = std::make_shared<LatencyConfig>();
		config->extra = {{bank, Latency{1, 1}}};
		CHECK_THROWS_AS(f.add("card", config), std::logic_error);
	}
}

TEST_CASE("A pass-through between banks of different sizes, the wrong way or of an unknown bank is a type bug",
          "[node_host][pass_through]") {
	const std::vector<std::vector<BankPair>> bad{
		{{"play", "capture"}},             // 2 ports to 1
		{{"capture", "play"}},             // an output to an input
		{{"play", "nope"}},                // no such bank
		{},                                // no pair at all
		{{"play", "out"}, {"play", "out"}} // one output bank written twice
	};
	const auto which = GENERATE(range(0, 5));
	Fixture f;
	auto config = std::make_shared<PassConfig>();
	config->pairs = bad.at(static_cast<std::size_t>(which));
	CHECK_THROWS_AS(f.add("card", config), std::logic_error);
}

namespace {

/// A run object with its own thread, as a network node's: close() only records `closed`, which the test calls later,
/// as that thread would; events are kept from its recipe (Ready) on.
struct ThreadedRun final : RunObject {
	Log& log;
	TestConsumer consumer;
	InsidesEvents* events = nullptr;
	std::move_only_function<void()> closed;
	bool closeAtOnce = false; // closes inside close(), as a run whose threads already ended
	explicit ThreadedRun(Log& l) : log(l) {}
	~ThreadedRun() override { log.push_back("run destroyed"); }
	void close(std::move_only_function<void()> call) override {
		log.push_back("run close");
		if (closeAtOnce) call();
		else closed = std::move(call);
	}
};

struct ThreadedConfig final : NodeConfig {
	Log& log;
	Ports ports_{{{Direction::input, "in", {"a"}}}};
	int handoffs = 0;
	bool closeAtOnce = false;
	mutable ThreadedRun* run = nullptr; // the latest run object built
	mutable InsidesEvents* events = nullptr;
	explicit ThreadedConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& context) const override {
		auto made = std::make_unique<ThreadedRun>(log);
		made->closeAtOnce = closeAtOnce;
		for (int i = 0; i < handoffs; ++i) context.acquire(HandoffRequest{"in", 1024, &made->consumer});
		return Started{std::make_unique<TestAudioCode>(log),
		               [this, made = std::move(made)](
						   Ready& ready) mutable -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
						   made->events = &ready.events();
						   events = made->events;
						   run = made.get();
						   return std::move(made);
					   }};
	}
};

} // namespace

TEST_CASE("A run object closes after every handoff has closed, and goes only after it says closed", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	config->handoffs = 1;
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.stopAll();
	CHECK(f.log == Log{"node net stopping", "block destroyed", "release", "handoff close"});
	f.log.clear();

	f.handoffs.finishOne(7);
	CHECK(f.log == Log{"handoff destroyed", "run close"});
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty()); // nothing goes until it says so

	// From the run's own thread: it crosses to the main loop, so nothing happens there yet.
	std::thread([&] { config->run->closed(); }).join();
	CHECK(f.log.empty());
	CHECK(f.allGone == 0);
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"run destroyed", "node net gone dropped=7", "all gone"});
}

TEST_CASE("A restart waits for the old run to close before it starts the new one (I7)", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.update("net", next);
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close"});
	f.log.clear();

	std::thread([&] { old->run->closed(); }).join();
	CHECK(f.log.empty());
	f.mainLoop.runPosted();
	CHECK(f.log ==
	      Log{"run destroyed", "start 44100 next", "prepare 44100/256", "drive code", "run built", "node net running"});
	f.stopAll();
}

TEST_CASE("A name added back while its old run is closing starts only once it is gone", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.host.nodeRemoved("net");
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.add("net", next);
	// Its block comes at once; its run waits.
	CHECK(f.log == Log{"node net starting", "make net 2 inputs"});
	f.log.clear();

	SECTION("then it starts, after the old node is gone") {
		std::thread([&] { old->run->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"run destroyed", "node net gone dropped=0", "start 44100 next", "prepare 44100/256",
		                   "drive code", "run built"});
	}
	SECTION("an update while it waits replaces what will start, and nothing starts yet") {
		auto newer = std::make_shared<TestConfig>(f.log, "newer");
		f.update("net", newer);
		CHECK(f.log.empty());
		std::thread([&] { old->run->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"run destroyed", "node net gone dropped=0", "start 44100 newer", "prepare 44100/256",
		                   "drive code", "run built"});
	}
	SECTION("removed again while it waits, it never starts") {
		f.host.nodeRemoved("net");
		CHECK(f.log == Log{"node net stopping", "block destroyed", "node net gone dropped=0"});
		f.log.clear();
		std::thread([&] { old->run->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"run destroyed", "node net gone dropped=0"});
	}
	SECTION("removed and added back while it waits, only the newest starts") {
		f.host.nodeRemoved("net");
		f.log.clear();
		auto newest = std::make_shared<TestConfig>(f.log, "newest");
		f.add("net", newest);
		f.log.clear();
		std::thread([&] { old->run->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"run destroyed", "node net gone dropped=0", "start 44100 newest", "prepare 44100/256",
		                   "drive code", "run built"});
	}
	f.stopAll();
}

TEST_CASE("A name added back whose block is lost while it waits never starts a run (I1)", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.host.nodeRemoved("net");
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	f.blocks.events.at(1)->lost("the engine refused the block");
	f.log.clear();

	std::thread([&] { old->run->closed(); }).join();
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"run destroyed", "node net gone dropped=0", "config next destroyed"});
	CHECK(f.host.health().at(0).state == NodeState::failed);
	f.log.clear();
	f.stopAll();
	CHECK(f.log == Log{"node net stopping", "block destroyed", "node net gone dropped=0", "all gone"});
}

TEST_CASE("A name added back after its old node has gone starts at once", "[node_host]") {
	Fixture f;
	f.add("net", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("net");
	f.log.clear();
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	CHECK(f.log == Log{"node net starting", "make net 2 inputs", "start 44100 next", "prepare 44100/256", "drive code",
	                   "run built"});
	f.stopAll();
}

TEST_CASE("Stopping while a name waits for its old run stops both, in order", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.host.nodeRemoved("net");
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	f.log.clear();
	f.stopAll();
	CHECK(f.allGone == 0);
	std::thread([&] { old->run->closed(); }).join();
	f.mainLoop.runPosted();
	CHECK(f.allGone == 1);
	CHECK(std::ranges::count(f.log, std::string("start 44100 next")) == 0);
}

TEST_CASE("What a run says reaches the note listeners on the main loop, in order, and nothing after closed",
          "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	constexpr int notes = 200;
	std::thread talker([&] {
		for (int i = 0; i < notes; ++i) config->events->say(std::to_string(i));
	});
	talker.join();
	CHECK(f.log.empty()); // never inline
	f.mainLoop.runPosted();
	REQUIRE(f.log.size() == notes);
	for (int i = 0; i < notes; ++i) CHECK(f.log[static_cast<std::size_t>(i)] == "note net " + std::to_string(i));
	f.log.clear();

	f.stopAll();
	f.log.clear();
	// Still heard while closing; a note posted after `closed` was never told.
	InsidesEvents* events = config->events;
	events->say("closing");
	config->run->closed();
	events->say("too late");
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"note net closing", "run destroyed", "node net gone dropped=0", "all gone"});
}

TEST_CASE("A note still queued when its node has gone reaches nothing", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.host.nodeRemoved("net");
	config->run->closed();
	f.mainLoop.runPosted();
	// The node is pruned at the next add; what its run left behind holds no pointer to it.
	f.add("other", std::make_shared<TestConfig>(f.log));
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	f.stopAll();
}

TEST_CASE("A run lost from its own thread fails a running node with start-failed, and its block "
          "and routes stay",
          "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	std::thread([&] { config->events->lost("the client died"); }).join();
	CHECK(f.log.empty());
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"node net failed start-failed: the client died"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);

	f.log.clear();
	f.stopAll();
	CHECK(f.log == Log{"node net stopping", "block destroyed", "release", "run close"});
	config->run->closed();
	f.mainLoop.runPosted();
}

TEST_CASE("A loss told once close began changes nothing", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	// Told before the restart, heard after it began: the node must not fail behind the restart.
	config->events->lost("late");
	f.update("net", std::make_shared<TestConfig>(f.log, "next"));
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	config->run->closed();
	f.mainLoop.runPosted();
	CHECK(std::ranges::find(f.log, std::string("node net running")) != f.log.end());
	f.stopAll();
}

// The two-phase start (docs/design/node-framework.md section 7.2).

namespace {

/// A config whose recipe fails, or logs what Ready hands it.
struct RecipeConfig final : NodeConfig {
	Log& log;
	bool fail = false;
	Ports ports_ = testPorts();
	explicit RecipeConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& context) const override {
		log.push_back("start acquires");
		context.acquire(motuOut);
		return Started{std::make_unique<TestAudioCode>(log),
		               [this](Ready& ready) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
						   log.push_back("recipe at " + std::to_string(ready.format().rate));
						   ready.events().say("built");
						   if (fail)
							   return std::unexpected(
								   FailureReason{FailureReason::Code::startFailed, "cannot read the key file"});
						   return std::make_unique<TestRun>(log);
					   }};
	}
};

} // namespace

TEST_CASE("Start only acquires: the code drives the block at once, and the recipe runs once every resource is ready",
          "[node_host][two_phase]") {
	Fixture f;
	auto config = std::make_shared<RecipeConfig>(f.log);
	f.add("motu", config);
	CHECK(f.log ==
	      Log{"node motu starting", "make motu 2 inputs", "start acquires", "prepare 44100/256", "drive code"});
	f.log.clear();
	f.blocks.events.at(0)->ready();
	CHECK(f.log == Log{"card motu.playback of motu Pro24 pb.1,pb.2 at 44100"});
	f.log.clear();

	SECTION("a recipe that builds its run object makes the node running; what it says is heard") {
		f.cards.events.at(0)->ready();
		CHECK(f.log == Log{"recipe at 44100", "node motu running"});
		f.mainLoop.runPosted();
		CHECK(f.log.back() == "note motu built");
	}
	SECTION("a recipe that fails fails the node with start-failed; the block and the card stay until a restart") {
		config->fail = true;
		f.cards.events.at(0)->ready();
		CHECK(f.log == Log{"recipe at 44100", "node motu failed start-failed: cannot read the key file"});
		f.log.clear();
		f.update("motu", std::make_shared<TestConfig>(f.log, "B"));
		CHECK(f.log.at(0) == "node motu starting");
		CHECK(std::ranges::find(f.log, "card motu.playback destroyed") != f.log.end());
	}
	f.log.clear();
	f.stopAll();
	CHECK(f.log.back() == "all gone");
}

// Teardown by dependency, with asynchronous close (docs/design/node-framework.md section 6, I4 to I7 and I5a).

namespace {

/// A handoff whose own thread drains into a consumer that blocks, as libsndfile on a hung disk would: close() starts
/// the thread, which calls finish() and then posts `closed` to the main loop, as ThreadHandoffs does.
struct BlockingHandoff final : Handoff {
	Executor& mainLoop;
	HandoffConsumer& consumer;
	TestWriter writer_;
	std::thread thread;
	BlockingHandoff(Executor& m, HandoffConsumer& c) : mainLoop(m), consumer(c) {}
	~BlockingHandoff() override {
		if (thread.joinable()) thread.join();
	}
	RtWriter& writer() noexcept override { return writer_; }
	void close(std::move_only_function<void(std::uint64_t)> closed) override {
		thread = std::thread([this, closed = std::move(closed)]() mutable {
			consumer.finish();
			mainLoop.post([closed = std::move(closed)]() mutable { closed(0); });
		});
	}
};

struct BlockingHandoffs final : HandoffAdapter {
	Executor& mainLoop;
	explicit BlockingHandoffs(Executor& m) : mainLoop(m) {}
	std::unique_ptr<Handoff> make(const NodeName&, std::uint32_t, std::uint32_t, HandoffConsumer& consumer) override {
		return std::make_unique<BlockingHandoff>(mainLoop, consumer);
	}
};

/// A consumer whose finish() blocks until the test lets it go.
struct BlockedConsumer final : HandoffConsumer {
	std::promise<void> entered;
	std::shared_future<void> release;
	explicit BlockedConsumer(std::shared_future<void> r) : release(std::move(r)) {}
	void consume(std::span<const float>, std::uint32_t) override {}
	void finish() override {
		entered.set_value();
		release.wait();
	}
};

/// A run object owning a blocked consumer; never destroyed while the handoff thread may use it.
struct BlockedRun final : RunObject {
	Log& log;
	BlockedConsumer consumer;
	BlockedRun(Log& l, std::shared_future<void> release) : log(l), consumer(std::move(release)) {}
	~BlockedRun() override { log.push_back("run destroyed"); }
	void close(std::move_only_function<void()> closed) override {
		log.push_back("run close");
		closed();
	}
};

struct BlockedConfig final : NodeConfig {
	Log& log;
	std::shared_future<void> release;
	mutable BlockedRun* run = nullptr;
	Ports ports_{{{Direction::input, "record", {"left", "right"}}}};
	BlockedConfig(Log& l, std::shared_future<void> r) : log(l), release(std::move(r)) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& context) const override {
		auto made = std::make_unique<BlockedRun>(log, release);
		run = made.get();
		context.acquire(HandoffRequest{"record", 1024, &made->consumer});
		return Started{std::make_unique<TestAudioCode>(log), handOver(log, std::move(made))};
	}
};

/// The fixture with handoffs that run real threads.
struct ThreadedFixture {
	Log log;
	TestBlocks blocks{log};
	QueueExecutor mainLoop;
	BlockingHandoffs handoffs{mainLoop};
	TestCards cards{log};
	TestReceivers receivers{log};
	ResourceRegistry resources{cards, receivers, handoffs};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};
	FakeTimers timers;
	TestDiagnostics diagnostics{log};
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	ThreadedFixture() {
		host.subscribe(listener);
		host.subscribeDiagnostics(diagnostics);
	}
};

} // namespace

TEST_CASE("A handoff consumer blocked at teardown blocks no main-loop turn, and the run waits for it, then closes (I5)",
          "[node_host][teardown]") {
	ThreadedFixture f;
	std::promise<void> letGo;
	auto config = std::make_shared<BlockedConfig>(f.log, letGo.get_future().share());
	f.host.nodeAdded(NodeSpec{"rec", "test", {}}, config);
	f.host.nodeAdded(NodeSpec{"noise", "test", {}}, std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.blocks.events.at(1)->ready();
	f.log.clear();

	f.host.nodeRemoved("rec"); // returns although the consumer is stuck
	config->run->consumer.entered.get_future().wait();
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "release"});
	f.log.clear();

	// The main loop keeps serving other nodes meanwhile.
	f.host.nodeRemoved("noise");
	CHECK(f.log.back() == "node noise gone dropped=0");
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty()); // the run is not told to close while its handoff drains

	letGo.set_value();
	REQUIRE(f.mainLoop.waitFor(1));
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"run close", "run destroyed", "node rec gone dropped=0"});
	CHECK(f.diagnostics.details.empty());
	f.host.stopAll(f.graph.close(), [] {});
}

TEST_CASE("A handoff consumer blocked past the close deadline leaks the run and the node moves on (I5a)",
          "[node_host][teardown]") {
	ThreadedFixture f;
	std::promise<void> letGo;
	auto config = std::make_shared<BlockedConfig>(f.log, letGo.get_future().share());
	f.host.nodeAdded(NodeSpec{"rec", "test", {}}, config);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	int allGone = 0;
	f.host.stopAll(f.graph.close(), [&] { ++allGone; });
	config->run->consumer.entered.get_future().wait();
	f.timers.advance(closeDeadline - std::chrono::milliseconds(1));
	CHECK(allGone == 0);

	CHECK(f.host.leakedRuns() == 0);
	f.timers.advance(std::chrono::milliseconds(1));
	// Stop proceeds past the bound: the node is gone, the run object was never closed nor destroyed.
	CHECK(allGone == 1);
	CHECK(f.host.leakedRuns() == 1); // main.cpp then ends without destroying the main loop under the leaked run
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "release", "diagnostic rec ear.close-overdue",
	                   "node rec gone dropped=0"});
	REQUIRE(f.diagnostics.details.size() == 1);
	CHECK(f.diagnostics.details[0].find("1 handoffs open") != std::string::npos);
	f.log.clear();

	// The handoff thread finally finishes: its `closed` reaches nothing, and the consumer it used is still alive.
	letGo.set_value();
	REQUIRE(f.mainLoop.waitFor(1));
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
}

TEST_CASE("A run never closing past the deadline is leaked, and a restart then starts the next run (I5a, I7)",
          "[node_host][teardown]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	InsidesEvents* oldEvents = old->events;
	ThreadedRun* oldRun = old->run;
	f.log.clear();

	f.update("net", std::make_shared<TestConfig>(f.log, "next"));
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close"});
	f.log.clear();
	CHECK(f.timers.live() == 1);

	f.timers.advance(closeDeadline);
	// The old run is leaked (never destroyed: its threads may still touch it); the next run starts.
	CHECK(f.log == Log{"diagnostic net ear.close-overdue", "start 44100 next", "prepare 44100/256", "drive code",
	                   "run built", "node net running"});
	CHECK(std::ranges::count(f.log, std::string("run destroyed")) == 0);
	CHECK(f.host.leakedRuns() == 1);
	f.log.clear();

	// What the leaked run tells late lands nowhere: not on the new run (I6), and without touching freed memory.
	oldEvents->say("still here");
	oldEvents->lost("the client died");
	oldRun->closed();
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("A run mid-handler at teardown finishes off the main loop: close returns at once, a fact it tells is dropped "
          "and its frees wait for closed (I2, I5, I6)",
          "[node_host][teardown]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	ThreadedRun* run = config->run;
	f.log.clear();

	std::promise<void> inHandler;
	std::promise<void> finishHandler;
	std::thread loop([&] {
		inHandler.set_value();
		finishHandler.get_future().wait(); // the handler is still running when close() is called
		run->events->say("handler done");
		run->events->lost("the handler failed on the way out");
		run->closed();
	});
	inHandler.get_future().wait();

	f.host.nodeRemoved("net");
	CHECK(f.log == Log{"node net stopping", "block destroyed", "release", "run close"});
	CHECK(f.host.health().at(0).state == NodeState::stopping);
	f.log.clear();

	finishHandler.set_value();
	loop.join();
	CHECK(f.log.empty()); // all of it crosses to the main loop first (I2)
	f.mainLoop.runPosted();
	// The note is heard (notes last until closed); the loss is not, as the run's close had begun.
	CHECK(f.log == Log{"note net handler done", "run destroyed", "node net gone dropped=0"});
	f.stopAll();
}

TEST_CASE("Nothing from an old run lands on a new one, even when it closed at once and the next run started in the "
          "same turn (I6)",
          "[node_host][teardown]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	old->closeAtOnce = true;
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	InsidesEvents* oldEvents = old->events;
	f.log.clear();

	// Written by the old run's thread just before its close; heard on the main loop only after the next run started.
	std::thread([&] {
		oldEvents->say("from the old run");
		oldEvents->lost("old run failed");
	}).join();
	auto next = std::make_shared<ThreadedConfig>(f.log);
	f.update("net", next);
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close", "run destroyed",
	                   "prepare 44100/256", "drive code", "node net running"});
	f.log.clear();

	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	CHECK(f.host.health().at(0).state == NodeState::running);

	// The new run's own writes land.
	next->events->say("from the new run");
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"note net from the new run"});
	f.stopAll();
	next->run->closed();
	f.mainLoop.runPosted();
	CHECK(f.allGone == 1);
}

TEST_CASE("Every teardown starts with the barrier: drive(nullptr) for a restart, the block's destruction for a "
          "removal, before release and before anything is freed (I4)",
          "[node_host][teardown]") {
	Fixture f;
	auto rec = withCards(f.log);
	rec->handoffs = 1;
	f.add("rec", rec);
	f.blocks.events.at(0)->ready();
	f.cards.events.at(0)->ready();
	f.cards.events.at(1)->ready();
	f.log.clear();

	f.update("rec", withCards(f.log, "B"));
	f.handoffs.finishOne(0);
	CHECK(f.log.at(1) == "drive nothing");
	CHECK(f.log.at(2) == "release");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "card rec.playback destroyed"));
	CHECK(indexOf(f.log, "card rec.capture destroyed") < indexOf(f.log, "handoff close"));
	CHECK(indexOf(f.log, "handoff destroyed") < indexOf(f.log, "run destroyed"));
	f.log.clear();

	f.host.nodeRemoved("rec");
	CHECK(f.log.at(1) == "block destroyed");
	CHECK(f.log.at(2) == "release");
	f.stopAll();
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

namespace {

/// Code that reads from its run object (as sendspin's code reads its run's feed and mailbox), so it must go first.
struct ReadingCode final : AudioCode {
	Log& log;
	explicit ReadingCode(Log& l) : log(l) {}
	~ReadingCode() override { log.push_back("code destroyed"); }
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {}
	void release() noexcept override {}
};

struct ReadingConfig final : NodeConfig {
	Log& log;
	Ports ports_{{{Direction::input, "in", {"a"}}}};
	explicit ReadingConfig(Log& l) : log(l) {}
	~ReadingConfig() override { log.push_back("config destroyed"); }
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override {
		return Started{std::make_unique<ReadingCode>(log), handOver(log, std::make_unique<TestRun>(log))};
	}
};

} // namespace

TEST_CASE("Step 5 frees the code before the run object it reads, and the config last (I5)", "[node_host][teardown]") {
	Fixture f;
	f.add("net", std::make_shared<ReadingConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.log.clear();
	f.host.nodeRemoved("net");
	CHECK(f.log == Log{"node net stopping", "block destroyed", "code destroyed", "run destroyed", "config destroyed",
	                   "node net gone dropped=0"});
	f.stopAll();
}
