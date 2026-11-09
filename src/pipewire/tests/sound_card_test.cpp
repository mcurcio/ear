// AlsaSoundCards on the real in-process engine (docs/design/batch2.md sections 4.3 and 5;
// acceptance criteria E2(1)-(5); docs/design/node-framework.md section 10, slice 7: the internal
// clock always drives and a linked card follows it). The cards are ALSA PCMs from a test ALSA
// configuration (tests/alsa/alsa.conf): card "Test" is alsa-lib's file PCM over its null PCM, so
// what it is sent lands in a file; any other card id cannot open. The test card has no clock, so
// as a follower its rate match would never settle: it is made with clocklessCardProperties(),
// which turns matching off (bit-exact). Every wait is event-gated: the main loop runs until the
// events a test waits for have arrived; the deadline only fails the test. The engine, the Waiter,
// the block events and the registry come from engine_harness.hpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/core/pass_through.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "temp_folder.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

/// The test ALSA configuration, with card Test's file in `folder`. Returns the file.
std::filesystem::path useTestAlsa(const ear::test::TempFolder& folder) {
	setenv("ALSA_CONFIG_PATH", EAR_ALSA_TEST_DIR "/alsa.conf", 1);
	setenv("EAR_ALSA_OUT", folder.path().c_str(), 1);
	return folder.path() / "card.raw";
}

CardId cardId(std::string_view text) {
	auto card = CardId::parse(text);
	REQUIRE(card.has_value());
	return *card;
}

/// Counts its cycles, the driver and discontinuities the framework told it, and the latency
/// downstream of its first output. Writes zeros everywhere, except a ramp of exact floats
/// (k * 2^-24, k from 1) on output `rampOn` when it is set.
class Code final : public AudioCode {
public:
	Code(Waiter& waiter, std::optional<std::size_t> rampOn = std::nullopt) : waiter_(waiter), rampOn_(rampOn) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		if (rampOn_ && *rampOn_ < outputs.size()) {
			for (std::uint32_t i = 0; i < frames; ++i) {
				outputs[*rampOn_][i] = static_cast<float>(++next_) * kStep;
			}
		}
		if (calls_.load(std::memory_order_relaxed) > 0 && cycle.discontinuity)
			discontinuities_.fetch_add(1, std::memory_order_relaxed);
		driver_.store(cycle.driverId, std::memory_order_relaxed);
		downstream_.store(cycle.downstreamOf(0).maxFrames, std::memory_order_relaxed);
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	/// Cycles after the first that the framework marked discontinuous: the clock jumped or another driver took over.
	std::uint64_t discontinuities() const { return discontinuities_.load(std::memory_order_acquire); }
	/// The driver of the latest cycle, by the framework.
	std::uint32_t driver() const { return driver_.load(std::memory_order_acquire); }
	/// The latency downstream of output 0 in the latest cycle.
	std::uint32_t downstream() const { return downstream_.load(std::memory_order_acquire); }
	static constexpr float kStep = 1.0F / 16777216.0F;

private:
	Waiter& waiter_;
	std::optional<std::size_t> rampOn_;
	std::uint32_t next_ = 0;
	std::atomic<std::uint64_t> calls_{0};
	std::atomic<std::uint64_t> discontinuities_{0};
	std::atomic<std::uint32_t> driver_{0};
	std::atomic<std::uint32_t> downstream_{0};
};

/// What a card tells, with what the registry showed at the moment it was told.
class CardEvents final : public OutsideNodeEvents {
public:
	CardEvents(Waiter& waiter, Registry& registry, NodeName engineNode)
		: waiter_(waiter), registry_(registry), engineNode_(std::move(engineNode)) {}
	void ready() override {
		++readyCount;
		if (onReady) onReady();
		waiter_.wake();
	}
	void lost(std::string detail) override {
		lostDetail = std::move(detail);
		lostInsideMake = insideMake;
		nodePresentAtLost = registry_.present(engineNode_);
		waiter_.wake();
		if (onLost) onLost();
	}
	std::function<void()> onReady; // told inside ready(), to read the engine at that moment
	std::function<void()> onLost;  // told last inside lost(), as NodeHost destroys a failed configuration's card
	int readyCount = 0;
	bool insideMake = false;
	std::optional<std::string> lostDetail;
	bool lostInsideMake = false;
	bool nodePresentAtLost = false;

private:
	Waiter& waiter_;
	Registry& registry_;
	NodeName engineNode_;
};

