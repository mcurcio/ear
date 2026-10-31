// The node framework driven through fake engine factories: how a node starts, what makes it
// running or failed, and the order stopAll takes everything down in
// (docs/design/m1-white-to-file.md section 3, S2 criterion 5), on batch 2's interfaces
// (docs/design/batch2.md section 4.1: the port block is made first and the insides drive it;
// C1 criteria 7-11: restart, sound cards, a lost block, removal). Every step a fake engine would
// take later (ready, lost, a handoff closing, a card ready or lost) is an event the test sends.
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <memory>
#include <string>
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
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>>,
	             std::uint32_t) noexcept override {}
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

/// A node config whose start asks for `handoffs` handoffs and `cards` sound cards, then succeeds
/// or fails. A config with a tag logs its start and its destruction with that tag.
struct TestConfig final : NodeConfig {
	Log& log;
	std::string tag;
	Ports ports_{{"FL", "FR"}, {}, {"CAP1"}, {"PB1", "PB2"}};
	int handoffs = 0;
	std::vector<SoundCardRequest> cards;
	bool fail = false;
	mutable NodeGrant* grant = nullptr;   // the grant start() was given, kept to misuse it later
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
		log.push_back("make " + node + " " + std::to_string(ports.inputs.size()) + " inputs");
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

/// Interface stage (docs/design/rtp.md section 7): no test asks for a receiver yet; slice R1
/// replaces this with a fake that records and reports like TestCards.
struct NoReceivers final : RtpReceiverFactory {
	std::unique_ptr<OutsideNode> make(const NodeName&, const NodeName&, const RtpReceiverRequest&,
	                                  OutsideNodeEvents&) override {
		FAIL("an RTP receiver was made");
		return nullptr;
	}
};

struct TestListener final : NodeStatusListener {
	Log& log;
	explicit TestListener(Log& l) : log(l) {}
	void nodeChanged(const NodeStatus& status) override {
		std::string line = "node " + status.name + " " + std::string(stateName(status.state));
		if (status.reason) line += " " + std::string(failureCode(status.reason->code)) + ": " + status.reason->detail;
		log.push_back(line);
	}
	void nodeGone(const NodeName& node, std::uint64_t dropped) override {
		log.push_back("node " + node + " gone dropped=" + std::to_string(dropped));
	}
};

struct Fixture {
	Log log;
	TestBlocks blocks{log};
	TestHandoffs handoffs{log};
	TestCards cards{log};
	NoReceivers receivers;
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};   // only for close(), the proof stopAll takes
	NodeHost host{AudioFormat{44100, 256}, blocks, handoffs, cards, receivers};
	int allGone = 0;   // how often stopAll's callback has run
	Fixture() { host.subscribe(listener); }
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

TEST_CASE("A node gets its port block, then starts, prepares its audio code and drives the block",
          "[node_host]") {
	Fixture f;
	auto rec = std::make_shared<TestConfig>(f.log);
	rec->handoffs = 1;

	f.add("rec", rec);

	CHECK(f.log == Log{"node rec starting", "make rec 2 inputs", "start 44100", "handoff rec 2 channels",
	                   "prepare 44100/256", "drive code"});
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].state == NodeState::starting);

	f.stopAll();
	f.handoffs.finishOne(0);
}

TEST_CASE("A node becomes running when its port block is ready", "[node_host]") {
	Fixture f;
	f.add("rec", std::make_shared<TestConfig>(f.log));
	f.log.clear();

	f.blocks.events.at(0)->ready();
	f.blocks.events.at(0)->ready();   // at most once; a repeat changes nothing

	CHECK(f.log == Log{"node rec running"});
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].state == NodeState::running);
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
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].state == NodeState::failed);
	REQUIRE(f.host.status()[0].reason.has_value());
	CHECK(f.host.status()[0].reason->code == FailureReason::Code::startFailed);

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
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].state == NodeState::stopping);

	f.handoffs.finishOne(4);
	CHECK(f.log == Log{"handoff destroyed", "handoff destroyed", "insides destroyed", "node rec gone dropped=7",
	                   "all gone"});
	CHECK(f.allGone == 1);
	CHECK(f.host.status().empty());
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
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].name == "rec");

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

