// The node framework driven through fake engine adapters: how a node starts, what makes it running or failed, and the
// order a configuration is torn down in (docs/architecture.md#the-node-lifecycle): the port block made first; restart,
// sound cards, a lost block, removal; RTP receivers on the same engine-companion path; the resource registry, named
// inside banks, the framework's pass-through, the two-phase start, and teardown by dependency with its close deadline
// (docs/architecture.md#teardown-by-dependency). NodeHost runs on the lifecycle machines: every drive and silence is
// asynchronous, its completion posted to the main loop by the fake block (or held for the test to run), a config with
// no engine companion goes through the legacy adapter and one with them through a class's configure, and a failed
// node's configuration is torn down at once.
// Every step a fake engine would take later (ready, lost, a completion, a handoff closing, a card or receiver ready or
// lost, a close deadline passing) is an event the test sends.
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/source_ring.hpp"

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

/// A handoff's consumer that counts what reaches it, and throws in consume when told to, as one with a bug.
struct TestConsumer final : HandoffConsumer {
	bool throwInConsume = false;
	int consumed = 0;
	int finished = 0;
	void consume(std::span<const float>, std::uint32_t) override {
		++consumed;
		if (throwInConsume) throw std::runtime_error("disk full");
	}
	void finish() override { ++finished; }
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

/// A sound card a TestConfig asks for: the inside bank it joins (its direction is the card's) and its shape.
struct CardAsk {
	std::string bank;
	SoundCardShape shape;
};
/// An RTP receiver a TestConfig asks for, likewise.
struct ReceiverAsk {
	std::string bank;
	RtpReceiverShape shape;
};

/// A node config whose start asks for `handoffs` handoffs, then succeeds or fails. A config with a tag logs its start
/// and its destruction with that tag. Its start returns a recipe handing over a TestRun only when it asks for no engine
/// companion, unless `recipe` says otherwise; its handoffs' consumer is the config's own, which outlives every
/// configuration made from it. With `cards` or `receivers` it is a class (CompanionInstance below): its configure runs
/// the same start, then asks for the cards, then the receivers (CreateContext::soundCard, rtpReceiver), as a type's
/// class does; a legacy start has no engine companions (docs/writing-a-node-type.md#the-old-interface).
struct TestConfig final : NodeConfig {
	Log& log;
	std::string tag;
	Ports ports_ = testPorts();
	int handoffs = 0;
	std::vector<CardAsk> cards;
	std::vector<ReceiverAsk> receivers;
	bool fail = false;
	std::optional<bool> recipe;              // default: a recipe when no engine companion is asked for
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
		if (fail) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open /nope"});
		if (!recipe.value_or(cards.empty() && receivers.empty())) return std::make_unique<TestAudioCode>(log);
		return StartResult{std::make_unique<TestAudioCode>(log), handOver(log, std::make_unique<TestRun>(log))};
	}
	std::unique_ptr<ErasedInstance> make(NodeContext& node) const override;
};

/// A TestConfig with engine companions, as a class: its configure is the config's start (its handoffs, its failure,
/// its code), then a card per CardAsk and a receiver per ReceiverAsk, in that order.
struct CompanionInstance final : ErasedInstance {
	const TestConfig& config;
	explicit CompanionInstance(const TestConfig& c) : config(c) {}
	Configured configure(InstanceContexts& cx) override {
		StartResult started = config.start(cx.legacy);
		if (started.failed()) return Configured::failed(std::move(started.failure().detail));
		for (const auto& card : config.cards) cx.create.soundCard(card.bank, card.shape);
		for (const auto& receiver : config.receivers) cx.create.rtpReceiver(receiver.bank, receiver.shape);
		cx.create.use(std::move(started.code()));
		return Configured::applied();
	}
};

std::unique_ptr<ErasedInstance> TestConfig::make(NodeContext& node) const {
	if (cards.empty() && receivers.empty()) return NodeConfig::make(node);
	return std::make_unique<CompanionInstance>(*this);
}

const CardAsk motuOut{"pb", SoundCardShape{*CardId::parse("Pro24")}};
const CardAsk motuIn{"cap", SoundCardShape{*CardId::parse("Pro24")}};

std::shared_ptr<TestConfig> withCards(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->cards = {motuOut, motuIn};
	return config;
}

const RtpReceiverShape fppStream{*RtpAddress::parse("239.69.0.1"), 5004, RtpEncoding::l24, 44100, 100};

std::shared_ptr<TestConfig> withReceiver(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->receivers = {{"cap", fppStream}};
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
/// applied with their event not yet dispatched, so only settle() runs them (a main loop that stalled).
struct BlockModes {
	bool hold = false;         // completions wait in TestBlocks::held until the test runs them
	bool stall = false;        // completions wait in the block until its settle()
	bool throwInCarry = false; // carryLatency throws, as a block that disagrees with NodeHost's plan check
	std::size_t settles = 0;
	std::vector<std::vector<bool>> faded;            // each drive's faded flags, in order
	std::vector<std::vector<SinkBinding>> sinks;     // each drive's taps (a silence's none), in order
	std::vector<std::vector<SourceBinding>> sources; // each drive's sources (a silence's none), in order
};

/// A block as docs/engine.md#drive-and-fade has it: a drive is applied at once (the code it runs is recorded) and its
/// `done` runs later on the main loop, posted there, never inside the call; or, when held, when the test says; or,
/// stalled, when settle() is called.
struct TestBlock final : PortBlock {
	Log& log;
	AudioCode** driven;
	std::vector<LatencyPlan>* plans;
	Executor& mainLoop;
	BlockModes& modes;
	Held& held;
	Held applied;       // completed, their event not yet dispatched
	NodeCounts counted; // what counts() answers: the test sets it, as a real block's cycles would
	TestBlock(Log& l, AudioCode** d, std::vector<LatencyPlan>* p, Executor& m, BlockModes& b, Held& hd)
		: log(l), driven(d), plans(p), mainLoop(m), modes(b), held(hd) {}
	~TestBlock() override { log.push_back("block destroyed"); }
	void carryLatency(const LatencyPlan& plan) override {
		if (modes.throwInCarry) throw std::logic_error("a plan the block cannot carry");
		plans->push_back(plan);
	}
	void drive(DriveRequest request, std::move_only_function<void(std::uint64_t)> done) override {
		*driven = request.code;
		log.push_back(request.code != nullptr ? "drive code" : "drive nothing");
		if (request.code != nullptr) modes.faded.emplace_back(request.faded.begin(), request.faded.end());
		modes.sinks.emplace_back(request.sinks.begin(), request.sinks.end());
		modes.sources.emplace_back(request.sources.begin(), request.sources.end());
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
	NodeCounts counts() const noexcept override { return counted; }

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
	bool breakRuleInMake = false;        // make() throws std::logic_error, as ear's own broken rule
	std::vector<TestBlock*> made;        // every block made, in make order (a destroyed one dangles: unused)
	TestBlocks(Log& l, Executor& m) : log(l), mainLoop(m) {}
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports, const AudioFormat&,
	                                PortBlockEvents& e) override {
		if (throwInMake) throw std::runtime_error("no memory for " + node);
		if (breakRuleInMake) throw std::logic_error("a block made twice for " + node);
		log.push_back("make " + node + " " + std::to_string(ports.inputs().size()) + " inputs");
		activities.push_back(MainLoopActivity::current());
		events.push_back(&e);
		auto block = std::make_unique<TestBlock>(log, &driven.emplace_back(nullptr), &plans.emplace_back(), mainLoop,
		                                         modes, held);
		made.push_back(block.get());
		return block;
	}
	/// The held completion `index` was completed by the data loop, its event not yet dispatched: block `block`'s
	/// settle() runs it.
	void applyHeld(std::size_t index, std::size_t block) {
		auto done = std::move(held.at(index));
		held.erase(held.begin() + static_cast<std::ptrdiff_t>(index));
		made.at(block)->applied.push_back(std::move(done));
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
	std::vector<HandoffConsumer*> consumers; // what each handoff made calls, in make order, as its thread would
	bool throwInMake = false; // make() throws std::system_error, as an adapter that cannot start a thread
	bool nullInMake = false;  // make() makes nothing, as an adapter with a bug
	explicit TestHandoffs(Log& l) : log(l) {}
	std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels, std::uint32_t,
	                              HandoffConsumer& consumer) override {
		if (throwInMake) throw std::system_error(EAGAIN, std::generic_category(), "no thread for " + node);
		if (nullInMake) return nullptr;
		consumers.push_back(&consumer);
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

/// A sink's consumer that logs its calls; one given an InsidesEvents says lost in begin, as a WAV it cannot open; one
/// given a call's name throws in that call, as one with a bug.
struct TestSinkConsumer final : SinkConsumer {
	Log& log;
	InsidesEvents* loseInBegin = nullptr;
	std::string throwIn;
	explicit TestSinkConsumer(Log& l, InsidesEvents* lose = nullptr, std::string t = {})
		: log(l), loseInBegin(lose), throwIn(std::move(t)) {}
	~TestSinkConsumer() override { log.push_back("consumer destroyed"); }
	void begin() override {
		log.push_back("consumer begin");
		if (loseInBegin != nullptr) loseInBegin->lost("cannot open take.wav");
		if (throwIn == "begin") throw std::runtime_error("no such folder");
	}
	void consume(const Chunk& chunk) override {
		log.push_back("consume " + std::to_string(chunk.frames));
		if (throwIn == "consume") throw std::runtime_error("disk full");
	}
	void finish() override {
		log.push_back("consumer finish");
		if (throwIn == "finish") throw std::runtime_error("cannot write the header");
	}
};

/// Sinks as docs/engine.md#sinks has them, on the test's thread: begin runs the consumer's begin and posts `begun`
/// (or, held, waits for the test); close finishes a consumer that began and posts `closed` (or, held, waits).
struct TestSink final : Sink, SinkTap {
	Log& log;
	Executor& mainLoop;
	std::string name;
	std::uint64_t dropped;
	bool holdBegin;
	bool holdClose;
	std::unique_ptr<SinkConsumer> consumer;
	std::move_only_function<void()> begun;
	std::move_only_function<void(std::uint64_t)> closed;
	bool began = false;
	TestSink(Log& l, Executor& m, std::string n, std::uint64_t d, bool hb, bool hc)
		: log(l), mainLoop(m), name(std::move(n)), dropped(d), holdBegin(hb), holdClose(hc) {}
	~TestSink() override { log.push_back("sink " + name + " destroyed"); }
	SinkTap& tap() noexcept override { return *this; }
	void begin(std::unique_ptr<SinkConsumer> c, std::move_only_function<void()> b) override {
		log.push_back("sink " + name + " begin");
		consumer = std::move(c);
		begun = std::move(b);
		if (!holdBegin) runBegin();
	}
	/// The sink's thread runs the consumer's begin, then posts `begun`.
	void runBegin() {
		consumer->begin();
		began = true;
		mainLoop.post(std::exchange(begun, nullptr));
	}
	void close(std::move_only_function<void(std::uint64_t)> c) override {
		log.push_back("sink " + name + " close");
		closed = std::move(c);
		if (!holdClose) runClose();
	}
	/// The sink's thread hands its consumer one chunk of `frames` silent frames.
	void feed(std::uint32_t frames) {
		const std::vector<float> interleaved(static_cast<std::size_t>(frames) * 2, 0.0F);
		consumer->consume(Chunk{.interleaved = interleaved,
		                        .frames = frames,
		                        .position = 0,
		                        .startNs = 0,
		                        .startRawNs = 0,
		                        .upstreamFrames = 0,
		                        .causes = {},
		                        .lostFrames = 0});
	}
	void runClose() {
		if (began) consumer->finish();
		mainLoop.post([closed = std::exchange(closed, nullptr), dropped = dropped]() mutable { closed(dropped); });
	}
	std::uint64_t droppedFrames() const noexcept override { return dropped; }
	void write(std::span<const std::span<const float>>, std::uint32_t, const ChunkStamp&) noexcept override {}
};

struct TestSinks final : SinkAdapter {
	Log& log;
	Executor& mainLoop;
	bool throwInMake = false;     // make() throws, as an adapter that cannot start a thread
	bool breakRuleInMake = false; // make() throws std::logic_error, as an adapter with a bug of its own
	bool holdBegin = false;       // a consumer begins only when the test says (TestSink::runBegin)
	bool holdClose = false;       // a close reports only when the test says (TestSink::runClose)
	std::uint64_t dropped = 0;    // what each sink reports refused at its close
	std::vector<TestSink*> made;  // every sink made, in order (a destroyed one dangles: unused)
	TestSinks(Log& l, Executor& m) : log(l), mainLoop(m) {}
	std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                           std::uint32_t capacityFrames, std::uint32_t /*rate*/) override {
		if (throwInMake) throw std::system_error(EAGAIN, std::generic_category(), "no thread for " + node);
		if (breakRuleInMake) throw std::logic_error("the sink adapter broke its own rule for " + node);
		const std::string name = node + "." + std::string(bank);
		log.push_back("sink " + name + " " + std::to_string(channels) + " channels " + std::to_string(capacityFrames) +
		              " frames");
		auto sink = std::make_unique<TestSink>(log, mainLoop, name, dropped, holdBegin, holdClose);
		made.push_back(sink.get());
		return sink;
	}
};

/// Sources over core's SourceRing (docs/writing-a-node-type.md#sources), logging what NodeHost does to them: stopped,
/// closed, destroyed. The writer never waits: it offers what fits.
struct TestSource final : Source, SourceWriter, SourceRing::Signals {
	Log& log;
	std::string name;
	SourceRing ring;
	TestSource(Log& l, std::string n, std::uint32_t channels, std::uint32_t frames, std::uint32_t rate)
		: log(l), name(std::move(n)), ring(channels, frames, rate, *this) {}
	~TestSource() override { log.push_back("source " + name + " destroyed"); }
	SourceWriter& writer() noexcept override { return *this; }
	SourceTap& tap() noexcept override { return ring; }
	void stopped() noexcept override {
		log.push_back("source " + name + " stopped");
		ring.dropForStop();
	}
	void stopping() noexcept override {
		log.push_back("source " + name + " stopping");
		ring.markIdle();
	}
	void close() noexcept override {
		log.push_back("source " + name + " close");
		ring.close();
	}
	std::uint64_t shortfallFrames() const noexcept override { return ring.shortfallFrames(); }
	std::uint64_t droppedFrames() const noexcept override { return ring.droppedFrames(); }
	std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds) override {
		return ring.offer(interleaved, frames);
	}
	void idle(std::chrono::milliseconds) override { ring.markIdle(); }
	void clear() noexcept override { ring.clear(); }
	bool closed() const noexcept override { return ring.closed(); }
	std::uint32_t capacityFrames() const noexcept override { return ring.capacityFrames(); }
	Played played() const noexcept override { return ring.played(); }
	Wake& playedWake() noexcept override { std::terminate(); } // no test watches it here
	void roomFreed() noexcept override {}
	void playedChanged() noexcept override {}
};

struct TestSources final : SourceAdapter {
	Log& log;
	bool throwInMake = false;      // make() throws, as an adapter that cannot make an eventfd
	std::vector<TestSource*> made; // every source made, in order (a destroyed one dangles: unused)
	explicit TestSources(Log& l) : log(l) {}
	std::unique_ptr<Source> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                             std::uint32_t capacityFrames, std::uint32_t rate) override {
		if (throwInMake) throw std::system_error(EMFILE, std::generic_category(), "no eventfd for " + node);
		const std::string name = node + "." + std::string(bank);
		log.push_back("source " + name + " " + std::to_string(channels) + " channels " +
		              std::to_string(capacityFrames) + " frames at " + std::to_string(rate));
		auto source = std::make_unique<TestSource>(log, name, channels, capacityFrames, rate);
		made.push_back(source.get());
		return source;
	}
};

struct TestCompanion final : EngineCompanion {
	Log& log;
	std::string what;
	TestCompanion(Log& l, std::string w) : log(l), what(std::move(w)) {}
	~TestCompanion() override { log.push_back(what + " destroyed"); }
};

std::string joined(const std::vector<PortName>& ports) {
	std::string all;
	for (const auto& port : ports) all += (all.empty() ? "" : ",") + port;
	return all;
}

/// Cards whose readiness and loss the test tells, as a real card's PCM and links report later.
struct TestCards final : SoundCardCompanions {
	Log& log;
	std::vector<EngineCompanionEvents*> events;
	bool readyInMake = false;            // tells ready() inside make(), as a card already running might
	bool throwInMake = false;            // make() throws, as an adapter with a bug
	std::vector<std::string> activities; // MainLoopActivity::current() inside each make
	std::vector<CompanionTarget> targets;
	explicit TestCards(Log& l) : log(l) {}
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const SoundCardShape& shape,
	                                      EngineCompanionEvents& e) override {
		if (throwInMake) throw std::runtime_error("no card " + shape.card.text());
		activities.push_back(MainLoopActivity::current());
		targets.push_back(target);
		log.push_back("card " + target.engineNode + " of " + target.blockNode + " " + shape.card.text() + " " +
		              joined(target.blockPorts) + " at " + std::to_string(target.format.rate));
		events.push_back(&e);
		if (readyInMake) {
			e.ready(); // heard once make() returned
			log.push_back("card " + target.engineNode + " made");
		}
		return std::make_unique<TestCompanion>(log, "card " + target.engineNode);
	}
};

