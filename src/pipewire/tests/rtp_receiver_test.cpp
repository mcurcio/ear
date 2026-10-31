// RtpReceivers on the real in-process engine (docs/design/rtp.md sections 4, 5 and 7.2; R1b
// criteria 1-6): PipeWire's rtp-source module loaded into EAR's own engine, no daemon, joined to a
// port block by the one outside-node lifecycle. The sender is the tests' own thread writing RTP to
// 127.0.0.1 (test/support/rtp_tone.hpp). Every wait is event-gated: the main loop runs until the
// events a test waits for have arrived (a receiver event, a registry change, the block's cycles);
// the deadline only fails the test.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "engine_harness.hpp"
#include "rtp_tone.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;
using ear::test::RtpPcm;
using ear::test::RtpSender;
using ear::test::RtpTone;

constexpr double kAmplitude = 0.5;

/// Records the block's inside inputs (from_rtp_1, from_rtp_2) into buffers made before it runs,
/// up to kCapacity frames, and notes the first frame that is not silent and every onset: a frame
/// that is not silent after at least kSilentRun silent frames (every channel exactly zero). Writes
/// zeros to every output.
class Recorder final : public AudioCode {
public:
  static constexpr std::size_t kCapacity = 20 * kRate;
  static constexpr std::size_t kSilentRun = kRate / 20;   // 50 ms

  Recorder(Waiter& waiter, std::size_t channels)
      : waiter_(waiter), channels_(channels, std::vector<float>(kCapacity, 0.0F)) {}
  void prepare(const AudioFormat&) override {}
  void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
               std::uint32_t frames) noexcept override {
    for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
    const std::size_t at = count_.load(std::memory_order_relaxed);
    const std::size_t take = std::min<std::size_t>(frames, kCapacity - at);
    for (std::size_t channel = 0; channel < channels_.size() && channel < inputs.size(); ++channel) {
      for (std::size_t i = 0; i < take; ++i) {
        const float sample = inputs[channel][i];
        channels_[channel][at + i] = sample;
        if (sample != 0.0F && first_.load(std::memory_order_relaxed) == kNone) {
          first_.store(at + i, std::memory_order_relaxed);
        }
      }
    }
    for (std::size_t i = 0; i < take; ++i) {
      const bool silent = std::ranges::all_of(channels_, [&](const auto& channel) { return channel[at + i] == 0.0F; });
      if (silent) {
        ++silentRun_;
        continue;
      }
      const std::size_t onsets = onsetCount_.load(std::memory_order_relaxed);
      if (silentRun_ >= kSilentRun && onsets < onsets_.size()) {
        onsets_[onsets] = at + i;
        onsetCount_.store(onsets + 1, std::memory_order_relaxed);
      }
      silentRun_ = 0;
    }
    count_.store(at + take, std::memory_order_release);
    waiter_.wake();
  }
  void release() noexcept override {}

  static constexpr std::size_t kNone = SIZE_MAX;
  std::size_t count() const { return count_.load(std::memory_order_acquire); }
  /// The first recorded frame of any channel that was not silent; kNone if none yet.
  std::size_t first() const {
    count();   // acquire: first_ was stored before count_
    return first_.load(std::memory_order_relaxed);
  }
  /// The first onset at or after frame `from`; kNone if none yet.
  std::size_t onsetFrom(std::size_t from) const {
    count();   // acquire: the onsets were stored before count_
    const std::size_t onsets = onsetCount_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < onsets; ++i) {
      if (onsets_[i] >= from) return onsets_[i];
    }
    return kNone;
  }
  /// Channel `channel`'s frames [from, from + frames); only frames already counted.
  std::span<const float> frames(std::size_t channel, std::size_t from, std::size_t frames) const {
    return std::span<const float>(channels_[channel]).subspan(from, frames);
  }

private:
  Waiter& waiter_;
  std::vector<std::vector<float>> channels_;
  std::atomic<std::size_t> count_{0};
  std::atomic<std::size_t> first_{kNone};
  std::size_t silentRun_ = kSilentRun;   // the data thread's only; the very first sound is an onset
  std::array<std::size_t, 64> onsets_{};
  std::atomic<std::size_t> onsetCount_{0};
};

