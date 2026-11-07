// The node framework driven through fake engine factories: how a node starts, what makes it
// running or failed, and the order stopAll takes everything down in
// (docs/design/m1-white-to-file.md section 3, S2 criterion 5), on batch 2's interfaces
// (docs/design/batch2.md section 4.1: the port block is made first and the insides drive it;
// C1 criteria 7-11: restart, sound cards, a lost block, removal; docs/design/rtp.md R1a criteria
// 3-8: RTP receivers on the same outside-node path). Every step a fake engine would take later
// (ready, lost, a handoff closing, a card or receiver ready or lost) is an event the test sends.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <memory>
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

struct TestInsides final : NodeInsides {
	Log& log;
	TestAudioCode code;
	TestConsumer consumer;
	explicit TestInsides(Log& l) : log(l), code(l) {}
	~TestInsides() override { log.push_back("insides destroyed"); }
	AudioCode& audioCode() override { return code; }
};

/// A node config whose start asks for `handoffs` handoffs, `cards` sound cards and `receivers`
/// RTP receivers, then succeeds or fails. A config with a tag logs its start and its destruction with that tag.
struct TestConfig final : NodeConfig {
	Log& log;
	std::string tag;
	Ports ports_{{{Direction::input, "record", {"left", "right"}}}, {"CAP1"}, {"PB1", "PB2"}};
	int handoffs = 0;
	std::vector<SoundCardRequest> cards;
	std::vector<RtpReceiverRequest> receivers;
	bool fail = false;
	mutable NodeGrant* grant = nullptr; // the grant start() was given, kept to misuse it later
	explicit TestConfig(Log& l, std::string t = {}) : log(l), tag(std::move(t)) {}
	~TestConfig() override {
		if (!tag.empty()) log.push_back("config " + tag + " destroyed");
	}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& g) const override {
		grant = &g;
		const AudioFormat format = g.format();
		log.push_back("start " + std::to_string(format.rate) + (tag.empty() ? "" : " " + tag));
		auto insides = std::make_unique<TestInsides>(log);
		for (int i = 0; i < handoffs; ++i) g.handoff(2, 1024, insides->consumer);
		for (const auto& card : cards) g.soundCard(card);
		for (const auto& receiver : receivers) g.rtpReceiver(receiver);
		if (fail) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open /nope"});
		return insides;
	}
};

const SoundCardRequest motuOut{*CardId::parse("Pro24"), CardDirection::playback, {"PB1", "PB2"}};
const SoundCardRequest motuIn{*CardId::parse("Pro24"), CardDirection::capture, {"CAP1"}};

std::shared_ptr<TestConfig> withCards(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->cards = {motuOut, motuIn};
	return config;
}

const RtpReceiverRequest fppStream{*RtpAddress::parse("239.69.0.1"), 5004, RtpEncoding::l24, 44100, 100, {"CAP1"}};

std::shared_ptr<TestConfig> withReceiver(Log& log, std::string tag = {}) {
	auto config = std::make_shared<TestConfig>(log, std::move(tag));
	config->receivers = {fppStream};
	return config;
}

struct TestBlock final : PortBlock {
	Log& log;
	explicit TestBlock(Log& l) : log(l) {}
	~TestBlock() override { log.push_back("block destroyed"); }
	void drive(AudioCode* code) override { log.push_back(code != nullptr ? "drive code" : "drive nothing"); }
};

