// The node framework driven through fake engine adapters: how a node starts, what makes it running or failed, and the
// order a configuration is torn down in (docs/design/m1-white-to-file.md section 3, S2 criterion 5;
// docs/design/batch2.md section 4.1: the port block is made first; C1 criteria 7-11: restart, sound cards, a lost
// block, removal; docs/design/rtp.md R1a criteria 3-8: RTP receivers on the same outside-node path;
// docs/design/node-framework.md slice 1: the resource registry, named inside banks, the framework's pass-through, the
// two-phase start, and the invariants I4 to I7 and I5a). Step L-3a of docs/design/node-lifecycle.md rewrote it for
// NodeHost on the lifecycle machines (its section 11.1): every drive and silence is asynchronous, its completion
// posted to the main loop by the fake block (or held for the test to run), every type goes through the legacy adapter
// (a recipe beside an outside node is a type bug, D-L40), and a failed node's configuration is torn down at once.
// Every step a fake engine would take later (ready, lost, a completion, a handoff closing, a card or receiver ready or
// lost, a close deadline passing) is an event the test sends.
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/main_loop_activity.hpp"
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
/// succeeds or fails. A config with a tag logs its start and its destruction with that tag. Its start returns a recipe
/// handing over a TestRun only when it asks for no outside node, as the legacy adapter requires (node-lifecycle.md
/// 5.10, D-L40), unless `recipe` says otherwise; its handoffs' consumer is the config's own, which outlives every
/// configuration made from it.
struct TestConfig final : NodeConfig {
	Log& log;
	std::string tag;
	Ports ports_ = testPorts();
	int handoffs = 0;
	std::vector<SoundCardRequest> cards;
	std::vector<RtpReceiverRequest> receivers;
	bool fail = false;
	std::optional<bool> recipe;              // default: a recipe when no outside node is asked for
	mutable StartContext* context = nullptr; // the context start() was given, kept to misuse it later
	mutable TestConsumer consumer;
	explicit TestConfig(Log& l, std::string t = {}) : log(l), tag(std::move(t)) {}
	~TestConfig() override {
		if (!tag.empty()) log.push_back("config " + tag + " destroyed");
	}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& c) const override {
		context = &c;
		const AudioFormat format = c.format();
		log.push_back("start " + std::to_string(format.rate) + (tag.empty() ? "" : " " + tag));
		for (int i = 0; i < handoffs; ++i) c.acquire(HandoffRequest{"record", 1024, &consumer});
		for (const auto& card : cards) c.acquire(card);
		for (const auto& receiver : receivers) c.acquire(receiver);
		if (fail) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open /nope"});
		if (!recipe.value_or(cards.empty() && receivers.empty())) return std::make_unique<TestAudioCode>(log);
		return StartResult{std::make_unique<TestAudioCode>(log), handOver(log, std::make_unique<TestRun>(log))};
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
	/// Runs everything posted, and what that posts, until nothing is left.
	void settle() {
		for (;;) {
			{
				const std::scoped_lock lock(mutex);
				if (queue.empty()) return;
			}
			runPosted();
		}
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

/// Completions a block holds until the test runs them (TestBlocks::hold).
using Held = std::vector<std::move_only_function<void()>>;

/// How the fake blocks complete what they are asked (TestBlocks): posted to the main loop, held for the test, or
/// applied with their event not yet dispatched, so only settle() runs them (a main loop that stalled, 5.4's M1).
struct BlockModes {
	bool hold = false;  // completions wait in TestBlocks::held until the test runs them
	bool stall = false; // completions wait in the block until its settle()
	std::size_t settles = 0;
	std::vector<std::vector<bool>> faded; // each drive's faded flags, in order
};

/// A block as node-lifecycle.md 5.3 has it: a drive is applied at once (the code it runs is recorded) and its `done`
/// runs later on the main loop, posted there, never inside the call; or, when held, when the test says; or, stalled,
/// when settle() is called.
struct TestBlock final : PortBlock {
	Log& log;
	AudioCode** driven;
	std::vector<LatencyPlan>* plans;
	Executor& mainLoop;
	BlockModes& modes;
	Held& held;
	Held applied; // completed, their event not yet dispatched
	TestBlock(Log& l, AudioCode** d, std::vector<LatencyPlan>* p, Executor& m, BlockModes& b, Held& hd)
		: log(l), driven(d), plans(p), mainLoop(m), modes(b), held(hd) {}
	~TestBlock() override { log.push_back("block destroyed"); }
	void carryLatency(const LatencyPlan& plan) override { plans->push_back(plan); }
	void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		*driven = request.code;
		log.push_back(request.code != nullptr ? "drive code" : "drive nothing");
		if (request.code != nullptr) modes.faded.emplace_back(request.faded.begin(), request.faded.end());
		complete([done = std::move(done)]() mutable { done(0); });
	}
	void fadeOut(std::move_only_function<void()> done) override {
		log.push_back("fade");
		complete(std::move(done));
	}
	void settle() override {
		++modes.settles;
		Held now = std::exchange(applied, {});
		for (auto& done : now) done();
	}
	std::uint64_t takenAt() const noexcept override { return 0; }
	std::uint32_t blockId() const noexcept override { return 0xFFFFFFFFU; }

private:
	void complete(std::move_only_function<void()> done) {
		if (modes.hold) held.push_back(std::move(done));
		else if (modes.stall) applied.push_back(std::move(done));
		else mainLoop.post(std::move(done));
	}
};

struct TestBlocks final : PortBlockFactory {
	Log& log;
	Executor& mainLoop;
	std::vector<PortBlockEvents*> events;
	std::deque<AudioCode*> driven;              // what each block runs now, in make order
	std::deque<std::vector<LatencyPlan>> plans; // every latency plan each block was given, in make order
	BlockModes modes;
	bool& hold = modes.hold; // completions wait in `held` instead of being posted
	Held held;
	std::vector<std::string> activities; // MainLoopActivity::current() inside each make
	bool throwInMake = false;            // make() throws, as a factory with a bug
	TestBlocks(Log& l, Executor& m) : log(l), mainLoop(m) {}
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports, const AudioFormat&,
	                                PortBlockEvents& e) override {
		if (throwInMake) throw std::runtime_error("no memory for " + node);
		log.push_back("make " + node + " " + std::to_string(ports.inputs().size()) + " inputs");
		activities.push_back(MainLoopActivity::current());
		events.push_back(&e);
		return std::make_unique<TestBlock>(log, &driven.emplace_back(nullptr), &plans.emplace_back(), mainLoop, modes,
		                                   held);
	}
	/// Runs the held completion `index`, as the block's event would on the main loop.
	void complete(std::size_t index) {
		auto done = std::move(held.at(index));
		held.erase(held.begin() + static_cast<std::ptrdiff_t>(index));
		done();
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
	bool readyInMake = false;            // tells ready() inside make(), as a card already running might
	bool throwInMake = false;            // make() throws, as an adapter with a bug
	std::vector<std::string> activities; // MainLoopActivity::current() inside each make
	std::vector<OutsideTarget> targets;
	explicit TestCards(Log& l) : log(l) {}
	std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const SoundCardRequest& request,
	                                  OutsideNodeEvents& e) override {
		if (throwInMake) throw std::runtime_error("no card " + request.card.text());
		activities.push_back(MainLoopActivity::current());
		targets.push_back(target);
		log.push_back("card " + target.engineNode + " of " + target.blockNode + " " + request.card.text() + " " +
		              joined(target.blockPorts) + " at " + std::to_string(target.format.rate));
		events.push_back(&e);
		if (readyInMake) {
			e.ready(); // heard once make() returned (5.5, D-L39)
			log.push_back("card " + target.engineNode + " made");
		}
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
	std::chrono::milliseconds clock{0};
	std::vector<std::shared_ptr<Armed>> armed;
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		auto timer = std::make_shared<Armed>(Armed{clock + delay, std::move(due)});
		armed.push_back(timer);
		return std::make_unique<Handle>(timer);
	}
	std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::time_point{clock}; }
	/// Timers still armed (not cancelled, not yet due).
	std::size_t live() const {
		return static_cast<std::size_t>(
			std::ranges::count_if(armed, [](const auto& timer) { return !timer->cancelled && timer->due; }));
	}
	/// Moves the clock and runs what came due, in arming order.
	void advance(std::chrono::milliseconds by) {
		clock += by;
		auto due = armed; // a due timer may arm or cancel others
		for (const auto& timer : due) {
			if (timer->cancelled || !timer->due || timer->at > clock) continue;
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

struct TestDiagnostics final : NodeDiagnosticListener, EngineDiagnosticListener {
	Log& log;
	std::vector<std::string> details;
	explicit TestDiagnostics(Log& l) : log(l) {}
	void nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail,
	                    std::span<const DiagnosticField> fields) override {
		std::string line = "diagnostic " + node + " " + std::string(code);
		for (const DiagnosticField& field : fields)
			line += " " + std::string(field.key) + "=" + std::to_string(field.value);
		log.push_back(line);
		details.push_back(detail);
	}
	void engineDiagnostic(const EngineDiagnostic& diagnostic) override {
		std::string line = "engine diagnostic " + std::string(diagnostic.code);
		for (const DiagnosticField& field : diagnostic.fields)
			line += " " + std::string(field.key) + "=" + std::to_string(field.value);
		log.push_back(line);
		details.push_back(diagnostic.detail);
	}
};

constexpr std::chrono::milliseconds closeDeadline{10'000};

struct Fixture {
	Log log;
	QueueExecutor mainLoop;
	TestBlocks blocks{log, mainLoop};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	ResourceRegistry resources{cards, receivers, handoffs};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue}; // only for close(), the proof stopAll takes
	FakeTimers timers;
	TestNotes notes{log};
	TestDiagnostics diagnostics{log};
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	int allGone = 0; // how often stopAll's callback has run
	Fixture() {
		host.subscribe(listener);
		host.subscribeNotes(notes);
		host.subscribeDiagnostics(diagnostics);
		host.subscribeEngineDiagnostics(diagnostics);
	}
	/// The node added, then the turn ends: what it posted runs (its flush makes its block, 5.5).
	void add(const NodeName& name, std::shared_ptr<const NodeConfig> config) {
		try {
			addInTurn(name, std::move(config));
		} catch (...) {
			mainLoop.runPosted(); // the turn ends all the same
			throw;
		}
		mainLoop.runPosted();
	}
	/// The node added; the turn goes on (nothing posted runs yet).
	void addInTurn(const NodeName& name, std::shared_ptr<const NodeConfig> config) {
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
	/// The main loop turns until nothing is left: every completion the blocks posted, and what follows from it.
	void settle() { mainLoop.settle(); }
	/// The turn ends: what was posted so far runs (the turn's flush among it, 5.5), and nothing it posts.
	void endTurn() { mainLoop.runPosted(); }
};

std::ptrdiff_t indexOf(const Log& log, const std::string& line) {
	return std::find(log.begin(), log.end(), line) - log.begin();
}

} // namespace

TEST_CASE("A node gets its port block, then starts, builds its run and prepares its code, and drives the block once it "
          "is ready",
          "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;

	f.add("rec", rec);

	// T2, then T3 in the same turn (no outside node); the block is made in the turn's flush (5.5): nothing drives a
	// block that is not ready.
	CHECK(f.log == Log{"node rec starting", "start 44100", "handoff rec 2 channels", "run built", "prepare 44100/256",
	                   "make rec 2 inputs"});
	CHECK(f.blocks.driven.at(0) == nullptr);
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::starting);
	f.log.clear();

	f.blocks.events.at(0)->ready();
	CHECK(f.log == Log{"drive code"});
	CHECK(f.blocks.driven.at(0) != nullptr);

	f.stopAll();
	f.settle();
	f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

TEST_CASE("A drive's completion arrives later than the call, and the node is running only then", "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.log.clear();

	f.blocks.events.at(0)->ready();
	f.blocks.events.at(0)->ready(); // at most once; a repeat changes nothing

	CHECK(f.log == Log{"drive code"}); // the call returned; its completion waits on the main loop
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::starting);
	f.log.clear();

	f.settle();
	CHECK(f.log == Log{"node rec running"});
	CHECK(f.host.health()[0].state == NodeState::running);
	f.stopAll();
	f.settle();
}