/// What a receiver tells, with what the registry showed at the moment it was told.
class ReceiverEvents final : public OutsideNodeEvents {
public:
  ReceiverEvents(Waiter& waiter, Registry& registry) : waiter_(waiter), registry_(registry) {}
  void ready() override {
    ++readyCount;
    if (onReady) onReady();
    waiter_.wake();
  }
  void lost(std::string detail) override {
    lostDetail = std::move(detail);
    lostInsideMake = insideMake;
    nodePresentAtLost = registry_.present("fpp.rtp");
    linksAtLost = nodeId ? registry_.linksOf(*nodeId).size() : 0;
    waiter_.wake();
  }
  bool settled() const { return readyCount > 0 || lostDetail.has_value(); }

  std::function<void()> onReady;   // told inside ready(), to read the engine at that moment
  int readyCount = 0;
  bool insideMake = false;
  std::optional<std::uint32_t> nodeId;   // set by the test once known, for linksAtLost
  std::optional<std::string> lostDetail;
  bool lostInsideMake = false;
  bool nodePresentAtLost = false;
  std::size_t linksAtLost = 0;

private:
  Waiter& waiter_;
  Registry& registry_;
};

/// One port block "fpp" (outputs channel_1, channel_2; inside inputs from_rtp_1, from_rtp_2)
/// driven by a Recorder, running on the dummy driver, and the receivers' factory.
struct Graph {
  Graph()
      : engine(makeEngine()), waiter(*engine), registry(*engine, waiter), blocks(*engine, bell),
        receivers(*engine), recorder(waiter, 2), blockEvents(waiter) {
    block = blocks.make("fpp", Ports{{}, {"channel_1", "channel_2"}, {"from_rtp_1", "from_rtp_2"}, {}}, kFormat,
                        blockEvents);
    block->drive(&recorder);
    REQUIRE(waiter.until([&] { return blockEvents.isReady && registry.id("Dummy-Driver") && registry.props("fpp"); }));
    dummy = *registry.id("Dummy-Driver");
  }
  ~Graph() { block->drive(nullptr); }

  std::unique_ptr<OutsideNode> makeReceiver(std::uint16_t port, RtpEncoding encoding, std::uint32_t rate,
                                            ReceiverEvents& events) {
    events.insideMake = true;
    auto receiver = receivers.make("fpp.rtp", "fpp",
                                   RtpReceiverRequest{*RtpAddress::parse("0.0.0.0"), port, encoding, rate, 100,
                                                      {"from_rtp_1", "from_rtp_2"}},
                                   events);
    events.insideMake = false;
    return receiver;
  }

  /// The block's code ran `cycles` more times.
  bool runs(std::uint64_t cycles) {
    const std::size_t until = recorder.count() + cycles * kBlock;
    return waiter.until([&] { return recorder.count() >= until; });
  }

  /// The recorder's frames reached `frames` (the block's own clock moved on that far).
  bool recorded(std::size_t frames, std::chrono::seconds deadline = 10s) {
    return waiter.until([&] { return recorder.count() >= frames; }, deadline);
  }

  std::unique_ptr<Engine> engine;
  Waiter waiter;
  Registry registry;
  NoBell bell;
  FilterPortBlocks blocks;
  RtpReceivers receivers;
  Recorder recorder;
  BlockEvents blockEvents;
  std::unique_ptr<PortBlock> block;
  std::uint32_t dummy = 0;
};

