// Slice S1's tests (docs/design/m1-white-to-file.md section 3, acceptance criteria S1): the
// real in-process engine, port blocks, live ports and links, no daemon and no hardware.
// Every wait is event-gated: the main loop runs until the events a test waits for have
// arrived; the deadline only fails the test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace std::chrono_literals;

constexpr std::uint32_t kRate = 44100;
constexpr std::uint32_t kBlock = 256;
constexpr AudioFormat kFormat{kRate, kBlock};

/// The engine with nothing from a desktop session around it: no daemon to reach, no runtime
/// directory, no D-Bus.
std::unique_ptr<Engine> makeEngine() {
  for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE",
                           "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS"}) {
    unsetenv(name);
  }
  auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, kRate, kBlock});
  if (!engine) FAIL("Engine::create: " << engine.error().detail);
  return std::move(*engine);
}

/// Runs the engine's main loop until `done` holds. Every event a test listens for calls
/// wake() (from any thread), which re-checks `done` on the main loop.
class Waiter {
public:
  explicit Waiter(Engine& engine) : engine_(engine) {
    wake_ = pw_loop_add_event(engine.mainLoop(), &Waiter::onWake, this);
  }
  ~Waiter() { pw_loop_destroy_source(engine_.mainLoop(), wake_); }

  void wake() noexcept { pw_loop_signal_event(engine_.mainLoop(), wake_); }

  bool until(std::function<bool()> done, std::chrono::seconds deadline = 5s) {
    done_ = std::move(done);
    if (done_()) return true;
    timedOut_ = false;
    spa_source* timer = pw_loop_add_timer(engine_.mainLoop(), &Waiter::onTimeout, this);
    timespec value{static_cast<time_t>(deadline.count()), 0};
    pw_loop_update_timer(engine_.mainLoop(), timer, &value, nullptr, false);
    engine_.run();
    pw_loop_destroy_source(engine_.mainLoop(), timer);
    return !timedOut_ && done_();
  }

private:
  static void onWake(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<Waiter*>(data);
    if (self->done_ && self->done_()) self->engine_.quit();
  }
  static void onTimeout(void* data, std::uint64_t /*expirations*/) {
    auto* self = static_cast<Waiter*>(data);
    self->timedOut_ = true;
    self->engine_.quit();
  }

  Engine& engine_;
  spa_source* wake_ = nullptr;
  std::function<bool()> done_;
  bool timedOut_ = false;
};

/// Records its first input's samples (up to a fixed capacity) and every block's frame count.
class Recorder final : public AudioCode {
public:
  explicit Recorder(Waiter& waiter, std::size_t capacity = 8 * kRate)
      : waiter_(waiter), samples_(capacity) {}
  void prepare(const AudioFormat&) override {}
  void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
               std::uint32_t frames) noexcept override {
    std::size_t at = recorded_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < frames && at < samples_.size(); ++i) samples_[at++] = inputs[0][i];
    recorded_.store(at, std::memory_order_release);
    if (frames != kBlock) wrongFrames_.store(frames, std::memory_order_relaxed);
    calls_.fetch_add(1, std::memory_order_release);
    waiter_.wake();
  }
  void release() noexcept override {}

  std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
  std::uint32_t wrongFrames() const { return wrongFrames_.load(std::memory_order_relaxed); }
  /// The recorded samples so far.
  std::span<const float> samples() const {
    return {samples_.data(), recorded_.load(std::memory_order_acquire)};
  }

private:
  Waiter& waiter_;
  std::vector<float> samples_;
  std::atomic<std::size_t> recorded_{0};
  std::atomic<std::uint64_t> calls_{0};
  std::atomic<std::uint32_t> wrongFrames_{0};
};