struct TestBlocks final : PortBlockFactory {
	Log& log;
	std::vector<PortBlockEvents*> events;
	explicit TestBlocks(Log& l) : log(l) {}
	std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports, const AudioFormat&,
	                                PortBlockEvents& e) override {
		log.push_back("make " + node + " " + std::to_string(ports.inputs().size()) + " inputs");
		events.push_back(&e);
		return std::make_unique<TestBlock>(log);
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
struct TestHandoffs final : HandoffFactory {
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

struct TestCard final : OutsideNode {
	Log& log;
	NodeName name;
	TestCard(Log& l, NodeName n) : log(l), name(std::move(n)) {}
	~TestCard() override { log.push_back("card " + name + " destroyed"); }
};

/// Cards whose readiness and loss the test tells, as a real card's PCM and links report later.
struct TestCards final : SoundCardFactory {
	Log& log;
	std::vector<OutsideNodeEvents*> events;
	explicit TestCards(Log& l) : log(l) {}
	std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
	                                  const SoundCardRequest& request, const AudioFormat& format,
	                                  OutsideNodeEvents& e) override {
		std::string ports;
		for (const auto& port : request.blockPorts) ports += (ports.empty() ? "" : ",") + port;
		log.push_back("card " + engineNode + " of " + blockNode + " " + request.card.text() + " " + ports + " at " +
		              std::to_string(format.rate));
		events.push_back(&e);
		return std::make_unique<TestCard>(log, engineNode);
	}
};

struct TestReceiver final : OutsideNode {
	Log& log;
	NodeName name;
	TestReceiver(Log& l, NodeName n) : log(l), name(std::move(n)) {}
	~TestReceiver() override { log.push_back("receiver " + name + " destroyed"); }
};

/// RTP receivers whose readiness and loss the test tells; each request is kept as it was given.
struct TestReceivers final : RtpReceiverFactory {
	Log& log;
	std::vector<OutsideNodeEvents*> events;
	std::vector<RtpReceiverRequest> requests;
	explicit TestReceivers(Log& l) : log(l) {}
	std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
	                                  const RtpReceiverRequest& request, OutsideNodeEvents& e) override {
		log.push_back("receiver " + engineNode + " of " + blockNode);
		events.push_back(&e);
		requests.push_back(request);
		return std::make_unique<TestReceiver>(log, engineNode);
	}
};

struct TestListener final : NodeHealthListener {
	Log& log;
	explicit TestListener(Log& l) : log(l) {}
	void nodeChanged(const NodeHealth& status) override {
		std::string line = "node " + status.name + " " + std::string(stateName(status.state));
		if (status.reason) line += " " + std::string(failureCode(status.reason->code)) + ": " + status.reason->detail;
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
	std::size_t waiting() {
		const std::scoped_lock lock(mutex);
		return queue.size();
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

struct TestNotes final : NodeNoteListener {
	Log& log;
	explicit TestNotes(Log& l) : log(l) {}
	void nodeSays(const NodeName& node, const std::string& text) override {
		log.push_back("note " + node + " " + text);
	}
};

struct Fixture {
	Log log;
	TestBlocks blocks{log};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	TestReceivers receivers{log};
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue}; // only for close(), the proof stopAll takes
	QueueExecutor mainLoop;
	TestNotes notes{log};
	NodeHost host{AudioFormat{44100, 256}, blocks, handoffs, cards, receivers, mainLoop};
	int allGone = 0; // how often stopAll's callback has run
	Fixture() {
		host.subscribe(listener);
		host.subscribeNotes(notes);
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

} // namespace

TEST_CASE("A node gets its port block, then starts, prepares its audio code and drives the block", "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;

	f.add("rec", rec);

	CHECK(f.log == Log{"node rec starting", "make rec 2 inputs", "start 44100", "handoff rec 2 channels",
	                   "prepare 44100/256", "drive code"});
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
	                   "insides destroyed", "handoff destroyed", "node rec failed start-failed: cannot open /nope"});
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
	CHECK(f.log == Log{"node rec stopping", "block destroyed", "release", "insides destroyed",
	                   "node rec gone dropped=0", "all gone"});
}

TEST_CASE("stopAll takes a node down in order: block, release, handoffs, insides, gone", "[node_host]") {
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
	      Log{"handoff destroyed", "handoff destroyed", "insides destroyed", "node rec gone dropped=7", "all gone"});
	CHECK(f.allGone == 1);
	CHECK(f.host.health().empty());
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

TEST_CASE("An update restarts the insides behind the same port block, letting the old config go last", "[node_host]") {
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

	SECTION("the old insides go, then their config, then the new config starts") {
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "insides destroyed", "config A destroyed", "start 44100 B",
		                   "handoff rec 2 channels", "prepare 44100/256", "drive code", "node rec running"});
	}
	SECTION("two updates during one restart start only the latest config") {
		auto c = std::make_shared<TestConfig>(f.log, "C");
		f.update("rec", std::move(c));
		CHECK(f.log == Log{"config B destroyed"}); // B never started; nothing else happened
		f.log.clear();
		f.handoffs.finishOne(0);
		CHECK(f.log == Log{"handoff destroyed", "insides destroyed", "config A destroyed", "start 44100 C",
		                   "prepare 44100/256", "drive code", "node rec running"});
	}
	SECTION("a removal during a restart stops the node and starts nothing") {
		f.host.nodeRemoved("rec");
		CHECK(f.log == Log{"config B destroyed", "node rec stopping", "block destroyed"});
		f.log.clear();
		f.handoffs.finishOne(2);
		CHECK(f.log == Log{"handoff destroyed", "insides destroyed", "config A destroyed", "node rec gone dropped=2"});
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
	                   "drive code", "node rec running"});
	f.stopAll();
}

