// AlsaSoundCards on the real in-process engine (docs/engine.md#sound-cards; docs/engine.md#the-internal-clock: the
// internal clock always drives and a linked card follows it). The cards are ALSA PCMs from a test ALSA configuration
// (tests/alsa/alsa.conf): card "Test" is alsa-lib's file PCM over its null PCM, so what it is sent lands in a file; any
// other card id cannot open. The test card has no clock, so as a follower its rate match would never settle: it is made
// with clocklessCardProperties(), which turns matching off (bit-exact). Every wait is event-gated: the main loop runs
// until the events a test waits for have arrived; the deadline only fails the test. The engine, the Waiter, the block
// events and the registry come from engine_harness.hpp.
#include <algorithm>
#include <array>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>

#include "../callback_boundary.hpp"
#include "catch2/catch.hpp"
#include "ear/core/pass_through.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "../engine_properties.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "inducers.hpp"
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
/// (k * 2^-24, k from 1) on output `rampOn` when it is set; then it records each cycle in its
/// ledger, with the ramp's first k in it and the graph cycle it ran in, by `driver`.
class Code final : public AudioCode {
public:
	Code(Waiter& waiter, const DriverCycles& driver, std::optional<std::size_t> rampOn = std::nullopt)
		: waiter_(waiter), driverCycles_(driver), rampOn_(rampOn), ledger_(rampOn ? kRampCycles : 0),
		  firstK_(ledger_.capacity()), graphCycle_(ledger_.capacity()) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		const std::uint64_t graphCycle = driverCycles_.started(); // the cycle this one runs in; 0 before it listened
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		if (rampOn_ && *rampOn_ < outputs.size()) {
			const std::size_t n = ledger_.count(); // this thread alone records
			if (n < ledger_.capacity()) {          // before record() publishes the cycle
				firstK_[n] = next_ + 1;
				graphCycle_[n] = graphCycle;
			}
			for (std::uint32_t i = 0; i < frames; ++i) {
				outputs[*rampOn_][i] = static_cast<float>(++next_) * kStep;
			}
			ledger_.record(cycle, frames);
		}
		if (calls_.load(std::memory_order_relaxed) > 0 && cycle.discontinuity) {
			// A host's stall can cut short the cycles this block would have run, so its clock jumps by them: a
			// discontinuity of its own (docs/engine.md#xruns-on-a-loaded-host-are-not-bugs). It is excused only as such:
			// on the same driver, forward by exactly the cycles started since this code last ran, every one of them
			// cut short by the driver's own account. Anything else (a new driver, a step back, a jump over a cycle the
			// driver did not cut) is the graph's own; so is any discontinuity before the driver was listened to.
			const std::uint64_t skipped = graphCycle > lastGraphCycle_ ? graphCycle - lastGraphCycle_ - 1 : 0;
			bool cut = lastGraphCycle_ > 0 && cycle.driverId == lastDriver_ && skipped > 0 &&
			           cycle.position == lastEnd_ + skipped * frames;
			for (std::uint64_t g = lastGraphCycle_ + 1; cut && g < graphCycle; ++g) cut = driverCycles_.cutShort(g);
			(cut ? hostJumps_ : discontinuities_).fetch_add(1, std::memory_order_relaxed);
		}
		lastEnd_ = cycle.position + frames;
		lastDriver_ = cycle.driverId;
		lastGraphCycle_ = graphCycle;
		driver_.store(cycle.driverId, std::memory_order_relaxed);
		downstream_.store(cycle.downstreamOf(0).maxFrames, std::memory_order_relaxed);
		if (graphCycle > 0) watched_.store(true, std::memory_order_release);
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	/// Cycles after the first that the framework marked discontinuous for a reason of the graph's: another driver took
	/// over, the clock stepped back, or it jumped over cycles the driver did not cut short.
	std::uint64_t discontinuities() const { return discontinuities_.load(std::memory_order_acquire); }
	/// Cycles after the first that the framework marked discontinuous where this block's clock jumped over cycles the
	/// driver cut short (a host's stall: counted, never required).
	std::uint64_t hostJumps() const { return hostJumps_.load(std::memory_order_acquire); }
	/// It has run a cycle the driver's listener counted.
	bool watched() const { return watched_.load(std::memory_order_acquire); }
	/// The driver of the latest cycle, by the framework.
	std::uint32_t driver() const { return driver_.load(std::memory_order_acquire); }
	/// The latency downstream of output 0 in the latest cycle.
	std::uint32_t downstream() const { return downstream_.load(std::memory_order_acquire); }
	static constexpr float kStep = 1.0F / 16777216.0F;
	static constexpr std::size_t kRampCycles = 1 << 14; // about 87 s of cycles