TEST_CASE("A node whose start fails is failed with start-failed and keeps its port block", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;
	rec->fail = true;

	f.add("rec", rec);

	// The handoff it asked for goes at once (nothing ever wrote to it); no prepare, no drive.
	CHECK(f.log == Log{"node rec starting", "start 44100", "handoff rec 2 channels", "handoff destroyed",
	                   "node rec failed start-failed: cannot open /nope", "make rec 2 inputs"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	REQUIRE(f.host.health()[0].reason.has_value());
	CHECK(f.host.health()[0].reason->code == FailureReason::Code::startFailed);

	// The block becoming ready leaves the node failed, and drives nothing.
	f.log.clear();
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(f.log.empty());

	// Nothing was driven, so the removal needs no barrier (D-L7); the node is gone once the flush destroyed its block.
	f.stopAll();
	CHECK(f.log == Log{"node rec stopping"});
	f.settle();
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "node rec gone dropped=0", "all gone"});
}

TEST_CASE("A node whose port block is lost is failed with engine-refused, and its configuration is torn down",
          "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	f.blocks.events.at(0)->lost("driver gone");

	// No barrier can wait on a lost block, but the silence completes by signal (T13).
	CHECK(f.log == Log{"node rec failed engine-refused: driver gone", "drive nothing"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"release", "run destroyed"});
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "node rec gone dropped=0", "all gone"});
}

TEST_CASE("stopAll takes a node down in order: the barrier, release, handoffs, block, run, gone", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 2;
	f.add("rec", rec);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	f.stopAll();
	CHECK(f.log == Log{"node rec stopping", "fade"}); // its inside output pb fades before the silence
	f.log.clear();
	f.settle();

	// Nothing the consumers live in goes until every handoff has closed; the block goes once the silence completed.
	CHECK(f.log == Log{"drive nothing", "release", "handoff close", "handoff close", "block destroyed"});
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
	CHECK(f.timers.live() == 0); // the close deadline went with the configuration
}

TEST_CASE("allGone runs once, after the last node is gone", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;
	f.add("rec", rec);
	f.add("noise", std::make_shared<TestConfig>(f.log));
	f.log.clear();

	f.stopAll();

	// Neither was ever driven: noise has no handoff, so it is gone once the flush destroyed its block; rec waits for
	// its handoff.
	f.settle();
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

TEST_CASE("An update restarts the configuration behind the same port block, letting the old config go last",
          "[node_host]") {
	Fixture f;
	{
		auto a = std::make_shared<TestConfig>(f.log, "A");
		a->handoffs = 1;
		f.add("rec", std::move(a)); // the host holds the only reference
	}
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	auto b = std::make_shared<TestConfig>(f.log, "B");
	b->handoffs = 1;
	f.update("rec", std::move(b));

	// The block fades and stops running the old code before release(); the handoff then closes.
	CHECK(f.log == Log{"node rec starting", "fade"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"drive nothing", "release", "handoff close"});
	f.log.clear();

	SECTION("the old configuration goes, then its config, then the new config starts") {
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 B",
		                   "handoff rec 2 channels", "run built", "prepare 44100/256", "drive code"});
		f.log.clear();
		f.settle();
		CHECK(f.log == Log{"node rec running"});
	}
	SECTION("two updates during one restart start only the latest config") {
		auto c = std::make_shared<TestConfig>(f.log, "C");
		f.update("rec", std::move(c));
		CHECK(f.log == Log{"config B destroyed"}); // B never started; nothing else happened
		f.log.clear();
		f.handoffs.finishOne(0);
		f.settle();
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 C", "run built",
		                   "prepare 44100/256", "drive code", "node rec running"});
	}
	SECTION("a removal during a restart stops the node and starts nothing") {
		f.host.nodeRemoved("rec");
		// Silenced already, so the block goes in this turn's flush.
		CHECK(f.log == Log{"config B destroyed", "node rec stopping"});
		f.settle();
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
	f.settle();
	CHECK(f.log.at(1) == "fade");
	CHECK(f.log.at(2) == "drive nothing");
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

TEST_CASE("An update of a node whose start failed configures the new config", "[node_host]") {
	Fixture f;
	auto a = std::make_shared<TestConfig>(f.log, "A");
	a->fail = true;
	f.add("rec", std::move(a));
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.update("rec", std::make_shared<TestConfig>(f.log, "B"));

	CHECK(f.log == Log{"config A destroyed", "node rec starting", "start 44100 B", "run built", "prepare 44100/256",
	                   "drive code"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"node rec running"});
	f.stopAll();
	f.settle();
}

TEST_CASE("A node's sound cards are made once its port block is ready, and it is driven and running once every card "
          "is ready",
          "[node_host]") {
	Fixture f;
	f.add("motu", withCards(f.log));

	// No card before the block is ready, whatever the configuration asked for; nothing is prepared or driven yet.
	CHECK(f.log == Log{"node motu starting", "start 44100", "make motu 2 inputs"});
	CHECK(f.cards.events.empty());
	f.log.clear();

	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty()); // made in the turn's flush (5.5)
	f.endTurn();
	// Each card joins the ports of the inside bank it named, under its one name.
	CHECK(f.log ==
	      Log{"card motu.playback of motu Pro24 pb.1,pb.2 at 44100", "card motu.capture of motu Pro24 cap.1 at 44100"});
	REQUIRE(f.cards.events.size() == 2);
	CHECK(f.host.health().at(0).state == NodeState::starting);
	f.log.clear();

	f.cards.events[1]->ready();
	CHECK(f.log.empty()); // T3 waits for every resource
	f.cards.events[0]->ready();
	CHECK(f.log == Log{"prepare 44100/256", "drive code"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"node motu running"});
	f.log.clear();

	SECTION("a lost card fails the node with the card's detail, and the configuration is torn down") {
		f.cards.events[0]->lost("unplugged");
		CHECK(f.log == Log{"fade", "node motu failed engine-refused: unplugged"}); // to_card's pb fades first
		f.log.clear();
		f.settle();
		CHECK(f.log == Log{"drive nothing", "release", "card motu.playback destroyed", "card motu.capture destroyed"});
		f.log.clear();
	}
	SECTION("a restart remakes the cards at once, the block being ready, in the flush that destroys the old ones") {
		f.update("motu", withCards(f.log, "B"));
		f.settle();
		CHECK(f.log == Log{"node motu starting", "fade", "drive nothing", "release", "start 44100 B",
		                   "card motu.playback destroyed", "card motu.capture destroyed",
		                   "card motu.playback of motu Pro24 pb.1,pb.2 at 44100",
		                   "card motu.capture of motu Pro24 cap.1 at 44100"});
		REQUIRE(f.cards.events.size() == 4);
		f.cards.events[2]->ready();
		f.cards.events[3]->ready();
		f.settle();
		CHECK(f.log.back() == "node motu running");
		f.log.clear();
	}

	// Teardown takes the cards after the barrier and release().
	f.stopAll();
	f.settle();
	CHECK(f.log.back() == "all gone");
	if (f.cards.events.size() == 4) {
		CHECK(f.log.at(1) == "fade");
		CHECK(f.log.at(2) == "drive nothing");
		CHECK(indexOf(f.log, "release") < indexOf(f.log, "card motu.playback destroyed"));
		CHECK(indexOf(f.log, "card motu.capture destroyed") < indexOf(f.log, "node motu gone dropped=0"));
	}
}