TEST_CASE("A node's sound cards are made once its port block is ready, and it runs when every card "
          "is ready",
          "[node_host]") {
	Fixture f;
	f.add("motu", withCards(f.log));

	// No card before the block is ready, whatever the insides asked for.
	CHECK(f.log == Log{"node motu starting", "make motu 2 inputs", "start 44100", "prepare 44100/256", "drive code"});
	CHECK(f.cards.events.empty());
	f.log.clear();

	f.blocks.events.at(0)->ready();
	CHECK(f.log ==
	      Log{"card motu.playback of motu Pro24 PB1,PB2 at 44100", "card motu.capture of motu Pro24 CAP1 at 44100"});
	REQUIRE(f.cards.events.size() == 2);
	CHECK(f.host.health().at(0).state == NodeState::starting);
	f.log.clear();

	f.cards.events[1]->ready();
	CHECK(f.log.empty());
	f.cards.events[0]->ready();
	CHECK(f.log == Log{"node motu running"});
	f.log.clear();

	SECTION("a lost card fails the node with the card's detail") {
		f.cards.events[0]->lost("unplugged");
		CHECK(f.log == Log{"node motu failed engine-refused: unplugged"});
		f.log.clear();
	}
	SECTION("a restart remakes the cards at once, the block being ready") {
		f.update("motu", withCards(f.log, "B"));
		CHECK(f.log == Log{"node motu starting", "drive nothing", "release", "card motu.playback destroyed",
		                   "card motu.capture destroyed", "insides destroyed", "start 44100 B", "prepare 44100/256",
		                   "drive code", "card motu.playback of motu Pro24 PB1,PB2 at 44100",
		                   "card motu.capture of motu Pro24 CAP1 at 44100"});
		REQUIRE(f.cards.events.size() == 4);
		f.cards.events[2]->ready();
		f.cards.events[3]->ready();
		CHECK(f.log.back() == "node motu running");
		f.log.clear();
	}

	// Teardown takes the cards before the insides.
	f.stopAll();
	const auto at = [&](const std::string& line) {
		return std::find(f.log.begin(), f.log.end(), line) - f.log.begin();
	};
	CHECK(f.log.front() == "node motu stopping");
	CHECK(at("release") < at("card motu.playback destroyed"));
	CHECK(at("card motu.capture destroyed") < at("insides destroyed"));
	CHECK(f.log.back() == "all gone");
}