/// Two blocks: "card" (two inputs, the inside output bank "to_card": "to_card.1", "to_card.2") and "other"
/// (one input), each driven by its own code, both running on the dummy driver, and clockless cards.
struct Graph {
	explicit Graph(std::optional<std::size_t> rampOn = std::nullopt)
		: out(useTestAlsa(alsaOut)), engine(makeEngine()), waiter(*engine), registry(*engine, waiter),
		  blocks(*engine, bell), cards(*engine, clocklessCardProperties()), cardCode(waiter, rampOn), otherCode(waiter),
		  cardBlockEvents(waiter), otherBlockEvents(waiter) {
		cardBlock = blocks.make("card",
		                        Ports{{{Direction::input, "play", {"1", "2"}},
		                               {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}},
		                        kFormat, cardBlockEvents);
		REQUIRE(ear::testing::driveNow(*cardBlock, &cardCode));
		otherBlock = blocks.make("other", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, otherBlockEvents);
		REQUIRE(ear::testing::driveNow(*otherBlock, &otherCode));
		REQUIRE(waiter.until([&] {
			return cardBlockEvents.isReady && otherBlockEvents.isReady && registry.id("Dummy-Driver") &&
			       registry.props("card") && registry.props("other");
		}));
		dummy = *registry.id("Dummy-Driver");
		REQUIRE(blocksDrivenBy(dummy));
	}
	~Graph() {
		ear::testing::silenceOrAbort(*cardBlock);
		ear::testing::silenceOrAbort(*otherBlock);
	}

	std::unique_ptr<OutsideNode> makeCard(const CardId& id, CardEvents& events) {
		events.insideMake = true;
		auto card = cards.make(OutsideTarget{"card.playback", "card", {"to_card.1", "to_card.2"}, kFormat},
		                       SoundCardRequest{id, CardDirection::playback, "to_card"}, events);
		events.insideMake = false;
		return card;
	}

	/// Both blocks' code ran `cycles` more times.
	bool bothRun(std::uint64_t cycles) {
		const std::uint64_t card = cardCode.calls() + cycles;
		const std::uint64_t other = otherCode.calls() + cycles;
		return waiter.until([&] { return cardCode.calls() >= card && otherCode.calls() >= other; });
	}
	/// Both blocks' node.driver-id, read afresh, is `driver`.
	bool blocksDrivenBy(std::uint32_t driver) {
		return registry.reread({"card", "other"}) && registry.driverId("card") == driver &&
		       registry.driverId("other") == driver;
	}

	ear::test::TempFolder alsaOut{"card"}; // declared before `out`, so removed after the engine
	std::filesystem::path out;
	std::unique_ptr<Engine> engine;
	Waiter waiter;
	Registry registry;
	NoBell bell;
	FilterPortBlocks blocks;
	AlsaSoundCards cards;
	Code cardCode;
	Code otherCode;
	BlockEvents cardBlockEvents;
	BlockEvents otherBlockEvents;
	std::unique_ptr<PortBlock> cardBlock;
	std::unique_ptr<PortBlock> otherBlock;
	std::uint32_t dummy = 0;
};

/// Interleaved 32-bit float frames of `channels` channels from card Test's file.
std::vector<float> readFrames(const std::filesystem::path& file, std::size_t channels) {
	std::ifstream in(file, std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const std::size_t frameBytes = channels * sizeof(float);
	std::vector<float> samples(bytes.size() / frameBytes * channels);
	std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));
	return samples;
}

} // namespace