TEST_CASE("Cards that say ready inside make() let T3 run only once every card is made (7.2)", "[node_host]") {
	Fixture f;
	f.cards.readyInMake = true;
	f.add("motu", withCards(f.log));
	f.log.clear();
	f.blocks.events.at(0)->ready();
	f.endTurn();
	CHECK(f.log == Log{"card motu.playback of motu Pro24 pb.1,pb.2 at 44100", "card motu.playback made",
	                   "card motu.capture of motu Pro24 cap.1 at 44100", "card motu.capture made", "prepare 44100/256",
	                   "drive code"});
	f.settle();
	CHECK(f.log.back() == "node motu running");
	f.stopAll();
	f.settle();
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
	f.settle();
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
	f.endTurn();
	f.cards.events.at(0)->ready();
	f.cards.events.at(1)->ready();
	f.settle();
	REQUIRE(f.host.health().at(0).state == NodeState::running);
	f.log.clear();

	f.blocks.events.at(0)->lost("driver gone");
	CHECK(f.log == Log{"node motu failed engine-refused: driver gone", "drive nothing"});
	f.log.clear();

	// A card failing afterwards, during the silence, changes nothing: the block's reason stands.
	f.cards.events.at(0)->lost("unplugged");
	CHECK(f.log.empty());
	f.settle();
	f.handoffs.finishOne(0);
	// The cards' destruction is queued into the turn's flush, which runs after the handoffs were told to close.
	CHECK(f.log == Log{"release", "handoff close", "card motu.playback destroyed", "card motu.capture destroyed",
	                   "handoff destroyed", "config A destroyed"});
	f.log.clear();

	// An update starts nothing; no starting health.
	f.update("motu", withCards(f.log, "B"));
	f.settle();
	CHECK(f.log == Log{"config B destroyed"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	REQUIRE(f.host.health()[0].reason.has_value());
	CHECK(f.host.health()[0].reason->detail == "driver gone");
	CHECK(f.cards.events.size() == 2);
	f.log.clear();

	// Removal still ends with nodeGone.
	f.host.nodeRemoved("motu");
	f.settle();
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
	f.settle();
	f.log.clear();

	f.host.nodeRemoved("noise");
	f.settle();
	// The block's destruction waits for the turn's flush; the configuration goes meanwhile (5.7).
	CHECK(f.log == Log{"node noise stopping", "fade", "drive nothing", "release", "run destroyed", "block destroyed",
	                   "node noise gone dropped=0"});
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
		f.settle();
		f.handoffs.finishOne(5);
		CHECK(std::ranges::find(f.log, "node rec gone dropped=5") != f.log.end());
		CHECK(f.log.back() == "prepare 44100/256"); // the newer one waited for it; its block is not ready
		REQUIRE(f.host.health().size() == 1);
		CHECK(f.host.health()[0].state == NodeState::starting);
	}
	SECTION("stopAll after a removal still waits for it") {
		f.host.nodeRemoved("rec");
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 0);
		f.handoffs.finishOne(0);
		CHECK(f.allGone == 1);
		return;
	}
	f.stopAll();
	f.settle();
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
	f.endTurn();
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
	f.settle();
	CHECK(f.log == Log{"prepare 44100/256", "drive code", "node fpp running"});
	f.stopAll();
	f.settle();
}

TEST_CASE("A node with a card and a receiver runs only once both are ready, in either order", "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {fppStream};
	f.add("mix", config);
	f.log.clear();
	f.blocks.events.at(0)->ready();
	f.endTurn();
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
	f.settle();
	CHECK(f.log == Log{"prepare 44100/256", "drive code", "node mix running"});
	f.stopAll();
	f.settle();
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
	f.endTurn();
	f.receivers.events.at(0)->ready();
	f.settle();
	REQUIRE(f.host.health().at(0).state == NodeState::running);
	f.log.clear();

	f.receivers.events[0]->lost("the engine removed the RTP receiver");
	CHECK(f.log == Log{"fade", "node fpp failed engine-refused: the engine removed the RTP receiver"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"drive nothing", "release", "handoff close", "receiver fpp.rtp destroyed"});
	f.log.clear();
	f.handoffs.finishOne(0);
	CHECK(f.log == Log{"handoff destroyed"}); // A stays the node's newest config until B replaces it
	f.log.clear();

	f.update("fpp", withReceiver(f.log, "B"));
	f.endTurn();
	CHECK(f.log == Log{"config A destroyed", "node fpp starting", "start 44100 B", "receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.events.size() == 2); // made once more, not twice
	CHECK(f.blocks.events.size() == 1);      // the block stayed
	f.log.clear();
	f.receivers.events[1]->ready();
	f.settle();
	CHECK(f.log == Log{"prepare 44100/256", "drive code", "node fpp running"});
	f.stopAll();
	f.settle();
}

TEST_CASE("An outside node lost before the node ran fails it with engine-refused and the outside "
          "node's detail, and its code is never prepared or driven",
          "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {fppStream};
	f.add("mix", config);
	f.blocks.events.at(0)->ready();
	f.endTurn();
	REQUIRE(f.cards.events.size() == 1);
	REQUIRE(f.receivers.events.size() == 1);
	f.log.clear();

	// Never driven: torn down at once, with no barrier; the outside nodes go in the turn's flush.
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
	f.settle();
	CHECK(f.log == Log{"card mix.playback destroyed", "receiver mix.rtp destroyed"});
	f.stopAll();
	f.settle();
	CHECK(std::ranges::count(f.log, std::string("prepare 44100/256")) == 0);
	CHECK(std::ranges::count(f.log, std::string("drive code")) == 0);
	CHECK(std::ranges::count(f.log, std::string("drive nothing")) == 0);
	CHECK(f.allGone == 1);
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

	// A configuration a type bug was thrown out of asks for nothing more.
	for (auto* events : f.blocks.events) events->ready();
	CHECK(f.receivers.events.empty());
	CHECK(f.cards.events.empty());
	// They still stop: ~NodeHost's precondition.
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A legacy type with a recipe that asks for an outside node is a std::logic_error at T2",
          "[node_host][legacy]") {
	Fixture f;
	auto config = withCards(f.log);
	config->recipe = true;
	CHECK_THROWS_WITH(
		f.add("motu", config),
		Catch::Contains("a legacy type with a recipe may ask for no outside node: node-lifecycle.md 5.10"));
	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty());
	CHECK(std::ranges::count(f.log, std::string("run built")) == 0);
	f.stopAll(); // ~NodeHost's precondition; it asked for no handoff
	f.settle();
	CHECK(f.allGone == 1);

	// A recipe alone, or outside nodes alone, is fine.
	Fixture g;
	g.add("rec", std::make_shared<TestConfig>(g.log));
	g.add("motu", withCards(g.log));
	CHECK(g.host.health().at(0).state == NodeState::starting);
	CHECK(g.host.health().at(1).state == NodeState::starting);
	g.stopAll();
	g.settle();
	CHECK(g.allGone == 1);
}

TEST_CASE("Restart and removal destroy the receiver after the barrier, in the turn's flush, before the handoffs finish "
          "and the config goes",
          "[node_host][rtp]") {
	Fixture f;
	auto a = withReceiver(f.log, "A");
	a->handoffs = 1;
	f.add("fpp", std::move(a));
	f.blocks.events.at(0)->ready();
	f.endTurn();
	f.receivers.events.at(0)->ready();
	f.settle();
	f.log.clear();

	SECTION("restart") {
		f.update("fpp", withReceiver(f.log, "B"));
		f.settle();
		f.handoffs.finishOne(0);
	}
	SECTION("removal") {
		f.host.nodeRemoved("fpp");
		f.settle();
		f.handoffs.finishOne(0);
		CHECK(f.log.back() == "node fpp gone dropped=0");
	}
	const auto destroyed = indexOf(f.log, "receiver fpp.rtp destroyed");
	CHECK(destroyed < static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(indexOf(f.log, "drive nothing") < indexOf(f.log, "release"));
	CHECK(indexOf(f.log, "release") < destroyed);
	CHECK(indexOf(f.log, "handoff close") < destroyed); // told to close in the teardown, the flush after it (5.5)
	CHECK(destroyed < indexOf(f.log, "handoff destroyed"));
	CHECK(indexOf(f.log, "handoff destroyed") < indexOf(f.log, "config A destroyed"));
	f.log.clear();
	f.stopAll();
	f.settle();
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
	StartResult start(StartContext&) const override { return StartResult{passThrough(pairs)}; }
};

} // namespace

TEST_CASE("The framework's pass-through copies each bank to its pair and silences every output no pair names",
          "[node_host][pass_through]") {
	Fixture f;
	f.add("card", std::make_shared<PassConfig>());
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(f.host.health().at(0).state == NodeState::running); // no recipe: running once its drive completed
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
	f.settle();
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
		return passThrough(pass.pairs);
	}
};
} // namespace

