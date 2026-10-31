// AlsaSoundCards on the real in-process engine (docs/design/batch2.md sections 4.3 and 5;
// acceptance criteria E2(1)-(5)). The cards are ALSA PCMs from a test ALSA configuration
// (tests/alsa/alsa.conf): card "Test" is alsa-lib's file PCM over its null PCM, so what it is
// sent lands in a file; any other card id cannot open. Every wait is event-gated: the main loop
// runs until the events a test waits for have arrived; the deadline only fails the test. The
// engine, the Waiter, the block events and the registry come from engine_harness.hpp.
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
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "engine_harness.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

/// The test ALSA configuration, and a fresh folder for card Test's file. Returns the file.
std::filesystem::path useTestAlsa() {
  setenv("ALSA_CONFIG_PATH", EAR_ALSA_TEST_DIR "/alsa.conf", 1);
  std::string folder = (std::filesystem::temp_directory_path() / "ear-card-XXXXXX").string();
  REQUIRE(mkdtemp(folder.data()) != nullptr);
  setenv("EAR_ALSA_OUT", folder.c_str(), 1);
  return std::filesystem::path(folder) / "card.raw";
}

CardId cardId(std::string_view text) {
  auto card = CardId::parse(text);
  REQUIRE(card.has_value());
  return *card;
}

/// Counts its cycles. Writes zeros everywhere, except a ramp of exact floats (k * 2^-24, k
/// from 1) on output `rampOn` when it is set.
class Code final : public AudioCode {
public:
  Code(Waiter& waiter, std::optional<std::size_t> rampOn = std::nullopt)
      : waiter_(waiter), rampOn_(rampOn) {}
  void prepare(const AudioFormat&) override {}
  void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
               std::uint32_t frames) noexcept override {
    for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
    if (rampOn_ && *rampOn_ < outputs.size()) {
      for (std::uint32_t i = 0; i < frames; ++i) {
        outputs[*rampOn_][i] = static_cast<float>(++next_) * kStep;
      }
    }
    calls_.fetch_add(1, std::memory_order_release);
    waiter_.wake();
  }
  void release() noexcept override {}
  std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
  static constexpr float kStep = 1.0F / 16777216.0F;

private:
  Waiter& waiter_;
  std::optional<std::size_t> rampOn_;
  std::uint32_t next_ = 0;
  std::atomic<std::uint64_t> calls_{0};
};

/// What a card tells, with what the registry showed at the moment it was told.
class CardEvents final : public OutsideNodeEvents {
public:
  CardEvents(Waiter& waiter, Registry& registry, NodeName engineNode)
      : waiter_(waiter), registry_(registry), engineNode_(std::move(engineNode)) {}
  void ready() override {
    ++readyCount;
    waiter_.wake();
  }
  void lost(std::string detail) override {
    lostDetail = std::move(detail);
    lostInsideMake = insideMake;
    nodePresentAtLost = registry_.present(engineNode_);
    waiter_.wake();
  }
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

/// Two blocks: "card" (two inputs, two inside outputs "to_card_1", "to_card_2") and "other"
/// (one input), each driven by its own code, both running on the dummy driver.
struct Graph {
  explicit Graph(std::optional<std::size_t> rampOn = std::nullopt)
      : out(useTestAlsa()), engine(makeEngine()), waiter(*engine), registry(*engine, waiter),
        blocks(*engine, bell), cards(*engine), cardCode(waiter, rampOn), otherCode(waiter),
        cardBlockEvents(waiter), otherBlockEvents(waiter) {
    cardBlock = blocks.make("card", Ports{{"playback_1", "playback_2"}, {}, {}, {"to_card_1", "to_card_2"}},
                            kFormat, cardBlockEvents);
    cardBlock->drive(&cardCode);
    otherBlock = blocks.make("other", Ports{{"IN"}, {}}, kFormat, otherBlockEvents);
    otherBlock->drive(&otherCode);
    REQUIRE(waiter.until([&] {
      return cardBlockEvents.isReady && otherBlockEvents.isReady && registry.id("Dummy-Driver") &&
             registry.props("card") && registry.props("other");
    }));
    dummy = *registry.id("Dummy-Driver");
    REQUIRE(blocksDrivenBy(dummy));
  }
  ~Graph() {
    cardBlock->drive(nullptr);
    otherBlock->drive(nullptr);
  }

