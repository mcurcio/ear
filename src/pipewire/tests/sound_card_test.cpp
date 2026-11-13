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
#include "cycle_judge.hpp"
#include "inducers.hpp"
#include "temp_folder.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;
using ear::test::StallLength;
using ear::test::StallPlan;

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

/// Counts its cycles and records each in its ledger, with the facts its block stated (docs/engine.md#graphcycle), for a
/// judge; keeps the driver the framework told it and the latency downstream of its first output. Writes zeros
/// everywhere, except a ramp of exact floats (k * 2^-24, k from 1) on output `rampOn` when it is set, with the ramp's
/// first k of each cycle. Given stalls (a judge's), once armed its ramp stalls the data thread as their plan says,
/// counted from the first armed cycle, and it notes which ledger cycles they were.
class Code final : public AudioCode {
public:
	explicit Code(Waiter& waiter, std::optional<std::size_t> rampOn = std::nullopt, ear::test::Stalls* stalls = nullptr)
		: waiter_(waiter), rampOn_(rampOn), ledger_(kCycles), firstK_(kCycles), stalls_(stalls) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		const std::size_t n = ledger_.count();              // this thread alone records
		if (n < ledger_.capacity()) firstK_[n] = next_ + 1; // before record() publishes the cycle
		if (rampOn_ && *rampOn_ < outputs.size()) {
			for (std::uint32_t i = 0; i < frames; ++i) {
				outputs[*rampOn_][i] = static_cast<float>(++next_) * kStep;
			}
		}
		const std::optional<std::size_t> recorded = ledger_.record(cycle, frames);
		driver_.store(cycle.driverId, std::memory_order_relaxed);
		downstream_.store(cycle.downstreamOf(0).maxFrames, std::memory_order_relaxed);
		calls_.fetch_add(1, std::memory_order_release);
		// The stalled cycle is cut short straight after it (docs/testing.md#the-cycle-judge).
		if (stalls_ != nullptr && armed_.load(std::memory_order_acquire) && (*stalls_)(armedCycles_++, frames, kRate) &&
		    recorded) {
			const std::size_t at = stalledCount_.load(std::memory_order_relaxed);
			if (at < stalledAt_.size()) {
				stalledAt_[at] = *recorded;
				stalledCount_.store(at + 1, std::memory_order_release);
			}
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	/// The driver of the latest cycle, by the framework.
	std::uint32_t driver() const { return driver_.load(std::memory_order_acquire); }
	/// The latency downstream of output 0 in the latest cycle.
	std::uint32_t downstream() const { return downstream_.load(std::memory_order_acquire); }
	static constexpr float kStep = 1.0F / 16777216.0F;
	static constexpr std::size_t kCycles = 1 << 14; // about 87 s of cycles

	/// From its next cycle on, the ramp stalls as its plan says.
	void arm() { armed_.store(true, std::memory_order_release); }
	/// Stalls made in its recorded cycles, and the ledger cycle the `i`-th was made in (below stalledCount()).
	std::size_t stalledCount() const { return stalledCount_.load(std::memory_order_acquire); }
	std::size_t stalledAt(std::size_t i) const { return stalledAt_[i]; }
	/// The cycles it ran, with their facts; of cycle `n`, the ramp's first k.
	const ear::test::CycleLedger& ledger() const { return ledger_; }
	std::uint64_t firstK(std::size_t n) const { return firstK_[n]; }

private:
	Waiter& waiter_;
	std::optional<std::size_t> rampOn_;
	ear::test::CycleLedger ledger_;
	std::vector<std::uint64_t> firstK_;
	ear::test::Stalls* stalls_;
	std::atomic<bool> armed_{false};
	std::size_t armedCycles_ = 0;
	std::array<std::size_t, 16> stalledAt_{};
	std::atomic<std::size_t> stalledCount_{0};
	std::uint32_t next_ = 0;
	std::atomic<std::uint64_t> calls_{0};
	std::atomic<std::uint32_t> driver_{0};
	std::atomic<std::uint32_t> downstream_{0};
};

/// Every cycle `code` ran so far, through the one judge (docs/testing.md#the-cycle-judge): its first its drive (and its
/// block's start, when the code was driven before the block's first cycle), each after it judged, or excused where the
/// driver cut what it missed. Another driver, a pause or any jump the driver did not cut fails. The cycles from ledger
/// index `from` on (the event a test judges: a card's ready, its loss) have a judge of their own, which must judge 16
/// boundaries there and expects nothing, so the planned count is never met by cycles before the event. It stalls
/// nothing: the card's clock tests judge the clock a card leaves the blocks on, and the excuse path is proven every run
/// by the ramp onto channel 2, through the same Code (which judges its own block through this too).
ear::test::JudgeReport judgeClock(const Code& code, std::size_t from = 0) {
	const FrameClock clock(kRate);
	ear::test::CycleJudge before(ear::test::JudgePlan{.judged = 1, .induce = StallPlan{}, .expected = {.drives = 1}},
	                             clock, nullptr);
	ear::test::CycleJudge after(ear::test::JudgePlan{.judged = 16,
	                                                 .induce = StallPlan{},
	                                                 .expected = from == 0
	                                                                 ? ear::test::JudgePlan::Expected{.drives = 1}
	                                                                 : ear::test::JudgePlan::Expected{.starts = 0}},
	                            clock, nullptr);
	const std::span<const ear::test::LedgerCycle> cycles = code.ledger().cycles();
	for (std::size_t n = 0; n < cycles.size(); ++n) (n < from ? before : after).at(cycles[n].facts, cycles[n].frames);
	ear::test::JudgeReport report = after.report();
	const ear::test::JudgeReport earlier = before.report();
	if (report.firstFailure.empty()) report.firstFailure = earlier.firstFailure;
	report.failed += earlier.failed;
	return report;
}

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
	explicit Graph(std::optional<std::size_t> rampOn = std::nullopt, Faults* faults = nullptr,
	               std::optional<ear::test::JudgePlan> plan = std::nullopt)
		: out(useTestAlsa(alsaOut)), engine(makeEngine(faults)), waiter(*engine),
		  judge(plan ? std::make_unique<ear::test::CycleJudge>(*plan, FrameClock(kRate), &engine->cuts()) : nullptr),
		  registry(*engine, waiter), blocks(*engine, bell), cards(*engine, clocklessCardProperties()),
		  cardCode(waiter, rampOn, judge ? &judge->stalls() : nullptr), otherCode(waiter), cardBlockEvents(waiter),
		  otherBlockEvents(waiter) {
		cardBlock = blocks.make("card",
		                        Ports{{{Direction::input, "play", {"1", "2"}},
		                               {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}},
		                        kFormat, cardBlockEvents);
		otherBlock = blocks.make("other", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, otherBlockEvents);
		// The engine's record numbers every cycle from its start, so every cycle a code runs states its graph cycle.
		REQUIRE(waiter.until([&] { return cardBlockEvents.isReady && otherBlockEvents.isReady; }));
		REQUIRE(ear::testing::driveNow(*cardBlock, &cardCode));
		REQUIRE(ear::testing::driveNow(*otherBlock, &otherCode));
		REQUIRE(waiter.until([&] {
			return registry.id("Dummy-Driver") && registry.props("card") && registry.props("other") &&
			       cardCode.calls() > 0 && otherCode.calls() > 0;
		}));
		dummy = *registry.id("Dummy-Driver");
		REQUIRE(blocksDrivenBy(dummy));
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
	/// The ramp's judge, given a plan: the codes call its stalls, so it outlives them.
	std::unique_ptr<ear::test::CycleJudge> judge;
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

/// Card Test's file read as it grows, the ramp of a Code on channel 2 (its k * 2^-24) and channel 1 silent, judged
/// against the ramp's ledger through the one judge (docs/testing.md#the-cycle-judge): a card runs no code of ear's, so
/// its own facts are not seen, and the judge answers for it from the engine's record of the ramp's cycles. Across a
/// block boundary each non-zero sample is the last one plus a step (a boundary judged), except where whole blocks of
/// the ramp are missing between them, each excused only if the driver cut the cycle the ramp wrote it in. A cycle in
/// which the card gets no block plays one whole block of silence and loses nothing (256 zeros mid-stream, then the ramp
/// goes on where it stopped): allowed only where the ramp's own block missed that many cut cycles there
/// (silenceExplained). Any other zero run, and anything else, is a break.
class RampReader {
public:
	RampReader(const Code& code, ear::test::CycleJudge& judge) : code_(code), judge_(judge) {}
	/// The samples read so far, `samples` the whole file again: only what is past the last read is judged.
	void read(const std::vector<float>& samples) {
		const ear::test::CycleLedger& ledger = code_.ledger();
		const std::size_t cycles = ledger.count();
		const auto endK = [&](std::size_t n) { return code_.firstK(n) + ledger[n].frames; };
		// The ramp's cycle that wrote k, if its ledger holds it (firstK rises cycle by cycle).
		const auto cycleOf = [&](std::uint64_t k) -> std::optional<std::size_t> {
			std::size_t lo = 0, hi = cycles;
			while (lo < hi) {
				const std::size_t mid = lo + (hi - lo) / 2;
				if (code_.firstK(mid) <= k) lo = mid + 1;
				else hi = mid;
			}
			if (lo == 0 || k >= endK(lo - 1)) return std::nullopt;
			return lo - 1;
		};
		for (; left_ < samples.size(); left_ += 2)
			if (samples[left_] != 0.0F) ++leftNonZero;
		if (!last_) {
			while (at_ < samples.size() && samples[at_] == 0.0F) at_ += 2;
			if (at_ >= samples.size()) return;
			last_ = kOf(samples[at_]);
			firstK = *last_;
			at_ += 2;
		}
		for (; at_ < samples.size(); at_ += 2) {
			if (samples[at_] == 0.0F) {
				++zeroRun_;
				continue;
			}
			const std::uint64_t k = kOf(samples[at_]);
			const std::optional<std::size_t> from = cycleOf(*last_), to = cycleOf(k);
			if (zeroRun_ % kBlock != 0) { // silence the card plays for a block it missed is whole blocks too
				breakAt(std::to_string(zeroRun_) + " zeros before k " + std::to_string(k));
			} else if (!from || !to) {
				breakAt("k " + std::to_string(k) + " after " + std::to_string(*last_) + ", outside the ramp's ledger");
			} else if (k == *last_ + 1 && zeroRun_ > 0 && !silenceExplained(*from, *to, zeroRun_ / kBlock)) {
				breakAt(std::to_string(zeroRun_ / kBlock) + " silent blocks before k " + std::to_string(k) +
				        " that no cut cycle of the ramp's block explains");
			} else if (k == *last_ + 1) {
				silentBlocks += zeroRun_ / kBlock;
				if (*to != *from) judge_.blockHeard();
			} else if (*to > *from + 1 && *last_ + 1 == endK(*from) && k == code_.firstK(*to)) {
				// The card lost those cycles: the judge excuses each only where the driver cut it.
				for (std::size_t c = *from + 1; c < *to; ++c)
					if (judge_.blockMissing(ledger[c].facts.graphCycle) == ear::test::Verdict::excused)
						excusedCycles.push_back(c);
			} else {
				breakAt("k " + std::to_string(k) + " after " + std::to_string(*last_));
			}
			zeroRun_ = 0;
			last_ = k;
			lastCycle = to.value_or(lastCycle);
		}
	}

	std::size_t breaks = 0;
	std::size_t silentBlocks = 0;
	std::size_t leftNonZero = 0;
	std::uint64_t firstK = 0;
	std::size_t lastCycle = 0;              // the ramp's cycle of the last sample heard
	std::vector<std::size_t> excusedCycles; // the ramp's cycles missing from the file, each run in a cut cycle
	std::string firstBreaks;                // the first few, for the failure's message

private:
	/// The card plays a whole block of silence for a cycle the ramp's block did not run (the driver cut it before the
	/// block, so the card got nothing that cycle) and the ramp goes on where it stopped: `blocks` of silence between the
	/// ramp's cycles `from` and `to` are explained only when `to` follows `from` and its block missed at least that many
	/// cycles, every one cut (a catch-up the judge excuses from the ramp's own facts).
	bool silenceExplained(std::size_t from, std::size_t to, std::size_t blocks) const {
		if (to != from + 1) return false;
		const CycleFacts& facts = code_.ledger()[to].facts;
		return facts.causes.has(CycleCause::catchUp) && facts.confirmed() && blocks <= facts.missed;
	}
	static std::uint64_t kOf(float sample) {
		return static_cast<std::uint64_t>(static_cast<double>(sample) * 16777216.0);
	}
	void breakAt(const std::string& what) {
		if (++breaks <= 4) firstBreaks += what + "; ";
	}

	const Code& code_;
	ear::test::CycleJudge& judge_;
	std::size_t left_ = 0; // the next channel 1 sample
	std::size_t at_ = 1;   // the next channel 2 sample
	std::optional<std::uint64_t> last_;
	std::size_t zeroRun_ = 0;
};

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
	std::size_t cardAtReady = 0, otherAtReady = 0; // the codes' ledger indices at ready, where the clock is judged from
	events.onReady = [&] {
		const auto node = graph.registry.id("card.playback");
		activeAtReady = node ? graph.registry.activeLinksOf(*node) : 0;
		cardAtReady = graph.cardCode.ledger().count();
		otherAtReady = graph.otherCode.ledger().count();
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
	// The card joining moved no clock: every cycle each code ran is judged, or excused where the driver cut what it
	// missed (a loaded host's stall, docs/engine.md#xruns-on-a-loaded-host-are-not-bugs), to a planned count from ready.
	CHECK(graph.waiter.until([&] {
		return judgeClock(graph.cardCode, cardAtReady).done() && judgeClock(graph.otherCode, otherAtReady).done();
	}));
	for (const auto& [code, from] :
	     {std::pair{&graph.cardCode, cardAtReady}, std::pair{&graph.otherCode, otherAtReady}}) {
		const ear::test::JudgeReport report = judgeClock(*code, from);
		INFO(report);
		CHECK(report.ok());
	}
}

// docs/architecture.md#phases-states-and-what-is-wanted: the join names the block by its global id, so beside an older
// block of the same name (one still going) it reaches only the block it was given.
TEST_CASE("A card joins its block by the block's id: beside an older block of the same name it reaches only the newer",
          "[cards][join]") {
	Graph graph;
	Code newerCode(graph.waiter);
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
	// The ramp on the card block's second output, to_card.2. The ramp's code and the card each count only the cycles
	// they run, and a cycle cut short is run by an upstream prefix of the graph only
	// (docs/engine.md#the-one-data-loop-and-invokes): one a host's stall cuts after the block moves the ramp on by a
	// block the card never plays. So within a block the ramp must be whole, and each block boundary is judged or each
	// missing block excused by the judge (RampReader), to a planned count. A host cuts cycles only by chance, so the ramp
	// makes the judge's stalls itself once the card is ready (one and a half cycles each, so the driver cuts the cycle
	// before the card ran it), and the excuse is taken, and proven, on every run. The file PCM writes in chunks, so the
	// file is read again as it grows.
	Graph graph(1, nullptr, ear::test::JudgePlan{.judged = 32});
	ear::test::CycleJudge& judge = *graph.judge;
	CardEvents events(graph.waiter, graph.registry, "card.playback");
	const auto card = graph.makeCard(cardId("Test"), events);
	REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	graph.cardCode.arm();
	const ear::test::CycleLedger& ledger = graph.cardCode.ledger();
	RampReader heard(graph.cardCode, judge);
	std::uintmax_t readAt = 0;
	const std::size_t planned = judge.report().stallsPlanned;
	const bool came = graph.waiter.until(
		[&] {
			std::error_code error;
			const auto size = std::filesystem::file_size(graph.out, error);
			if (!error && size >= readAt + kBlock * 2 * sizeof(float)) { // read again once it grew by a block
				readAt = size;
				heard.read(readFrames(graph.out, 2));
			}
			if (ledger.full() || judge.report().failed > 0 || heard.breaks > 0) return true;
			const std::size_t stalled = graph.cardCode.stalledCount();
			return judge.done() && stalled >= planned && heard.lastCycle > graph.cardCode.stalledAt(planned - 1) + 1;
		},
		30s);
	INFO(judge.report() << "; read " << readAt << " bytes; whole silent blocks: " << heard.silentBlocks
	                    << "; first breaks: " << heard.firstBreaks);
	REQUIRE(came);
	{
		INFO("the ramp filled its " << ledger.capacity() << " cycles first");
		REQUIRE_FALSE(ledger.full());
	}
	CHECK(heard.leftNonZero == 0);
	CHECK(heard.breaks == 0);
	CHECK(heard.firstK >= 1);
	CHECK(judge.report().ok());
	// And the ramp's own block lost nothing it is not excused for: a block that did not run a cycle would leave the
	// card a whole silent block, which the file alone cannot tell from a late one.
	const ear::test::JudgeReport own = judgeClock(graph.cardCode);
	INFO(own);
	CHECK(own.ok());
	// Every cycle the ramp stalled in is missing from the file, excused by the driver's word.
	REQUIRE(graph.cardCode.stalledCount() == planned);
	for (std::size_t i = 0; i < planned; ++i) {
		const std::size_t stalled = graph.cardCode.stalledAt(i);
		INFO("the ramp's cycle " << stalled << ", stalled on purpose");
		CHECK(std::ranges::find(heard.excusedCycles, stalled) != heard.excusedCycles.end());
	}
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
	std::size_t cardAtLoss = 0,
				otherAtLoss = 0; // the codes' ledger indices at the loss, where the clock is judged from
	events.onLost = [&] {
		cardAtLoss = graph.cardCode.ledger().count();
		otherAtLoss = graph.otherCode.ledger().count();
	};
	REQUIRE(graph.registry.destroy(links.front()));
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(graph.blocksDrivenBy(graph.dummy));
	CHECK(graph.bothRun(16));
	CHECK(graph.cardCode.driver() == graph.dummy);
	CHECK(graph.otherCode.driver() == graph.dummy);
	// The card's loss moves no block's clock: every cycle each code ran is judged, or excused where the driver cut what
	// it missed (a loaded host's stall, docs/engine.md#xruns-on-a-loaded-host-are-not-bugs), to a planned count from the
	// loss.
	CHECK(graph.waiter.until([&] {
		return judgeClock(graph.cardCode, cardAtLoss).done() && judgeClock(graph.otherCode, otherAtLoss).done();
	}));
	for (const auto& [code, from] :
	     {std::pair{&graph.cardCode, cardAtLoss}, std::pair{&graph.otherCode, otherAtLoss}}) {
		const ear::test::JudgeReport report = judgeClock(*code, from);
		INFO(report);
		CHECK(report.ok());
	}
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