TEST_CASE("A configuration's latency plan reaches its block with its drive: the pass-through's pairs and what start "
          "declared, cleared at its teardown",
          "[node_host][latency]") {
	Fixture f;
	auto config = std::make_shared<LatencyConfig>();
	f.add("card", config);
	CHECK(f.blocks.plans.at(0).empty()); // nothing is carried before the drive
	CHECK_THROWS_AS(config->context->declareLatency("spare", Latency{1, 1}), std::logic_error); // outside start()
	f.blocks.events.at(0)->ready();
	REQUIRE(f.blocks.plans.at(0).size() == 1);
	const LatencyPlan& plan = f.blocks.plans.at(0).at(0);
	CHECK(plan.pairs == config->pass.pairs);
	REQUIRE(plan.internal.size() == 1);
	CHECK(plan.internal[0].first == "capture");
	CHECK(plan.internal[0].second == Latency{100, 200});
	f.settle();

	f.update("card", std::make_shared<LatencyConfig>());
	f.settle();
	REQUIRE(f.blocks.plans.at(0).size() == 3); // cleared by the teardown, then the new configuration's
	CHECK(f.blocks.plans.at(0).at(1).pairs.empty());
	CHECK(f.blocks.plans.at(0).at(1).internal.empty());
	CHECK(f.blocks.plans.at(0).at(2).internal.size() == 1);
	f.stopAll();
	f.settle();
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

// The asynchronous barrier (docs/design/node-lifecycle.md sections 4.2, 5.3 and 5.6).

TEST_CASE("A stale completion (an older barrier, a removed node) changes nothing", "[node_host][async]") {
	Fixture f;
	f.blocks.hold = true;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	REQUIRE(f.blocks.held.size() == 1); // the drive's completion
	f.log.clear();

	// The block is lost with the drive in flight: a silence is asked for behind it.
	f.blocks.events.at(0)->lost("driver gone");
	REQUIRE(f.blocks.held.size() == 2);
	f.log.clear();

	// The drive's completion, heard after the silence was asked for, is an older barrier's: no running, nothing.
	f.blocks.complete(0);
	CHECK(f.log.empty());
	CHECK(f.host.health().at(0).state == NodeState::failed);

	// The silence's is current: the configuration is torn down.
	f.blocks.complete(0);
	CHECK(f.log == Log{"release", "run destroyed"});
	f.log.clear();

	// A completion that outlives its node finds nothing: a new node of the name never hears it.
	f.blocks.hold = true;
	f.host.nodeRemoved("rec");
	f.add("other", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(1)->ready();
	REQUIRE(f.blocks.held.size() == 1);
	f.host.nodeRemoved("other"); // its drive in flight: the silence waits for its completion
	CHECK(f.blocks.held.size() == 1);
	f.add("other", std::make_shared<TestConfig>(f.log));
	f.log.clear();
	f.blocks.complete(0); // the old node's drive: it fades and silences that node, never the new one
	CHECK(f.log == Log{"fade"});
	CHECK(f.host.health().back().state == NodeState::starting);
	f.blocks.hold = false;
	f.blocks.complete(0);
	f.settle();
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A restart while the drive is in flight silences it at its completion and never reports running",
          "[node_host][async]") {
	Fixture f;
	f.blocks.hold = true;
	f.add("rec", std::make_shared<TestConfig>(f.log, "A"));
	f.blocks.events.at(0)->ready();
	f.log.clear();

	f.update("rec", std::make_shared<TestConfig>(f.log, "B"));
	CHECK(f.log.empty()); // nothing to silence until the drive is applied (6.3's "stop in starting")
	f.blocks.complete(0);
	CHECK(f.log == Log{"fade"});
	f.blocks.complete(0);
	CHECK(f.log == Log{"fade", "drive nothing"});
	f.blocks.complete(0);
	CHECK(f.log == Log{"fade", "drive nothing", "release", "run destroyed", "config A destroyed", "start 44100 B",
	                   "run built", "prepare 44100/256", "drive code"});
	f.blocks.complete(0);
	CHECK(f.log.back() == "node rec running");
	CHECK(std::ranges::count(f.log, std::string("node rec running")) == 1);
	f.blocks.hold = false;
	f.stopAll();
	f.settle();
}

TEST_CASE("Removal silences before it destroys the block, and a node never driven is destroyed in the turn's flush",
          "[node_host][async]") {
	Fixture f;
	f.add("noise", std::make_shared<TestConfig>(f.log));
	f.add("idle", std::make_shared<TestConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	f.host.nodeRemoved("noise");
	CHECK(f.log == Log{"node noise stopping", "fade"}); // the block stays until the fade and the silence completed
	f.log.clear();
	f.settle();
	// The block goes in the flush, the configuration meanwhile; the node is gone once both are (5.7).
	CHECK(f.log == Log{"drive nothing", "release", "run destroyed", "block destroyed", "node noise gone dropped=0"});
	f.log.clear();

	f.host.nodeRemoved("idle"); // no code was driven since its last silence (D-L7)
	CHECK(f.log == Log{"node idle stopping", "release", "run destroyed"});
	f.endTurn();
	CHECK(f.log ==
	      Log{"node idle stopping", "release", "run destroyed", "block destroyed", "node idle gone dropped=0"});
	f.stopAll();
	CHECK(f.allGone == 1);
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
		return StartResult{std::make_unique<TestAudioCode>(log),
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

TEST_CASE("A fault while a restart's drive is in flight is dropped, and the restart configures the new config",
          "[node_host][async]") {
	SECTION("the run's own thread says it died") {
		Fixture f;
		f.blocks.hold = true;
		auto old = std::make_shared<ThreadedConfig>(f.log);
		f.add("net", old);
		f.blocks.events.at(0)->ready(); // the drive is in flight
		REQUIRE(f.blocks.held.size() == 1);
		f.update("net", std::make_shared<TestConfig>(f.log, "next"));
		std::thread([&] { old->events->lost("died"); }).join();
		f.blocks.complete(0);   // the drive: its completion silences it (no output to fade)
		f.mainLoop.runPosted(); // the loss, told once the restart began
		f.blocks.complete(0);   // the silence
		f.blocks.hold = false;
		old->run->closed();
		f.settle();
		CHECK(std::ranges::count(f.log, std::string("node net failed start-failed: died")) == 0);
		CHECK(std::ranges::find(f.log, std::string("start 44100 next")) != f.log.end());
		CHECK(f.log.back() == "node net running");
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("a card is lost") {
		Fixture f;
		f.blocks.hold = true;
		f.add("motu", withCards(f.log, "A"));
		f.blocks.events.at(0)->ready();
		f.endTurn();
		f.cards.events.at(0)->ready();
		f.cards.events.at(1)->ready();
		REQUIRE(f.blocks.held.size() == 1);
		f.update("motu", withCards(f.log, "B"));
		f.cards.events.at(0)->lost("unplugged");
		f.blocks.complete(0); // the drive
		f.blocks.complete(0); // the fade
		f.blocks.complete(0); // the silence
		f.blocks.hold = false;
		CHECK(std::ranges::count(f.log, std::string("node motu failed engine-refused: unplugged")) == 0);
		CHECK(std::ranges::find(f.log, std::string("start 44100 B")) != f.log.end());
		f.endTurn();
		REQUIRE(f.cards.events.size() == 4);
		f.cards.events.at(2)->ready();
		f.cards.events.at(3)->ready();
		f.settle();
		CHECK(f.log.back() == "node motu running");
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("A run object closes after every handoff has closed, and goes only after it says closed", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	config->handoffs = 1;
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	f.stopAll();
	f.settle();
	CHECK(f.log == Log{"node net stopping", "drive nothing", "release", "handoff close", "block destroyed"});
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

TEST_CASE("A restart waits for the old configuration to close before it configures the new one (I7)", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.update("net", next);
	f.settle();
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close"});
	f.log.clear();

	std::thread([&] { old->run->closed(); }).join();
	CHECK(f.log.empty());
	f.settle();
	CHECK(f.log ==
	      Log{"run destroyed", "start 44100 next", "run built", "prepare 44100/256", "drive code", "node net running"});
	f.stopAll();
	f.settle();
}

TEST_CASE("A name added back while its old configuration is closing configures only once it is gone", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.host.nodeRemoved("net");
	f.settle();
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.add("net", next);
	// Its block comes at once, the older block being destroyed already (T1); its configuration waits.
	CHECK(f.log == Log{"node net starting", "make net 2 inputs"});
	f.log.clear();

	SECTION("then it configures, after the old node is gone") {
		std::thread([&] { old->run->closed(); }).join();
		f.settle();
		CHECK(f.log ==
		      Log{"run destroyed", "node net gone dropped=0", "start 44100 next", "run built", "prepare 44100/256"});
	}
	SECTION("an update while it waits replaces what will configure, and nothing configures yet") {
		auto newer = std::make_shared<TestConfig>(f.log, "newer");
		f.update("net", newer);
		CHECK(f.log.empty());
		std::thread([&] { old->run->closed(); }).join();
		f.settle();
		CHECK(f.log ==
		      Log{"run destroyed", "node net gone dropped=0", "start 44100 newer", "run built", "prepare 44100/256"});
	}
	SECTION("removed again while it waits, it never configures") {
		f.host.nodeRemoved("net");
		f.endTurn();
		CHECK(f.log == Log{"node net stopping", "block destroyed", "node net gone dropped=0"});
		f.log.clear();
		std::thread([&] { old->run->closed(); }).join();
		f.settle();
		CHECK(f.log == Log{"run destroyed", "node net gone dropped=0"});
	}
	SECTION("removed and added back while it waits, only the newest configures") {
		f.host.nodeRemoved("net");
		f.log.clear();
		auto newest = std::make_shared<TestConfig>(f.log, "newest");
		f.add("net", newest);
		f.log.clear();
		std::thread([&] { old->run->closed(); }).join();
		f.settle();
		CHECK(f.log ==
		      Log{"run destroyed", "node net gone dropped=0", "start 44100 newest", "run built", "prepare 44100/256"});
	}
	f.stopAll();
	f.settle();
}

TEST_CASE("A name added back whose block is lost while it waits never configures (I1)", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.host.nodeRemoved("net");
	f.settle();
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	f.log.clear();
	f.blocks.events.at(1)->lost("the engine refused the block");
	CHECK(f.log == Log{"node net failed engine-refused: the engine refused the block", "config next destroyed"});
	f.log.clear();

	std::thread([&] { old->run->closed(); }).join();
	f.settle();
	CHECK(f.log == Log{"run destroyed", "node net gone dropped=0"});
	CHECK(f.host.health().at(0).state == NodeState::failed);
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(f.log == Log{"node net stopping", "block destroyed", "node net gone dropped=0", "all gone"});
}

TEST_CASE("A name added back after its old node has gone configures at once", "[node_host]") {
	Fixture f;
	f.add("net", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("net");
	f.endTurn(); // the old block destroyed: the old node is gone
	f.log.clear();
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	CHECK(f.log == Log{"node net starting", "start 44100 next", "run built", "prepare 44100/256", "make net 2 inputs"});
	f.stopAll();
}

TEST_CASE("Stopping while a name waits for its old configuration stops both, in order", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.host.nodeRemoved("net");
	f.settle();
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	f.log.clear();
	f.stopAll();
	CHECK(f.allGone == 0);
	std::thread([&] { old->run->closed(); }).join();
	f.settle();
	CHECK(f.allGone == 1);
	CHECK(std::ranges::count(f.log, std::string("start 44100 next")) == 0);
}

TEST_CASE("What a run says reaches the note listeners on the main loop, in order, and nothing after closed",
          "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
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
	f.settle(); // the run is told to close
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

TEST_CASE("A run lost from its own thread fails a running node with start-failed and tears its configuration down, "
          "and its block and routes stay",
          "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	std::thread([&] { config->events->lost("the client died"); }).join();
	CHECK(f.log.empty());
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"drive nothing", "node net failed start-failed: the client died"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"release", "run close"});

	f.log.clear();
	f.stopAll();
	f.endTurn();
	CHECK(f.log == Log{"node net stopping", "block destroyed"});
	f.log.clear();
	config->run->closed();
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"run destroyed", "node net gone dropped=0", "all gone"});
}

TEST_CASE("A loss told once close began changes nothing", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	// Told before the restart, heard after it began: the node must not fail behind the restart.
	config->events->lost("late");
	f.update("net", std::make_shared<TestConfig>(f.log, "next"));
	f.settle();
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close"});
	f.log.clear();
	config->run->closed();
	f.settle();
	CHECK(std::ranges::find(f.log, std::string("node net running")) != f.log.end());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
}

// The two-phase start (docs/design/node-framework.md section 7.2), through the legacy adapter
// (docs/design/node-lifecycle.md section 5.10).

namespace {

/// A config whose recipe fails, or logs what Ready hands it. It asks for no outside node (D-L40).
struct RecipeConfig final : NodeConfig {
	Log& log;
	bool fail = false;
	Ports ports_ = testPorts();
	explicit RecipeConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override {
		log.push_back("start acquires");
		return StartResult{std::make_unique<TestAudioCode>(log),
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

TEST_CASE("Start only acquires, the recipe runs once every resource is ready and before the code is prepared, and the "
          "code drives the block only after it",
          "[node_host][two_phase]") {
	Fixture f;
	auto config = std::make_shared<RecipeConfig>(f.log);

	SECTION("a recipe that builds its run object makes the node running once driven; what it says is heard") {
		f.add("net", config);
		// What the recipe said crosses to the main loop, posted behind the turn's flush.
		CHECK(f.log == Log{"node net starting", "start acquires", "recipe at 44100", "prepare 44100/256",
		                   "make net 2 inputs", "note net built"});
		CHECK(f.blocks.driven.at(0) == nullptr);
		f.log.clear();
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.log == Log{"drive code", "node net running"});
	}
	SECTION("a recipe that fails fails the node with start-failed; its code is never prepared or driven") {
		config->fail = true;
		f.add("net", config);
		CHECK(f.log == Log{"node net starting", "start acquires", "recipe at 44100",
		                   "node net failed start-failed: cannot read the key file", "make net 2 inputs"});
		f.log.clear();
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.log.empty());
		f.update("net", std::make_shared<TestConfig>(f.log, "B"));
		CHECK(f.log.at(0) == "node net starting"); // at once, as before the teardown
		CHECK(f.log.at(1) == "start 44100 B");
	}
	f.log.clear();
	f.stopAll();
	f.settle();
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
		return StartResult{std::make_unique<TestAudioCode>(log), handOver(log, std::move(made))};
	}
};

/// The fixture with handoffs that run real threads.
struct ThreadedFixture {
	Log log;
	QueueExecutor mainLoop;
	TestBlocks blocks{log, mainLoop};
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
		host.subscribeEngineDiagnostics(diagnostics);
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
	f.mainLoop.runPosted(); // the turn's flush makes the blocks
	f.blocks.events.at(0)->ready();
	f.blocks.events.at(1)->ready();
	f.mainLoop.settle();
	f.log.clear();

	f.host.nodeRemoved("rec");
	f.mainLoop.settle(); // returns although the consumer is stuck
	config->run->consumer.entered.get_future().wait();
	CHECK(f.log == Log{"node rec stopping", "drive nothing", "release", "block destroyed"});
	f.log.clear();

	// The main loop keeps serving other nodes meanwhile.
	f.host.nodeRemoved("noise");
	f.mainLoop.settle();
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
	f.mainLoop.runPosted();
	f.blocks.events.at(0)->ready();
	f.mainLoop.settle();
	f.log.clear();

	int allGone = 0;
	f.host.stopAll(f.graph.close(), [&] { ++allGone; });
	f.mainLoop.settle();
	config->run->consumer.entered.get_future().wait();
	f.timers.advance(closeDeadline - std::chrono::milliseconds(1));
	CHECK(allGone == 0);

	CHECK(f.host.leaked() == 0);
	f.timers.advance(std::chrono::milliseconds(1));
	// Stop proceeds past the bound: the node is gone, the run object was never closed nor destroyed.
	CHECK(allGone == 1);
	CHECK(f.host.leaked() == 1); // main.cpp then ends without destroying the main loop under the leaked run
	CHECK(f.log == Log{"node rec stopping", "drive nothing", "release", "block destroyed",
	                   "diagnostic rec ear.close-overdue", "node rec gone dropped=0"});
	REQUIRE(f.diagnostics.details.size() == 1);
	CHECK(f.diagnostics.details[0].find("1 handoffs open") != std::string::npos);
	f.log.clear();

	// The handoff thread finally finishes: its `closed` reaches nothing, and the consumer it used is still alive.
	letGo.set_value();
	REQUIRE(f.mainLoop.waitFor(1));
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
}

TEST_CASE("A run never closing past the deadline is leaked, and a restart then configures the next one (I5a, I7)",
          "[node_host][teardown]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	InsidesEvents* oldEvents = old->events;
	ThreadedRun* oldRun = old->run;
	f.log.clear();

	f.update("net", std::make_shared<TestConfig>(f.log, "next"));
	f.settle();
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close"});
	f.log.clear();
	CHECK(f.timers.live() == 1);

	f.timers.advance(closeDeadline);
	// The old run is leaked (never destroyed: its threads may still touch it); the next configuration begins.
	CHECK(f.log ==
	      Log{"diagnostic net ear.close-overdue", "start 44100 next", "run built", "prepare 44100/256", "drive code"});
	CHECK(std::ranges::count(f.log, std::string("run destroyed")) == 0);
	CHECK(f.host.leaked() == 1);
	f.settle();
	CHECK(f.log.back() == "node net running");
	f.log.clear();

	// What the leaked run tells late lands nowhere: not on the new configuration (I6), and without touching freed
	// memory.
	oldEvents->say("still here");
	oldEvents->lost("the client died");
	oldRun->closed();
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A run mid-handler at teardown finishes off the main loop: close returns at once, a fact it tells is dropped "
          "and its frees wait for closed (I2, I5, I6)",
          "[node_host][teardown]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
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
	f.settle();
	CHECK(f.log == Log{"node net stopping", "drive nothing", "release", "run close", "block destroyed"});
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

TEST_CASE("Nothing from an old run lands on a new one, even when it closed at once and the next configuration began "
          "in the same turn (I6)",
          "[node_host][teardown]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	old->closeAtOnce = true;
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	InsidesEvents* oldEvents = old->events;
	f.log.clear();

	// Written by the old run's thread just before its close; heard on the main loop only after the next configuration
	// began, its silence's completion having come first.
	f.blocks.hold = true;
	auto next = std::make_shared<ThreadedConfig>(f.log);
	f.update("net", next);
	std::thread([&] {
		oldEvents->say("from the old run");
		oldEvents->lost("old run failed");
	}).join();
	f.blocks.complete(0); // the silence
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "run close", "run destroyed",
	                   "prepare 44100/256", "drive code"});
	f.log.clear();

	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	f.blocks.complete(0); // the new drive
	CHECK(f.log == Log{"node net running"});
	f.log.clear();
	f.blocks.hold = false;

	// The new run's own writes land.
	next->events->say("from the new run");
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"note net from the new run"});
	f.stopAll();
	f.settle();
	next->run->closed();
	f.mainLoop.runPosted();
	CHECK(f.allGone == 1);
}

TEST_CASE("Every teardown starts with the barrier, a drive of no code, for a restart and a removal: before release, "
          "before anything is freed and before the block is destroyed (I4)",
          "[node_host][teardown]") {
	Fixture f;
	auto rec = withCards(f.log);
	rec->handoffs = 1;
	f.add("rec", rec);
	f.blocks.events.at(0)->ready();
	f.endTurn();
	f.cards.events.at(0)->ready();
	f.cards.events.at(1)->ready();
	f.settle();
	f.log.clear();

	f.update("rec", withCards(f.log, "B"));
	f.settle();
	f.handoffs.finishOne(0);
	CHECK(f.log.at(0) == "node rec starting");
	CHECK(f.log.at(1) == "fade"); // the barrier comes after the fade of what the code wrote
	CHECK(f.log.at(2) == "drive nothing");
	CHECK(f.log.at(3) == "release");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "card rec.playback destroyed"));
	// The cards go in the turn's flush, after the handoffs were told to close and before they finished (5.5).
	CHECK(indexOf(f.log, "handoff close") < indexOf(f.log, "card rec.playback destroyed"));
	CHECK(indexOf(f.log, "card rec.capture destroyed") < indexOf(f.log, "handoff destroyed"));
	CHECK(indexOf(f.log, "handoff destroyed") < indexOf(f.log, "start 44100 B"));
	f.endTurn(); // B's cards
	f.cards.events.at(2)->ready();
	f.cards.events.at(3)->ready();
	f.settle();
	f.log.clear();

	f.host.nodeRemoved("rec");
	CHECK(f.log.at(0) == "node rec stopping");
	CHECK(f.log.at(1) == "fade");
	f.settle();
	CHECK(f.log.at(2) == "drive nothing");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "block destroyed"));
	CHECK(indexOf(f.log, "card rec.capture destroyed") < indexOf(f.log, "block destroyed"));
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
		return StartResult{std::make_unique<ReadingCode>(log), handOver(log, std::make_unique<TestRun>(log))};
	}
};

} // namespace