TEST_CASE("A playback card becomes ready, every channel's link active, and then follows: the internal clock drives "
          "it and every port block",
          "[cards][clock]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	// Ready must come from the last link's ACTIVE. This connection hears each link's state first on
	// the lifecycle's own proxy (bound when the link was made) and then on the registry's binding
	// (PipeWire tells a link's resources in the order they were bound, on one ordered socket), so
	// at ready the registry has heard every other link ACTIVE and not yet the last one; ready on
	// an earlier link would find fewer.
	std::optional<std::size_t> activeAtReady;
	events.onReady = [&] {
		const auto node = graph.registry.id("card.playback");
		activeAtReady = node ? graph.registry.activeLinksOf(*node) : 0;
	};
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	CHECK(events.readyCount == 1);
	CHECK(activeAtReady == std::optional<std::size_t>(1)); // of 2
	CHECK(graph.bothRun(8));
	CHECK(graph.blocksDrivenBy(graph.dummy));
	REQUIRE(graph.registry.reread({"card.playback"}));
	CHECK(graph.registry.driverId("card.playback") == graph.dummy);
	CHECK(graph.cardCode.driver() == graph.dummy); // what the framework tells the code: the same driver
	CHECK(graph.cardCode.discontinuities() == 0);  // the card joining moved no clock
}

TEST_CASE("A card running alone holds no port block: they stay on the dummy until it is ready", "[cards][clock]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	// The card links to a block that does not exist, so it runs alone and never becomes ready.
	// The moment the registry shows it running, the blocks are read afresh: the engine answers
	// those reads before it hears of the card's failed links.
	bool sampled = false;
	std::optional<std::map<std::string, std::string>> cardProps;
	graph.registry.onNodeInfo = [&](const NodeName& name) {
		if (sampled || name != "card.playback" || graph.registry.state(name) != PW_NODE_STATE_RUNNING) {
			return;
		}
		sampled = true;
		cardProps = *graph.registry.props(name);
		graph.registry.rebind({"card", "other"});
	};
	const auto card = graph.cards.make(OutsideTarget{"card.playback", "nowhere", {"to_card.1", "to_card.2"}, kFormat},
	                                   SoundCardRequest{cardId("Test"), CardDirection::playback, "to_card"}, events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	graph.registry.onNodeInfo = nullptr;
	CHECK(events.readyCount == 0);
	REQUIRE(sampled);
	REQUIRE(graph.waiter.until([&] { return graph.registry.props("card") && graph.registry.props("other"); }));
	CHECK(graph.registry.driverId("card").value_or(SPA_ID_INVALID) == graph.dummy);
	CHECK(graph.registry.driverId("other").value_or(SPA_ID_INVALID) == graph.dummy);
	CHECK_FALSE(cardProps->contains(PW_KEY_NODE_GROUP));
}

TEST_CASE("What a port block sends to inside output to_card.2 lands on the card's channel 2", "[cards][channels]") {
	Graph graph(1); // the ramp on the card block's second output: to_card.2
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	// The file PCM writes in chunks: check, at each cycle of the card's clock, for 64 blocks.
	constexpr std::uintmax_t kWanted = 64 * kBlock * 2 * sizeof(float);
	REQUIRE(graph.waiter.until([&] {
		std::error_code error;
		const auto size = std::filesystem::file_size(graph.out, error);
		return !error && size >= kWanted;
	}));

	const auto samples = readFrames(graph.out, 2);
	std::size_t first = samples.size();
	for (std::size_t i = 1; i < samples.size(); i += 2) {
		if (samples[i] != 0.0F) {
			first = i;
			break;
		}
	}
	REQUIRE(first + 2 * 16 * kBlock < samples.size());
	std::size_t leftNonZero = 0;
	for (std::size_t i = 0; i < samples.size(); i += 2) {
		if (samples[i] != 0.0F) ++leftNonZero;
	}
	// Every ramp sample arrives once, in order: each non-zero sample is the last one plus a step.
	// A cycle in which the card gets no block in time plays one whole block of silence and loses
	// nothing (seen about 1 run in 10 without realtime scheduling: 256 zeros mid-stream, then
	// the ramp goes on where it stopped). Any other zero run is a break.
	std::size_t breaks = 0;
	std::size_t rampSamples = 1;
	std::size_t silentBlocks = 0;
	float last = samples[first];
	std::size_t zeroRun = 0;
	for (std::size_t i = first + 2; i < samples.size(); i += 2) {
		if (samples[i] == 0.0F) {
			++zeroRun;
			continue;
		}
		if (zeroRun % kBlock != 0) ++breaks;
		silentBlocks += zeroRun / kBlock;
		zeroRun = 0;
		if (samples[i] != last + Code::kStep) ++breaks;
		last = samples[i];
		++rampSamples;
	}
	INFO("whole silent blocks: " << silentBlocks);
	CHECK(leftNonZero == 0);
	CHECK(breaks == 0);
	CHECK(rampSamples >= 16 * kBlock);
	CHECK(samples[first] >= Code::kStep);
}

TEST_CASE("A card that cannot open is lost after make returns, its node already gone, and the "
          "blocks keep running",
          "[cards][lost]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Absent"), events);
	CHECK_FALSE(events.lostDetail.has_value()); // never inside make()
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.lostInsideMake);
	CHECK(events.readyCount == 0);
	CHECK_FALSE(events.lostDetail->empty());
	// PipeWire 1.4.2's adapter reads the PCM's formats while it makes the node, so a card that
	// cannot open never becomes an engine node: the factory refuses it.
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(graph.blocksDrivenBy(graph.dummy));
	CHECK(graph.bothRun(16));
}

