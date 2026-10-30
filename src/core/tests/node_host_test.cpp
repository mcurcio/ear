// The node framework driven through fake engine factories: how a node starts, what makes it
// running or failed, and the order stopAll takes everything down in
// (docs/design/m1-white-to-file.md section 3, S2 criterion 5), on batch 2's interfaces
// (docs/design/batch2.md section 4.1: the port block is made first and the insides drive it).
// Every step a fake engine would take later (ready, lost, a handoff closing) is an event the
// test sends. Slice C1 adds updates, removal and sound cards.
#include <functional>
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

/// A node config whose start asks for `handoffs` handoffs, then succeeds or fails.
struct TestConfig final : NodeConfig {
	Log& log;
	Ports ports_{{"FL", "FR"}, {}};
	int handoffs = 0;
	bool fail = false;
	explicit TestConfig(Log& l) : log(l) {}
	const Ports& ports() const override { return ports_; }
	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		const AudioFormat format = grant.format();
		log.push_back("start " + std::to_string(format.rate));
		auto insides = std::make_unique<TestInsides>(log);
		for (int i = 0; i < handoffs; ++i) grant.handoff(2, 1024, insides->consumer);
		if (fail) return std::unexpected(FailureReason{FailureReason::Code::startFailed, "cannot open /nope"});
		return insides;
	}
};

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

/// No test here asks for a sound card.
struct NoCards final : SoundCardFactory {
	std::unique_ptr<SoundCard> make(const NodeName&, const NodeName&, const SoundCardRequest&, const AudioFormat&,
	                                SoundCardEvents&) override {
		FAIL("a sound card was made");
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
	NoCards cards;
	TestListener listener{log};
	NodeCatalogue catalogue{{}};
	DesiredGraph graph{catalogue};   // only for close(), the proof stopAll takes
	NodeHost host{AudioFormat{44100, 256}, blocks, handoffs, cards};
	int allGone = 0;   // how often stopAll's callback has run
	Fixture() { host.subscribe(listener); }
	void add(const NodeName& name, std::shared_ptr<const NodeConfig> config) {
		host.nodeAdded(NodeSpec{name, "test", {}}, std::move(config));
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