TEST_CASE("Step 5 frees the code before the run object it reads, and the config last (I5)", "[node_host][teardown]") {
	Fixture f;
	f.add("net", std::make_shared<ReadingConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();
	f.host.nodeRemoved("net");
	f.settle();
	// The configuration goes in its own input; the block in the turn's flush, after it (5.5).
	CHECK(f.log == Log{"node net stopping", "drive nothing", "code destroyed", "run destroyed", "config destroyed",
	                   "block destroyed", "node net gone dropped=0"});
	f.stopAll();
}

namespace {

/// Recordings that log their open and close; "missing/..." is refused as a folder would refuse it.
struct TestRecordings final : RecordingAdapter {
	Log& log;
	explicit TestRecordings(Log& l) : log(l) {}
	struct File final : RecordingFile {
		Log& log;
		int fd;
		File(Log& l, int d) : log(l), fd(d) {}
		~File() override { log.push_back("recording closed"); }
		int descriptor() const noexcept override { return fd; }
	};
	std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName& node,
	                                                                const std::string& path) override {
		log.push_back("recording " + node + " " + path);
		if (path.starts_with("missing/")) return std::unexpected(std::string("no such folder"));
		return std::make_unique<File>(log, 42);
	}
};

/// Acquires the recording at `path`, then a handoff, as "file" does; fails its start when the recording is refused.
struct RecordingConfig final : NodeConfig {
	Log& log;
	std::string path;
	Ports ports_ = testPorts();
	RecordingConfig(Log& l, std::string p) : log(l), path(std::move(p)) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& c) const override {
		const auto fd = c.acquire(RecordingRequest{path});
		if (!fd) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open: " + fd.error()});
		log.push_back("descriptor " + std::to_string(*fd));
		auto run = std::make_unique<TestRun>(log);
		c.acquire(HandoffRequest{"record", 1024, &run->consumer});
		return StartResult{std::make_unique<TestAudioCode>(log), handOver(log, std::move(run))};
	}
};