	/// The ramp's cycles (none without a ramp); of cycle `n`, the ramp's first k and the graph cycle it ran in.
	const ear::test::CycleLedger& ledger() const { return ledger_; }
	std::uint64_t firstK(std::size_t n) const { return firstK_[n]; }
	std::uint64_t graphCycle(std::size_t n) const { return graphCycle_[n]; }

private:
	Waiter& waiter_;
	const DriverCycles& driverCycles_;
	std::optional<std::size_t> rampOn_;
	ear::test::CycleLedger ledger_;
	std::vector<std::uint64_t> firstK_;
	std::vector<std::uint64_t> graphCycle_;
	std::uint32_t next_ = 0;
	std::atomic<std::uint64_t> calls_{0};
	std::atomic<std::uint64_t> discontinuities_{0};
	std::atomic<std::uint64_t> hostJumps_{0};
	std::atomic<bool> watched_{false};
	std::uint64_t lastEnd_ = 0;        // data thread: the last cycle's end
	std::uint32_t lastDriver_ = 0;     // its driver
	std::uint64_t lastGraphCycle_ = 0; // and the graph cycle it ran in
	std::atomic<std::uint32_t> driver_{0};
	std::atomic<std::uint32_t> downstream_{0};
};

/// What a card tells, with what the registry showed at the moment it was told.
class CardEvents final : public EngineCompanionEvents {
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
	explicit Graph(std::optional<std::size_t> rampOn = std::nullopt, Faults* faults = nullptr)
		: out(useTestAlsa(alsaOut)), engine(makeEngine(faults)), waiter(*engine), driverCycles(*engine, 1 << 16),
		  registry(*engine, waiter), blocks(*engine, bell), cards(*engine, clocklessCardProperties()),
		  cardCode(waiter, driverCycles, rampOn), otherCode(waiter, driverCycles), cardBlockEvents(waiter),
		  otherBlockEvents(waiter) {
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
		// From here on every cycle each code runs names its graph cycle, so every discontinuity is judged.
		REQUIRE(waiter.until([&] { return cardCode.watched() && otherCode.watched(); }));
	}
	~Graph() {
		ear::testing::silenceOrAbort(*cardBlock);
		ear::testing::silenceOrAbort(*otherBlock);
	}