/// Channel `channel` carries `wanted` Hz at kAmplitude and not `other` Hz, over `frames` frames
/// from `from`, checked in tenths of a second: the receiver steers its resampler by the packets'
/// arrival, so its rate wobbles by parts in ten thousand, which over a whole second moves a
/// sine's phase too far for one fit (a different frequency, such as an unresampled 816 Hz for
/// 750 Hz, still fails every tenth).
void checkTone(const Recorder& recorder, std::size_t channel, std::size_t from, std::size_t frames, double wanted,
               double other) {
  INFO("from_rtp_" << channel + 1 << ": " << wanted << " Hz, frames " << from << "+" << frames);
  const auto samples = recorder.frames(channel, from, frames);
  constexpr std::size_t tenth = kRate / 10;
  std::string profile;
  double worstAmplitude = kAmplitude;
  double worstResidual = 0.0;
  double worstOther = 0.0;
  for (std::size_t at = 0; at + tenth <= samples.size(); at += tenth) {
    const auto window = samples.subspan(at, tenth);
    const auto fit = ear::test::fitSine(window, wanted, kRate);
    profile += std::to_string(fit.amplitude) + "/" + std::to_string(fit.residual) + " ";
    if (std::abs(fit.amplitude - kAmplitude) > std::abs(worstAmplitude - kAmplitude)) worstAmplitude = fit.amplitude;
    worstResidual = std::max(worstResidual, fit.residual);
    worstOther = std::max(worstOther, ear::test::fitSine(window, other, kRate).amplitude);
  }
  INFO("amplitude/residual per tenth: " << profile);
  CHECK(worstAmplitude == Approx(kAmplitude).margin(0.01));
  CHECK(worstResidual < 0.1);
  CHECK(worstOther < 0.01);
}

/// Sends `seconds` of 750 Hz on channel 1 and 1500 Hz on channel 2 at `rate` in `pcm` to a
/// ready receiver, then checks one second of what the block heard, half a second after the
/// first sound (the receiver's rate control settles in that time).
void checkTwoTones(RtpPcm pcm, RtpEncoding encoding, std::uint32_t senderRate) {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  const std::uint16_t port = ear::test::freeUdpPort();
  const auto receiver = graph.makeReceiver(port, encoding, senderRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");

  RtpSender sender(port, pcm, senderRate, RtpTone{{750.0, 1500.0}, kAmplitude});
  sender.send(2 * senderRate);
  REQUIRE(graph.waiter.until([&] { return graph.recorder.first() != Recorder::kNone; }));
  const std::size_t from = graph.recorder.first() + kRate / 2;
  REQUIRE(graph.recorded(from + kRate));
  checkTone(graph.recorder, 0, from, kRate, 750.0, 1500.0);
  checkTone(graph.recorder, 1, from, kRate, 1500.0, 750.0);
  CHECK(sender.waitSent(10s));
  CHECK_FALSE(events.lostDetail.has_value());
}

}  // namespace

TEST_CASE("A receiver is ready once, every channel's link active, and it and the block follow the "
          "Dummy-Driver", "[rtp][clock]") {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  // Ready must come from the last link's ACTIVE. This connection hears each link's state first on
  // the lifecycle's own proxy (bound when the link was made) and then on the registry's binding
  // (PipeWire tells a link's resources in the order they were bound, on one ordered socket), so
  // at ready the registry has heard every other link ACTIVE and not yet the last one; ready on
  // an earlier link would find fewer.
  std::optional<std::size_t> activeAtReady;
  events.onReady = [&] {
    const auto node = graph.registry.id("fpp.rtp");
    activeAtReady = node ? graph.registry.activeLinksOf(*node) : 0;
  };
  const auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
  CHECK_FALSE(events.settled());   // never inside make()
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  CHECK(events.readyCount == 1);
  CHECK(activeAtReady == std::optional<std::size_t>(1));   // of 2
  REQUIRE(graph.registry.reread({"fpp.rtp", "fpp"}));
  CHECK(graph.registry.driverId("fpp.rtp") == graph.dummy);
  CHECK(graph.registry.driverId("fpp") == graph.dummy);
  CHECK(graph.runs(16));
  CHECK(events.readyCount == 1);
}

TEST_CASE("L24 at the graph rate: channel 1 reaches from_rtp_1 and channel 2 from_rtp_2", "[rtp][channels]") {
  checkTwoTones(RtpPcm::l24, RtpEncoding::l24, kRate);
}

TEST_CASE("L16 sent at 44100 into the 48000 graph is resampled", "[rtp][channels]") {
  checkTwoTones(RtpPcm::l16, RtpEncoding::l16, 44100);
}