struct RecordingFixture {
	Log log;
	QueueExecutor mainLoop;
	TestBlocks blocks{log, mainLoop};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	TestRecordings recordings{log};
	ResourceRegistry resources{cards, receivers, handoffs, recordings};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};
	FakeTimers timers;
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
};

} // namespace

TEST_CASE("A recording is opened at start and closed with the run, after its handoff (I5 step 5)",
          "[node_host][recording]") {
	RecordingFixture f;
	f.host.nodeAdded(NodeSpec{"rec", "test", {}}, std::make_shared<RecordingConfig>(f.log, "take.wav"));
	CHECK(indexOf(f.log, "recording rec take.wav") < indexOf(f.log, "descriptor 42"));
	f.mainLoop.runPosted();
	f.blocks.events.at(0)->ready();
	f.mainLoop.settle();
	f.log.clear();
	f.host.stopAll(f.graph.close(), [] {});
	f.mainLoop.settle();
	f.handoffs.finishOne(0);
	const auto handoffGone = indexOf(f.log, "handoff destroyed");
	const auto runGone = indexOf(f.log, "run destroyed");
	const auto closed = indexOf(f.log, "recording closed");
	REQUIRE(closed < static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(handoffGone < closed);
	CHECK(runGone < closed);
}

TEST_CASE("A recording the folder refuses fails the start, and a registry with no folder refuses every one",
          "[node_host][recording]") {
	RecordingFixture f;
	f.host.nodeAdded(NodeSpec{"rec", "test", {}}, std::make_shared<RecordingConfig>(f.log, "missing/take.wav"));
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::failed);
	CHECK(f.host.health()[0].reason->detail == "cannot open: no such folder");
	CHECK(indexOf(f.log, "handoff rec 2 channels") == static_cast<std::ptrdiff_t>(f.log.size()));
	f.host.stopAll(f.graph.close(), [] {});

	Fixture plain; // its registry was given no recordings
	plain.add("rec", std::make_shared<RecordingConfig>(plain.log, "take.wav"));
	REQUIRE(plain.host.health().size() == 1);
	CHECK(plain.host.health()[0].reason->detail == "cannot open: no recordings folder");
	plain.stopAll();
}

TEST_CASE("A host destroyed with a recording node still running frees its run object and recipe before the recording",
          "[node_host][recording]") {
	for (const bool driven : {true, false}) {
		INFO((driven ? "driven" : "never driven"));
		RecordingFixture f;
		auto host = std::make_unique<NodeHost>(AudioFormat{44100, 256}, f.blocks, f.resources, f.mainLoop, f.timers,
		                                       closeDeadline);
		host->nodeAdded(NodeSpec{"rec", "test", {}}, std::make_shared<RecordingConfig>(f.log, "take.wav"));
		if (driven) {
			f.mainLoop.runPosted();
			f.blocks.events.at(0)->ready();
			f.mainLoop.settle();
		}
		f.log.clear();
		host.reset(); // no stopAll: the configuration goes by its own destruction, as when a test throws or a boot fails
		const Log& log = f.log;
		const auto runGone = indexOf(log, "run destroyed");
		const auto closed = indexOf(log, "recording closed");
		REQUIRE(runGone < static_cast<std::ptrdiff_t>(log.size()));
		REQUIRE(closed < static_cast<std::ptrdiff_t>(log.size()));
		CHECK(runGone < closed);
	}
}

// The per-turn flush of PipeWire's object changes (docs/design/node-lifecycle.md section 5.5, D-L8, D-L39), T1's wait
// for an older block of the name (section 5.7) and MainLoopActivity (section 5.12): step L-3b.

namespace {

/// A config asking for the playback card only, with no recipe (D-L40).
std::shared_ptr<TestConfig> withPlayback(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->cards = {motuOut};
	return config;
}

/// Adds a node and runs it: its block made and ready, its cards ready, its drive completed.
void addRunning(Fixture& f, const NodeName& name, std::shared_ptr<const NodeConfig> config) {
	const std::size_t block = f.blocks.events.size();
	const std::size_t cards = f.cards.events.size();
	f.add(name, std::move(config));
	f.blocks.events.at(block)->ready();
	f.endTurn();
	for (std::size_t i = cards; i < f.cards.events.size(); ++i) f.cards.events[i]->ready();
	f.settle();
	REQUIRE(f.host.health().back().state == NodeState::running);
}

} // namespace