/// Writes 1, 2, 3, ... to its one output, one value per sample, across blocks.
class Ramp final : public AudioCode {
public:
  explicit Ramp(Waiter& waiter) : waiter_(waiter) {}
  void prepare(const AudioFormat&) override {}
  void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
               std::uint32_t frames) noexcept override {
    for (std::uint32_t i = 0; i < frames; ++i) outputs[0][i] = static_cast<float>(++next_);
    calls_.fetch_add(1, std::memory_order_release);
    waiter_.wake();
  }
  void release() noexcept override {}
  std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }

private:
  Waiter& waiter_;
  std::uint32_t next_ = 0;
  std::atomic<std::uint64_t> calls_{0};
};

class BlockEvents final : public PortBlockEvents {
public:
  explicit BlockEvents(Waiter& waiter) : waiter_(waiter) {}
  void ready() override {
    ++readyCount;
    waiter_.wake();
  }
  void lost(std::string detail) override {
    lostDetail = std::move(detail);
    waiter_.wake();
  }
  int readyCount = 0;
  std::optional<std::string> lostDetail;

private:
  Waiter& waiter_;
};

class PortLog final : public LivePortListener {
public:
  explicit PortLog(Waiter& waiter) : waiter_(waiter) {}
  void portAppeared(const LivePort& port) override {
    appeared.push_back(port);
    waiter_.wake();
  }
  void portGone(EnginePortId id) override {
    gone.push_back(id);
    waiter_.wake();
  }
  std::optional<LivePort> find(const NodeName& node, const PortName& port) const {
    for (const LivePort& live : appeared) {
      if (live.port.node == node && live.port.port == port) return live;
    }
    return std::nullopt;
  }
  bool isGone(EnginePortId id) const { return std::ranges::find(gone, id) != gone.end(); }
  std::vector<LivePort> appeared;
  std::vector<EnginePortId> gone;

private:
  Waiter& waiter_;
};

class LinkLog final : public LinkListener {
public:
  explicit LinkLog(Waiter& waiter) : waiter_(waiter) {}
  void linkActive(LinkId link) override {
    active.push_back(link);
    waiter_.wake();
  }
  void linkFailed(LinkId link, std::string detail) override {
    failed.emplace_back(link, std::move(detail));
    waiter_.wake();
  }
  void linkGone(LinkId link) override {
    gone.push_back(link);
    waiter_.wake();
  }
  std::vector<LinkId> active;
  std::vector<std::pair<LinkId, std::string>> failed;
  std::vector<LinkId> gone;

private:
  Waiter& waiter_;
};

/// Index of the first nonzero sample, or samples.size().
std::size_t firstSound(std::span<const float> samples) {
  return static_cast<std::size_t>(
      std::ranges::find_if(samples, [](float s) { return s != 0.0f; }) - samples.begin());
}

/// A source block (ramp) linked to a sink block (recorder) by their port ids.
struct LinkedPair {
  explicit LinkedPair(Engine& engine, Waiter& waiter)
      : blocks(engine), live(engine), links(engine), ports(waiter), linkLog(waiter),
        ramp(waiter), recorder(waiter), sourceEvents(waiter), sinkEvents(waiter) {
    live.subscribe(ports);
    links.subscribe(linkLog);
    source = blocks.make("ramp", Ports{{}, {"OUT"}}, kFormat, ramp, sourceEvents);
    sink = blocks.make("rec", Ports{{"FL"}, {}}, kFormat, recorder, sinkEvents);
  }
  FilterPortBlocks blocks;
  LiveGraph live;
  Links links;
  PortLog ports;
  LinkLog linkLog;
  Ramp ramp;
  Recorder recorder;
  BlockEvents sourceEvents;
  BlockEvents sinkEvents;
  std::unique_ptr<PortBlock> source;
  std::unique_ptr<PortBlock> sink;
};

constexpr LinkId kLink = 7;

}  // namespace