TEST_CASE("A node asks for sound cards only inside start", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	f.add("motu", config);
	REQUIRE(config->grant != nullptr);
	CHECK_THROWS_AS(config->grant->soundCard(motuOut), std::logic_error);
	f.blocks.events.at(0)->ready();
	CHECK(f.cards.events.empty());
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

	// An update tears the old insides down and starts nothing; no starting status.
	f.update("motu", withCards(f.log, "B"));
	f.handoffs.finishOne(0);
	CHECK(f.log == Log{"drive nothing", "release", "card motu.playback destroyed", "card motu.capture destroyed",
	                   "handoff close", "config B destroyed", "handoff destroyed", "insides destroyed",
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
	      Log{"node noise stopping", "block destroyed", "release", "insides destroyed", "node noise gone dropped=0"});
	REQUIRE(f.host.health().size() == 1);
	CHECK(f.host.health()[0].name == "rec");
	CHECK(f.host.health()[0].state == NodeState::running);
	f.log.clear();

	SECTION("a removed node still stopping comes before a newer node of the same name") {
		f.host.nodeRemoved("rec");
		f.add("rec", std::make_shared<TestConfig>(f.log));
		const auto status = f.host.health();
		REQUIRE(status.size() == 2);
		CHECK(status[0].state == NodeState::stopping);
		CHECK(status[1].state == NodeState::starting);
		// An update names the newer one.
		f.update("rec", std::make_shared<TestConfig>(f.log));
		f.handoffs.finishOne(5);
		CHECK(std::ranges::find(f.log, "node rec gone dropped=5") != f.log.end());
		CHECK(f.log.back() == "drive code"); // the newer one waited for it
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
          "request unchanged",
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
	CHECK(made.blockPorts == fppStream.blockPorts);
	CHECK(f.host.health().at(0).state == NodeState::starting);
	f.log.clear();

	f.receivers.events[0]->ready();
	CHECK(f.log == Log{"node fpp running"});
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
	CHECK(f.log == Log{"card mix.playback of mix Pro24 PB1,PB2 at 44100", "receiver mix.rtp of mix"});
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
	CHECK(f.log == Log{"node mix running"});
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
	CHECK(f.log == Log{"handoff destroyed", "insides destroyed", "config A destroyed", "start 44100 B",
	                   "prepare 44100/256", "drive code", "receiver fpp.rtp of fpp"});
	REQUIRE(f.receivers.events.size() == 2); // made once more, not twice
	CHECK(f.blocks.events.size() == 1);      // the block stayed
	f.log.clear();
	f.receivers.events[1]->ready();
	CHECK(f.log == Log{"node fpp running"});
	f.stopAll();
}

TEST_CASE("An outside node lost before the node ran fails it with engine-refused and the outside "
          "node's detail",
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
	f.stopAll();
}

TEST_CASE("A node asks for an RTP receiver only inside start, and at most once per start", "[node_host][rtp]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	f.add("fpp", config);
	REQUIRE(config->grant != nullptr);
	CHECK_THROWS_AS(config->grant->rtpReceiver(fppStream), std::logic_error);

	auto twice = std::make_shared<TestConfig>(f.log);
	twice->receivers = {fppStream, fppStream};
	CHECK_THROWS_AS(f.add("fpp2", twice), std::logic_error);
	for (auto* events : f.blocks.events) events->ready();
	CHECK(f.receivers.events.empty());
}

TEST_CASE("Restart and removal destroy the receiver before the handoffs close and the insides go", "[node_host][rtp]") {
	Fixture f;
	auto a = withReceiver(f.log, "A");
	a->handoffs = 1;
	f.add("fpp", std::move(a));
	f.blocks.events.at(0)->ready();
	f.receivers.events.at(0)->ready();
	f.log.clear();

	const auto at = [&](const std::string& line) {
		return std::find(f.log.begin(), f.log.end(), line) - f.log.begin();
	};
	SECTION("restart") {
		f.update("fpp", withReceiver(f.log, "B"));
		f.handoffs.finishOne(0);
	}
	SECTION("removal") {
		f.host.nodeRemoved("fpp");
		f.handoffs.finishOne(0);
		CHECK(f.log.back() == "node fpp gone dropped=0");
	}
	const auto destroyed = at("receiver fpp.rtp destroyed");
	CHECK(destroyed < static_cast<std::ptrdiff_t>(f.log.size()));
	CHECK(at("release") < destroyed);
	CHECK(destroyed < at("handoff close"));
	CHECK(destroyed < at("insides destroyed"));
	f.log.clear();
	f.stopAll();
	while (!f.handoffs.pendingCloses.empty()) f.handoffs.finishOne(0);
	CHECK(f.allGone == 1);
}

namespace {

/// Insides with their own thread, as a network node's: close() only records `closed`, which the
/// test calls later, as that thread would; events are kept past start().
struct ThreadedInsides final : NodeInsides {
	Log& log;
	TestAudioCode code;
	TestConsumer consumer;
	std::move_only_function<void()> closed;
	explicit ThreadedInsides(Log& l) : log(l), code(l) {}
	~ThreadedInsides() override { log.push_back("insides destroyed"); }
	AudioCode& audioCode() override { return code; }
	void close(std::move_only_function<void()> call) override {
		log.push_back("insides close");
		closed = std::move(call);
	}
};

struct ThreadedConfig final : NodeConfig {
	Log& log;
	Ports ports_{{{Direction::input, "in", {"a"}}}};
	int handoffs = 0;
	mutable ThreadedInsides* insides = nullptr;
	mutable InsidesEvents* events = nullptr;
	explicit ThreadedConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		auto made = std::make_unique<ThreadedInsides>(log);
		for (int i = 0; i < handoffs; ++i) grant.handoff(2, 1024, made->consumer);
		insides = made.get();
		events = &grant.events();
		return made;
	}
};

} // namespace

TEST_CASE("The insides' events are granted inside start only", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<TestConfig>(f.log);
	f.add("rec", config);
	CHECK_THROWS_AS(config->grant->events(), std::logic_error);
	f.stopAll();
}

TEST_CASE("Insides close after every handoff has closed, and go only after they say closed", "[node_host]") {
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
	CHECK(f.log == Log{"handoff destroyed", "insides close"});
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty()); // nothing goes until they say so

	// From the insides' own thread: it crosses to the main loop, so nothing happens there yet.
	std::thread([&] { config->insides->closed(); }).join();
	CHECK(f.log.empty());
	CHECK(f.allGone == 0);
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"insides destroyed", "node net gone dropped=7", "all gone"});
}