/// RTP receivers whose readiness and loss the test tells; each shape and target is kept as it was given.
struct TestReceivers final : RtpReceiverCompanions {
	Log& log;
	std::vector<EngineCompanionEvents*> events;
	std::vector<RtpReceiverShape> shapes;
	std::vector<CompanionTarget> targets;
	explicit TestReceivers(Log& l) : log(l) {}
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const RtpReceiverShape& shape,
	                                      EngineCompanionEvents& e) override {
		log.push_back("receiver " + target.engineNode + " of " + target.blockNode);
		events.push_back(&e);
		shapes.push_back(shape);
		targets.push_back(target);
		return std::make_unique<TestCompanion>(log, "receiver " + target.engineNode);
	}
};

/// The node's state (the phase's colour) and failure, logged when either changes, as standard output's records are
/// written (docs/output-records.md#node); every health told is kept whole in `told`.
struct TestListener final : NodeHealthListener {
	Log& log;
	std::vector<NodeHealth> told;
	std::map<NodeName, std::string> last;
	explicit TestListener(Log& l) : log(l) {}
	void nodeChanged(const NodeHealth& health) override {
		told.push_back(health);
		std::string line = "node " + health.name + " " + std::string(stateName(health.state));
		if (health.reason) line += " " + std::string(failureCode(health.reason->code)) + ": " + health.reason->detail;
		if (std::exchange(last[health.name], line) != line) log.push_back(line);
	}
	void nodeGone(const NodeName& node, std::uint64_t dropped) override {
		log.push_back("node " + node + " gone dropped=" + std::to_string(dropped));
	}
	/// Every phase told for `node`, with its cause, in order: "added", "stopping (restarting settings)".
	std::vector<std::string> phases(const NodeName& node) const {
		std::vector<std::string> out;
		for (const NodeHealth& health : told) {
			if (health.name != node) continue;
			std::string line(phaseName(health.phase));
			if (health.cause) {
				line += " (" + std::string(causeName(health.cause->kind));
				if (health.cause->restart) line += " " + std::string(causeName(*health.cause->restart));
				line += ")";
			}
			out.push_back(line);
		}
		return out;
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

/// Every ear.internal-error NodeHost tells (docs/architecture.md#faults-by-owner), as "<callback>|<node>|<where>|<what>".
struct TestInternalErrors final : InternalErrorListener {
	std::vector<std::string> told;
	void internalError(const InternalError& error) override {
		told.push_back(std::string(error.callback) + "|" + std::string(error.node) + "|" + std::string(error.where) +
		               "|" + std::string(error.what));
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
	TestSinks sinks{log, mainLoop};
	TestSources sources{log};
	ResourceRegistry resources{cards, receivers, handoffs, sinks, sources};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue}; // only for close(), the proof stopAll takes
	FakeTimers timers;
	TestNotes notes{log};
	TestDiagnostics diagnostics{log};
	TestInternalErrors internalErrors;
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	int allGone = 0; // how often stopAll's callback has run
	Fixture() {
		host.subscribe(listener);
		host.subscribeNotes(notes);
		host.subscribeDiagnostics(diagnostics);
		host.subscribeEngineDiagnostics(diagnostics);
		host.subscribeInternalErrors(internalErrors);
	}
	/// The node added, then the turn ends: what it posted runs (its flush makes its block).
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
	/// The turn ends: what was posted so far runs (the turn's flush among it), and nothing it posts.
	void endTurn() { mainLoop.runPosted(); }
};

std::ptrdiff_t indexOf(const Log& log, const std::string& line) {
	return std::find(log.begin(), log.end(), line) - log.begin();
}

/// The failure a node's configure gave, "<code>: <detail>", or "" while it has none.
std::string failureOf(const Fixture& f, const NodeName& node) {
	for (const NodeHealth& health : f.host.health())
		if (health.name == node && health.reason)
			return std::string(failureCode(health.reason->code)) + ": " + health.reason->detail;
	return {};
}

} // namespace

TEST_CASE("A node gets its port block, then starts, builds its run and prepares its code, and drives the block once it "
          "is ready",
          "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;

	f.add("rec", rec);

	// T2, then T3 in the same turn (no engine companion); the block is made in the turn's flush: nothing drives a
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

	// Nothing was driven, so the removal needs no barrier; the node is gone once the flush destroyed its block.
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

	// The block fades and stops running the old code before release(); the handoff then closes. Health is told after
	// the transition's actions: stopping (restarting).
	CHECK(f.log == Log{"fade", "node rec stopping"});
	f.log.clear();
	f.settle();
	CHECK(f.log == Log{"drive nothing", "release", "handoff close"});
	f.log.clear();

	SECTION("the old configuration goes, then its config, then the new config starts") {
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 B",
		                   "handoff rec 2 channels", "node rec starting", "run built", "prepare 44100/256",
		                   "drive code"});
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
		CHECK(f.log == Log{"handoff destroyed", "run destroyed", "config A destroyed", "start 44100 C",
		                   "node rec starting", "run built", "prepare 44100/256", "drive code", "node rec running"});
	}
	SECTION("a removal during a restart stops the node and starts nothing") {
		f.host.nodeRemoved("rec");
		// Silenced already, so the block goes in this turn's flush; stopping already (restarting, now removing).
		CHECK(f.log == Log{"config B destroyed"});
		f.settle();
		CHECK(f.log == Log{"config B destroyed", "block destroyed"});
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

	CHECK(f.log == Log{"config A destroyed", "start 44100 B", "node rec starting", "run built", "prepare 44100/256",
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
	CHECK(f.cards.events.empty()); // made in the turn's flush
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
		CHECK(f.log == Log{"fade", "node motu stopping", "drive nothing", "release", "start 44100 B",
		                   "node motu starting", "card motu.playback destroyed", "card motu.capture destroyed",
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
	// The block's destruction waits for the turn's flush; the configuration goes meanwhile.
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
          "shape unchanged and the ports of the bank it named",
          "[node_host][rtp]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	CHECK(f.receivers.events.empty()); // nothing before the block is ready
	f.log.clear();

	f.blocks.events.at(0)->ready();
	f.endTurn();
	CHECK(f.log == Log{"receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.shapes.size() == 1);
	const RtpReceiverShape& made = f.receivers.shapes[0];
	CHECK(made.address == fppStream.address);
	CHECK(made.port == fppStream.port);
	CHECK(made.encoding == fppStream.encoding);
	CHECK(made.rate == fppStream.rate);
	CHECK(made.latencyMs == fppStream.latencyMs);
	CHECK(f.receivers.targets[0].bank == Direction::input);
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
	config->receivers = {{"cap", fppStream}};
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
	CHECK(f.log == Log{"config A destroyed", "start 44100 B", "node fpp starting", "receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.events.size() == 2); // made once more, not twice
	CHECK(f.blocks.events.size() == 1);      // the block stayed
	f.log.clear();
	f.receivers.events[1]->ready();
	f.settle();
	CHECK(f.log == Log{"prepare 44100/256", "drive code", "node fpp running"});
	f.stopAll();
	f.settle();
}

TEST_CASE("An engine companion lost before the node ran fails it with engine-refused and the companion's "
          "detail, and its code is never prepared or driven",
          "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {{"cap", fppStream}};
	f.add("mix", config);
	f.blocks.events.at(0)->ready();
	f.endTurn();
	REQUIRE(f.cards.events.size() == 1);
	REQUIRE(f.receivers.events.size() == 1);
	f.log.clear();

	// Never driven: torn down at once, with no barrier; the engine companions go in the turn's flush.
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

TEST_CASE("One configuration names each engine object once: a second of a name, or a bank of the wrong kind, is a "
          "type bug that fails the node start-failed",
          "[node_host][rtp]") {
	Fixture f;
	auto twice = std::make_shared<TestConfig>(f.log);
	twice->receivers = {{"cap", fppStream}, {"cap", fppStream}};
	f.add("fpp", twice); // never thrown out of NodeHost: one type's bug never takes ear down
	CHECK(failureOf(f, "fpp") == "start-failed: configure threw: a second resource named fpp.rtp in one configuration");

	auto twoPlayback = std::make_shared<TestConfig>(f.log);
	twoPlayback->cards = {motuOut, motuOut};
	f.add("motu", twoPlayback);
	CHECK(failureOf(f, "motu") ==
	      "start-failed: configure threw: a second resource named motu.playback in one configuration");

	auto routable = std::make_shared<TestConfig>(f.log);
	routable->receivers = {{"record", fppStream}};
	f.add("routable", routable);
	CHECK(failureOf(f, "routable") ==
	      "start-failed: configure threw: an RTP receiver joins an inside input bank, not \"record\"");

	auto routableCard = std::make_shared<TestConfig>(f.log);
	routableCard->cards = {{"record", motuIn.shape}};
	f.add("routablecard", routableCard);
	CHECK(failureOf(f, "routablecard") ==
	      "start-failed: configure threw: a sound card joins an inside bank, not \"record\"");

	// One companion per inside bank, whatever their kinds and names.
	auto shared = std::make_shared<TestConfig>(f.log);
	shared->cards = {motuIn};
	shared->receivers = {{"cap", fppStream}};
	f.add("shared", shared);
	CHECK(failureOf(f, "shared") == "start-failed: configure threw: a second companion on bank cap");

	auto missing = std::make_shared<TestConfig>(f.log);
	missing->cards = {{"nope", motuIn.shape}};
	f.add("missing", missing);
	CHECK(failureOf(f, "missing").starts_with("start-failed: configure threw: "));

	// A configuration a type bug was thrown out of asks for nothing more.
	for (auto* events : f.blocks.events) events->ready();
	CHECK(f.receivers.events.empty());
	CHECK(f.cards.events.empty());
	// They still stop: ~NodeHost's precondition.
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
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
	CHECK(indexOf(f.log, "handoff close") < destroyed); // told to close in the teardown, the flush after it
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
          "declared, by role, cleared at its teardown",
          "[node_host][latency]") {
	Fixture f;
	auto config = std::make_shared<LatencyConfig>();
	// Besides "capture" (the end of the pair in -> capture): an output no pair ends at, and an input a pair starts at.
	config->extra = {{"spare", Latency{1, 2}}, {"play", Latency{3, 4}}};
	f.add("card", config);
	CHECK(f.blocks.plans.at(0).empty()); // nothing is carried before the drive
	CHECK_THROWS_AS(config->context->declareLatency("spare", Latency{1, 1}), std::logic_error); // outside start()
	f.blocks.events.at(0)->ready();
	REQUIRE(f.blocks.plans.at(0).size() == 1);
	const LatencyPlan& plan = f.blocks.plans.at(0).at(0);
	CHECK(plan.pairs == config->pass.pairs);
	// Declared on an output a pair ends at: the code's delay on that pair's path, counted both ways on it.
	REQUIRE(plan.onPair.size() == 1);
	CHECK(plan.onPair[0].first == BankPair{"in", "capture"});
	CHECK(plan.onPair[0].second == Delay{{100, 200}});
	// Declared on an output no pair ends at, or on an input: what that bank alone tells.
	REQUIRE(plan.onBank.size() == 2);
	CHECK(plan.onBank[0] == std::pair<std::string, Delay>{"spare", Delay{{1, 2}}});
	CHECK(plan.onBank[1] == std::pair<std::string, Delay>{"play", Delay{{3, 4}}});
	CHECK(plan.beyond.empty());
	f.settle();

	f.update("card", std::make_shared<LatencyConfig>());
	f.settle();
	REQUIRE(f.blocks.plans.at(0).size() == 3); // cleared by the teardown, then the new configuration's
	CHECK(f.blocks.plans.at(0).at(1).pairs.empty());
	CHECK(f.blocks.plans.at(0).at(1).onPair.empty());
	CHECK(f.blocks.plans.at(0).at(1).onBank.empty());
	CHECK(f.blocks.plans.at(0).at(2).onPair.size() == 1);
	CHECK(f.blocks.plans.at(0).at(2).onBank.empty());
	f.stopAll();
	f.settle();
}

TEST_CASE("Latency declared twice for a bank, or for a bank the node lacks, is a type bug that fails the node",
          "[node_host][latency]") {
	for (const std::string bank : {"capture", "nowhere"}) {
		Fixture f;
		auto config = std::make_shared<LatencyConfig>();
		config->extra = {{bank, Latency{1, 1}}};
		f.add("card", config);
		CHECK(failureOf(f, "card").starts_with("start-failed: configure threw: "));
		if (bank == "capture") CHECK(failureOf(f, "card").ends_with("latency declared twice for bank capture"));
		f.blocks.events.at(0)->ready();
		CHECK(f.blocks.plans.at(0).empty()); // never driven
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
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
	f.add("card", config); // checked at configure's return, never thrown out
	// A pass-through of no pair is refused at use; the rest at the return, against the node's ports.
	CHECK(failureOf(f, "card").starts_with(which == 3 ? "start-failed: configure threw: use given no code"
	                                                  : "start-failed: configure gave a pass-through: "));
	f.blocks.events.at(0)->ready();
	CHECK(f.blocks.driven.at(0) == nullptr);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

// The asynchronous barrier (docs/architecture.md#the-node-lifecycle).

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
	// Nothing to silence until the drive is applied (6.3's "stop in starting"); stopping (restarting) at once (4.3 row 6).
	CHECK(f.log == Log{"node rec stopping"});
	f.blocks.complete(0);
	CHECK(f.log == Log{"node rec stopping", "fade"});
	f.blocks.complete(0);
	CHECK(f.log == Log{"node rec stopping", "fade", "drive nothing"});
	f.blocks.complete(0);
	CHECK(f.log == Log{"node rec stopping", "fade", "drive nothing", "release", "run destroyed", "config A destroyed",
	                   "start 44100 B", "node rec starting", "run built", "prepare 44100/256", "drive code"});
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
	// The block goes in the flush, the configuration meanwhile; the node is gone once both are.
	CHECK(f.log == Log{"drive nothing", "release", "run destroyed", "block destroyed", "node noise gone dropped=0"});
	f.log.clear();

	f.host.nodeRemoved("idle"); // no code was driven since its last silence
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

TEST_CASE("A restart waits for the old configuration to close before it configures the new one", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.update("net", next);
	f.settle();
	CHECK(f.log == Log{"drive nothing", "node net stopping", "release", "run close"});
	f.log.clear();

	std::thread([&] { old->run->closed(); }).join();
	CHECK(f.log.empty());
	f.settle();
	CHECK(f.log == Log{"run destroyed", "start 44100 next", "node net starting", "run built", "prepare 44100/256",
	                   "drive code", "node net running"});
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

TEST_CASE("A name added back whose block is lost while it waits never configures", "[node_host]") {
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

TEST_CASE("A run lost from its own thread fails a running node with lost and tears its configuration down, "
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
	CHECK(f.log == Log{"drive nothing", "node net failed lost: the client died"}); // it had rested
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
	CHECK(f.log == Log{"drive nothing", "node net stopping", "release", "run close"});
	f.log.clear();
	config->run->closed();
	f.settle();
	CHECK(std::ranges::find(f.log, std::string("node net running")) != f.log.end());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
}

// The two-phase start (docs/writing-a-node-type.md#the-class), through the legacy adapter
// (docs/writing-a-node-type.md#the-old-interface).

namespace {

/// A config whose recipe fails, or logs what Ready hands it. It asks for no engine companion.
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
		CHECK(f.log.at(0) == "start 44100 B"); // failed until its configure runs (rule 3)
		CHECK(f.log.at(1) == "node net starting");
	}
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(f.log.back() == "all gone");
}

// Teardown by dependency, with asynchronous close (docs/architecture.md#the-node-lifecycle).

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
	TestSinks sinks{log, mainLoop};
	TestSources sources{log};
	ResourceRegistry resources{cards, receivers, handoffs, sinks, sources};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};
	FakeTimers timers;
	TestDiagnostics diagnostics{log};
	TestInternalErrors internalErrors;
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
	ThreadedFixture() {
		host.subscribe(listener);
		host.subscribeDiagnostics(diagnostics);
		host.subscribeEngineDiagnostics(diagnostics);
		host.subscribeInternalErrors(internalErrors);
	}
};

} // namespace

TEST_CASE("A handoff consumer blocked at teardown blocks no main-loop turn, and the run waits for it, then closes",
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

TEST_CASE("A handoff consumer blocked past the close deadline leaks the run and the node moves on",
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

TEST_CASE("A run never closing past the deadline is leaked, and a restart then configures the next one",
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
	CHECK(f.log == Log{"drive nothing", "node net stopping", "release", "run close"});
	f.log.clear();
	CHECK(f.timers.live() == 1);

	f.timers.advance(closeDeadline);
	// The old run is leaked (never destroyed: its threads may still touch it); the next configuration begins.
	CHECK(f.log == Log{"diagnostic net ear.close-overdue", "start 44100 next", "node net starting", "run built",
	                   "prepare 44100/256", "drive code"});
	CHECK(std::ranges::count(f.log, std::string("run destroyed")) == 0);
	CHECK(f.host.leaked() == 1);
	f.settle();
	CHECK(f.log.back() == "node net running");
	f.log.clear();

	// What the leaked run tells late lands nowhere: not on the new configuration, and without touching freed
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
          "and its frees wait for closed",
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
	CHECK(f.log.empty()); // all of it crosses to the main loop first
	f.mainLoop.runPosted();
	// The note is heard (notes last until closed); the loss is not, as the run's close had begun.
	CHECK(f.log == Log{"note net handler done", "run destroyed", "node net gone dropped=0"});
	f.stopAll();
}

TEST_CASE("Nothing from an old run lands on a new one, even when it closed at once and the next configuration began "
          "in the same turn",
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
	CHECK(f.log == Log{"drive nothing", "node net stopping", "release", "run close", "run destroyed",
	                   "node net starting", "prepare 44100/256", "drive code"});
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

TEST_CASE("Every teardown starts with the barrier, a drive of no code, for a restart and a removal, before release, "
          "before anything is freed and before the block is destroyed",
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
	CHECK(f.log.at(0) == "fade");              // the barrier comes after the fade of what the code wrote
	CHECK(f.log.at(1) == "node rec stopping"); // told after the transition's actions
	CHECK(f.log.at(2) == "drive nothing");
	CHECK(f.log.at(3) == "release");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "card rec.playback destroyed"));
	// The cards go in the turn's flush, after the handoffs were told to close and before they finished.
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

TEST_CASE("Step 5 frees the code before the run object it reads, and the config last", "[node_host][teardown]") {
	Fixture f;
	f.add("net", std::make_shared<ReadingConfig>(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.log.clear();
	f.host.nodeRemoved("net");
	f.settle();
	// The configuration goes in its own input; the block in the turn's flush, after it.
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
		if (path.starts_with("emfile/")) throw std::system_error(EMFILE, std::generic_category(), "no descriptor");
		if (path.starts_with("bad_alloc/")) throw std::bad_alloc();
		if (path.starts_with("bug/")) throw std::logic_error("the folder adapter broke its own rule");
		if (path.starts_with("null/")) return nullptr;
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
	TestSinks sinks{log, mainLoop};
	TestSources sources{log};
	ResourceRegistry resources{cards, receivers, handoffs, sinks, sources, recordings};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};
	FakeTimers timers;
	NodeHost host{AudioFormat{44100, 256}, blocks, resources, mainLoop, timers, closeDeadline};
};

} // namespace

TEST_CASE("A recording is opened at start and closed with the run, after its handoff", "[node_host][recording]") {
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

// docs/architecture.md#faults-by-owner: an adapter's refusal is the node's start-failed through every path to it,
// a nested context call (a recording through the legacy start's acquire) among them, and never ear's bug.
TEST_CASE("An adapter's refusal through any path to it, a nested context call among them, is start-failed with no "
          "record",
          "[node_host][recording][type-throw]") {
	for (const std::string path : {"emfile/take.wav", "bad_alloc/take.wav"}) {
		INFO(path);
		RecordingFixture f;
		TestInternalErrors errors;
		f.host.subscribeInternalErrors(errors);
		// The legacy start's acquire(RecordingRequest) is a context call inside which openRecording is another.
		f.host.nodeAdded(NodeSpec{"rec", "test", {}}, std::make_shared<RecordingConfig>(f.log, path));
		CHECK_FALSE(MainLoopActivity::takeThrown()); // nothing thrown out of NodeHost
		REQUIRE(f.host.health().size() == 1);
		REQUIRE(f.host.health()[0].reason);
		CHECK(f.host.health()[0].reason->code == FailureReason::Code::startFailed);
		CHECK(f.host.health()[0].reason->detail.starts_with("configure threw: "));
		CHECK(errors.told.empty());
		f.host.stopAll(f.graph.close(), [] {});
		f.mainLoop.settle();
	}
	SECTION("a handoff's refusal through acquire") {
		Fixture g;
		g.handoffs.throwInMake = true;
		auto config = std::make_shared<TestConfig>(g.log);
		config->handoffs = 1;
		g.add("rec", config);
		CHECK(failureOf(g, "rec").starts_with("start-failed: configure threw: no thread for rec"));
		CHECK(g.internalErrors.told.empty());
		CHECK_FALSE(MainLoopActivity::takeThrown());
		g.stopAll();
		g.settle();
	}
}

TEST_CASE("An adapter that makes nothing, or throws its own std::logic_error, through a nested context call is ear's "
          "bug, its node lost and a record",
          "[node_host][recording][type-throw]") {
	const std::vector<std::pair<std::string, std::string>> cases{
		{"null/take.wav", "the recording adapter opened no file"},
		{"bug/take.wav", "the folder adapter broke its own rule"}};
	for (const auto& [path, what] : cases) {
		INFO(path);
		RecordingFixture f;
		TestInternalErrors errors;
		f.host.subscribeInternalErrors(errors);
		f.host.nodeAdded(NodeSpec{"rec", "test", {}}, std::make_shared<RecordingConfig>(f.log, path));
		CHECK_FALSE(MainLoopActivity::takeThrown());
		CHECK(errors.told == std::vector<std::string>{"RecordingAdapter::open|rec|configure rec|" + what});
		REQUIRE(f.host.health().at(0).reason);
		CHECK(f.host.health()[0].reason->code == FailureReason::Code::lost);
		CHECK(f.host.health()[0].reason->detail == "internal error in RecordingAdapter::open: " + what);
		f.host.stopAll(f.graph.close(), [] {});
		f.mainLoop.settle();
	}
	SECTION("a handoff adapter that makes nothing") {
		Fixture f;
		f.handoffs.nullInMake = true;
		auto config = std::make_shared<TestConfig>(f.log);
		config->handoffs = 1;
		f.add("rec", config);
		CHECK(f.internalErrors.told ==
		      std::vector<std::string>{"HandoffAdapter::make|rec|configure rec|the handoff adapter made no handoff"});
		CHECK(failureOf(f, "rec") ==
		      "lost: internal error in HandoffAdapter::make: the handoff adapter made no handoff");
		f.stopAll();
		f.settle();
	}
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

// The per-turn flush of PipeWire's object changes (docs/architecture.md#isolation), T1's wait
// for an older block of the name and MainLoopActivity.

namespace {

/// A config asking for the playback card only, with no recipe.
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

TEST_CASE("PipeWire's object changes of one turn run together in one flush, destroys first (engine companions, then "
          "blocks), then makes",
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
	f.host.nodeRemoved("x");            // never driven: its destruction is queued behind its making
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

TEST_CASE("An engine companion's ready told inside its making, during the flush, is handled after that flush item, "
          "and a block a destroy frees is made in the same flush",
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
	CHECK(f.cards.activities == std::vector<std::string>{"make engine companion c"});
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
		NodeCounts counts() const noexcept override { return {}; }
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

TEST_CASE("An engine companion is made with its block's id, read once the block is ready (5.7)", "[node_host][flush]") {
	Log log;
	QueueExecutor mainLoop;
	IdBlocks blocks{mainLoop};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	TestSinks sinks{log, mainLoop};
	TestSources sources{log};
	ResourceRegistry resources{cards, receivers, handoffs, sinks, sources};
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

// Fades, bounds and the wedged data loop (docs/architecture.md#the-node-lifecycle).

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
	CHECK(f.log == Log{"fade", "node amp stopping"}); // the barrier waits for the fade's acknowledgement
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

// The fake cannot model STREAMING: a block that leaves it completes by signal, which port_block_test.cpp's "A block that
// is PAUSED takes a drive in its invoke and completes it" and "A fade-out on a block that is not processing, or that
// leaves STREAMING mid-fade, is acknowledged by signal" show on the real engine (B1). Here: such completions, late but
// inside their bounds, judge nothing.
TEST_CASE("A drive or a fade completed late but inside its bound judges nothing, and the loop is not judged wedged",
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

// docs/architecture.md#faults-by-owner: a factory refuses through lost(), never by a throw, so one that throws has a bug
// of its own; NodeHost changed nothing yet where it calls it, so the fault is its node's alone.
TEST_CASE("A factory or an adapter that throws inside the flush is told as ear's bug and fails only its node lost",
          "[node_host][flush]") {
	SECTION("the port block's factory") {
		Fixture f;
		addRunning(f, "ok", std::make_shared<OutputConfig>(f.log));
		f.blocks.throwInMake = true;
		f.addInTurn("amp", std::make_shared<OutputConfig>(f.log));
		f.endTurn(); // nothing thrown out of NodeHost
		CHECK_FALSE(MainLoopActivity::takeThrown());
		CHECK(f.internalErrors.told ==
		      std::vector<std::string>{"PortBlockFactory::make|amp|make block amp|no memory for amp"});
		CHECK(failureOf(f, "amp") == "lost: internal error in PortBlockFactory::make: no memory for amp");
		CHECK(f.host.health().at(1).blockLost);
		CHECK(f.host.health().at(0).state == NodeState::running); // the rest of the graph runs on
		CHECK(MainLoopActivity::current().empty());
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("an engine companion's adapter") {
		Fixture f;
		f.cards.throwInMake = true;
		f.add("desk", withPlayback(f.log));
		f.blocks.events.at(0)->ready();
		f.endTurn(); // nothing thrown out of NodeHost
		CHECK_FALSE(MainLoopActivity::takeThrown());
		CHECK(f.internalErrors.told == std::vector<std::string>{"SoundCardCompanions::make|desk|make engine companion "
		                                                        "desk|no card Pro24"});
		CHECK(failureOf(f, "desk") == "lost: internal error in SoundCardCompanions::make: no card Pro24");
		CHECK_FALSE(f.host.health().at(0).blockLost); // a retry makes the companion afresh
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("A bound whose settle ran an earlier completion gives the request a grace instead of a judgement",
          "[node_host][wedge]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.hold = true;
	f.host.nodeRemoved("amp");
	f.timers.advance(std::chrono::milliseconds(100)); // the fade unacknowledged: the silence posted behind it
	REQUIRE(f.blocks.held.size() == 2);               // the fade, then the silence
	// The fade's invoke ran late, its event not yet dispatched: the silence's invoke is posted only by the settle
	// the barrier's bound makes, so the data loop has had no chance at it.
	f.blocks.applyHeld(0, 0);
	f.log.clear();
	SECTION("the silence completes within the grace") {
		f.timers.advance(std::chrono::milliseconds(5'000));
		CHECK(f.log.empty());
		f.timers.advance(std::chrono::milliseconds(900));
		f.blocks.complete(0);
		f.endTurn();
		CHECK(f.log == Log{"release", "block destroyed", "node amp gone dropped=0"});
		CHECK(f.host.leaked() == 0);
	}
	SECTION("it does not: judged at the grace's end, with the whole wait") {
		f.timers.advance(std::chrono::milliseconds(5'000));
		f.timers.advance(std::chrono::milliseconds(1'000));
		CHECK(f.log ==
		      Log{"diagnostic amp ear.barrier-overdue ms=6000", "engine diagnostic ear.data-loop-wedged ms=6000"});
	}
	f.stopAll();
	f.endTurn();
	CHECK(f.allGone == 1);
}

TEST_CASE("A bound whose timer fires late (the whole process stalled) gives the request a grace instead of a judgement",
          "[node_host][wedge]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.hold = true;
	f.host.nodeRemoved("amp");
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(6'000)); // frozen 6 s: the fade's timer fires 5.9 s late
	CHECK(f.log.empty());
	f.timers.advance(std::chrono::milliseconds(100)); // then on time, after its grace
	CHECK(f.log == Log{"diagnostic amp ear.fade-unacknowledged ms=6100", "drive nothing"});
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(7'000)); // frozen again past the barrier's bound
	CHECK(f.log.empty());                               // no wedge: the data loop was frozen with the rest
	f.blocks.complete(1);                               // the silence, inside its grace
	f.endTurn();
	CHECK(f.log == Log{"release", "block destroyed", "node amp gone dropped=0"});
	CHECK(f.diagnostics.details.size() == 1);
	f.blocks.complete(0); // the late fade's acknowledgement finds nothing
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("A node whose block was leaked on a wedged loop is never freed: the block may still tell it",
          "[node_host][wedge]") {
	Fixture f;
	addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
	f.blocks.hold = true;
	f.host.nodeRemoved("amp");
	f.timers.advance(std::chrono::milliseconds(100));
	f.timers.advance(std::chrono::milliseconds(5'000));
	f.endTurn();
	REQUIRE(f.host.health().empty());
	f.add("next", std::make_shared<OutputConfig>(f.log)); // prunes removed nodes, but keeps amp's for its block
	f.blocks.events.at(0)->lost("the engine removed it"); // amp's leaked block tells it: nothing is freed, nothing told
	f.blocks.events.at(0)->ready();
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].name == "next");
	f.stopAll();
	f.endTurn();
	CHECK(f.allGone == 1);
}

TEST_CASE("A card's playback bank, an inside output, fades in at every drive and out before a restart's and a "
          "removal's silence",
          "[node_host][fade]") {
	Fixture f;
	addRunning(f, "desk", withPlayback(f.log, "A"));
	REQUIRE(f.blocks.modes.faded.size() == 1);
	CHECK(f.blocks.modes.faded[0] == std::vector<bool>{true, true}); // pb.1 and pb.2, which feed desk.playback
	f.log.clear();
	f.update("desk", withPlayback(f.log, "B"));
	CHECK(f.log.at(0) == "fade");
	f.settle();
	CHECK(indexOf(f.log, "fade") < indexOf(f.log, "drive nothing"));
	f.cards.events.back()->ready();
	f.settle();
	REQUIRE(f.blocks.modes.faded.size() == 2);
	CHECK(f.blocks.modes.faded[1] == std::vector<bool>{true, true});
	f.log.clear();
	f.host.nodeRemoved("desk");
	CHECK(f.log == Log{"node desk stopping", "fade"});
	f.settle();
	CHECK(f.log.at(2) == "drive nothing");
	CHECK(f.log.back() == "node desk gone dropped=0");
	f.stopAll();
	CHECK(f.allGone == 1);
}

// ---- The node's health (docs/architecture.md#the-node-lifecycle): the wanted state, health per transition,
// waitingFor's pacing, the operations and the port-caused replace.

namespace {

using Phases = std::vector<std::string>;

NodeChange outcome(NodeOutcome what, std::vector<std::string> keys = {}) { return NodeChange{what, std::move(keys)}; }

void change(Fixture& f, const NodeName& name, std::shared_ptr<const NodeConfig> config, NodeOutcome what,
            bool disabled) {
	f.host.nodeChanged(NodeSpec{name, "test", {{"new", true}}, disabled}, std::move(config), outcome(what, {"new"}));
}

std::string replyText(const LifecycleReply& reply) {
	if (reply) return std::string(appliedName(*reply));
	switch (reply.error().code) {
	case LifecycleRefusal::Code::unknownNode:
		return "unknown-node";
	case LifecycleRefusal::Code::nodeDisabled:
		return "node-disabled";
	case LifecycleRefusal::Code::notRestartable:
		return reply.error().reason == LifecycleRefusal::Reason::blockLost ? "not-restartable block-lost"
		                                                                   : "not-restartable not-started";
	case LifecycleRefusal::Code::stopping:
		return "stopping";
	}
	return "?";
}

} // namespace

TEST_CASE("A health notification for every change of any member, in order: a normal start is added, configuring, "
          "configured, starting, running",
          "[node_host][health]") {
	Fixture f;
	addRunning(f, "noise", std::make_shared<OutputConfig>(f.log));
	CHECK(f.listener.phases("noise") == Phases{"added", "configuring", "configured", "starting", "running"});
	for (const NodeHealth& told : f.listener.told) {
		CHECK(told.wanted == Wanted::running);
		CHECK(told.waitingFor.empty());
		CHECK(told.state == stateOf(told.phase));
	}
	f.stopAll();
	f.settle();
	CHECK(f.listener.phases("noise").back() == "stopping (removing)");
	CHECK(f.allGone == 1);
}

TEST_CASE("A stop keeps the configuration: no resource released, and start drives it again",
          "[node_host][operations]") {
	Fixture f;
	addRunning(f, "desk", withPlayback(f.log));
	f.log.clear();
	f.listener.told.clear();

	CHECK(replyText(f.host.stop("desk")) == "stop");
	CHECK(f.log == Log{"fade", "node desk stopping"});
	f.settle();
	// Silenced, released and prepared afresh; the card stays.
	CHECK(f.log ==
	      Log{"fade", "node desk stopping", "drive nothing", "release", "prepare 44100/256", "node desk stopped"});
	CHECK(f.listener.phases("desk") == Phases{"stopping (stop)", "stopped"});
	CHECK(f.host.health()[0].wanted == Wanted::stopped);
	CHECK(replyText(f.host.stop("desk")) == "unchanged");
	f.log.clear();
	f.listener.told.clear();

	CHECK(replyText(f.host.start("desk")) == "start");
	f.settle();
	CHECK(f.log == Log{"drive code", "node desk starting", "node desk running"});
	CHECK(f.listener.phases("desk") == Phases{"starting", "running"});
	REQUIRE(f.blocks.modes.faded.size() == 2);
	CHECK(f.blocks.modes.faded[1] == std::vector<bool>{true, true}); // the card's bank fades in again
	CHECK(replyText(f.host.start("desk")) == "unchanged");
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("An operation's judged callback hears the reply before the stop's fade and health, and a refusal's too",
          "[node_host][operations]") {
	Fixture f;
	addRunning(f, "desk", withPlayback(f.log));
	f.log.clear();
	f.listener.told.clear();
	const auto judged = [&f](const LifecycleReply& reply) { f.log.push_back("judged " + replyText(reply)); };

	CHECK(replyText(f.host.stop("desk", judged)) == "stop");
	CHECK(f.log == Log{"judged stop", "fade", "node desk stopping"});
	f.settle();
	f.log.clear();
	CHECK(replyText(f.host.start("desk", judged)) == "start");
	CHECK(f.log.front() == "judged start");
	CHECK(replyText(f.host.restart("ghost", judged)) == "unknown-node");
	CHECK(f.log.back() == "judged unknown-node");
	f.stopAll();
	CHECK(replyText(f.host.stop("desk", judged)) == "stopping");
	CHECK(f.log.back() == "judged stopping");
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A card's playback bank fades out before a stop's and a disable's silence too", "[node_host][fade]") {
	Fixture f;
	addRunning(f, "desk", withPlayback(f.log, "A"));
	f.log.clear();
	REQUIRE(replyText(f.host.stop("desk")) == "stop");
	f.settle();
	CHECK(indexOf(f.log, "fade") < indexOf(f.log, "drive nothing"));
	REQUIRE(replyText(f.host.start("desk")) == "start");
	f.settle();
	f.log.clear();
	change(f, "desk", withPlayback(f.log, "A"), NodeOutcome::disable, true);
	CHECK(f.log.at(0) == "fade");
	f.settle();
	CHECK(indexOf(f.log, "fade") < indexOf(f.log, "drive nothing"));
	CHECK(indexOf(f.log, "drive nothing") < indexOf(f.log, "node desk disabled"));
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE(
	"disable tears the whole configuration down, keeps the block and drops the stop, and enable configures afresh",
	"[node_host][disabled]") {
	Fixture f;
	addRunning(f, "desk", withPlayback(f.log, "A"));
	REQUIRE(replyText(f.host.stop("desk")) == "stop");
	f.settle();
	f.log.clear();
	f.listener.told.clear();

	change(f, "desk", withPlayback(f.log, "B"), NodeOutcome::disable, true);
	f.settle();
	// Stopped already, so no fade or barrier (T20); the configuration goes, its card in the turn's flush; the block stays.
	CHECK(f.log == Log{"release", "node desk stopping", "config A destroyed", "node desk disabled",
	                   "card desk.playback destroyed"});
	CHECK(f.listener.phases("desk") == Phases{"stopping (disabling)", "disabled"});
	CHECK(f.host.health()[0].wanted == Wanted::disabled);
	CHECK(replyText(f.host.stop("desk")) == "node-disabled");
	CHECK(replyText(f.host.start("desk")) == "node-disabled");
	CHECK(replyText(f.host.restart("desk")) == "node-disabled");
	f.log.clear();
	f.listener.told.clear();

	// A change while disabled is stored; the enable takes the newest, and the stop was dropped: it runs.
	change(f, "desk", withPlayback(f.log, "C"), NodeOutcome::stored, true);
	CHECK(f.log == Log{"config B destroyed"});
	f.log.clear();
	change(f, "desk", withPlayback(f.log, "D"), NodeOutcome::enable, false);
	f.endTurn();
	f.cards.events.back()->ready();
	f.settle();
	CHECK(f.log == Log{"config C destroyed", "start 44100 D", "node desk starting",
	                   "card desk.playback of desk Pro24 pb.1,pb.2 at 44100", "prepare 44100/256", "drive code",
	                   "node desk running"});
	CHECK(f.listener.phases("desk") == Phases{"configuring (enabling)", "configured", "starting", "running"});
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A node added disabled is never configured, and its removal needs no barrier", "[node_host][disabled]") {
	Fixture f;
	f.host.nodeAdded(NodeSpec{"porch", "test", {}, true}, std::make_shared<TestConfig>(f.log, "P"));
	f.endTurn();
	CHECK(f.log == Log{"node porch disabled", "make porch 2 inputs"}); // only disabled told (T19), the block made
	CHECK(f.listener.phases("porch") == Phases{"disabled"});
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(f.log.size() == 2);
	f.log.clear();
	f.host.nodeRemoved("porch");
	f.endTurn();
	CHECK(f.log == Log{"config P destroyed", "node porch stopping", "block destroyed", "node porch gone dropped=0"});
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("Every phase a turn passes is told, in order: a restart of a stopped node tells stopping, configuring and "
          "stopped",
          "[node_host][health]") {
	Fixture f;
	addRunning(f, "noise", std::make_shared<OutputConfig>(f.log));
	REQUIRE(replyText(f.host.stop("noise")) == "stop");
	f.settle();
	f.listener.told.clear();
	CHECK(replyText(f.host.restart("noise")) == "restart");
	f.settle();
	CHECK(f.listener.phases("noise") == Phases{"stopping (restarting action)", "configuring (action)", "stopped"});
	CHECK(f.host.health()[0].phase == NodePhase::stopped);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("start on a failed node retries and clears the stop, and stop on a failed node changes only wanted",
          "[node_host][operations]") {
	Fixture f;
	auto bad = std::make_shared<TestConfig>(f.log, "bad");
	bad->fail = true;
	f.add("rec", bad);
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.host.health()[0].phase == NodePhase::failed);
	f.listener.told.clear();
	CHECK(replyText(f.host.stop("rec")) == "stop");
	REQUIRE(f.listener.told.size() == 1); // wanted alone changed: one notification
	CHECK(f.listener.told[0].phase == NodePhase::failed);
	CHECK(f.listener.told[0].wanted == Wanted::stopped);
	bad->fail = false;
	CHECK(replyText(f.host.start("rec")) == "retry");
	f.settle();
	CHECK(f.listener.phases("rec") == Phases{"failed", "configuring (retry)", "configured", "starting", "running"});
	CHECK(f.host.health()[0].wanted == Wanted::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("lost(detail) is start-failed before the node rested and lost after", "[node_host][health]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	config->closeAtOnce = true;
	f.add("net", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.host.health()[0].state == NodeState::running);
	config->events->lost("the library died");
	f.settle();
	REQUIRE(f.host.health()[0].reason.has_value());
	CHECK(f.host.health()[0].reason->code == FailureReason::Code::lost);
	CHECK(std::ranges::count(f.log, std::string("node net failed lost: the library died")) == 1);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A node removed by an edit refuses start, stop and restart unknown-node, and during stopAll every operation "
          "is stopping",
          "[node_host][operations]") {
	Fixture f;
	addRunning(f, "noise", std::make_shared<OutputConfig>(f.log));
	addRunning(f, "rec", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("noise"); // still stopping: its fade is posted
	CHECK(replyText(f.host.start("noise")) == "unknown-node");
	CHECK(replyText(f.host.stop("noise")) == "unknown-node");
	CHECK(replyText(f.host.restart("noise")) == "unknown-node");
	CHECK(replyText(f.host.stop("nobody")) == "unknown-node");
	f.stopAll();
	CHECK(replyText(f.host.stop("rec")) == "stopping");
	CHECK(replyText(f.host.start("rec")) == "stopping");
	CHECK(replyText(f.host.restart("rec")) == "stopping");
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("waitingFor reaches health only after 2 s in the phase, then changes at most once a second",
          "[node_host][health]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut, motuIn};
	f.add("desk", config);
	f.blocks.events.at(0)->ready();
	f.endTurn(); // both cards made, neither ready: configuring
	REQUIRE(f.host.health()[0].phase == NodePhase::configuring);
	CHECK(f.host.health()[0].waitingFor.empty());
	f.listener.told.clear();

	f.timers.advance(std::chrono::milliseconds(1'999));
	CHECK(f.listener.told.empty());
	f.timers.advance(std::chrono::milliseconds(1));
	REQUIRE(f.listener.told.size() == 1);
	CHECK(f.listener.told[0].waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::companion, "desk.playback"},
	                                                               {WaitingFor::Kind::companion, "desk.capture"}});

	// A change within the second waits for it; then the newest list is told.
	f.cards.events[0]->ready();
	f.settle();
	CHECK(f.listener.told.size() == 1);
	f.timers.advance(std::chrono::milliseconds(1'000));
	REQUIRE(f.listener.told.size() == 2);
	CHECK(f.listener.told[1].waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::companion, "desk.capture"}});

	// The phase ends: the list goes with it.
	f.cards.events[1]->ready();
	f.settle();
	CHECK(f.host.health()[0].phase == NodePhase::running);
	CHECK(f.host.health()[0].waitingFor.empty());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A name added back says it waits for the older node after 2 s", "[node_host][health]") {
	Fixture f;
	addRunning(f, "rec", std::make_shared<TestConfig>(f.log, "old"));
	f.blocks.hold = true;
	f.host.nodeRemoved("rec");
	f.add("rec", std::make_shared<TestConfig>(f.log, "new"));
	REQUIRE(f.host.health().back().phase == NodePhase::added);
	f.timers.advance(waitingForDelay);
	CHECK(f.host.health().back().waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::olderNode, "rec"}});
	f.blocks.hold = false;
	while (!f.blocks.held.empty()) {
		f.blocks.complete(0);
		f.settle();
	}
	f.settle();
	CHECK(f.host.health().back().waitingFor.empty());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A change of ports keeps the node's stop and one health stream for the name, the old node telling nothing",
          "[node_host][replace]") {
	Fixture f;
	addRunning(f, "desk", std::make_shared<TestConfig>(f.log, "two"));
	REQUIRE(replyText(f.host.stop("desk")) == "stop");
	f.settle();
	f.log.clear();
	f.listener.told.clear();

	const NodeChange ports{NodeOutcome::restart, {"playback"}, true};
	f.host.nodeReplacing("desk", ports);
	f.host.nodeReplaced(NodeSpec{"desk", "test", {{"playback", 4}}}, std::make_shared<TestConfig>(f.log, "four"),
	                    ports);
	f.settle();
	CHECK(f.listener.phases("desk") == Phases{"stopping (restarting settings)", "configuring (settings)", "stopped"});
	CHECK(std::ranges::count(f.log, std::string("node desk gone dropped=0")) == 0); // the old one told nothing
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].wanted == Wanted::stopped);
	CHECK(f.host.health()[0].phase == NodePhase::stopped);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

/// What NodeHost tells of each name's span of counts (docs/engine.md#reporting-the-counters), in order.
struct CountsSpans final : NodeCountsListener {
	std::vector<std::string> told;
	void countsBegan(const NodeName& node, std::uint64_t span) override {
		told.push_back(std::format("began {} {}", node, span));
	}
	void countsEnded(const NodeName& node, std::uint64_t span, const NodeCounts& total) override {
		told.push_back(std::format("ended {} {} {},{},{},{}", node, span, total.missedCycles, total.overruns,
		                           total.repositions, total.driverChanges));
	}
};

// docs/engine.md#counters: a node's counters run from its add, its block's kept when the block goes; a port-caused
// replace is the same node to the operator (one health stream, its dropped frames), so it carries them too, and they
// never step back on the way; a replace by another type is a new node, from zero.
TEST_CASE("A node's counters are its block's, kept when the block goes and carried across a change of ports",
          "[node_host][replace][counters]") {
	Fixture f;
	CountsSpans spans;
	f.host.subscribeCounts(spans);
	CHECK_FALSE(f.host.counts("desk"));
	addRunning(f, "desk", std::make_shared<TestConfig>(f.log, "two"));
	CHECK(f.host.counts("desk") == NodeCounts{});
	TestBlock* older = f.blocks.made.at(0);
	older->counted = NodeCounts{.missedCycles = 1, .overruns = 2, .repositions = 3, .driverChanges = 4};
	CHECK(f.host.counts("desk") == older->counted);

	const NodeChange ports{NodeOutcome::restart, {"playback"}, true};
	f.host.nodeReplacing("desk", ports);
	f.host.nodeReplaced(NodeSpec{"desk", "test", {{"playback", 4}}}, std::make_shared<TestConfig>(f.log, "four"),
	                    ports);
	CHECK(f.host.counts("desk") == NodeCounts{1, 2, 3, 4}); // the older node's, read from it while it closes
	older->counted.overruns = 5;                            // its block still runs until the barrier
	CHECK(f.host.counts("desk") == NodeCounts{1, 5, 3, 4});
	// Turn by turn until the older block is destroyed and the newer made, the counts never step back.
	std::size_t turns = 0;
	while (f.blocks.made.size() < 2 && turns++ < 50) {
		f.endTurn();
		CHECK(f.host.counts("desk") == NodeCounts{1, 5, 3, 4});
	}
	REQUIRE(f.blocks.made.size() == 2);
	f.blocks.made.at(1)->counted = NodeCounts{.missedCycles = 10, .overruns = 20};
	CHECK(f.host.counts("desk") == NodeCounts{11, 25, 3, 4});
	f.blocks.events.back()->ready();
	f.settle();
	REQUIRE(f.host.health().size() == 1); // the older node is gone: its counts are carried
	CHECK(f.host.counts("desk") == NodeCounts{11, 25, 3, 4});
	CHECK(f.host.spanCounts(1) == NodeCounts{11, 25, 3, 4}); // the older node's span, which the newer carries on
	CHECK_FALSE(f.host.spanCounts(2));                       // no span of its own
	f.blocks.made.at(1)->counted.repositions = 1;
	CHECK(f.host.counts("desk") == NodeCounts{11, 25, 4, 4});

	// Removed: still its counts while it stops, its block's kept past the block; none once it is gone, when its span
	// ends with them. The port-caused replace began no span and ended none: one span, the name's.
	f.host.nodeRemoved("desk");
	CHECK(f.host.counts("desk") == NodeCounts{11, 25, 4, 4});
	f.settle();
	CHECK_FALSE(f.host.counts("desk"));
	CHECK_FALSE(f.host.spanCounts(1));
	// The newer node (serial 2) went on in span 1, which its counts answer, and ended it.
	CHECK(spans.told == std::vector<std::string>{"began desk 1", "ended desk 1 11,25,4,4"});
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A replace by another type starts the name's counters from zero", "[node_host][replace][counters]") {
	Fixture f;
	CountsSpans spans;
	f.host.subscribeCounts(spans);
	addRunning(f, "x", std::make_shared<TestConfig>(f.log, "old"));
	f.blocks.made.at(0)->counted = NodeCounts{.missedCycles = 7};
	const NodeChange type{NodeOutcome::restart, {}, false};
	f.host.nodeReplacing("x", type);
	f.host.nodeReplaced(NodeSpec{"x", "other", {}}, std::make_shared<TestConfig>(f.log, "new"), type);
	CHECK(f.host.counts("x") == NodeCounts{}); // the newer node's, though the older still closes
	f.settle();
	f.blocks.events.back()->ready();
	f.settle();
	CHECK(f.host.counts("x") == NodeCounts{});
	f.blocks.made.back()->counted = NodeCounts{.overruns = 2};
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
	// A span each, the superseded older node's ended with its final counts as it went.
	CHECK(spans.told == std::vector<std::string>{"began x 1", "began x 2", "ended x 1 7,0,0,0", "ended x 2 0,2,0,0"});
}

TEST_CASE("A hand edit that keeps a node keeps its stop, and one that renames it drops it", "[node_host][replace]") {
	Fixture f;
	addRunning(f, "a", std::make_shared<TestConfig>(f.log, "a"));
	REQUIRE(replyText(f.host.stop("a")) == "stop");
	f.settle();
	change(f, "a", std::make_shared<TestConfig>(f.log, "a2"), NodeOutcome::restart, false);
	f.settle();
	CHECK(f.host.health()[0].phase == NodePhase::stopped);
	f.host.nodeRemoved("a");
	f.add("b", std::make_shared<TestConfig>(f.log, "b"));
	f.blocks.events.back()->ready();
	f.settle();
	CHECK(f.host.health().back().name == "b");
	CHECK(f.host.health().back().phase == NodePhase::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A replace of another type sends the older node's health to standard output alone", "[node_host][replace]") {
	struct WireListener final : NodeHealthListener {
		Log& log;
		explicit WireListener(Log& l) : log(l) {}
		void nodeChanged(const NodeHealth& health) override {
			log.push_back("wire " + health.name + " " + std::string(phaseName(health.phase)));
		}
		void nodeGone(const NodeName& node, std::uint64_t) override { log.push_back("wire " + node + " gone"); }
		bool hearsSuperseded() const noexcept override { return false; }
	};
	Fixture f;
	WireListener wire{f.log};
	f.host.subscribe(wire);
	addRunning(f, "x", std::make_shared<TestConfig>(f.log, "old"));
	f.log.clear();
	const NodeChange type{NodeOutcome::restart, {}, false};
	f.host.nodeReplacing("x", type);
	f.host.nodeReplaced(NodeSpec{"x", "other", {}}, std::make_shared<TestConfig>(f.log, "new"), type);
	f.settle();
	f.blocks.events.back()->ready();
	f.settle();
	// Standard output hears the older node stopping and gone; the wire hears only the newer node.
	CHECK(indexOf(f.log, "node x stopping") < indexOf(f.log, "node x gone dropped=0"));
	CHECK(indexOf(f.log, "wire x stopping") == static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(indexOf(f.log, "wire x gone") == static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(indexOf(f.log, "wire x added") < indexOf(f.log, "wire x running"));
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A port-caused replace's new node reports its older node's dropped frames, and its own removal waits for "
          "the older node to be gone",
          "[node_host][replace]") {
	Fixture f;
	auto two = std::make_shared<TestConfig>(f.log, "two");
	two->handoffs = 1;
	addRunning(f, "desk", two);
	const NodeChange ports{NodeOutcome::restart, {"playback"}, true};

	SECTION("the frames carried into the new node's removed") {
		f.host.nodeReplacing("desk", ports);
		f.host.nodeReplaced(NodeSpec{"desk", "test", {{"playback", 4}}}, std::make_shared<TestConfig>(f.log, "four"),
		                    ports);
		f.settle();
		f.handoffs.finishOne(5); // the older node's handoff closes: it is gone, telling nobody
		f.settle();
		f.blocks.events.back()->ready();
		f.settle();
		REQUIRE(f.host.health().back().phase == NodePhase::running);
		f.log.clear();
		f.stopAll();
		f.settle();
		CHECK(f.log.back() == "all gone");
		CHECK(std::ranges::count(f.log, std::string("node desk gone dropped=5")) == 1);
	}
	SECTION("removed while the older node still closes: gone only after it, with its frames") {
		f.host.nodeReplacing("desk", ports);
		f.host.nodeReplaced(NodeSpec{"desk", "test", {{"playback", 4}}}, std::make_shared<TestConfig>(f.log, "four"),
		                    ports);
		f.settle();
		f.log.clear();
		f.host.nodeRemoved("desk"); // the new node: its block still waits for the older one's
		f.settle();
		CHECK(std::ranges::count(f.log, std::string("node desk gone dropped=0")) == 0);
		REQUIRE(f.host.health().size() == 2);
		f.handoffs.finishOne(5);
		f.settle();
		CHECK(f.log.back() == "node desk gone dropped=5");
		CHECK(f.host.health().empty());
		f.stopAll();
		CHECK(f.allGone == 1);
	}
	SECTION("a new node added disabled still stands for the older node's removal and frames") {
		f.host.nodeReplacing("desk", ports);
		f.host.nodeReplaced(NodeSpec{"desk", "test", {{"playback", 4}}, true},
		                    std::make_shared<TestConfig>(f.log, "four"), ports);
		f.settle();
		CHECK(f.host.health().back().phase == NodePhase::disabled);
		f.log.clear();
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 0); // the older node's handoff still closes
		f.handoffs.finishOne(5);
		f.settle();
		CHECK(std::ranges::count(f.log, std::string("node desk gone dropped=5")) == 1);
		CHECK(f.allGone == 1);
	}
}

// The type's class, configure and its contexts, and the hooks (docs/writing-a-node-type.md): an
// instance a config makes (NodeConfig::make), the rules a type breaks failing its node start-failed,
// start() and stop() paired (docs/writing-a-node-type.md#the-class; tested here with a probe), and every hook timed
// (ear.hook-slow).

namespace {

/// Log entries NodeHost writes (subscribeLog).
struct TestLog final : LogSink {
	std::vector<LogEntry> entries;
	void log(const LogEntry& entry) override { entries.push_back(entry); }
};

/// An instance whose hooks log themselves and do what the test says: what NodeHost drives for the new SPI.
struct ProbeInstance final : ErasedInstance {
	Log& log;
	std::function<Configured(InstanceContexts&)> onConfigure;
	std::function<void()> onStart;
	std::function<void()> onStop;
	CreateContext** kept;
	ProbeInstance(Log& l, std::function<Configured(InstanceContexts&)> c, std::function<void()> s,
	              std::function<void()> t, CreateContext** k)
		: log(l), onConfigure(std::move(c)), onStart(std::move(s)), onStop(std::move(t)), kept(k) {}
	~ProbeInstance() override { log.push_back("instance destroyed"); }
	Configured configure(InstanceContexts& cx) override {
		log.push_back("configure");
		*kept = &cx.create;
		if (onConfigure) return onConfigure(cx);
		cx.create.use(std::make_unique<TestAudioCode>(log));
		return Configured::applied();
	}
	void start() override {
		log.push_back("start()");
		if (onStart) onStart();
	}
	void stop() override {
		log.push_back("stop()");
		if (onStop) onStop();
	}
};

/// A config of the new SPI: it makes a ProbeInstance per configuration and has no start.
struct ProbeConfig final : NodeConfig {
	Log& log;
	Ports ports_ = testPorts();
	std::function<Configured(InstanceContexts&)> onConfigure;
	std::function<void()> onStart;
	std::function<void()> onStop;
	mutable CreateContext* kept = nullptr; // the context configure was handed, kept to misuse it later
	explicit ProbeConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	std::unique_ptr<ErasedInstance> make(NodeContext&) const override {
		return std::make_unique<ProbeInstance>(log, onConfigure, onStart, onStop, &kept);
	}
};

std::size_t countOf(const Log& log, const std::string& line) {
	return static_cast<std::size_t>(std::ranges::count(log, line));
}

} // namespace

TEST_CASE("A config of the new SPI makes an instance per configuration: configure, start() once driven, stop() before "
          "each silence after a start() that returned",
          "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	addRunning(f, "probe", config);
	CHECK(countOf(f.log, "configure") == 1);
	CHECK(countOf(f.log, "start()") == 1);
	CHECK(countOf(f.log, "stop()") == 0);
	CHECK(indexOf(f.log, "start()") > indexOf(f.log, "drive code"));

	// A stop: stop() before its fade, and the instance kept; a start: start() again, once driven.
	f.log.clear();
	REQUIRE(f.host.stop("probe"));
	f.settle();
	CHECK(f.log.at(0) == "stop()");
	CHECK(indexOf(f.log, "stop()") < indexOf(f.log, "fade"));
	CHECK(countOf(f.log, "instance destroyed") == 0);
	REQUIRE(f.host.start("probe"));
	f.settle();
	CHECK(countOf(f.log, "start()") == 1);

	// A restart: stop(), the teardown (the code freed before the instance), a new instance configured and started.
	f.log.clear();
	REQUIRE(f.host.restart("probe"));
	f.settle();
	CHECK(f.log.at(0) == "stop()");
	CHECK(indexOf(f.log, "release") < indexOf(f.log, "instance destroyed"));
	CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "configure"));
	CHECK(countOf(f.log, "start()") == 1);

	// A stop of a stopped node calls no stop(); a removal of a running one calls it once.
	REQUIRE(f.host.stop("probe"));
	f.settle();
	f.log.clear();
	REQUIRE(f.host.stop("probe"));
	f.settle();
	CHECK(countOf(f.log, "stop()") == 0);
	REQUIRE(f.host.start("probe"));
	f.settle();
	f.log.clear();
	f.host.nodeRemoved("probe");
	f.settle();
	CHECK(countOf(f.log, "stop()") == 1);
	CHECK(countOf(f.log, "instance destroyed") == 1);
	f.stopAll();
	CHECK(f.allGone == 1);
}

TEST_CASE("stop() pairs only with a start() that returned: a failure stops a started node once, and a start() that "
          "threw gets none",
          "[node_host][class]") {
	SECTION("a card lost while running") {
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = [&](InstanceContexts& cx) {
			cx.create.soundCard("pb", motuOut.shape);
			cx.create.use(std::make_unique<TestAudioCode>(f.log));
			return Configured::applied();
		};
		addRunning(f, "probe", config);
		f.log.clear();
		f.cards.events.at(0)->lost("unplugged");
		f.settle();
		CHECK(countOf(f.log, "stop()") == 1);
		CHECK(failureOf(f, "probe") == "engine-refused: unplugged");
		f.stopAll();
		f.settle();
		CHECK(countOf(f.log, "stop()") == 1); // nothing more to pair
	}
	SECTION("a start() that threw: start-failed, not lost (it never reached running), and no stop()") {
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onStart = [] { throw std::runtime_error("no device"); };
		f.add("probe", config);
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(failureOf(f, "probe") == "start-failed: start threw: no device");
		CHECK(countOf(f.log, "stop()") == 0);
		CHECK(countOf(f.log, "node probe running") == 0); // the running its batch would have told was never told
		CHECK(countOf(f.log, "instance destroyed") == 1); // torn down at once
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("A stop() that throws during a stop is a fault held until the node rests, and during a teardown a log "
          "record and the teardown goes on",
          "[node_host][class]") {
	SECTION("during a stop") {
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onStop = [] { throw std::runtime_error("stuck"); };
		addRunning(f, "probe", config);
		REQUIRE(f.host.stop("probe"));
		f.settle();
		CHECK(failureOf(f, "probe") == "lost: stop threw: stuck"); // it had rested: lost (4.2's code by fact)
		f.stopAll();
		f.settle();
	}
	SECTION("during a removal") {
		Fixture f;
		TestLog logs;
		f.host.subscribeLog(logs);
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onStop = [] { throw std::runtime_error("stuck"); };
		addRunning(f, "probe", config);
		f.host.nodeRemoved("probe");
		f.settle();
		CHECK(countOf(f.log, "node probe gone dropped=0") == 1);
		REQUIRE(logs.entries.size() == 1);
		CHECK(logs.entries[0].level == LogLevel::error);
		CHECK(logs.entries[0].source == "ear");
		CHECK(logs.entries[0].text == "stop() threw: stuck");
		CHECK(logs.entries[0].node == NodeName("probe"));
		f.stopAll();
	}
}

TEST_CASE("A hook past 20 ms on the main loop is ear.hook-slow, naming the hook, once per node and hook",
          "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&](InstanceContexts& cx) {
		f.timers.advance(std::chrono::milliseconds(31)); // the hook takes 31 ms of the main loop's clock
		cx.create.use(std::make_unique<TestAudioCode>(f.log));
		return Configured::applied();
	};
	config->onStart = [&] { f.timers.advance(std::chrono::milliseconds(20)); }; // at the bound: not slow
	addRunning(f, "rec", config);
	CHECK(countOf(f.log, "diagnostic rec ear.hook-slow ms=31") == 1);
	CHECK(f.diagnostics.details == std::vector<std::string>{"configure ran 31 ms"});

	// Slow again after a restart: said once for the node's life.
	REQUIRE(f.host.restart("rec"));
	f.settle();
	CHECK(f.diagnostics.details.size() == 1);

	// Another hook is its own.
	config->onStop = [&] { f.timers.advance(std::chrono::milliseconds(25)); };
	REQUIRE(f.host.restart("rec")); // the instance it stops was made with no onStop; the next one has it
	f.settle();
	REQUIRE(f.host.stop("rec"));
	f.settle();
	CHECK(countOf(f.log, "diagnostic rec ear.hook-slow ms=25") == 1);
	CHECK(f.diagnostics.details.back() == "stop ran 25 ms");
	f.stopAll();
	f.settle();
}

TEST_CASE("Every rule a type breaks at a call or at configure's return fails its node start-failed with the rule's "
          "text, never thrown out of NodeHost",
          "[node_host][class]") {
	using Configure = std::function<Configured(InstanceContexts&)>;
	Log codes; // the codes' own log, apart from f.log's order
	const std::vector<std::pair<Configure, std::string>> cases{
		{[&](InstanceContexts& cx) {
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: use called twice"},
		{[](InstanceContexts&) { return Configured::applied(); }, "start-failed: configure gave no code"},
		{[](InstanceContexts&) { return Configured::failed("cannot open /nope"); }, "start-failed: cannot open /nope"},
		{[](InstanceContexts&) -> Configured { throw std::runtime_error("bad state"); },
	     "start-failed: configure threw: bad state"},
		{[](InstanceContexts&) -> Configured { throw 7; },
	     "start-failed: configure threw: an exception that is no "
	     "std::exception"},
		{[&](InstanceContexts& cx) {
			 cx.create.soundCard("record", motuIn.shape);
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: a sound card joins an inside bank, not \"record\""},
		{[&](InstanceContexts& cx) {
			 cx.create.sink("pb", SinkShape{.frames = 64}).consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.soundCard("pb", motuOut.shape);
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: a second companion on bank pb"},
		{[&](InstanceContexts& cx) {
			 cx.create.soundCard("pb", motuOut.shape);
			 cx.create.sink("pb", SinkShape{.frames = 64}).consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: a second companion on bank pb"},
		{[&](InstanceContexts& cx) {
			 cx.create.declareLatency("record", "pb", Latency{1, 1});
			 cx.create.declareLatency("record", "pb", Latency{2, 2});
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: latency declared twice for the path record to pb"},
		{[&](InstanceContexts& cx) {
			 cx.create.declareLatency("cap", "pb", Latency{2, 2});
			 cx.create.use(passThrough({{"record", "pb"}}));
			 return Configured::applied();
		 },
	     "start-failed: configure declared latency on cap to pb, a path its pass-through does not copy"},
		{[&](InstanceContexts& cx) {
			 cx.create.declareLatency("pb", "record", Latency{1, 1});
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: declareLatency names a pair: an input bank, then an output bank"},
		{[](InstanceContexts& cx) {
			 cx.create.use(passThrough({{"record", "pb"}, {"record", "pb"}}));
			 return Configured::applied();
		 },
	     "start-failed: configure gave a pass-through: "},
	};
	for (std::size_t which = 0; which < cases.size(); ++which) {
		INFO("case " << which);
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = cases[which].first;
		f.add("probe", config);
		CHECK(failureOf(f, "probe").starts_with(cases[which].second));
		CHECK(countOf(f.log, "instance destroyed") == 1); // its configuration torn down at once, the block kept
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.blocks.driven.at(0) == nullptr);
		CHECK(f.cards.events.empty());
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

// Regression (the class of 3b.2, 6c1299b556): a type's own code declares its delay from `record` to `pb`; with no
// pass-through to name the pair, the delay must still lie on that path, so it reaches what `record` tells upstream
// (and a source there dates Played late enough), not only what `pb` tells.
TEST_CASE("A typed declareLatency is a latency path: carried as a pair with its delay on it, for a code of the type's "
          "own as for a pass-through",
          "[node_host][latency][class]") {
	Log codes;
	const BankPair path{"record", "pb"};
	for (const bool own : {true, false}) {
		INFO((own ? "the type's own code" : "a pass-through of the same pair"));
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = [&](InstanceContexts& cx) {
			cx.create.declareLatency("record", "pb", Latency{3, 5});
			if (own) cx.create.use(std::make_unique<TestAudioCode>(codes));
			else cx.create.use(passThrough({{"record", "pb"}}));
			return Configured::applied();
		};
		f.add("probe", config);
		f.blocks.events.at(0)->ready();
		f.settle();
		REQUIRE(f.host.health()[0].state == NodeState::running);
		REQUIRE_FALSE(f.blocks.plans.at(0).empty());
		const LatencyPlan& plan = f.blocks.plans.at(0).back();
		CHECK(plan.pairs == std::vector<BankPair>{path});
		REQUIRE(plan.onPair.size() == 1);
		CHECK(plan.onPair[0].first == path);
		CHECK(plan.onPair[0].second == Delay{{3, 5}});
		CHECK(plan.onBank.empty());
		// What the block works out from it with no peers: the delay in both banks' told latency.
		const LatencyFacts facts = workOut(config->ports(), plan, std::vector<std::optional<Latency>>(3),
		                                   std::vector<std::optional<Latency>>(2));
		CHECK(facts.told == std::vector<Latency>{{3, 5}, {3, 5}, {0, 0}, {3, 5}, {3, 5}});
		f.stopAll();
		f.settle();
	}
}

// A mixer: two inputs into one output, each path declared with its own delay. A typed declaration is keyed by its
// path, so both are carried, each both ways: in what pb tells and in what its own input tells.
TEST_CASE("A code with two inputs into one output declares each path's delay, each carried both ways",
          "[node_host][latency][class]") {
	Log codes;
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&](InstanceContexts& cx) {
		cx.create.declareLatency("record", "pb", Latency{3, 5});
		cx.create.declareLatency("cap", "pb", Latency{7, 7});
		cx.create.use(std::make_unique<TestAudioCode>(codes));
		return Configured::applied();
	};
	f.add("probe", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.host.health()[0].state == NodeState::running);
	REQUIRE_FALSE(f.blocks.plans.at(0).empty());
	const LatencyPlan& plan = f.blocks.plans.at(0).back();
	const BankPair fromRecord{"record", "pb"}, fromCap{"cap", "pb"};
	CHECK(plan.pairs == std::vector<BankPair>{fromRecord, fromCap});
	REQUIRE(plan.onPair.size() == 2);
	CHECK(plan.onPair[0] == std::pair<BankPair, Delay>{fromRecord, Delay{{3, 5}}});
	CHECK(plan.onPair[1] == std::pair<BankPair, Delay>{fromCap, Delay{{7, 7}}});
	CHECK(plan.onBank.empty());
	// With no peers: each input tells its own path's delay; pb tells both paths side by side.
	const LatencyFacts facts =
		workOut(config->ports(), plan, std::vector<std::optional<Latency>>(3), std::vector<std::optional<Latency>>(2));
	CHECK(facts.told == std::vector<Latency>{{3, 5}, {3, 5}, {7, 7}, {3, 7}, {3, 7}});
	f.stopAll();
	f.settle();
}

TEST_CASE("A context kept past its configure is refused at the call: from start() the node fails",
          "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onStart = [&] { config->kept->use(std::make_unique<TestAudioCode>(f.log)); };
	f.add("probe", config);
	REQUIRE(config->kept != nullptr);
	CHECK_THROWS_WITH(config->kept->openRecording(RecordingRequest{"x.wav"}), "openRecording outside configure");
	CHECK_THROWS_WITH(config->kept->soundCard("pb", motuOut.shape), "soundCard outside configure");
	CHECK_THROWS_WITH(config->kept->rtpReceiver("cap", fppStream), "rtpReceiver outside configure");
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(failureOf(f, "probe") == "start-failed: start threw: use outside configure");
	CHECK(f.cards.events.empty());
	CHECK(f.receivers.events.empty());
	f.stopAll();
	f.settle();
}

TEST_CASE("One companion per inside bank, whatever the kinds: a card and a sink on one inside output bank, or a source "
          "and a receiver on one inside input bank, in either order, fail the node start-failed",
          "[node_host][class]") {
	using Configure = std::function<Configured(InstanceContexts&)>;
	Log codes;
	const auto sink = [&](InstanceContexts& cx) {
		cx.create.sink("pb", SinkShape{.frames = 64}).consumedBy(std::make_unique<TestSinkConsumer>(codes));
	};
	const auto source = [](InstanceContexts& cx) { static_cast<void>(cx.create.source("cap", SourceShape{64})); };
	const auto then = [&](InstanceContexts& cx) {
		cx.create.use(std::make_unique<TestAudioCode>(codes));
		return Configured::applied();
	};
	const std::vector<std::pair<Configure, std::string>> cases{
		{[&](InstanceContexts& cx) {
			 cx.create.soundCard("pb", motuOut.shape);
			 sink(cx);
			 return then(cx);
		 },
	     "pb"},
		{[&](InstanceContexts& cx) {
			 sink(cx);
			 cx.create.soundCard("pb", motuOut.shape);
			 return then(cx);
		 },
	     "pb"},
		{[&](InstanceContexts& cx) {
			 source(cx);
			 cx.create.rtpReceiver("cap", fppStream);
			 return then(cx);
		 },
	     "cap"},
		{[&](InstanceContexts& cx) {
			 cx.create.rtpReceiver("cap", fppStream);
			 source(cx);
			 return then(cx);
		 },
	     "cap"},
	};
	for (std::size_t which = 0; which < cases.size(); ++which) {
		INFO("case " << which);
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = cases[which].first;
		f.add("probe", config);
		f.settle();
		CHECK(failureOf(f, "probe") ==
		      "start-failed: configure threw: a second companion on bank " + cases[which].second);
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.cards.events.empty());
		CHECK(f.receivers.events.empty());
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("A configure that threw after it opened a recording closes the recording after its instance",
          "[node_host][class][recording]") {
	RecordingFixture f;
	TestListener listener{f.log};
	f.host.subscribe(listener);
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&](InstanceContexts& cx) -> Configured {
		const auto fd = cx.create.openRecording(RecordingRequest{"take.wav"});
		REQUIRE(fd);
		f.log.push_back("descriptor " + std::to_string(*fd));
		throw std::runtime_error("after the recording");
	};
	f.host.nodeAdded(NodeSpec{"rec", "test", {}}, config);
	f.mainLoop.settle();
	CHECK(indexOf(f.log, "descriptor 42") < indexOf(f.log, "instance destroyed"));
	CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "recording closed"));
	CHECK(std::ranges::count(f.log,
	                         std::string("node rec failed start-failed: configure threw: after the recording")) == 1);
	f.host.stopAll(f.graph.close(), [] {});
	f.mainLoop.settle();
}

TEST_CASE("A broken rule the type catches still fails its node with the rule's text", "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&](InstanceContexts& cx) {
		cx.create.use(std::make_unique<TestAudioCode>(f.log));
		try {
			cx.create.soundCard("record", motuIn.shape);
		} catch (const std::exception&) {
			// swallowed: the type goes on half-acquired
		}
		return Configured::applied();
	};
	f.add("probe", config);
	CHECK(failureOf(f, "probe") == "start-failed: configure threw: a sound card joins an inside bank, not \"record\"");
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(f.blocks.driven.at(0) == nullptr);
	f.stopAll();
	f.settle();
}

TEST_CASE("A context call caught inside start() still fails the node", "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onStart = [&] {
		try {
			config->kept->use(std::make_unique<TestAudioCode>(f.log));
		} catch (const std::logic_error&) {
		}
	};
	f.add("probe", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(failureOf(f, "probe") == "start-failed: start threw: use outside configure");
	f.stopAll();
	f.settle();
}

TEST_CASE("use with no code and failed with no detail are refused with rule texts", "[node_host][class]") {
	for (const bool nullCode : {true, false}) {
		Fixture f;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = [&](InstanceContexts& cx) {
			if (!nullCode) return Configured::failed("");
			cx.create.use(std::unique_ptr<TestAudioCode>());
			return Configured::applied();
		};
		f.add("probe", config);
		CHECK(failureOf(f, "probe") ==
		      (nullCode ? "start-failed: configure threw: use given no code: a null code or a pass-through of no pair"
		                : "start-failed: configure answered failed with no detail"));
		f.stopAll();
		f.settle();
	}
}

namespace {
/// A legacy config whose start fails engine-refused, or whose recipe throws, or whose recipe breaks a rule and catches
/// it (a start context kept past its start), or whose run object's close throws.
struct LegacyFaults final : NodeConfig {
	enum class Fault { engine, recipe, recipeRule, close } fault;
	Log& log;
	Ports ports_ = testPorts();
	mutable TestConsumer consumer;
	LegacyFaults(Fault f, Log& l) : fault(f), log(l) {}
	const Ports& ports() const override { return ports_; }
	struct ThrowingClose final : RunObject {
		void close(std::move_only_function<void()>) override { throw std::runtime_error("cannot join"); }
	};
	StartResult start(StartContext& context) const override {
		if (fault == Fault::engine)
			return std::unexpected(FailureReason{FailureReason::Code::engineRefused, "no such device"});
		if (fault == Fault::recipeRule)
			return StartResult{std::make_unique<TestAudioCode>(log),
			                   [this, &context](Ready&) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
								   try {
									   context.acquire(HandoffRequest{"record", 64, &consumer});
								   } catch (const std::logic_error&) { // caught: the rule is still broken
								   }
								   return std::make_unique<TestRun>(log);
							   }};
		if (fault == Fault::recipe)
			return StartResult{std::make_unique<TestAudioCode>(log),
			                   [](Ready&) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
								   throw std::runtime_error("no thread");
							   }};
		return StartResult{std::make_unique<TestAudioCode>(log),
		                   [](Ready&) -> std::expected<std::unique_ptr<RunObject>, FailureReason> {
							   return std::make_unique<ThrowingClose>();
						   }};
	}
};
} // namespace

TEST_CASE("A legacy start's engine refusal keeps its fact, and a recipe or close that throws is contained",
          "[node_host][legacy]") {
	SECTION("engine-refused") {
		Fixture f;
		f.add("old", std::make_shared<LegacyFaults>(LegacyFaults::Fault::engine, f.log));
		CHECK(failureOf(f, "old") == "engine-refused: no such device");
		f.stopAll();
		f.settle();
	}
	SECTION("a recipe that throws") {
		Fixture f;
		f.add("old", std::make_shared<LegacyFaults>(LegacyFaults::Fault::recipe, f.log)); // never thrown out
		CHECK(failureOf(f, "old") == "start-failed: recipe threw: no thread");
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("a recipe that breaks a rule and catches it") {
		Fixture f;
		f.add("old", std::make_shared<LegacyFaults>(LegacyFaults::Fault::recipeRule, f.log));
		CHECK(failureOf(f, "old") == "start-failed: recipe threw: acquire outside configure");
		CHECK(countOf(f.log, "drive code") == 0);
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("a close that throws") {
		Fixture f;
		TestLog logs;
		f.host.subscribeLog(logs);
		addRunning(f, "old", std::make_shared<LegacyFaults>(LegacyFaults::Fault::close, f.log));
		f.host.nodeRemoved("old");
		f.settle();
		CHECK(std::ranges::count(f.log, std::string("node old gone dropped=0")) == 1);
		REQUIRE(logs.entries.size() == 1);
		CHECK(logs.entries[0].text == "close threw: cannot join");
		f.stopAll();
	}
}

TEST_CASE("A stop() that throws during a disable or a failure is a log record, and the teardown goes on",
          "[node_host][class]") {
	for (const bool disable : {true, false}) {
		INFO((disable ? "disable" : "failure"));
		Fixture f;
		TestLog logs;
		f.host.subscribeLog(logs);
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onStop = [] { throw std::runtime_error("stuck"); };
		config->onConfigure = [&](InstanceContexts& cx) {
			cx.create.soundCard("pb", motuOut.shape);
			cx.create.use(std::make_unique<TestAudioCode>(f.log));
			return Configured::applied();
		};
		addRunning(f, "probe", config);
		if (disable) f.host.nodeChanged(NodeSpec{"probe", "test", {}, true}, config, {NodeOutcome::disable, {}});
		else f.cards.events.at(0)->lost("unplugged");
		f.settle();
		CHECK(f.host.health().at(0).phase == (disable ? NodePhase::disabled : NodePhase::failed));
		if (!disable) CHECK(failureOf(f, "probe") == "engine-refused: unplugged");
		REQUIRE(logs.entries.size() == 1);
		CHECK(logs.entries[0].text == "stop() threw: stuck");
		CHECK(countOf(f.log, "instance destroyed") == 1);
		f.stopAll();
		f.settle();
	}
}

TEST_CASE("A start() that throws on a start after a stop fails the node start-failed, with no stop()",
          "[node_host][class]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	bool failing = false;
	config->onStart = [&] {
		if (failing) throw std::runtime_error("no device");
	};
	addRunning(f, "probe", config);
	REQUIRE(f.host.stop("probe"));
	f.settle();
	f.log.clear();
	failing = true;
	REQUIRE(f.host.start("probe"));
	f.settle();
	CHECK(failureOf(f, "probe") == "start-failed: start threw: no device");
	CHECK(countOf(f.log, "stop()") == 0);
	CHECK(countOf(f.log, "node probe running") == 0);
	f.stopAll();
	f.settle();
}

// ---------------------------------------------------------------------------------------------------------------------
// The interim sink (docs/engine.md#sinks, docs/writing-a-node-type.md#sinks): made at configure,
// its consumer begun before the node is configured, its tap bound by every drive and by no silence, its bank never
// faded, closed at the teardown (its drops the node's), and every rule about it a failure of the node.

namespace {

/// configure on the probe: a sink on "pb" (two ports, a 1024-frame ring) consumed by a TestSinkConsumer (one that says
/// lost in begin when asked), and a code.
std::function<Configured(InstanceContexts&)> withSink(Log& log, bool loseInBegin = false) {
	return [&log, loseInBegin](InstanceContexts& cx) {
		cx.create.sink("pb", SinkShape{.frames = 1024})
			.consumedBy(std::make_unique<TestSinkConsumer>(log, loseInBegin ? &cx.legacy.events() : nullptr));
		cx.create.use(std::make_unique<TestAudioCode>(log));
		return Configured::applied();
	};
}

} // namespace

TEST_CASE("A sink's consumer begins before the node is configured, its bank is never faded, every drive binds its tap "
          "and no silence does, and a stop closes nothing",
          "[node_host][sink]") {
	Fixture f;
	f.sinks.holdBegin = true;
	f.sinks.dropped = 7;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	f.add("rec", config);
	CHECK(indexOf(f.log, "sink rec.pb 2 channels 1024 frames") < indexOf(f.log, "sink rec.pb begin"));
	f.blocks.events.at(0)->ready();
	f.settle();
	// Not begun: the configuration waits (readiness 4'), nothing is driven.
	CHECK(f.blocks.driven.at(0) == nullptr);
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].phase == NodePhase::configuring);
	CHECK(countOf(f.log, "consumer begin") == 0);

	f.sinks.made.at(0)->runBegin();
	f.settle();
	CHECK(indexOf(f.log, "consumer begin") < indexOf(f.log, "drive code"));
	CHECK(f.host.health()[0].state == NodeState::running);
	REQUIRE_FALSE(f.blocks.modes.faded.empty());
	CHECK(f.blocks.modes.faded.back() == std::vector<bool>{false, false}); // pb is the sink's: never faded
	REQUIRE(f.blocks.modes.sinks.back().size() == 1);
	const SinkBinding bound = f.blocks.modes.sinks.back()[0];
	CHECK(bound.tap == &f.sinks.made.at(0)->tap());
	CHECK(bound.firstOutput == 0);
	CHECK(bound.channels == 2);

	// A stop: the silence binds no tap, and the sink is not closed; a start binds the same tap again.
	REQUIRE(f.host.stop("rec"));
	f.settle();
	CHECK(f.blocks.driven.at(0) == nullptr);
	CHECK(f.blocks.modes.sinks.back().empty());
	CHECK(countOf(f.log, "sink rec.pb close") == 0);
	REQUIRE(f.host.start("rec"));
	f.settle();
	REQUIRE(f.blocks.modes.sinks.back().size() == 1);
	CHECK(f.blocks.modes.sinks.back()[0].tap == bound.tap);
	CHECK(f.sinks.made.size() == 1);

	// The teardown closes it (the consumer finished), then frees the code, the instance and the sink, in that order;
	// its drops are the node's.
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(indexOf(f.log, "sink rec.pb close") < indexOf(f.log, "consumer finish"));
	CHECK(indexOf(f.log, "consumer finish") < indexOf(f.log, "instance destroyed"));
	CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "sink rec.pb destroyed"));
	CHECK(indexOf(f.log, "sink rec.pb destroyed") < indexOf(f.log, "consumer destroyed"));
	CHECK(countOf(f.log, "node rec gone dropped=7") == 1);
	CHECK(f.allGone == 1);
}

TEST_CASE("A lost said in a sink consumer's begin fails the node start-failed before it is configured, and its close "
          "still finishes the consumer that began",
          "[node_host][sink]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log, true);
	f.add("rec", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	CHECK(failureOf(f, "rec") == "start-failed: cannot open take.wav");
	CHECK(f.blocks.driven.at(0) == nullptr);
	CHECK(countOf(f.log, "drive code") == 0);
	CHECK(indexOf(f.log, "consumer begin") < indexOf(f.log, "sink rec.pb close"));
	CHECK(indexOf(f.log, "sink rec.pb close") < indexOf(f.log, "consumer finish"));
	CHECK(countOf(f.log, "sink rec.pb destroyed") == 1);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A sink with no consumer or two, on a bank that is no inside output, beside a resource of its name, or with "
          "no thread fails the node start-failed, and a sink never begun is freed without a close",
          "[node_host][sink]") {
	using Configure = std::function<Configured(InstanceContexts&)>;
	Log codes;
	const auto sink = [](InstanceContexts& cx, const char* bank) -> SinkRequest& {
		return cx.create.sink(bank, SinkShape{.frames = 1024});
	};
	struct Case {
		Configure configure;
		std::string failure;
		bool throwInMake = false;
	};
	const std::vector<Case> cases{
		{[&](InstanceContexts& cx) {
			 sink(cx, "pb");
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure gave sink pb no consumer"},
		{[&](InstanceContexts& cx) {
			 SinkRequest& request = sink(cx, "pb");
			 request.consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 request.consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: consumedBy called twice on sink pb"},
		{[&](InstanceContexts& cx) {
			 sink(cx, "pb").consumedBy(nullptr);
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: consumedBy given no consumer on sink pb"},
		{[&](InstanceContexts& cx) {
			 sink(cx, "record").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: a sink joins an inside output bank, not \"record\""},
		{[&](InstanceContexts& cx) {
			 sink(cx, "pb").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 sink(cx, "pb").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: a second resource named rec.pb in one configuration"},
		{[&](InstanceContexts& cx) -> Configured {
			 sink(cx, "pb").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 throw std::runtime_error("after the sink");
		 },
	     "start-failed: configure threw: after the sink"},
		{[&](InstanceContexts& cx) {
			 sink(cx, "pb").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: no thread for rec", true},
		{[&](InstanceContexts& cx) {
			 try {
				 sink(cx, "pb").consumedBy(std::make_unique<TestSinkConsumer>(codes));
			 } catch (const std::exception&) {
				 // swallowed: the type goes on with no sink
			 }
			 cx.create.use(std::make_unique<TestAudioCode>(codes));
			 return Configured::applied();
		 },
	     "start-failed: configure threw: no thread for rec", true},
	};
	for (std::size_t which = 0; which < cases.size(); ++which) {
		INFO("case " << which);
		Fixture f;
		f.sinks.throwInMake = cases[which].throwInMake;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = cases[which].configure;
		f.add("rec", config);
		CHECK(failureOf(f, "rec").starts_with(cases[which].failure));
		CHECK(countOf(f.log, "sink rec.pb begin") == 0);
		CHECK(countOf(f.log, "sink rec.pb close") == 0);
		CHECK(countOf(f.log, "sink rec.pb destroyed") == f.sinks.made.size());
		CHECK(countOf(codes, "consumer begin") == 0);
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.blocks.driven.at(0) == nullptr);
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("A sink's engine name is one of its configuration's: a sink on bank playback beside a playback card is "
          "refused",
          "[node_host][sink]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->ports_ = Ports{{{Direction::output, "playback", {"1"}, PortBank::Reach::inside},
	                        {Direction::output, "pb", {"1"}, PortBank::Reach::inside}}};
	config->onConfigure = [&](InstanceContexts& cx) {
		cx.create.soundCard("pb", motuOut.shape);
		cx.create.sink("playback", SinkShape{.frames = 1024}).consumedBy(std::make_unique<TestSinkConsumer>(f.log));
		cx.create.use(std::make_unique<TestAudioCode>(f.log));
		return Configured::applied();
	};
	f.add("rec", config);
	CHECK(failureOf(f, "rec") == "start-failed: configure threw: a second resource named rec.playback in one "
	                             "configuration");
	CHECK(f.sinks.made.empty());
	f.stopAll();
	f.settle();
}

TEST_CASE("A restart waits for its sink's close before it frees the configuration, then makes a new sink",
          "[node_host][sink]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	addRunning(f, "rec", config);
	f.sinks.made.at(0)->holdClose = true;
	f.log.clear();
	REQUIRE(f.host.restart("rec"));
	f.settle();
	CHECK(countOf(f.log, "sink rec.pb close") == 1);
	CHECK(countOf(f.log, "instance destroyed") == 0); // step 4 waits for every sink
	CHECK(f.sinks.made.size() == 1);
	f.sinks.made.at(0)->runClose();
	f.settle();
	CHECK(indexOf(f.log, "consumer finish") < indexOf(f.log, "instance destroyed"));
	REQUIRE(f.sinks.made.size() == 2);
	CHECK(f.host.health()[0].state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A node waits for its sink's consumer, says so as waitingFor consumer after 2 s, and a sink's frames dropped "
          "reach nodeGone once its held close reports",
          "[node_host][sink]") {
	Fixture f;
	f.sinks.holdBegin = true;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	f.add("rec", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(waitingForDelay);
	f.settle();
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].phase == NodePhase::configuring);
	CHECK(f.host.health()[0].waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::consumer}});
	CHECK(waitingKindName(WaitingFor::Kind::consumer) == "consumer");

	f.sinks.made.at(0)->runBegin();
	f.settle();
	CHECK(f.host.health()[0].state == NodeState::running);
	CHECK(f.host.health()[0].waitingFor.empty());

	// The sink refused 5 frames over its life; its close is held: the node is gone only once it reports.
	f.sinks.made.at(0)->dropped = 5;
	f.sinks.made.at(0)->holdClose = true;
	f.stopAll();
	f.settle();
	CHECK(countOf(f.log, "sink rec.pb close") == 1);
	CHECK(f.allGone == 0);
	f.sinks.made.at(0)->runClose();
	f.settle();
	CHECK(countOf(f.log, "node rec gone dropped=5") == 1);
	CHECK(f.allGone == 1);
}

TEST_CASE("A sink whose close never reports is said by ear.close-overdue, counted among what is open, and leaked",
          "[node_host][sink]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	addRunning(f, "rec", config);
	f.sinks.made.at(0)->holdClose = true; // its consumer's finish hangs
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(countOf(f.log, "sink rec.pb close") == 1);
	CHECK(f.allGone == 0);
	f.timers.advance(closeDeadline);
	f.settle();
	CHECK(f.allGone == 1);
	CHECK(f.host.leaked() == 1);
	CHECK(countOf(f.log, "diagnostic rec ear.close-overdue") == 1);
	REQUIRE_FALSE(f.diagnostics.details.empty());
	CHECK(f.diagnostics.details.back().find("0 handoffs open, 1 sinks open") != std::string::npos);
	CHECK(countOf(f.log, "sink rec.pb destroyed") == 0); // leaked, never freed: its thread may still run
	CHECK(countOf(f.log, "instance destroyed") == 0);
	// The close reports at last: it lands nowhere.
	f.sinks.made.at(0)->runClose();
	f.settle();
	CHECK(countOf(f.log, "node rec gone dropped=0") == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources (docs/writing-a-node-type.md#sources): made at configure and ready at once, bound by every drive and by no
// silence, their capacity declared beyond their bank, their ring dropped once a stop's silence completed, closed at the
// teardown's first step and freed after the instance, and every rule about them a failure of the node.

namespace {

/// configure on the probe: a source on "cap" (one port, 1000 frames asked) and a code; the writer kept in `writer`.
std::function<Configured(InstanceContexts&)> withSource(Log& log, SourceWriter** writer) {
	return [&log, writer](InstanceContexts& cx) {
		*writer = &cx.create.source("cap", SourceShape{.frames = 1000});
		cx.create.use(std::make_unique<TestAudioCode>(log));
		return Configured::applied();
	};
}

} // namespace

TEST_CASE("A source is ready at once, every drive binds it with its capacity beyond its bank, a stop's silence binds "
          "none and drops its ring, and a start binds it again",
          "[node_host][source]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	SourceWriter* writer = nullptr;
	config->onConfigure = withSource(f.log, &writer);
	f.add("rx", config);
	CHECK(countOf(f.log, "source rx.cap 1 channels 1000 frames at 44100") == 1);
	REQUIRE(writer != nullptr);
	REQUIRE(f.sources.made.size() == 1);
	CHECK(writer->capacityFrames() == 1024);
	f.blocks.events.at(0)->ready();
	f.settle();
	// Nothing to wait for: driven and running.
	CHECK(f.host.health()[0].state == NodeState::running);
	REQUIRE_FALSE(f.blocks.modes.sources.empty());
	REQUIRE(f.blocks.modes.sources.back().size() == 1);
	const SourceBinding bound = f.blocks.modes.sources.back()[0];
	CHECK(bound.tap == &f.sources.made.at(0)->tap());
	CHECK(bound.firstInput == 2); // record.left, record.right, then cap.1
	CHECK(bound.channels == 1);
	REQUIRE_FALSE(f.blocks.plans.at(0).empty());
	const LatencyPlan& plan = f.blocks.plans.at(0).back();
	REQUIRE(plan.beyond.size() == 1);
	CHECK(plan.beyond[0].first == "cap");
	CHECK(plan.beyond[0].second == Delay{{0, 1024}});

	// Written, then stopped: the silence binds none, and once it completed the ring is dropped and writes wait.
	const std::vector<float> frames(300, 0.5F);
	REQUIRE(writer->write(frames, 300, std::chrono::milliseconds(0)) == 300);
	f.log.clear();
	REQUIRE(f.host.stop("rx"));
	f.settle();
	CHECK(f.blocks.modes.sources.back().empty());
	CHECK(indexOf(f.log, "source rx.cap stopping") < indexOf(f.log, "stop()")); // its writer idle before stop()
	CHECK(indexOf(f.log, "stop()") < indexOf(f.log, "fade"));
	CHECK(indexOf(f.log, "drive nothing") < indexOf(f.log, "source rx.cap stopped"));
	CHECK(f.sources.made.at(0)->droppedFrames() == 300);
	CHECK(writer->write(frames, 300, std::chrono::milliseconds(0)) == 0);
	CHECK_FALSE(writer->closed());
	REQUIRE(f.host.start("rx"));
	f.settle();
	REQUIRE(f.blocks.modes.sources.back().size() == 1);
	CHECK(f.blocks.modes.sources.back()[0].tap == bound.tap);
	CHECK(f.sources.made.size() == 1);

	// The teardown: the writer closed at its first step, before the type's stop(); the source freed after the
	// instance.
	f.log.clear();
	f.stopAll();
	f.settle();
	CHECK(indexOf(f.log, "source rx.cap close") < indexOf(f.log, "stop()"));
	CHECK(indexOf(f.log, "stop()") < indexOf(f.log, "drive nothing"));
	CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "source rx.cap destroyed"));
	CHECK(countOf(f.log, "source rx.cap destroyed") == 1);
	CHECK(f.allGone == 1);
}

TEST_CASE("A restart closes its source, frees it after the instance and makes a new one", "[node_host][source]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	SourceWriter* writer = nullptr;
	config->onConfigure = withSource(f.log, &writer);
	addRunning(f, "rx", config);
	f.log.clear();
	REQUIRE(f.host.restart("rx"));
	f.settle();
	CHECK(indexOf(f.log, "source rx.cap close") < indexOf(f.log, "release"));
	CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "source rx.cap destroyed"));
	CHECK(indexOf(f.log, "source rx.cap destroyed") < indexOf(f.log, "configure"));
	REQUIRE(f.sources.made.size() == 2);
	CHECK(writer == &f.sources.made.at(1)->writer()); // the new configuration's
	CHECK(f.blocks.modes.sources.back().at(0).tap == &f.sources.made.at(1)->tap());
	CHECK(f.host.health()[0].state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A source on a bank that is no inside input, a second companion on its bank, no frames, or no eventfd fails "
          "the node start-failed, and a source made is closed and freed after the instance",
          "[node_host][source]") {
	using Configure = std::function<Configured(InstanceContexts&)>;
	Log codes;
	const auto source = [](InstanceContexts& cx, const char* bank, std::uint32_t frames = 1000) -> SourceWriter& {
		return cx.create.source(bank, SourceShape{.frames = frames});
	};
	struct Case {
		Configure configure;
		std::string failure;
		bool throwInMake = false;
	};
	const auto then = [&](InstanceContexts& cx) {
		cx.create.use(std::make_unique<TestAudioCode>(codes));
		return Configured::applied();
	};
	const std::vector<Case> cases{
		{[&](InstanceContexts& cx) {
			 source(cx, "record");
			 return then(cx);
		 },
	     "start-failed: configure threw: a source joins an inside input bank, not \"record\""},
		{[&](InstanceContexts& cx) {
			 source(cx, "pb");
			 return then(cx);
		 },
	     "start-failed: configure threw: a source joins an inside input bank, not \"pb\""},
		{[&](InstanceContexts& cx) {
			 source(cx, "cap", 0);
			 return then(cx);
		 },
	     "start-failed: configure threw: a source on bank cap holds no frames"},
		{[&](InstanceContexts& cx) {
			 source(cx, "cap", maxSourceFrames + 1);
			 return then(cx);
		 },
	     "start-failed: configure threw: a source on bank cap holds more than 16777216 frames"},
		{[&](InstanceContexts& cx) {
			 source(cx, "cap");
			 source(cx, "cap");
			 return then(cx);
		 },
	     "start-failed: configure threw: a second resource named rx.cap in one configuration"},
		{[&](InstanceContexts& cx) {
			 cx.create.rtpReceiver("cap", fppStream);
			 source(cx, "cap");
			 return then(cx);
		 },
	     "start-failed: configure threw: a second companion on bank cap"},
		{[&](InstanceContexts& cx) {
			 source(cx, "cap");
			 cx.create.rtpReceiver("cap", fppStream);
			 return then(cx);
		 },
	     "start-failed: configure threw: a second companion on bank cap"},
		{[&](InstanceContexts& cx) -> Configured {
			 source(cx, "cap");
			 throw std::runtime_error("after the source");
		 },
	     "start-failed: configure threw: after the source"},
		{[&](InstanceContexts& cx) {
			 source(cx, "cap");
			 return then(cx);
		 },
	     "start-failed: configure threw: no eventfd for rx", true},
		{[&](InstanceContexts& cx) {
			 try {
				 source(cx, "cap");
			 } catch (const std::exception&) {
				 // swallowed: the type goes on with no source
			 }
			 return then(cx);
		 },
	     "start-failed: configure threw: no eventfd for rx", true},
	};
	for (std::size_t which = 0; which < cases.size(); ++which) {
		INFO("case " << which);
		Fixture f;
		f.sources.throwInMake = cases[which].throwInMake;
		auto config = std::make_shared<ProbeConfig>(f.log);
		config->onConfigure = cases[which].configure;
		f.add("rx", config);
		f.settle();
		CHECK(failureOf(f, "rx").starts_with(cases[which].failure));
		CHECK(countOf(f.log, "source rx.cap destroyed") == f.sources.made.size());
		if (!f.sources.made.empty()) {
			CHECK(indexOf(f.log, "source rx.cap close") < indexOf(f.log, "instance destroyed"));
			CHECK(indexOf(f.log, "instance destroyed") < indexOf(f.log, "source rx.cap destroyed"));
		}
		f.blocks.events.at(0)->ready();
		f.settle();
		CHECK(f.blocks.driven.at(0) == nullptr);
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

// docs/engine.md#readiness-deadlines: each engine companion and each sink's consumer has its kind's deadline from its
// request, on the main loop's timers; past it the node fails with the kind's text.

TEST_CASE("A card that never starts fails its node engine-refused at 10 s naming the card, and the teardown destroys "
          "every card it made",
          "[node_host][deadline]") {
	Fixture f;
	f.add("desk", withCards(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.cards.events.size() == 2);
	f.cards.events[0]->ready(); // the playback card runs; the capture card never does
	f.settle();
	f.timers.advance(cardReadinessBound - std::chrono::milliseconds(1));
	f.settle();
	CHECK(f.host.health().at(0).phase == NodePhase::configuring);
	CHECK(failureOf(f, "desk").empty());
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(1));
	CHECK(f.log == Log{"node desk failed engine-refused: the card never started: Pro24"});
	f.settle();
	CHECK(countOf(f.log, "card desk.playback destroyed") == 1);
	CHECK(countOf(f.log, "card desk.capture destroyed") == 1);
	CHECK(countOf(f.log, "drive code") == 0);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("An RTP receiver that never runs fails its node engine-refused at 5 s naming its UDP port, counted from its "
          "request, and one never made blames the block that was not ready",
          "[node_host][deadline][rtp]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	std::string expected;
	SECTION("its block ready, the receiver made and never ready") {
		f.blocks.events.at(0)->ready();
		f.settle();
		REQUIRE(f.receivers.events.size() == 1);
		expected = "engine-refused: the RTP receiver never ran (UDP port 5004 held?)";
	}
	SECTION("its block never ready, so the receiver is never made") {
		expected = "engine-refused: fpp.rtp was never made: the node's block was not ready within 5 s";
	}
	f.timers.advance(rtpReceiverReadinessBound - std::chrono::milliseconds(1));
	f.settle();
	CHECK(failureOf(f, "fpp").empty());
	f.timers.advance(std::chrono::milliseconds(1));
	f.settle();
	CHECK(failureOf(f, "fpp") == expected);
	CHECK(f.host.health().at(0).state == NodeState::failed);
	CHECK(countOf(f.log, "receiver fpp.rtp destroyed") == f.receivers.events.size());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("Companions ready before their deadlines cancel them: the node runs and later deadlines fail nothing",
          "[node_host][deadline]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	config->cards = {motuOut};
	config->receivers = {{"cap", fppStream}};
	f.add("mix", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.cards.events.size() == 1);
	REQUIRE(f.receivers.events.size() == 1);
	f.timers.advance(std::chrono::milliseconds(4'000));
	f.receivers.events[0]->ready();
	f.timers.advance(std::chrono::milliseconds(5'000));
	f.cards.events[0]->ready();
	f.settle();
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.timers.advance(std::chrono::milliseconds(60'000));
	f.settle();
	CHECK(f.host.health().at(0).state == NodeState::running);
	CHECK(failureOf(f, "mix").empty());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A companion ready in the turn its deadline comes due wins over the deadline", "[node_host][deadline]") {
	Fixture f;
	// Armed before the node's own deadline, so it runs first when the clock reaches them both.
	const auto readyFirst = f.timers.after(rtpReceiverReadinessBound, [&f] { f.receivers.events.at(0)->ready(); });
	f.add("fpp", withReceiver(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(rtpReceiverReadinessBound);
	f.settle();
	CHECK(failureOf(f, "fpp").empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A restart gives the new configuration's companions deadlines of their own, and the old ones fail nothing",
          "[node_host][deadline][rtp]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(std::chrono::milliseconds(4'000));
	REQUIRE(f.host.restart("fpp"));
	f.settle();
	REQUIRE(f.receivers.events.size() == 2); // the old one destroyed, a new one made
	f.timers.advance(std::chrono::milliseconds(2'000));
	f.settle();
	CHECK(failureOf(f, "fpp").empty());
	f.timers.advance(std::chrono::milliseconds(3'000));
	f.settle();
	CHECK(failureOf(f, "fpp") == "engine-refused: the RTP receiver never ran (UDP port 5004 held?)");
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A sink whose consumer has not begun within 5 s fails its node start-failed naming its bank, and the "
          "teardown closes the sink with no finish",
          "[node_host][deadline][sink]") {
	Fixture f;
	f.sinks.holdBegin = true;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	f.add("rec", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(sinkReadinessBound - std::chrono::milliseconds(1));
	f.settle();
	CHECK(failureOf(f, "rec").empty());
	f.log.clear();
	f.timers.advance(std::chrono::milliseconds(1));
	f.settle();
	CHECK(failureOf(f, "rec") == "start-failed: the sink pb's consumer did not begin within 5 s");
	CHECK(countOf(f.log, "sink rec.pb close") == 1);
	CHECK(countOf(f.log, "consumer finish") == 0);
	CHECK(countOf(f.log, "drive code") == 0);
	CHECK(countOf(f.log, "sink rec.pb destroyed") == 1); // freed once closed: its begin never runs
	CHECK(f.host.health().at(0).state == NodeState::failed);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A sink's consumer that began in time cancels its deadline, and a source has none",
          "[node_host][deadline][sink]") {
	Fixture f;
	f.sinks.holdBegin = true;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&log = f.log](InstanceContexts& cx) {
		cx.create.sink("pb", SinkShape{.frames = 1024}).consumedBy(std::make_unique<TestSinkConsumer>(log));
		static_cast<void>(cx.create.source("cap", SourceShape{.frames = 64}));
		cx.create.use(std::make_unique<TestAudioCode>(log));
		return Configured::applied();
	};
	f.add("rec", config);
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(std::chrono::milliseconds(4'999));
	f.sinks.made.at(0)->runBegin();
	f.settle();
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.timers.advance(std::chrono::milliseconds(60'000));
	f.settle();
	CHECK(f.host.health().at(0).state == NodeState::running);
	CHECK(failureOf(f, "rec").empty());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("Each kind's readiness bound is the one the design gives, and a source has none", "[node_host][deadline]") {
	CHECK(readinessBound(CompanionKind::cardPlayback) == cardReadinessBound);
	CHECK(readinessBound(CompanionKind::cardCapture) == cardReadinessBound);
	CHECK(readinessBound(CompanionKind::rtpReceiver) == rtpReceiverReadinessBound);
	CHECK(readinessBound(CompanionKind::sink) == sinkReadinessBound);
	CHECK_FALSE(readinessBound(CompanionKind::source).has_value());
	CHECK(cardReadinessBound == std::chrono::milliseconds(10'000));
	CHECK(rtpReceiverReadinessBound == std::chrono::milliseconds(5'000));
	CHECK(sinkReadinessBound == std::chrono::milliseconds(5'000));
	CHECK(companionKindName(CompanionKind::rtpReceiver) == "rtp-receiver");
}

TEST_CASE("A deadline whose timer fires late, after a main-loop stall, lets a ready the stall held back land first",
          "[node_host][deadline]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	REQUIRE(f.receivers.events.size() == 1);
	// The receiver ran during the stall: its ready waits on the main loop, dispatched behind the timer.
	f.mainLoop.post([&f] { f.receivers.events.at(0)->ready(); });
	SECTION("the timer fires late: the judgement waits behind the held ready, which wins") {
		f.timers.advance(rtpReceiverReadinessBound + std::chrono::milliseconds(2'000));
		CHECK(failureOf(f, "fpp").empty());
		f.settle();
		CHECK(failureOf(f, "fpp").empty());
		CHECK(f.host.health().at(0).state == NodeState::running);
	}
	SECTION("the timer fires on time: it judges at once") {
		f.timers.advance(rtpReceiverReadinessBound);
		CHECK(failureOf(f, "fpp") == "engine-refused: the RTP receiver never ran (UDP port 5004 held?)");
		f.settle();
		CHECK(f.host.health().at(0).state == NodeState::failed);
	}
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A late deadline whose companion never became ready still fails the node, once the stall's events landed",
          "[node_host][deadline]") {
	Fixture f;
	f.add("fpp", withReceiver(f.log));
	f.blocks.events.at(0)->ready();
	f.settle();
	f.timers.advance(rtpReceiverReadinessBound + std::chrono::milliseconds(2'000));
	CHECK(failureOf(f, "fpp").empty());
	f.settle();
	CHECK(failureOf(f, "fpp") == "engine-refused: the RTP receiver never ran (UDP port 5004 held?)");
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// A type's throw is a fact about its node (docs/writing-a-node-type.md#rules-the-framework-enforces): every call into a
// type's code goes through one catch, on the main loop (callType) or on a consumer's own thread, so the node fails with
// what threw and the rest of the graph runs on; what ear itself throws still leaves NodeHost for the callback boundary.

namespace {

/// A code whose prepare throws while `failing` says so, as one that cannot allocate its buffers.
struct PrepareFails final : AudioCode {
	Log& log;
	const bool& failing;
	PrepareFails(Log& l, const bool& f) : log(l), failing(f) {}
	void prepare(const AudioFormat&) override {
		log.push_back("prepare");
		if (failing) throw std::runtime_error("no buffers");
	}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {}
	void release() noexcept override { log.push_back("release"); }
};

/// configure on the probe: a sink on "pb" whose consumer throws in `call`, and a code.
std::function<Configured(InstanceContexts&)> withThrowingSink(Log& log, std::string call) {
	return [&log, call = std::move(call)](InstanceContexts& cx) {
		cx.create.sink("pb", SinkShape{.frames = 1024})
			.consumedBy(std::make_unique<TestSinkConsumer>(log, nullptr, call));
		cx.create.use(std::make_unique<TestAudioCode>(log));
		return Configured::applied();
	};
}

} // namespace

TEST_CASE("A code whose prepare throws fails its node, is never run, and the rest of the graph runs on",
          "[node_host][type-throw]") {
	Fixture f;
	addRunning(f, "ok", std::make_shared<ProbeConfig>(f.log));
	bool failing = true;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = [&](InstanceContexts& cx) {
		cx.create.soundCard("pb", motuOut.shape); // ready only after the block: the drive comes in prepare's batch
		cx.create.use(std::make_unique<PrepareFails>(f.log, failing));
		return Configured::applied();
	};
	SECTION("its first prepare: start-failed, and the drive of its batch runs no code") {
		f.log.clear();
		f.add("bad", config);
		f.blocks.events.at(1)->ready();
		f.endTurn();
		f.cards.events.at(0)->ready();
		f.settle(); // nothing thrown out of NodeHost
		CHECK(failureOf(f, "bad") == "start-failed: prepare threw: no buffers");
		CHECK_FALSE(MainLoopActivity::takeThrown()); // caught where it was thrown, so no place left for the boundary
		CHECK(countOf(f.log, "prepare") == 1);
		CHECK(countOf(f.log, "drive nothing") >= 1);
		CHECK(countOf(f.log, "drive code") == 0);
		CHECK(countOf(f.log, "start()") == 0);
		CHECK(countOf(f.log, "release") == 0); // never prepared, never released
		CHECK(countOf(f.log, "instance destroyed") == 1);
	}
	SECTION("its prepare after a stop: lost, since it had rested") {
		failing = false;
		addRunning(f, "bad", config);
		failing = true;
		f.log.clear();
		REQUIRE(f.host.stop("bad"));
		f.settle();
		CHECK(failureOf(f, "bad") == "lost: prepare threw: no buffers");
		CHECK(indexOf(f.log, "release") < indexOf(f.log, "prepare"));
		CHECK(countOf(f.log, "release") == 1); // the code whose prepare threw is not released again
		CHECK(countOf(f.log, "instance destroyed") == 1);
	}
	CHECK(failureOf(f, "ok").empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A sink's consumer that throws fails its node with what it threw, never its sink's thread, and the rest of "
          "the graph runs on",
          "[node_host][sink][type-throw]") {
	Fixture f;
	addRunning(f, "ok", std::make_shared<ProbeConfig>(f.log));
	TestLog logs;
	f.host.subscribeLog(logs);
	auto config = std::make_shared<ProbeConfig>(f.log);
	SECTION("in begin: start-failed before the node is configured, and nothing more is called") {
		config->onConfigure = withThrowingSink(f.log, "begin");
		f.log.clear();
		f.add("rec", config); // the fake sink's thread begins its consumer at once, inside this call
		f.blocks.events.at(1)->ready();
		f.settle();
		CHECK(failureOf(f, "rec") == "start-failed: the sink pb's consumer threw in begin: no such folder");
		CHECK(countOf(f.log, "drive code") == 0);
		CHECK(countOf(f.log, "sink rec.pb close") == 1);
		CHECK(countOf(f.log, "consumer finish") == 0); // its begin never returned: nothing to finish
		CHECK(logs.entries.empty());
	}
	SECTION("in consume: lost once running, no further chunk handed to it, and finished at the teardown") {
		config->onConfigure = withThrowingSink(f.log, "consume");
		addRunning(f, "rec", config);
		f.log.clear();
		f.sinks.made.at(0)->feed(64);
		f.sinks.made.at(0)->feed(64); // the sink's thread goes on; the consumer is not called again
		f.settle();
		CHECK(failureOf(f, "rec") == "lost: the sink pb's consumer threw in consume: disk full");
		CHECK(countOf(f.log, "consume 64") == 1);
		CHECK(indexOf(f.log, "sink rec.pb close") < indexOf(f.log, "consumer finish"));
		CHECK(countOf(f.log, "consumer finish") == 1);
		CHECK(logs.entries.empty());
	}
	SECTION("in finish, at the teardown: a log record, and the teardown goes on") {
		config->onConfigure = withThrowingSink(f.log, "finish");
		addRunning(f, "rec", config);
		f.host.nodeRemoved("rec");
		f.settle();
		CHECK(countOf(f.log, "node rec gone dropped=0") == 1);
		REQUIRE(logs.entries.size() == 1);
		CHECK(logs.entries[0].level == LogLevel::error);
		CHECK(logs.entries[0].source == "ear");
		CHECK(logs.entries[0].text == "the sink pb's consumer threw in finish: cannot write the header");
		CHECK(logs.entries[0].node == NodeName("rec"));
	}
	CHECK(failureOf(f, "ok").empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A legacy handoff's consumer that throws fails its node, and is still finished once at the teardown",
          "[node_host][legacy][type-throw]") {
	Fixture f;
	addRunning(f, "ok", std::make_shared<ProbeConfig>(f.log));
	auto config = std::make_shared<TestConfig>(f.log);
	config->handoffs = 1;
	addRunning(f, "old", config);
	REQUIRE(f.handoffs.consumers.size() == 1);
	config->consumer.throwInConsume = true;
	const std::vector<float> frames(8, 0.0F);
	f.handoffs.consumers[0]->consume(frames, 4);
	f.handoffs.consumers[0]->consume(frames, 4); // not handed on
	f.settle();
	CHECK(countOf(f.log, "handoff close") == 1); // torn down
	CHECK(config->consumer.consumed == 1);
	f.handoffs.consumers[0]->finish(); // the handoff's thread, closing
	f.handoffs.finishOne(0);
	f.settle();
	CHECK(failureOf(f, "old") == "lost: the handoff record's consumer threw in consume: disk full");
	CHECK(config->consumer.finished == 1);
	CHECK(failureOf(f, "ok").empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A type's std::logic_error is a fact about its node, while NodeHost's own still leaves NodeHost for the "
          "callback boundary, marked as NodeHost's",
          "[node_host][type-throw]") {
	Fixture f;
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onStart = [] { throw std::logic_error("bad state"); };
	f.add("probe", config);
	f.blocks.events.at(0)->ready();
	f.settle(); // nothing thrown out of NodeHost
	CHECK(failureOf(f, "probe") == "start-failed: start threw: bad state");
	CHECK_FALSE(MainLoopActivity::takeThrown());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);

	// A block that throws as NodeHost carries out its own actions (the drive's latency plan) interrupts NodeHost's own
	// update, so it is NodeHost's to answer (docs/architecture.md#faults-by-owner): it leaves, carrying a place, which
	// tells the callback boundary it came out of NodeHost, and ear ends. No stopAll: the fixture goes by destruction.
	Fixture g;
	g.blocks.modes.throwInCarry = true;
	g.add("amp", std::make_shared<ProbeConfig>(g.log));
	CHECK_THROWS_WITH(g.blocks.events.at(0)->ready(), "a plan the block cannot carry");
	CHECK(MainLoopActivity::takeThrown());
	CHECK(g.internalErrors.told.empty()); // the boundary tells it, not NodeHost
}

TEST_CASE("A consumer's throw the machine would drop, during a removal's fade, is a log record",
          "[node_host][sink][type-throw]") {
	Fixture f;
	TestLog logs;
	f.host.subscribeLog(logs);
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withThrowingSink(f.log, "consume");
	addRunning(f, "rec", config);
	f.blocks.hold = true; // the fade's acknowledgement waits for the test
	f.host.nodeRemoved("rec");
	f.settle();
	REQUIRE(f.blocks.held.size() == 1); // fading: the channel still takes facts
	f.sinks.made.at(0)->feed(64);       // the sink's thread hands it the fade's chunk; it throws
	f.settle();
	REQUIRE(logs.entries.size() == 1);
	CHECK(logs.entries[0].text == "the sink pb's consumer threw in consume: disk full");
	CHECK(logs.entries[0].node == NodeName("rec"));
	f.blocks.hold = false;
	while (!f.blocks.held.empty()) {
		f.blocks.complete(0);
		f.settle();
	}
	CHECK(countOf(f.log, "node rec gone dropped=0") == 1);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A consumer's throw from a configuration being torn down for a restart lands nothing on the next one",
          "[node_host][sink][type-throw]") {
	Fixture f;
	TestLog logs;
	f.host.subscribeLog(logs);
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withThrowingSink(f.log, "consume");
	f.sinks.holdClose = true; // every sink drains on its thread until the test says
	addRunning(f, "rec", config);
	REQUIRE(f.host.restart("rec"));
	f.settle();
	REQUIRE(countOf(f.log, "sink rec.pb close") == 1);
	TestSink& old = *f.sinks.made.at(0);
	old.feed(64); // a chunk drained at the close throws
	old.runClose();
	f.settle();
	REQUIRE(f.sinks.made.size() == 2); // the next configuration's sink, made once the old one closed
	CHECK(failureOf(f, "rec").empty());
	CHECK(f.host.health().at(0).state == NodeState::running);
	REQUIRE(logs.entries.size() == 1);
	CHECK(logs.entries[0].text == "the sink pb's consumer threw in consume: disk full");
	f.stopAll();
	f.settle();
	f.sinks.made.at(1)->runClose();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A consumer's throw during a restart's fade is a log record, and the restart completes without a failure",
          "[node_host][sink][type-throw]") {
	Fixture f;
	TestLog logs;
	f.host.subscribeLog(logs);
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withThrowingSink(f.log, "consume");
	addRunning(f, "rec", config);
	f.blocks.hold = true; // the fade's acknowledgement waits for the test
	REQUIRE(f.host.restart("rec"));
	f.settle();
	REQUIRE(f.blocks.held.size() == 1);
	f.sinks.made.at(0)->feed(64); // the fade's chunk: it throws, and the machine drops it (a restart outranks it)
	f.settle();
	REQUIRE(logs.entries.size() == 1);
	CHECK(logs.entries[0].text == "the sink pb's consumer threw in consume: disk full");
	f.blocks.hold = false;
	while (!f.blocks.held.empty()) {
		f.blocks.complete(0);
		f.settle();
	}
	CHECK(failureOf(f, "rec").empty());
	CHECK(f.host.health().at(0).state == NodeState::running); // the next configuration
	CHECK(f.sinks.made.size() == 2);
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

TEST_CASE("A second consumer throw during a stop's fade is a log record, and the first, held, fails the node once it "
          "rests",
          "[node_host][sink][type-throw]") {
	Fixture f;
	TestLog logs;
	f.host.subscribeLog(logs);
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withThrowingSink(f.log, "consume");
	config->onStop = [] { throw std::runtime_error("stuck"); }; // the stop's first fault, held
	addRunning(f, "rec", config);
	f.blocks.hold = true;
	REQUIRE(f.host.stop("rec"));
	f.settle();
	REQUIRE(f.blocks.held.size() == 1);
	CHECK(logs.entries.empty());
	f.sinks.made.at(0)->feed(64); // the fade's chunk throws: a second fault of the stop, dropped
	f.settle();
	REQUIRE(logs.entries.size() == 1);
	CHECK(logs.entries[0].text == "the sink pb's consumer threw in consume: disk full");
	f.blocks.hold = false;
	while (!f.blocks.held.empty()) {
		f.blocks.complete(0);
		f.settle();
	}
	CHECK(failureOf(f, "rec") == "lost: stop threw: stuck");
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

// docs/architecture.md#faults-by-owner: an adapter's std::logic_error is its bug, not a rule the type broke; its state
// is the node's alone, so the node fails lost, told as ear.internal-error, and the rest of the graph runs on.
TEST_CASE("An adapter's own std::logic_error inside a context call is told as ear's bug and fails only its node lost, "
          "even when the type catches it",
          "[node_host][sink][type-throw]") {
	for (const bool caught : {false, true}) {
		INFO("caught by the type: " << caught);
		Fixture f;
		addRunning(f, "ok", std::make_shared<OutputConfig>(f.log));
		f.sinks.breakRuleInMake = true;
		auto config = std::make_shared<ProbeConfig>(f.log);
		const auto configure = withSink(f.log);
		config->onConfigure = [&, configure](InstanceContexts& cx) -> Configured {
			if (!caught) return configure(cx);
			try {
				return configure(cx);
			} catch (const std::exception&) {
				return Configured::applied(); // swallowed: the type goes on with no sink
			}
		};
		f.add("rec", config); // nothing thrown out of NodeHost
		CHECK_FALSE(MainLoopActivity::takeThrown());
		CHECK(f.internalErrors.told ==
		      std::vector<std::string>{
				  "SinkAdapter::make|rec|configure rec|the sink adapter broke its own rule for rec"});
		CHECK(failureOf(f, "rec") ==
		      "lost: internal error in SinkAdapter::make: the sink adapter broke its own rule for "
		      "rec");
		CHECK(f.host.health().at(0).state == NodeState::running);
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}

TEST_CASE("An adapter's refusal inside a context call is the node's start-failed, not ear's bug",
          "[node_host][sink][type-throw]") {
	Fixture f;
	f.sinks.throwInMake = true; // std::system_error: no thread
	auto config = std::make_shared<ProbeConfig>(f.log);
	config->onConfigure = withSink(f.log);
	f.add("rec", config);
	CHECK(failureOf(f, "rec").starts_with("start-failed: configure threw: no thread for rec"));
	CHECK(f.internalErrors.told.empty());
	CHECK_FALSE(MainLoopActivity::takeThrown());
	f.stopAll();
	f.settle();
	CHECK(f.allGone == 1);
}

// docs/architecture.md#faults-by-owner: an adapter whose own callback threw tells its events faulted; the node fails
// lost, a block for good and an engine companion so that a retry makes it afresh.
TEST_CASE("A block or an engine companion told faulted fails its node lost: the block for good, the companion retried",
          "[node_host][type-throw]") {
	SECTION("a block") {
		Fixture f;
		addRunning(f, "amp", std::make_shared<OutputConfig>(f.log));
		f.blocks.events.at(0)->faulted("internal error in FilterPortBlock::onStateChanged: bad state");
		f.settle();
		CHECK(failureOf(f, "amp") == "lost: internal error in FilterPortBlock::onStateChanged: bad state");
		CHECK(f.host.health().at(0).blockLost);
		CHECK_FALSE(f.host.start("amp").has_value()); // not-restartable: block-lost
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
	SECTION("an engine companion") {
		Fixture f;
		f.add("desk", withPlayback(f.log));
		f.blocks.events.at(0)->ready();
		f.settle();
		REQUIRE(f.cards.events.size() == 1);
		f.cards.events.at(0)->faulted("internal error in AlsaCardBody::onInfo: bad info");
		f.settle();
		CHECK(failureOf(f, "desk") == "lost: internal error in AlsaCardBody::onInfo: bad info");
		CHECK_FALSE(f.host.health().at(0).blockLost);
		REQUIRE(f.host.start("desk").has_value()); // a retry makes the companion afresh
		f.settle();
		CHECK(f.cards.events.size() == 2);
		f.stopAll();
		f.settle();
		CHECK(f.allGone == 1);
	}
}