	std::unique_ptr<EngineCompanion> makeCard(const CardId& id, CardEvents& events) {
		events.insideMake = true;
		auto card = cards.make(
			CompanionTarget{
				"card.playback", "card", Direction::output, {"to_card.1", "to_card.2"}, kFormat, cardBlock->blockId()},
			SoundCardShape{id}, events);
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
	DriverCycles driverCycles; // before the codes, which read it, so it outlives them
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

/// What card Test's file shows of a Code's ramp on its channel 2, judged against the ramp's ledger and the driver.
struct RampHeard {
	std::size_t judged = 0;   // block boundaries heard on both sides, in order
	std::size_t unjudged = 0; // jumps across blocks that all ran in cycles the driver found cut short
	std::size_t breaks = 0;
	std::size_t silentBlocks = 0;
	std::size_t leftNonZero = 0;
	std::uint64_t firstK = 0;
	std::string firstBreaks; // the first few, for the failure's message
};

/// Interleaved two-channel `samples` from card Test's file, the ramp on channel 2 (its k * 2^-24) and channel 1 silent.
RampHeard judgeRamp(const std::vector<float>& samples, const Code& code, const DriverCycles& driver) {
	RampHeard heard;
	const ear::test::CycleLedger& ledger = code.ledger();
	const std::size_t cycles = ledger.count();
	const std::uint64_t started = driver.started(); // every cycle before this one has had its cut told
	const auto kOf = [](float sample) { return static_cast<std::uint64_t>(static_cast<double>(sample) * 16777216.0); };
	const auto endK = [&](std::size_t n) { return code.firstK(n) + ledger[n].frames; };
	// The ramp's cycle that wrote k, if its ledger holds it (firstK rises cycle by cycle).
	const auto cycleOf = [&](std::uint64_t k) -> std::optional<std::size_t> {
		std::size_t lo = 0, hi = cycles;
		while (lo < hi) {
			const std::size_t mid = lo + (hi - lo) / 2;
			if (code.firstK(mid) <= k) lo = mid + 1;
			else hi = mid;
		}
		if (lo == 0 || k >= endK(lo - 1)) return std::nullopt;
		return lo - 1;
	};
	// The ramp's cycles [from, to) all ran in graph cycles the driver found cut short.
	const auto allCut = [&](std::size_t from, std::size_t to) {
		for (std::size_t n = from; n < to; ++n) {
			const std::uint64_t cycle = code.graphCycle(n);
			if (cycle >= started || !driver.cutShort(cycle)) return false;
		}
		return true;
	};
	const auto breakAt = [&](const std::string& what) {
		if (++heard.breaks <= 4) heard.firstBreaks += what + "; ";
	};

	for (std::size_t i = 0; i < samples.size(); i += 2)
		if (samples[i] != 0.0F) ++heard.leftNonZero;
	std::size_t i = 1;
	while (i < samples.size() && samples[i] == 0.0F) i += 2;
	if (i >= samples.size()) return heard;
	std::uint64_t last = kOf(samples[i]);
	heard.firstK = last;
	std::size_t zeroRun = 0;
	for (i += 2; i < samples.size(); i += 2) {
		if (samples[i] == 0.0F) {
			++zeroRun;
			continue;
		}
		const std::uint64_t k = kOf(samples[i]);
		const std::optional<std::size_t> from = cycleOf(last), to = cycleOf(k);
		if (zeroRun % kBlock != 0) { // silence the card plays for a block it missed is whole blocks too
			breakAt(std::to_string(zeroRun) + " zeros before k " + std::to_string(k));
		} else if (!from || !to) {
			breakAt("k " + std::to_string(k) + " after " + std::to_string(last) + ", outside the ramp's ledger");
		} else if (k == last + 1) {
			heard.silentBlocks += zeroRun / kBlock;
			if (*to != *from) ++heard.judged;
		} else if (*to > *from + 1 && last + 1 == endK(*from) && k == code.firstK(*to) && allCut(*from + 1, *to)) {
			++heard.unjudged; // the card lost those cycles with the graph: nothing of the ramp's to judge
		} else {
			breakAt("k " + std::to_string(k) + " after " + std::to_string(last));
		}
		zeroRun = 0;
		last = k;
	}
	return heard;
}

} // namespace

TEST_CASE("A playback card becomes ready, every channel's link active, and then follows, the internal clock driving "
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
	INFO("discontinuities over cycles the driver cut short: " << graph.cardCode.hostJumps());
	CHECK(graph.cardCode.discontinuities() == 0); // the card joining moved no clock
}

// docs/architecture.md#phases-states-and-what-is-wanted: the join names the block by its global id, so beside an older
// block of the same name (one still going) it reaches only the block it was given.
TEST_CASE("A card joins its block by the block's id: beside an older block of the same name it reaches only the newer",
          "[cards][join]") {
	Graph graph;
	Code newerCode(graph.waiter, graph.driverCycles);
	BlockEvents newerEvents(graph.waiter);
	auto newer = graph.blocks.make("card",
	                               Ports{{{Direction::input, "play", {"1", "2"}},
	                                      {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}},
	                               kFormat, newerEvents);
	REQUIRE(ear::testing::driveNow(*newer, &newerCode));
	REQUIRE(graph.waiter.until([&] { return newerEvents.isReady && graph.registry.ids("card").size() == 2; }));
	const std::uint32_t olderId = graph.cardBlock->blockId();
	const std::uint32_t newerId = newer->blockId();
	REQUIRE(olderId != newerId);
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.cards.make(
		CompanionTarget{"card.playback", "card", Direction::output, {"to_card.1", "to_card.2"}, kFormat, newerId},
		SoundCardShape{cardId("Test")}, events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	CHECK(graph.registry.activeLinksOf(newerId) == 2);
	CHECK(graph.registry.linksOf(olderId).empty());
	ear::testing::silenceOrAbort(*newer);
}

TEST_CASE("A card running alone holds no port block: they stay on the dummy until it is ready", "[cards][clock]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	// The card's block has no id (none exists), so it runs alone and never becomes ready.
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
	const auto card = graph.cards.make(
		CompanionTarget{"card.playback", "nowhere", Direction::output, {"to_card.1", "to_card.2"}, kFormat},
		SoundCardShape{cardId("Test")}, events);
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
	// The card runs no code of ear's, so it keeps no ledger: the driver's own account of each cycle stands in for one.
	// The ramp names the graph cycle each of its cycles ran in, from before the card is made (Graph waits for it).
	const DriverCycles& driver = graph.driverCycles;
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");

	// The ramp's code and the card each count only the cycles they run, and a cycle cut short is run by an upstream
	// prefix of the graph only (docs/engine.md#the-one-data-loop-and-invokes): one a host's stall cuts after the block
	// moves the ramp on by a block the card never plays. So within a block the ramp must be whole, and across a block
	// boundary each non-zero sample is the last one plus a step, except where the blocks between them were all of
	// cycles the driver found cut short (counted, not judged). A cycle in which the card gets no block in time plays
	// one whole block of silence and loses nothing (seen about 1 run in 10 without realtime scheduling: 256 zeros
	// mid-stream, then the ramp goes on where it stopped); any other zero run is a break. The file PCM writes in
	// chunks, so the file is read again as it grows, to a planned count of boundaries judged.
	constexpr std::size_t kWanted = 32; // block boundaries heard on both sides
	const ear::test::CycleLedger& ledger = graph.cardCode.ledger();
	RampHeard heard;
	std::uintmax_t readAt = 0;
	const bool came = graph.waiter.until(
		[&] {
			std::error_code error;
			const auto size = std::filesystem::file_size(graph.out, error);
			if (!error && size >= readAt + kBlock * 2 * sizeof(float)) { // read again once it grew by a block
				readAt = size;
				heard = judgeRamp(readFrames(graph.out, 2), graph.cardCode, driver);
			}
			return heard.judged >= kWanted || ledger.full();
		},
		30s);
	INFO("judged " << heard.judged << " of " << kWanted << " boundaries in " << readAt
	               << " bytes; whole silent blocks: " << heard.silentBlocks << "; boundaries across blocks lost in "
	               << driver.cuts() << " cycles cut short, not judged: " << heard.unjudged
	               << "; first breaks: " << heard.firstBreaks);
	REQUIRE(came);
	{
		INFO("the ramp filled its " << ledger.capacity() << " cycles before " << kWanted << " boundaries were judged");
		REQUIRE_FALSE(ledger.full());
	}
	REQUIRE_FALSE(driver.overflowed());
	CHECK(heard.leftNonZero == 0);
	CHECK(heard.breaks == 0);
	CHECK(heard.firstK >= 1);
}

// docs/engine.md#the-quantum-guard: ear sets no force property on a card, and its test seam may not either.
TEST_CASE("A card's engine node carries no property that forces or locks the graph's quantum or rate, keeps the "
          "adapters' old quantum limit for its ALSA buffer, and the extra properties refuse every clock property",
          "[cards][quantum]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	REQUIRE(graph.registry.reread({"card.playback"}));
	const auto* props = graph.registry.props("card.playback");
	REQUIRE(props != nullptr);
	for (const char* key : {"node.force-quantum", "node.force-rate", "node.lock-quantum", "node.lock-rate"}) {
		INFO(key);
		CHECK_FALSE(props->contains(key));
	}
	// The card's ALSA buffer is set by its period size and count, not by the quantum limit (now the block size), so it
	// keeps what it had before the guard (docs/engine.md#the-quantum-guard).
	CHECK(props->at("clock.quantum-limit") == std::to_string(kBlock));
	CHECK(props->at("api.alsa.period-size") == std::to_string(kBlock));
	CHECK(props->at("api.alsa.period-num") == std::to_string(cardPeriods(kBlock)));
	CHECK(cardPeriods(kBlock) * kBlock / 2 == 32768); // a batch (USB) card: half-block periods, 4 x 8192 frames
	CHECK(props->at("priority.driver") == "1");
	CHECK_FALSE(props->contains("node.group"));
	CHECK_FALSE(props->contains("node.link-group"));
	// The seam refuses every key the property table sets on every card or forbids on one
	// (docs/engine.md#properties-ear-never-sets).
	std::vector<std::string> refused;
	for (const PropertyRow& row : engineProperties)
		if (row.home == Home::card) refused.emplace_back(row.key);
	for (const auto& [home, key] : forbidden)
		if (home == Home::card) refused.emplace_back(key);
	CHECK(refused.size() > 8);
	for (const std::string& key : refused) {
		INFO(key);
		CHECK_THROWS_AS(AlsaSoundCards(*graph.engine, AlsaSoundCards::Properties{{key, "1"}}), std::invalid_argument);
	}
	CHECK_NOTHROW(AlsaSoundCards(*graph.engine, clocklessCardProperties()));
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

namespace {

/// Writes one level on every output, every cycle.
class LevelCode final : public AudioCode {
public:
	explicit LevelCode(float level) : level_(level) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, level_);
	}
	void release() noexcept override {}

private:
	float level_;
};

std::uintmax_t fileSize(const std::filesystem::path& file) {
	std::error_code error;
	const auto size = std::filesystem::file_size(file, error);
	return error ? 0 : size;
}

} // namespace

// docs/engine.md#drive-and-fade: NodeHost fades every output a code writes before each planned
// silence (a stop, a restart, a disable, a removal: node_host_test.cpp shows each asks for the same fadeOut), inside
// outputs feeding a card included, so the card's speaker never clicks. Here the block's fade is followed to the
// card's own samples.
TEST_CASE("to_card's last 5 ms before a silence decay to zero, and its drive fades in", "[cards][fade]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");

	constexpr float kLevel = 0.5F;
	LevelCode level(kLevel);
	const std::array faded{true, true}; // to_card.1 and to_card.2, as NodeHost asks for a card's inside bank
	bool driven = false;
	graph.cardBlock->drive(DriveRequest{&level, faded}, [&](std::uint64_t) { driven = true; });
	REQUIRE(graph.waiter.until([&] { return driven; }));
	const std::uintmax_t atDrive = fileSize(graph.out); // the level code runs: let 16 blocks of it reach the card
	REQUIRE(graph.waiter.until([&] { return fileSize(graph.out) >= atDrive + 16 * kBlock * 2 * sizeof(float); }));
	bool fadedOut = false;
	graph.cardBlock->fadeOut([&] { fadedOut = true; });
	REQUIRE(graph.waiter.until([&] { return fadedOut; }));
	REQUIRE(ear::testing::driveNow(*graph.cardBlock, &graph.cardCode)); // the zeros code again: the silence's place
	// The card plays behind the graph by its buffer: wait until 32 blocks of zeros follow the fade in its file.
	REQUIRE(graph.waiter.until(
		[&] {
			const auto now = readFrames(graph.out, 2);
			const auto last = std::find_if(now.rbegin(), now.rend(), [](float sample) { return sample != 0.0F; });
			return last != now.rend() && static_cast<std::size_t>(last - now.rbegin()) >= 2 * 32 * kBlock;
		},
		10s));

	const auto samples = readFrames(graph.out, 2);
	constexpr std::size_t kFade = kRate * 5 / 1000; // 240 frames
	for (std::size_t channel = 0; channel < 2; ++channel) {
		INFO("to_card." << channel + 1);
		// Every non-zero sample, in order: a whole silent block the card may insert mid-stream changes none of them.
		std::vector<float> heard;
		std::size_t lastNonZero = 0;
		for (std::size_t i = channel; i < samples.size(); i += 2) {
			if (samples[i] == 0.0F) continue;
			heard.push_back(samples[i]);
			lastNonZero = i;
		}
		REQUIRE(heard.size() > 2 * kFade + kBlock);
		// The fade-in: frame k at k / N, from k = 1 (frame 0 is zero), then the level.
		for (std::size_t k = 1; k < kFade; ++k)
			CHECK(heard[k - 1] == kLevel * (static_cast<float>(k) / static_cast<float>(kFade)));
		CHECK(heard[kFade - 1] == kLevel);
		// The fade-out: its first frame at the level, frame k at (N - k) / N, the last non-zero one at 1 / N.
		const std::size_t tail = heard.size() - kFade;
		CHECK(heard[tail] == kLevel);
		for (std::size_t k = 1; k < kFade; ++k)
			CHECK(heard[tail + k] == kLevel * (static_cast<float>(kFade - k) / static_cast<float>(kFade)));
		// Zero from there on: the silence that followed never clicks back.
		CHECK(lastNonZero + 2 * 32 * kBlock < samples.size());
	}
}

// node_host.hpp's contract (T14, node_lifecycle.hpp): NodeHost tears a failed configuration down inside
// the lost() that failed it, so the engine companion is destroyed inside its own callback.
TEST_CASE("A card lost before it was ready may be destroyed inside its own lost(), and the blocks keep running",
          "[cards][lost]") {
	Graph graph;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	std::unique_ptr<EngineCompanion> card;
	events.onLost = [&] { card.reset(); };
	card = graph.makeCard(cardId("Absent"), events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(card == nullptr);
	CHECK(events.readyCount == 0);
	CHECK(graph.blocksDrivenBy(graph.dummy));
	CHECK(graph.bothRun(16));
}

TEST_CASE("A card that loses a link while it follows is lost with its node gone, and leaves the clock alone, every "
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
	// A loaded host's stall cuts cycles short, so a block's clock jumps over them, a discontinuity of its own
	// (docs/engine.md#xruns-on-a-loaded-host-are-not-bugs): each code tells those apart by the driver's own cuts and counts
	// them aside. The card's loss makes none of the graph's.
	INFO("discontinuities over cycles the driver cut short: " << graph.cardCode.hostJumps() << " and "
	                                                          << graph.otherCode.hostJumps());
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
	graph.cardBlock->carryLatency(LatencyPlan{.pairs = pairs});
	REQUIRE(ear::testing::driveNow(*graph.cardBlock, &pass));
	struct Silenced { // before `pass` goes, however the test ends
		PortBlock& block;
		~Silenced() { ear::testing::silenceOrAbort(block); }
	} silenced{*graph.cardBlock};

	Code sourceCode(graph.waiter, graph.driverCycles);
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
	// then the block can forget a lost peer's latency (docs/engine.md#latency-per-bank-pair).
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

// docs/architecture.md#faults-by-owner: a throw out of a card's body's own callback is ear's bug in state that is its
// node's alone: the body names its node and forwards the fault to the lifecycle, which tears the card down and tells
// it faulted (the boundary's injection seam throws inside the real callback, docs/testing.md#the-tools).
TEST_CASE("A card whose body's callback throws is torn down and told faulted, the report naming its node",
          "[cards][faults]") {
	FaultLog faults;
	Graph graph(std::nullopt, &faults);
	faults.onCaught = [&] { graph.waiter.wake(); };
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	boundary::throwInCallback("AlsaCardBody::onInfo");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(*events.lostDetail == "internal error in AlsaCardBody::onInfo: injected");
	CHECK_FALSE(events.nodePresentAtLost);
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "AlsaCardBody::onInfo");
	CHECK(faults.reports[0].node == "card");
	CHECK(faults.reports[0].owned);
	CHECK(graph.bothRun(8));
}

TEST_CASE("A card whose making throws after any registration leaves nothing registered", "[cards][faults]") {
	Graph graph;
	const std::size_t before = boundary::registrationsHeld();
	std::size_t made = 0;
	{
		CardEvents events(graph.waiter, graph.registry, "card.playback");
		const auto card = graph.makeCard(cardId("Test"), events);
		made = boundary::registrationsHeld() - before;
	}
	REQUIRE(made > 0);
	CHECK(boundary::registrationsHeld() == before);
	for (std::size_t n = 1; n <= made; ++n) {
		INFO("thrown after registration " << n << " of " << made);
		CardEvents unmade(graph.waiter, graph.registry, "card.playback");
		boundary::throwAfterRegistration(n);
		CHECK_THROWS_WITH(graph.makeCard(cardId("Test"), unmade), "injected after a registration");
		boundary::throwAfterRegistration(0);
		CHECK(boundary::registrationsHeld() == before);
	}
	CHECK(graph.bothRun(8));
}