TEST_CASE("A restart waits for the old insides to close before it starts the new ones", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.update("net", next);
	CHECK(f.log == Log{"node net starting", "drive nothing", "release", "insides close"});
	f.log.clear();

	std::thread([&] { old->insides->closed(); }).join();
	CHECK(f.log.empty());
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"insides destroyed", "start 44100 next", "prepare 44100/256", "drive code", "node net running"});
	f.stopAll();
}

TEST_CASE("A name added back while its old insides are closing starts only once they are gone", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.host.nodeRemoved("net");
	f.log.clear();

	auto next = std::make_shared<TestConfig>(f.log, "next");
	f.add("net", next);
	// Its block comes at once; its insides wait.
	CHECK(f.log == Log{"node net starting", "make net 2 inputs"});
	f.log.clear();

	SECTION("then they start, after the old node is gone") {
		std::thread([&] { old->insides->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"insides destroyed", "node net gone dropped=0", "start 44100 next", "prepare 44100/256",
		                   "drive code"});
	}
	SECTION("an update while it waits replaces what will start, and nothing starts yet") {
		auto newer = std::make_shared<TestConfig>(f.log, "newer");
		f.update("net", newer);
		CHECK(f.log.empty());
		std::thread([&] { old->insides->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"insides destroyed", "node net gone dropped=0", "start 44100 newer", "prepare 44100/256",
		                   "drive code"});
	}
	SECTION("removed again while it waits, it never starts") {
		f.host.nodeRemoved("net");
		CHECK(f.log == Log{"node net stopping", "block destroyed", "node net gone dropped=0"});
		f.log.clear();
		std::thread([&] { old->insides->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"insides destroyed", "node net gone dropped=0"});
	}
	SECTION("removed and added back while it waits, only the newest starts") {
		f.host.nodeRemoved("net");
		f.log.clear();
		auto newest = std::make_shared<TestConfig>(f.log, "newest");
		f.add("net", newest);
		f.log.clear();
		std::thread([&] { old->insides->closed(); }).join();
		f.mainLoop.runPosted();
		CHECK(f.log == Log{"insides destroyed", "node net gone dropped=0", "start 44100 newest", "prepare 44100/256",
		                   "drive code"});
	}
	f.stopAll();
}

TEST_CASE("A name added back after its old node has gone starts at once", "[node_host]") {
	Fixture f;
	f.add("net", std::make_shared<TestConfig>(f.log));
	f.host.nodeRemoved("net");
	f.log.clear();
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	CHECK(f.log ==
	      Log{"node net starting", "make net 2 inputs", "start 44100 next", "prepare 44100/256", "drive code"});
	f.stopAll();
}

TEST_CASE("Stopping while a name waits for its old insides stops both, in order", "[node_host]") {
	Fixture f;
	auto old = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", old);
	f.blocks.events.at(0)->ready();
	f.host.nodeRemoved("net");
	f.add("net", std::make_shared<TestConfig>(f.log, "next"));
	f.log.clear();
	f.stopAll();
	CHECK(f.allGone == 0);
	std::thread([&] { old->insides->closed(); }).join();
	f.mainLoop.runPosted();
	CHECK(f.allGone == 1);
	CHECK(std::ranges::count(f.log, std::string("start 44100 next")) == 0);
}

TEST_CASE("What the insides say reaches the note listeners on the main loop, in order, and nothing "
          "after closed",
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
	config->insides->closed();
	events->say("too late");
	f.mainLoop.runPosted();
	CHECK(f.log == Log{"note net closing", "insides destroyed", "node net gone dropped=0", "all gone"});
}

TEST_CASE("A note still queued when its node has gone reaches nothing", "[node_host]") {
	Fixture f;
	auto config = std::make_shared<ThreadedConfig>(f.log);
	f.add("net", config);
	f.host.nodeRemoved("net");
	config->insides->closed();
	f.mainLoop.runPosted();
	// The node is pruned at the next add; what its insides left behind holds no pointer to it.
	f.add("other", std::make_shared<TestConfig>(f.log));
	f.log.clear();
	f.mainLoop.runPosted();
	CHECK(f.log.empty());
	f.stopAll();
}

TEST_CASE("Insides lost from their own thread fail a running node with start-failed, and its block "
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
	CHECK(f.log == Log{"node net stopping", "block destroyed", "release", "insides close"});
	config->insides->closed();
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
	config->insides->closed();
	f.mainLoop.runPosted();
	CHECK(std::ranges::find(f.log, std::string("node net running")) != f.log.end());
	f.stopAll();
}