TEST_CASE("PipeWire's object changes of one turn run together in one flush, destroys first: outside nodes, then "
          "blocks, then makes",
          "[node_host][flush]") {
	Fixture f;
	addRunning(f, "a", withPlayback(f.log));
	addRunning(f, "b", std::make_shared<TestConfig>(f.log, "b"));
	f.blocks.hold = true;
	f.log.clear();

	// One turn: c is added first, then a and b are removed and their silences complete.
	f.addInTurn("c", std::make_shared<TestConfig>(f.log, "c"));
	f.host.nodeRemoved("a");
	f.host.nodeRemoved("b");
	f.blocks.complete(0);                                                  // a's fade
	f.blocks.complete(0);                                                  // b's fade
	f.blocks.complete(0);                                                  // a's silence
	f.blocks.complete(0);                                                  // b's silence
	CHECK(std::ranges::count(f.log, std::string("make c 2 inputs")) == 0); // nothing made inside an input
	CHECK(std::ranges::count(f.log, std::string("block destroyed")) == 0);
	CHECK(std::ranges::count(f.log, std::string("card a.playback destroyed")) == 0);
	REQUIRE(f.mainLoop.queue.size() == 1); // one flush posted for the turn
	f.log.clear();

	f.endTurn();
	CHECK(f.log == Log{"card a.playback destroyed", "block destroyed", "node a gone dropped=0", "block destroyed",
	                   "node b gone dropped=0", "make c 2 inputs"});
	CHECK(f.mainLoop.queue.empty());
	f.blocks.hold = false;
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A block whose removal comes in the turn its making was queued is never made", "[node_host][flush]") {
	Fixture f;
	f.addInTurn("x", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("x");            // never driven: its destruction is queued behind its making (D-L7)
	CHECK(f.host.health().size() == 1); // stopping until the flush ran
	f.log.clear();
	f.endTurn();
	CHECK(f.log == Log{"node x gone dropped=0"});
	CHECK(f.blocks.events.empty());
	CHECK(f.host.health().empty());
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("A name added back gets its block only once the older block is destroyed, in the same flush, and its "
          "configuration once the older node is gone",
          "[node_host][flush]") {
	Fixture f;
	auto old = std::make_shared<TestConfig>(f.log, "old");
	old->handoffs = 1; // the old configuration closes only when the test says
	addRunning(f, "rec", old);
	old.reset();
	f.log.clear();

	f.host.nodeRemoved("rec");
	f.addInTurn("rec", std::make_shared<TestConfig>(f.log, "next"));
	CHECK(f.log == Log{"node rec stopping", "fade", "node rec starting"}); // waiting: no block, no start
	f.log.clear();

	f.endTurn(); // the fade completes: the silence is posted
	CHECK(f.log == Log{"drive nothing"});
	f.log.clear();
	f.endTurn(); // the silence completes: the old block's destruction is queued
	CHECK(f.log == Log{"release", "handoff close"});
	f.log.clear();
	f.endTurn(); // one flush: the old block destroyed, then the new one made behind it
	CHECK(f.log == Log{"block destroyed", "make rec 2 inputs"});
	CHECK(f.mainLoop.queue.empty());
	CHECK(f.blocks.activities.back() == "make block rec");
	CHECK(MainLoopActivity::current().empty());
	f.log.clear();

	f.handoffs.finishOne(0); // the old node is gone: the new one configures
	CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config old destroyed", "node rec gone dropped=0",
	                   "start 44100 next", "run built", "prepare 44100/256"});
	f.log.clear();
	f.blocks.events.at(1)->ready();
	f.settle();
	CHECK(f.log == Log{"drive code", "node rec running"});
	CHECK(f.blocks.events.size() == 2);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A name added back three times in one turn has one block at a time, and only the newest configures",
          "[node_host][flush]") {
	Fixture f;
	auto old = std::make_shared<TestConfig>(f.log, "one");
	old->handoffs = 1;
	addRunning(f, "rec", old);
	old.reset();
	f.log.clear();

	f.host.nodeRemoved("rec");
	f.addInTurn("rec", std::make_shared<TestConfig>(f.log, "two"));
	f.host.nodeRemoved("rec");
	f.addInTurn("rec", std::make_shared<TestConfig>(f.log, "three"));
	f.host.nodeRemoved("rec");
	f.addInTurn("rec", std::make_shared<TestConfig>(f.log, "four"));
	// Two and three waited behind the first block and were removed with nothing to destroy.
	CHECK(std::ranges::count(f.log, std::string("node rec gone dropped=0")) == 2);
	REQUIRE(f.host.health().size() == 2);
	CHECK(f.host.health()[0].state == NodeState::stopping);
	CHECK(f.host.health()[1].state == NodeState::starting);
	f.log.clear();

	f.settle();
	CHECK(f.log == Log{"drive nothing", "release", "handoff close", "block destroyed", "make rec 2 inputs"});
	CHECK(f.blocks.events.size() == 2);
	f.log.clear();
	f.handoffs.finishOne(0);
	CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config one destroyed", "node rec gone dropped=0",
	                   "start 44100 four", "run built", "prepare 44100/256"});
	f.blocks.events.at(1)->ready();
	f.settle();
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("An outside node ready told inside its making, during the flush, is handled after that flush item, and a "
          "block a destroy frees is made in the same flush",
          "[node_host][flush]") {
	Fixture f;
	f.cards.readyInMake = true;
	f.add("c", withPlayback(f.log));
	addRunning(f, "a", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("a");
	f.endTurn(); // a's fade completes: its silence is posted
	f.endTurn(); // a's silence completes: its block's destruction is queued
	f.log.clear();

	// The same turn: a added back (its block waits for the old one), and c's block ready (its card's making queued).
	f.addInTurn("a", std::make_shared<TestConfig>(f.log, "back"));
	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty());
	f.log.clear();

	f.endTurn();
	CHECK(f.log == Log{"block destroyed", "node a gone dropped=0", "start 44100 back", "run built", "prepare 44100/256",
	                   "card c.playback of c Pro24 pb.1,pb.2 at 44100", "card c.playback made", "prepare 44100/256",
	                   "drive code", "make a 2 inputs"});
	REQUIRE(f.cards.targets.size() == 1);
	CHECK(f.cards.targets[0].blockId == 0xFFFFFFFFU); // TestBlock has no engine id
	CHECK(f.cards.activities == std::vector<std::string>{"make outside node c"});
	CHECK(f.blocks.activities.back() == "make block a");
	CHECK(MainLoopActivity::current().empty());
	CHECK(f.mainLoop.queue.size() == 1); // c's drive completion; no second flush
	f.settle();
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

namespace {

/// A block whose id is given, as FilterPortBlock reads its engine node's id once STREAMING.
struct IdBlocks final : PortBlockFactory {
	struct Block final : PortBlock {
		std::uint32_t id;
		Executor& mainLoop;
		Block(std::uint32_t i, Executor& m) : id(i), mainLoop(m) {}
		void drive(DriveRequest, std::move_only_function<void(std::uint64_t)> done) override {
			mainLoop.post([done = std::move(done)]() mutable { done(0); });
		}
		void fadeOut(std::move_only_function<void()> done) override { mainLoop.post(std::move(done)); }
		void settle() override {}
		std::uint64_t takenAt() const noexcept override { return 0; }
		std::uint32_t blockId() const noexcept override { return id; }
	};
	Executor& mainLoop;
	std::uint32_t next = 40;
	std::vector<PortBlockEvents*> events;
	explicit IdBlocks(Executor& m) : mainLoop(m) {}
	std::unique_ptr<PortBlock> make(const NodeName&, const Ports&, const AudioFormat&, PortBlockEvents& e) override {
		events.push_back(&e);
		return std::make_unique<Block>(next++, mainLoop);
	}
};

/// Records what each hook call sees in MainLoopActivity.
struct ActivityConfig final : NodeConfig {
	Log& log;
	Ports ports_ = testPorts();
	explicit ActivityConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override {
		log.push_back("start in " + MainLoopActivity::current());
		return StartResult{std::make_unique<TestAudioCode>(log),
		                   [this](Ready&) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
							   log.push_back("recipe in " + MainLoopActivity::current());
							   return std::make_unique<TestRun>(log);
						   }};
	}
};

} // namespace

TEST_CASE("An outside node is made with its block's id, read once the block is ready (5.7)", "[node_host][flush]") {
	Log log;
	QueueExecutor mainLoop;
	IdBlocks blocks{mainLoop};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	ResourceRegistry resources{cards, receivers, handoffs};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};
	FakeTimers timers;
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	host.nodeAdded(NodeSpec{"motu", "test", {}}, withCards(log));
	host.nodeAdded(NodeSpec{"fpp", "test", {}}, withReceiver(log));
	mainLoop.runPosted();
	REQUIRE(blocks.events.size() == 2);
	blocks.events[1]->ready();
	blocks.events[0]->ready();
	mainLoop.runPosted();
	REQUIRE(cards.targets.size() == 2);
	CHECK(cards.targets[0].blockId == 40);
	CHECK(cards.targets[1].blockId == 40);
	REQUIRE(receivers.targets.size() == 1);
	CHECK(receivers.targets[0].blockId == 41);
	CHECK(receivers.targets[0].blockNode == "fpp");
	host.stopAll(graph.close(), [] {});
	mainLoop.settle();
}

TEST_CASE("MainLoopActivity names each type hook while it runs, and is empty between inputs", "[node_host][flush]") {
	Fixture f;
	f.add("net", std::make_shared<ActivityConfig>(f.log));
	CHECK(f.log.at(1) == "start in configure net");
	CHECK(f.log.at(2) == "recipe in recipe net");
	CHECK(f.blocks.activities == std::vector<std::string>{"make block net"});
	CHECK(MainLoopActivity::current().empty());
	f.stopAll();
	f.settle();
	CHECK(MainLoopActivity::current().empty());
}

// Fades, bounds and the wedged data loop (docs/design/node-lifecycle.md sections 4.2, 5.3 and 5.4; step L-3c).

namespace {

/// The calls into PipeWire a fake block or adapter logs: none may follow a wedge.
bool isEngineCall(const std::string& line) {
	return line.starts_with("make ") || line == "fade" || line.starts_with("drive ") || line == "block destroyed" ||
	       line.starts_with("card ") || line.starts_with("receiver ");
}

std::size_t engineCalls(const Log& log) { return static_cast<std::size_t>(std::ranges::count_if(log, isEngineCall)); }

/// Routable output bank "out" (two) and the inside output bank "pb" (two), as a card's.
struct OutputConfig final : NodeConfig {
	Log& log;
	Ports ports_{{{Direction::input, "in", {"1"}},
	              {Direction::output, "out", {"left", "right"}},
	              {Direction::output, "pb", {"1", "2"}, PortBank::Reach::inside}}};
	explicit OutputConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override { return std::make_unique<TestAudioCode>(log); }
};

} // namespace

TEST_CASE("Every output the code writes is faded in at its drive and out before a silence, and a node with no output "
          "is silenced unfaded",
          "[node_host][fade]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	auto rec = std::make_shared<ThreadedConfig>(f.log); // inputs only
	rec->closeAtOnce = true;
	addRunning(f, "rec", rec);
	// Public outputs, then inside outputs (process()'s order): all four faded; the input-only node has none.
	REQUIRE(f.blocks.modes.faded.size() == 2);
	CHECK(f.blocks.modes.faded[0] == std::vector<bool>{true, true, true, true});
	CHECK(f.blocks.modes.faded[1].empty());
	f.log.clear();

	f.update("amp", std::make_shared<OutputConfig>(f.log));
	CHECK(f.log == Log{"node amp starting", "fade"}); // the barrier waits for the fade's acknowledgement
	f.log.clear();
	f.settle();
	CHECK(f.log.at(0) == "drive nothing");
	CHECK(f.log.back() == "node amp running");
	CHECK(f.blocks.modes.faded.back() == std::vector<bool>{true, true, true, true}); // the restart fades in again
	f.log.clear();

	f.host.nodeRemoved("rec");
	CHECK(f.log == Log{"node rec stopping", "drive nothing"}); // nothing to fade: the silence at once
	f.settle();
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
	CHECK(f.diagnostics.details.empty());
}