TEST_CASE("The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink "
          "block processes zeros", "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  FilterPortBlocks blocks(*engine);
  Recorder recorder(waiter);
  BlockEvents events(waiter);
  auto sink = blocks.make("rec", Ports{{"FL", "FR"}, {}}, kFormat, recorder, events);

  REQUIRE(waiter.until([&] { return events.readyCount > 0 && recorder.calls() >= 8; }));
  CHECK(events.readyCount == 1);
  CHECK_FALSE(events.lostDetail);
  const auto samples = recorder.samples();
  REQUIRE(samples.size() >= 8 * kBlock);
  CHECK(firstSound(samples) == samples.size());
}

TEST_CASE("Live ports name their node, port and direction", "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  FilterPortBlocks blocks(*engine);
  LiveGraph live(*engine);
  PortLog ports(waiter);
  live.subscribe(ports);
  CHECK(ports.appeared.empty());   // existing ports arrive later, never inside subscribe()
  Ramp ramp(waiter);
  Recorder recorder(waiter);
  BlockEvents sourceEvents(waiter), sinkEvents(waiter);
  auto source = blocks.make("ramp", Ports{{}, {"OUT"}}, kFormat, ramp, sourceEvents);
  auto sink = blocks.make("rec", Ports{{"FL", "FR"}, {}}, kFormat, recorder, sinkEvents);

  REQUIRE(waiter.until([&] { return ports.appeared.size() >= 3; }));
  const auto out = ports.find("ramp", "OUT");
  const auto fl = ports.find("rec", "FL");
  const auto fr = ports.find("rec", "FR");
  REQUIRE(out);
  REQUIRE(fl);
  REQUIRE(fr);
  CHECK(out->direction == Direction::output);
  CHECK(fl->direction == Direction::input);
  CHECK(fr->direction == Direction::input);
  CHECK(fl->id != fr->id);
  CHECK(ports.appeared.size() == 3);   // the dummy driver has no ports

  // A second subscriber is told the ports that already exist, later.
  PortLog late(waiter);
  live.subscribe(late);
  CHECK(late.appeared.empty());
  REQUIRE(waiter.until([&] { return late.appeared.size() >= 3; }));
  REQUIRE(late.find("rec", "FR"));
  CHECK(late.find("rec", "FR")->id == fr->id);
}

TEST_CASE("A link carries a source block's ramp to a sink block sample for sample", "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  LinkedPair pair(*engine, waiter);
  REQUIRE(waiter.until([&] { return pair.ports.find("ramp", "OUT") && pair.ports.find("rec", "FL"); }));

  pair.links.createLink(kLink, pair.ports.find("ramp", "OUT")->id, pair.ports.find("rec", "FL")->id);
  CHECK(pair.linkLog.active.empty());   // never inside createLink
  REQUIRE(waiter.until([&] { return !pair.linkLog.active.empty() || !pair.linkLog.failed.empty(); }));
  REQUIRE(pair.linkLog.failed.empty());
  CHECK(pair.linkLog.active == std::vector<LinkId>{kLink});

  constexpr std::size_t kWanted = 32 * kBlock;
  REQUIRE(waiter.until([&] {
    const auto samples = pair.recorder.samples();
    return samples.size() - std::min(firstSound(samples), samples.size()) >= kWanted;
  }));
  const auto samples = pair.recorder.samples();
  const std::size_t first = firstSound(samples);
  std::size_t breaks = 0;
  for (std::size_t i = first + 1; i < samples.size(); ++i) {
    if (samples[i] != samples[i - 1] + 1.0f) ++breaks;
  }
  CHECK(samples[first] >= 1.0f);
  CHECK(breaks == 0);
}