TEST_CASE("A sender that restarts with a new SSRC is heard again", "[rtp][ssrc]") {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  const std::uint16_t port = ear::test::freeUdpPort();
  const auto receiver = graph.makeReceiver(port, RtpEncoding::l24, kRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");

  {
    RtpSender first(port, RtpPcm::l24, kRate, RtpTone{{750.0, 1500.0}, kAmplitude}, 0xC0FFEE);
    first.send(kRate / 2);
    REQUIRE(first.waitSent(10s));
  }
  REQUIRE(graph.waiter.until([&] { return graph.recorder.first() != Recorder::kNone; }));

  // The second sender plays other tones under another SSRC. The data marks where it begins, never
  // the block's clock (a data-loop freeze and the driver's catch-up move block frames against
  // the sender): the first stream's last 100 ms play out, and the receiver's resync on the new
  // stream zeroes its ring, so the second stream is the first onset after `mark` (at least 50 ms
  // of exact zeros, then sound). The check starts half a second into it.
  const std::size_t mark = graph.recorder.count();
  RtpSender second(port, RtpPcm::l24, kRate, RtpTone{{500.0, 1000.0}, kAmplitude}, 0xBEEF);
  second.send(3 * kRate);
  REQUIRE(graph.waiter.until([&] { return graph.recorder.onsetFrom(mark) != Recorder::kNone; }));
  const std::size_t from = graph.recorder.onsetFrom(mark) + kRate / 2;
  REQUIRE(graph.recorded(from + kRate));
  checkTone(graph.recorder, 0, from, kRate, 500.0, 750.0);
  checkTone(graph.recorder, 1, from, kRate, 1000.0, 1500.0);
  CHECK(second.waitSent(10s));
  CHECK_FALSE(events.lostDetail.has_value());
}

TEST_CASE("A receiver whose node goes is lost after make returns, its node and links already gone, "
          "and the block keeps running on the dummy", "[rtp][lost]") {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  const auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  const std::uint32_t node = *graph.registry.id("fpp.rtp");
  events.nodeId = node;
  REQUIRE(graph.waiter.until([&] { return graph.registry.linksOf(node).size() == 2; }));

  REQUIRE(graph.registry.destroy(node));
  REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
  CHECK_FALSE(events.lostInsideMake);
  CHECK(*events.lostDetail == "the engine removed the RTP receiver");
  CHECK_FALSE(events.nodePresentAtLost);
  CHECK(events.linksAtLost == 0);
  REQUIRE(graph.registry.reread({"fpp"}));
  CHECK(graph.registry.driverId("fpp") == graph.dummy);
  CHECK(graph.runs(16));
  CHECK(receiver != nullptr);   // nobody destroyed it
}

namespace {

/// The engine's one rtp-source module, from its own objects (it runs in this process).
pw_impl_module* rtpSourceModule(Engine& engine) {
  pw_impl_module* found = nullptr;
  pw_context_for_each_global(
      engine.context(),
      [](void* data, pw_global* global) {
        if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Module)) return 0;
        auto* module = static_cast<pw_impl_module*>(pw_global_get_object(global));
        const pw_module_info* info = pw_impl_module_get_info(module);
        if (info->name == nullptr || std::string_view(info->name) != "libpipewire-module-rtp-source") return 0;
        *static_cast<pw_impl_module**>(data) = module;
        return 1;
      },
      &found);
  return found;
}

/// A ready receiver whose two links are up; its node id is in `events`.
std::unique_ptr<OutsideNode> readyReceiver(Graph& graph, ReceiverEvents& events) {
  auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  const std::uint32_t node = *graph.registry.id("fpp.rtp");
  events.nodeId = node;
  REQUIRE(graph.waiter.until([&] { return graph.registry.linksOf(node).size() == 2; }));
  return receiver;
}

}  // namespace

TEST_CASE("A receiver that loses a link while its node still runs is lost with its node and links "
          "already gone, and the block keeps running on the dummy", "[rtp][lost]") {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  const auto receiver = readyReceiver(graph, events);

  REQUIRE(graph.registry.destroy(graph.registry.linksOf(*events.nodeId).front()));
  REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
  CHECK_FALSE(events.lostInsideMake);
  CHECK(events.lostDetail->starts_with("the RTP receiver's link for channel "));
  CHECK_FALSE(events.nodePresentAtLost);
  CHECK(events.linksAtLost == 0);
  REQUIRE(graph.registry.reread({"fpp"}));
  CHECK(graph.registry.driverId("fpp") == graph.dummy);
  CHECK(graph.runs(16));
}