TEST_CASE("A fade unacknowledged for 100 ms is silenced unfaded, with ear.fade-unacknowledged", "[node_host][fade]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.hold = true;
	f.log.clear();

	f.host.nodeRemoved("amp");
	REQUIRE(f.blocks.held.size() == 1); // the fade, never acknowledged (its block is STREAMING and silent)
	f.timers.advance(std::chrono::milliseconds(99));
	CHECK(f.log == Log{"node amp stopping", "fade"});
	f.timers.advance(std::chrono::milliseconds(1));
	CHECK(f.log == Log{"node amp stopping", "fade", "diagnostic amp ear.fade-unacknowledged ms=100", "drive nothing"});
	CHECK(f.blocks.modes.settles == 1); // settled before it judged
	CHECK(f.diagnostics.details.back() ==
	      "the fade before its silence was not acknowledged within 100 ms, so the silence was taken unfaded");
	f.log.clear();

	f.blocks.complete(0); // a late acknowledgement is stale: dropped
	CHECK(f.log.empty());
	f.blocks.complete(0); // the silence
	f.endTurn();
	CHECK(f.log == Log{"release", "block destroyed", "node amp gone dropped=0"});
	CHECK(f.host.leaked() == 0);
	CHECK(f.timers.live() == 0);
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("A bound that fires before its completion was dispatched completes it from settle() and judges nothing",
          "[node_host][fade]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.modes.stall = true; // the data loop completes everything; the main loop stalls before its events
	f.log.clear();

	f.host.nodeRemoved("amp");
	// The main loop stalls 6 s: the fade's timer fires first, settles the block and hears the fade done.
	f.timers.advance(std::chrono::milliseconds(6'000));
	CHECK(f.log == Log{"node amp stopping", "fade", "drive nothing"});
	// It stalls again past the barrier's bound: the silence completed meanwhile, so nothing is judged.
	f.timers.advance(std::chrono::milliseconds(6'000));
	f.endTurn();
	CHECK(f.log ==
	      Log{"node amp stopping", "fade", "drive nothing", "release", "block destroyed", "node amp gone dropped=0"});
	CHECK(f.diagnostics.details.empty()); // no fade unacknowledged, no barrier overdue, no wedge
	CHECK(f.host.leaked() == 0);

	// The data loop is not wedged: a node added afterwards gets its block.
	f.blocks.modes.stall = false;
	f.add("next", std::make_shared<OutputConfig>(f.log));
	CHECK(f.log.back() == "make next 1 inputs");
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A block that leaves STREAMING with a drive or a fade in flight still completes it, and the loop is not "
          "judged wedged",
          "[node_host][fade]") {
	Fixture f;
	f.blocks.hold = true;
	f.add("amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.events.at(0)->ready();
	REQUIRE(f.blocks.held.size() == 1); // the drive, in flight as the block goes PAUSED
	f.timers.advance(std::chrono::milliseconds(4'000));
	f.blocks.complete(0); // completed by signal, applied in its invoke, before the bound
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.host.nodeRemoved("amp");
	REQUIRE(f.blocks.held.size() == 1); // the fade, acknowledged by signal on a block no longer processing
	f.timers.advance(std::chrono::milliseconds(50));
	f.blocks.complete(0);
	f.timers.advance(std::chrono::milliseconds(4'900));
	f.blocks.complete(0); // the silence, inside its bound
	f.timers.advance(std::chrono::milliseconds(10'000));
	f.endTurn();
	CHECK(f.diagnostics.details.empty());
	CHECK(f.host.health().empty());
	CHECK(f.host.leaked() == 0);
	f.blocks.hold = false;
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("A barrier overdue for 5 s marks the data loop wedged: every later teardown is leaked and no PipeWire call "
          "is made",
          "[node_host][wedge]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	addRunning(f, "desk", withPlayback(f.log));
	addRunning(f, "rec", std::make_shared<ThreadedConfig>(f.log));
	f.blocks.hold = true; // the data loop runs no invoke from here on
	f.log.clear();

	f.host.nodeRemoved("amp");
	f.timers.advance(std::chrono::milliseconds(100)); // the fade unacknowledged: the silence posted unfaded
	f.timers.advance(std::chrono::milliseconds(4'999));
	CHECK(std::ranges::count(f.log, std::string("engine diagnostic ear.data-loop-wedged ms=5000")) == 0);
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(1));
	// The barrier overdue names the node, the wedge is said once with no node, then the removal goes on, leaking.
	CHECK(f.log == Log{"diagnostic amp ear.barrier-overdue ms=5000", "engine diagnostic ear.data-loop-wedged ms=5000"});
	CHECK(f.diagnostics.details.back() == "node amp's barrier did not complete within 5000 ms: the data loop runs no "
	                                      "invoke, so no further engine change is applied");
	f.log.clear();
	f.endTurn(); // the flush: amp's block is leaked, not destroyed, and amp is gone
	CHECK(f.log == Log{"node amp gone dropped=0"});
	CHECK(f.host.leaked() == 2); // its configuration and its block
	f.log.clear();

	SECTION("a running node goes on running, and its removal leaks it with no fade, silence or destroy") {
		REQUIRE(f.host.health().size() == 2);
		CHECK(f.host.health()[0].state == NodeState::running);
		f.host.nodeRemoved("desk");
		f.host.nodeRemoved("rec");
		f.endTurn();
		CHECK(engineCalls(f.log) == 0);
		CHECK(f.host.health().empty());
		CHECK(std::ranges::count(f.log, std::string("node desk gone dropped=0")) == 1);
		CHECK(std::ranges::count(f.log, std::string("node rec gone dropped=0")) == 1);
		CHECK(f.host.leaked() ==
		      6); // and desk's configuration (its card with it) and block; rec's configuration and block
		CHECK(f.diagnostics.details.size() == 3); // nothing more said: fade, barrier, wedge
	}
	SECTION("a restart fails engine-refused: nothing is configured on a wedged loop") {
		f.update("desk", withPlayback(f.log, "B"));
		f.endTurn();
		CHECK(engineCalls(f.log) == 0);
		CHECK(f.log.back() == "node desk failed engine-refused: the data loop is wedged");
		CHECK(std::ranges::count(f.log, std::string("start 44100 B")) == 0);
	}
	SECTION("a drive whose invoke never runs is a barrier overdue too") {
		Fixture g;
		g.blocks.hold = true;
		g.add("amp", std::make_shared<OutputConfig>(g.log));
		g.blocks.events.at(0)->ready(); // the drive posted, never completed
		g.log.clear();
		g.timers.advance(std::chrono::milliseconds(5'000));
		CHECK(g.log == Log{"diagnostic amp ear.barrier-overdue ms=5000",
		                   "engine diagnostic ear.data-loop-wedged ms=5000",
		                   "node amp failed engine-refused: the data loop is wedged"});
		g.stopAll();
		g.endTurn();
		CHECK(g.allGone == 1);
		CHECK(engineCalls(g.log) == 0);
	}
	f.stopAll();
	f.endTurn();
	CHECK(f.allGone == 1);
	CHECK(engineCalls(f.log) == 0);
}

TEST_CASE("A node added after the wedge fails engine-refused without a block", "[node_host][wedge]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.hold = true;
	f.host.nodeRemoved("amp");
	f.timers.advance(std::chrono::milliseconds(100));
	f.timers.advance(std::chrono::milliseconds(5'000));
	f.endTurn();
	f.log.clear();

	f.add("late", std::make_shared<OutputConfig>(f.log));
	CHECK(f.log == Log{"node late failed engine-refused: the data loop is wedged"}); // no start, no block
	CHECK(f.blocks.events.size() == 1);
	f.host.nodeRemoved("late");
	f.endTurn();
	CHECK(f.log.back() == "node late gone dropped=0");
	CHECK(engineCalls(f.log) == 0);
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("stopAll after the wedge reaches allGone at once and makes no PipeWire call", "[node_host][wedge]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log); // with handoffs: a legacy teardown closes them on their threads
	rec->handoffs = 1;
	addRunning(f, "rec", rec);
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.add("fpp", withReceiver(f.log));
	f.blocks.events.back()->ready();
	f.endTurn();
	f.receivers.events.back()->ready();
	f.settle();
	REQUIRE(f.host.health().back().state == NodeState::running);
	f.blocks.hold = true;
	f.update("amp", std::make_shared<OutputConfig>(f.log)); // a restart's fade in flight when the loop wedges
	f.host.nodeRemoved("fpp");
	f.timers.advance(std::chrono::milliseconds(100));
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(5'000)); // fpp's silence overdue: wedged
	REQUIRE(std::ranges::count(f.log, std::string("engine diagnostic ear.data-loop-wedged ms=5000")) == 1);
	f.log.clear();

	f.stopAll();
	f.endTurn();
	CHECK(f.allGone == 1);
	CHECK(engineCalls(f.log) == 0);
	CHECK(f.host.health().empty());
	CHECK(f.handoffs.pendingCloses.empty()); // a leaked configuration's handoff is never closed: its writer may run
	CHECK(f.host.leaked() > 0);              // main.cpp then leaves through std::_Exit
}

TEST_CASE("Stopping 16 nodes posts every fade at once", "[node_host][fade]") {
	Fixture f;
	for (int i = 0; i < 16; ++i) addRunning(f, "n" + std::to_string(i), std::make_shared<OutputConfig>(f.log));
	f.log.clear();
	f.stopAll();
	CHECK(std::ranges::count(f.log, std::string("fade")) == 16);
	CHECK(std::ranges::count(f.log, std::string("drive nothing")) == 0);
	f.endTurn(); // one turn: every acknowledgement, then every silence
	CHECK(std::ranges::count(f.log, std::string("drive nothing")) == 16);
	f.settle();
	CHECK(f.allGone == 1);
	CHECK(std::ranges::count(f.log, std::string("block destroyed")) == 16);
}

TEST_CASE("A factory or an adapter that throws inside the flush fails its node and strands nothing",
          "[node_host][flush]") {
	SECTION("the port block's factory") {
		Fixture f;
		f.blocks.throwInMake = true;
		f.addInTurn("amp", std::make_shared<OutputConfig>(f.log));
		CHECK_THROWS_WITH(f.endTurn(), "no memory for amp");
		REQUIRE(f.host.health().size() == 1);
		CHECK(f.host.health()[0].state == NodeState::failed);
		REQUIRE(f.host.health()[0].reason.has_value());
		CHECK(f.host.health()[0].reason->detail == "making the port block threw: no memory for amp");
		CHECK(MainLoopActivity::current().empty());
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("an outside node's adapter") {
		Fixture f;
		f.cards.throwInMake = true;
		f.add("desk", withPlayback(f.log));
		f.blocks.events.at(0)->ready();
		CHECK_THROWS_WITH(f.endTurn(), "no card Pro24");
		REQUIRE(f.host.health().size() == 1);
		CHECK(f.host.health()[0].state == NodeState::failed);
		REQUIRE(f.host.health()[0].reason.has_value());
		CHECK(f.host.health()[0].reason->detail == "making desk.playback threw: no card Pro24");
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}