TEST_CASE("Blocks process at the graph frequency and block size", "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  FilterPortBlocks blocks(*engine);
  Recorder recorder(waiter);
  BlockEvents events(waiter);
  auto sink = blocks.make("rec", Ports{{"FL"}, {}}, kFormat, recorder, events);

  // The clock as a process callback sees it, read by a bare engine node beside the block.
  struct Clock {
    Waiter* waiter;
    std::atomic<std::uint32_t> rate{0};
    std::atomic<std::uint64_t> duration{0};
    std::atomic<std::uint64_t> cycles{0};
  } clock{&waiter};
  pw_filter_events probeEvents{};
  probeEvents.version = PW_VERSION_FILTER_EVENTS;
  probeEvents.process = [](void* data, spa_io_position* position) {
    auto* seen = static_cast<Clock*>(data);
    seen->rate.store(position->clock.rate.denom);
    seen->duration.store(position->clock.duration);
    seen->cycles.fetch_add(1);
    seen->waiter->wake();
  };
  pw_filter* probe = pw_filter_new(engine->core(), "clock-probe",
                                   pw_properties_new(PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
  REQUIRE(probe != nullptr);
  spa_hook probeHook{};
  pw_filter_add_listener(probe, &probeHook, &probeEvents, &clock);
  REQUIRE(pw_filter_connect(probe, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) == 0);

  // The block skips process() for a block longer than its maxBlock, so the wait is on the
  // bare node's cycles and the block's ready().
  const bool ran = waiter.until([&] { return clock.cycles.load() >= 8 && events.readyCount > 0; });
  pw_filter_destroy(probe);
  REQUIRE(ran);
  CHECK(clock.rate.load() == kRate);
  CHECK(clock.duration.load() == kBlock);
  CHECK(recorder.calls() > 0);
  CHECK(recorder.wrongFrames() == 0);
  CHECK(recorder.samples().size() == recorder.calls() * kBlock);
}

TEST_CASE("Destroying a sink block removes its ports and link, and it never processes again",
          "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  LinkedPair pair(*engine, waiter);
  REQUIRE(waiter.until([&] { return pair.ports.find("ramp", "OUT") && pair.ports.find("rec", "FL"); }));
  const EnginePortId fl = pair.ports.find("rec", "FL")->id;
  pair.links.createLink(kLink, pair.ports.find("ramp", "OUT")->id, fl);
  REQUIRE(waiter.until([&] { return !pair.linkLog.active.empty(); }));

  pair.sink.reset();
  const std::uint64_t callsAtDestroy = pair.recorder.calls();
  REQUIRE(waiter.until([&] { return pair.ports.isGone(fl) && !pair.linkLog.gone.empty(); }));
  CHECK(pair.linkLog.gone == std::vector<LinkId>{kLink});
  CHECK(pair.linkLog.failed.empty());

  // The source keeps running; after more cycles and a round trip through the engine, the sink's
  // count has not moved.
  const std::uint64_t sourceCalls = pair.ramp.calls();
  bool synced = false;
  pw_core_events coreEvents{};
  coreEvents.version = PW_VERSION_CORE_EVENTS;
  struct SyncState { int seq; bool* synced; Waiter* waiter; } sync{0, &synced, &waiter};
  coreEvents.done = [](void* data, std::uint32_t id, int seq) {
    auto* state = static_cast<SyncState*>(data);
    if (id == PW_ID_CORE && seq == state->seq) {
      *state->synced = true;
      state->waiter->wake();
    }
  };
  spa_hook coreHook{};
  pw_core_add_listener(engine->core(), &coreHook, &coreEvents, &sync);
  sync.seq = pw_core_sync(engine->core(), PW_ID_CORE, 0);
  const bool settled =
      waiter.until([&] { return synced && pair.ramp.calls() >= sourceCalls + 8; });
  spa_hook_remove(&coreHook);
  REQUIRE(settled);
  CHECK(pair.recorder.calls() == callsAtDestroy);
}

TEST_CASE("SIGTERM reaches the stop-signal handler as a main-loop event", "[engine]") {
  auto engine = makeEngine();
  Waiter waiter(*engine);
  bool stopped = false;
  engine->onStopSignal([&] {
    stopped = true;
    waiter.wake();
  });
  // To the process, as a container runtime sends it, not to this thread.
  REQUIRE(kill(getpid(), SIGTERM) == 0);
  REQUIRE(waiter.until([&] { return stopped; }));
}