TEST_CASE("An update restarts the insides behind the same port block, letting the old config go last",
          "[node_host]") {
	Fixture f;
	{
		auto a = std::make_shared<TestConfig>(f.log, "A");
		a->handoffs = 1;
		f.add("rec", std::move(a));   // the host holds the only reference
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
		CHECK(f.log == Log{"config B destroyed"});   // B never started; nothing else happened
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
		CHECK(f.host.status().empty());
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

	CHECK(f.log == Log{"node rec starting", "drive nothing", "config A destroyed", "start 44100 B",
	                   "prepare 44100/256", "drive code", "node rec running"});
	f.stopAll();
}

TEST_CASE("A node's sound cards are made once its port block is ready, and it runs when every card is ready",
          "[node_host]") {
	Fixture f;
	f.add("motu", withCards(f.log));

	// No card before the block is ready, whatever the insides asked for.
	CHECK(f.log == Log{"node motu starting", "make motu 2 inputs", "start 44100", "prepare 44100/256", "drive code"});
	CHECK(f.cards.events.empty());
	f.log.clear();

	f.blocks.events.at(0)->ready();
	CHECK(f.log == Log{"card motu.playback of motu Pro24 PB1,PB2 at 44100", "card motu.capture of motu Pro24 CAP1 at 44100"});
	REQUIRE(f.cards.events.size() == 2);
	CHECK(f.host.status().at(0).state == NodeState::starting);
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
	const auto at = [&](const std::string& line) { return std::find(f.log.begin(), f.log.end(), line) - f.log.begin(); };
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
	CHECK(f.host.status().at(0).state == NodeState::failed);
	f.stopAll();
}

TEST_CASE("A lost port block is final: the node stays failed through updates and later failures",
          "[node_host]") {
	Fixture f;
	{
		auto a = withCards(f.log, "A");
		a->handoffs = 1;
		f.add("motu", std::move(a));
	}
	f.blocks.events.at(0)->ready();
	f.cards.events.at(0)->ready();
	f.cards.events.at(1)->ready();
	REQUIRE(f.host.status().at(0).state == NodeState::running);
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
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].state == NodeState::failed);
	REQUIRE(f.host.status()[0].reason.has_value());
	CHECK(f.host.status()[0].reason->detail == "driver gone");
	CHECK(f.cards.events.size() == 2);
	f.log.clear();

	// Removal still ends with nodeGone.
	f.host.nodeRemoved("motu");
	CHECK(f.log == Log{"node motu stopping", "block destroyed", "node motu gone dropped=0"});
	CHECK(f.host.status().empty());
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
	CHECK(f.log == Log{"node noise stopping", "block destroyed", "release", "insides destroyed",
	                   "node noise gone dropped=0"});
	REQUIRE(f.host.status().size() == 1);
	CHECK(f.host.status()[0].name == "rec");
	CHECK(f.host.status()[0].state == NodeState::running);
	f.log.clear();

	SECTION("a removed node still stopping comes before a newer node of the same name") {
		f.host.nodeRemoved("rec");
		f.add("rec", std::make_shared<TestConfig>(f.log));
		const auto status = f.host.status();
		REQUIRE(status.size() == 2);
		CHECK(status[0].state == NodeState::stopping);
		CHECK(status[1].state == NodeState::starting);
		// An update names the newer one.
		f.update("rec", std::make_shared<TestConfig>(f.log));
		f.handoffs.finishOne(5);
		CHECK(f.log.back() == "node rec gone dropped=5");
		REQUIRE(f.host.status().size() == 1);
		CHECK(f.host.status()[0].state == NodeState::starting);
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