// node_host.hpp's contract (docs/design/node-lifecycle.md 4.2's T14): NodeHost tears a failed configuration down inside
// the lost() that failed it, so the outside node is destroyed inside its own callback.
TEST_CASE("A card lost before it was ready may be destroyed inside its own lost(), and the blocks keep running",
          "[cards][lost]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	std::unique_ptr<OutsideNode> card;
	events.onLost = [&] { card.reset(); };
	card = graph.makeCard(cardId("Absent"), events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(card == nullptr);
	CHECK(events.readyCount == 0);
	CHECK(graph.blocksDrivenBy(graph.dummy));
	CHECK(graph.bothRun(16));
}

TEST_CASE("A card that loses a link while it follows is lost with its node gone, and leaves the clock alone: every "
          "block keeps the internal clock with no discontinuity while the card object still exists",
          "[cards][lost][clock]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	const std::uint32_t cardNode = *graph.registry.id("card.playback");
	REQUIRE(graph.blocksDrivenBy(graph.dummy));
	REQUIRE(graph.bothRun(4));

	const auto links = graph.registry.linksOf(cardNode);
	REQUIRE(links.size() == 2);
	REQUIRE(graph.registry.destroy(links.front()));
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(graph.blocksDrivenBy(graph.dummy));
	CHECK(graph.bothRun(16));
	CHECK(graph.cardCode.driver() == graph.dummy);
	CHECK(graph.otherCode.driver() == graph.dummy);
	// A loaded host's stall makes the internal clock resync, which is a discontinuity of its own (rtp.md erratum 3);
	// the card's loss makes none.
	CHECK(graph.cardCode.discontinuities() == 0);
	CHECK(graph.otherCode.discontinuities() == 0);
	CHECK(card != nullptr); // nobody destroyed it
}

namespace {

/// The framework's pass-through between a card block's banks, with the latency downstream of its inside output
/// recorded each cycle.
class RecordingPassThrough final : public AudioCode {
public:
	RecordingPassThrough(const Ports& ports, const std::vector<BankPair>& pairs, Waiter& waiter)
		: code_(makePassThrough(ports, pairs)), waiter_(waiter), toCard_(ports.bank("to_card").first) {}
	void prepare(const AudioFormat& format) override { code_->prepare(format); }
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		code_->process(inputs, outputs, frames, cycle);
		downstream_.store(cycle.downstreamOf(toCard_).maxFrames, std::memory_order_relaxed);
		least_.store(cycle.downstreamOf(toCard_).minFrames, std::memory_order_relaxed);
		waiter_.wake();
	}
	void release() noexcept override { code_->release(); }
	std::uint32_t downstream() const { return downstream_.load(std::memory_order_relaxed); }
	std::uint32_t least() const { return least_.load(std::memory_order_relaxed); }

private:
	std::unique_ptr<AudioCode> code_;
	Waiter& waiter_;
	std::size_t toCard_;
	std::atomic<std::uint32_t> downstream_{0};
	std::atomic<std::uint32_t> least_{0};
};

/// A link by node and port names, as a route's would be by port ids.
pw_proxy* linkByName(Engine& engine, const char* fromNode, const char* fromPort, const char* toNode,
                     const char* toPort) {
	pw_properties* props = pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, fromNode, PW_KEY_LINK_OUTPUT_PORT, fromPort,
	                                         PW_KEY_LINK_INPUT_NODE, toNode, PW_KEY_LINK_INPUT_PORT, toPort, nullptr);
	auto* link = static_cast<pw_proxy*>(
		pw_core_create_object(engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
	pw_properties_free(props);
	return link;
}

} // namespace

TEST_CASE("A following card's latency reaches its block's inside bank, and the pass-through carries it to the block "
          "upstream of the card block",
          "[cards][latency]") {
	Graph graph;
	const Ports cardPorts{
		{{Direction::input, "play", {"1", "2"}}, {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}};
	const std::vector<BankPair> pairs{{"play", "to_card"}};
	RecordingPassThrough pass(cardPorts, pairs, graph.waiter);
	pass.prepare(kFormat);
	graph.cardBlock->carryLatency(LatencyPlan{pairs, {}});
	REQUIRE(ear::testing::driveNow(*graph.cardBlock, &pass));
	struct Silenced { // before `pass` goes, however the test ends
		PortBlock& block;
		~Silenced() { ear::testing::silenceOrAbort(block); }
	} silenced{*graph.cardBlock};

	Code sourceCode(graph.waiter);
	BlockEvents sourceEvents(graph.waiter);
	auto source = graph.blocks.make("source", Ports{{{Direction::output, "out", {"1", "2"}}}}, kFormat, sourceEvents);
	REQUIRE(ear::testing::driveNow(*source, &sourceCode));
	REQUIRE(graph.waiter.until([&] { return sourceEvents.isReady; }));
	std::vector<pw_proxy*> links{linkByName(*graph.engine, "source", "out.1", "card", "play.1"),
	                             linkByName(*graph.engine, "source", "out.2", "card", "play.2")};

	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	// ALSA's playback node reports a quantum and its headroom (alsa-pcm-sink.c, alsa-pcm.c): at least one block.
	REQUIRE(graph.waiter.until([&] { return pass.downstream() >= kBlock && sourceCode.downstream() > 0; }));
	CHECK(pass.least() == pass.downstream());
	CHECK(graph.waiter.until([&] { return sourceCode.downstream() == pass.downstream(); }));
	INFO("the card's latency: " << pass.downstream() << " frames");
	CHECK(pass.downstream() < 8 * kBlock);

	// A trap of PipeWire 1.4.2 (filter.c, handle_latency): when a port's last link goes, the engine clears the port's
	// latency with a null param, which a filter under custom latency is never told. So the card gone, its block keeps
	// the latency the card last reported until another peer reports one. Harmless while nothing is linked there (what
	// the block sends is heard nowhere); a card made again reports afresh. A PipeWire that tells it fails this check:
	// then the block can forget a lost peer's latency (docs/design/node-framework.md, slice 7 as built).
	const std::uint32_t reported = pass.downstream();
	CHECK(graph.waiter.until([&] {
		const auto node = graph.registry.id("card.playback");
		return node.has_value() && !graph.registry.linksOf(*node).empty();
	}));
	REQUIRE(graph.registry.destroy(graph.registry.linksOf(*graph.registry.id("card.playback")).front()));
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	const std::uint64_t later = sourceCode.calls() + 16;
	REQUIRE(graph.waiter.until([&] { return sourceCode.calls() >= later; }));
	CHECK(pass.downstream() == reported);
	CHECK(sourceCode.downstream() == reported);

	CardEvents again(graph.waiter, graph.registry, "card.playback");
	const auto second = graph.makeCard(cardId("Test"), again);
	REQUIRE(graph.waiter.until([&] { return again.readyCount > 0 || again.lostDetail.has_value(); }));
	REQUIRE(again.lostDetail.value_or("") == "");
	CHECK(graph.waiter.until([&] { return pass.downstream() == reported && sourceCode.downstream() == reported; }));

	for (pw_proxy* link : links) pw_proxy_destroy(link);
	REQUIRE(ear::testing::driveNow(*source, nullptr));
}