  std::unique_ptr<OutsideNode> makeCard(const CardId& id, CardEvents& events) {
    events.insideMake = true;
    auto card = cards.make("card.playback", "card",
                           SoundCardRequest{id, CardDirection::playback, {"to_card_1", "to_card_2"}},
                           kFormat, events);
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

}  // namespace

TEST_CASE("A playback card becomes ready and then drives every port block", "[cards][clock]") {
  Graph graph;
  CardEvents events(graph.waiter, graph.registry, "card.playback");
  const auto card = graph.makeCard(cardId("Test"), events);
  REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  CHECK(events.readyCount == 1);
  const std::uint32_t cardNode = *graph.registry.id("card.playback");
  CHECK(graph.blocksDrivenBy(cardNode));
  CHECK(graph.bothRun(8));
}

TEST_CASE("A card running alone holds no port block: they stay on the dummy until it is ready",
          "[cards][clock]") {
  Graph graph;
  CardEvents events(graph.waiter, graph.registry, "card.playback");
  // The card links to a block that does not exist, so it runs alone and never becomes ready.
  // The moment the registry shows it running, the blocks are read afresh: the engine answers
  // those reads before it hears of the card's failed links.
  bool sampled = false;
  std::optional<std::map<std::string, std::string>> cardProps;
  graph.registry.onNodeInfo = [&](const NodeName& name) {
    if (sampled || name != "card.playback" ||
        graph.registry.state(name) != PW_NODE_STATE_RUNNING) {
      return;
    }
    sampled = true;
    cardProps = *graph.registry.props(name);
    graph.registry.rebind({"card", "other"});
  };
  const auto card = graph.cards.make(
      "card.playback", "nowhere",
      SoundCardRequest{cardId("Test"), CardDirection::playback, {"to_card_1", "to_card_2"}},
      kFormat, events);
  REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
  graph.registry.onNodeInfo = nullptr;
  CHECK(events.readyCount == 0);
  REQUIRE(sampled);
  REQUIRE(graph.waiter.until([&] { return graph.registry.props("card") && graph.registry.props("other"); }));
  CHECK(graph.registry.driverId("card").value_or(SPA_ID_INVALID) == graph.dummy);
  CHECK(graph.registry.driverId("other").value_or(SPA_ID_INVALID) == graph.dummy);
  CHECK_FALSE(cardProps->contains(PW_KEY_NODE_GROUP));
}

TEST_CASE("What a port block sends to inside output to_card_2 lands on the card's channel 2",
          "[cards][channels]") {
  Graph graph(1);   // the ramp on the card block's second output: to_card_2
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
          "blocks keep running", "[cards][lost]") {
  Graph graph;
  CardEvents events(graph.waiter, graph.registry, "card.playback");
  const auto card = graph.makeCard(cardId("Absent"), events);
  CHECK_FALSE(events.lostDetail.has_value());   // never inside make()
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

TEST_CASE("A card that loses a link while it drives is lost with its node gone, and the dummy "
          "drives every block while the card object still exists", "[cards][lost]") {
  Graph graph;
  CardEvents events(graph.waiter, graph.registry, "card.playback");
  const auto card = graph.makeCard(cardId("Test"), events);
  REQUIRE(graph.waiter.until([&] { return events.readyCount > 0 || events.lostDetail.has_value(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  const std::uint32_t cardNode = *graph.registry.id("card.playback");
  REQUIRE(graph.blocksDrivenBy(cardNode));
  REQUIRE(graph.bothRun(4));

  const auto links = graph.registry.linksOf(cardNode);
  REQUIRE(links.size() == 2);
  REQUIRE(graph.registry.destroy(links.front()));
  REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
  CHECK_FALSE(events.nodePresentAtLost);
  CHECK(graph.blocksDrivenBy(graph.dummy));
  CHECK(graph.bothRun(16));
  CHECK(card != nullptr);   // nobody destroyed it
}