TEST_CASE("A receiver whose module is destroyed is lost with its node and links already gone",
          "[rtp][lost]") {
  Graph graph;
  ReceiverEvents events(graph.waiter, graph.registry);
  const auto receiver = readyReceiver(graph, events);

  // No client may destroy a module through the registry (PipeWire gives modules no such
  // permission); the module destroys itself, as rtp-source does when its own connection fails
  // (pw_impl_module_schedule_destroy, then pw_impl_module_destroy on the main loop). The test
  // does that last step in its place.
  pw_impl_module* module = rtpSourceModule(*graph.engine);
  REQUIRE(module != nullptr);
  pw_impl_module_destroy(module);
  REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
  CHECK(*events.lostDetail == "the engine removed the RTP receiver's module");
  CHECK_FALSE(events.nodePresentAtLost);
  CHECK(events.linksAtLost == 0);
  CHECK(graph.runs(16));
}

TEST_CASE("A receiver made while another node of its name is still going binds and links its own "
          "node, never the stale one", "[rtp][restart]") {
  Graph graph;
  // A stale fpp.rtp that stays: an rtp-source module the test loads itself, on another port.
  const std::string arguments = "{ remote.name = \"internal\" source.ip = \"0.0.0.0\" source.port = " +
                                std::to_string(ear::test::freeUdpPort()) +
                                " sess.media = \"audio\" audio.format = \"S24BE\" audio.rate = 48000"
                                " audio.channels = 2 audio.position = [ AUX0 AUX1 ] node.name = \"fpp.rtp\" }";
  const std::unique_ptr<pw_impl_module, void (*)(pw_impl_module*)> staleModule(
      pw_context_load_module(graph.engine->context(), "libpipewire-module-rtp-source", arguments.c_str(), nullptr),
      &pw_impl_module_destroy);
  REQUIRE(staleModule != nullptr);
  REQUIRE(graph.waiter.until([&] { return graph.registry.id("fpp.rtp").has_value(); }));
  const std::uint32_t stale = *graph.registry.id("fpp.rtp");

  ReceiverEvents events(graph.waiter, graph.registry);
  const auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  CHECK(events.readyCount == 1);
  REQUIRE(graph.waiter.until([&] { return graph.registry.ids("fpp.rtp").size() == 2; }));
  const std::uint32_t own = graph.registry.ids("fpp.rtp").back();
  REQUIRE(own != stale);
  REQUIRE(graph.waiter.until([&] { return graph.registry.linksOf(own).size() == 2; }));
  CHECK(graph.registry.linksOf(stale).empty());
  CHECK(graph.registry.linksOf(*graph.registry.id("fpp")).size() == 2);
}

TEST_CASE("A receiver made again at once for the same names is ready, never lost, and linked by its "
          "own node's id", "[rtp][restart]") {
  Graph graph;
  const std::uint16_t port = ear::test::freeUdpPort();
  ReceiverEvents firstEvents(graph.waiter, graph.registry);
  auto first = graph.makeReceiver(port, RtpEncoding::l24, kRate, firstEvents);
  REQUIRE(graph.waiter.until([&] { return firstEvents.settled(); }));
  REQUIRE(firstEvents.lostDetail.value_or("") == "");

  first.reset();   // a restart: the old receiver goes and the new one is made in the same turn
  ReceiverEvents events(graph.waiter, graph.registry);
  const auto second = graph.makeReceiver(port, RtpEncoding::l24, kRate, events);
  REQUIRE(graph.waiter.until([&] { return events.settled(); }));
  REQUIRE(events.lostDetail.value_or("") == "");
  CHECK(events.readyCount == 1);

  // Once the old node is gone (the registry's first node of that name is then the new one;
  // the engine may give it the old id again), both of the block's links come from it.
  REQUIRE(graph.waiter.until([&] {
    const auto id = graph.registry.id("fpp.rtp");
    return id && graph.registry.linksOf(*id).size() == 2;
  }));
  CHECK(graph.registry.linksOf(*graph.registry.id("fpp")).size() == 2);
  CHECK(graph.runs(16));
  CHECK_FALSE(events.lostDetail.has_value());
}
