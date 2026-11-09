// The real in-process engine (docs/engine.md): port blocks, live ports and links, no daemon and no hardware. A block
// is made silent and then driven.
// Every wait is event-gated: the main loop runs until the events a test waits for have
// arrived; the deadline only fails the test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/pipewire/sources.hpp"
#include "ear/testing/drive_now.hpp"
#include "inducers.hpp"

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
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, kRate, kBlock});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}

/// Port blocks ring this when they publish levels; these tests read none.
class NoLevelWake final : public LevelWake {
public:
	void levelsPublished() noexcept override {}
};

/// A port block made silent, then driven by `code` (which outlives it).
std::unique_ptr<PortBlock> makeDriven(FilterPortBlocks& blocks, const NodeName& node, const Ports& ports,
                                      AudioCode& code, PortBlockEvents& events) {
	auto block = blocks.make(node, ports, kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &code));
	return block;
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

/// Records its first input's samples (up to a fixed capacity) and every block's frame count, and each cycle whose
/// samples it recorded whole in its ledger, with the index of its first sample.
class Recorder final : public AudioCode {
public:
	explicit Recorder(Waiter& waiter, std::size_t capacity = 8 * kRate)
		: waiter_(waiter), samples_(capacity), ledger_(capacity / kBlock), cycleStart_(capacity / kBlock) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		const std::size_t from = recorded_.load(std::memory_order_relaxed);
		std::size_t at = from;
		for (std::uint32_t i = 0; i < frames && at < samples_.size(); ++i) samples_[at++] = inputs[0][i];
		if (at == from + frames && ledger_.count() < ledger_.capacity()) {
			cycleStart_[ledger_.count()] = from; // before record() publishes the cycle
			ledger_.record(cycle, frames);
		}
		recorded_.store(at, std::memory_order_release);
		if (frames != kBlock) wrongFrames_.store(frames, std::memory_order_relaxed);
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}

	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	std::uint32_t wrongFrames() const { return wrongFrames_.load(std::memory_order_relaxed); }
	/// The recorded samples so far.
	std::span<const float> samples() const { return {samples_.data(), recorded_.load(std::memory_order_acquire)}; }
	/// The cycles recorded whole, and where cycle `n`'s samples begin in samples().
	const ear::test::CycleLedger& ledger() const { return ledger_; }
	std::size_t cycleStart(std::size_t n) const { return cycleStart_[n]; }

private:
	Waiter& waiter_;
	std::vector<float> samples_;
	ear::test::CycleLedger ledger_;
	std::vector<std::size_t> cycleStart_;
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
	             std::uint32_t frames, const ear::GraphCycle&) noexcept override {
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
	return static_cast<std::size_t>(std::ranges::find_if(samples, [](float s) { return s != 0.0f; }) - samples.begin());
}

/// A source block (ramp) linked to a sink block (recorder) by their port ids.
struct LinkedPair {
	explicit LinkedPair(Engine& engine, Waiter& waiter)
		: blocks(engine, wake), live(engine), links(engine), ports(waiter), linkLog(waiter), ramp(waiter),
		  recorder(waiter), sourceEvents(waiter), sinkEvents(waiter) {
		live.subscribe(ports);
		links.subscribe(linkLog);
		source = makeDriven(blocks, "ramp", Ports{{{Direction::output, "out", {"a"}}}}, ramp, sourceEvents);
		sink = makeDriven(blocks, "rec", Ports{{{Direction::input, "record", {"left"}}}}, recorder, sinkEvents);
	}
	NoLevelWake wake;
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

} // namespace

TEST_CASE("The engine runs with no daemon, runtime directory or D-Bus, and an unlinked sink "
          "block processes zeros",
          "[engine]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	Recorder recorder(waiter);
	BlockEvents events(waiter);
	auto sink = makeDriven(blocks, "rec", Ports{{{Direction::input, "record", {"left", "right"}}}}, recorder, events);

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
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	LiveGraph live(*engine);
	PortLog ports(waiter);
	live.subscribe(ports);
	CHECK(ports.appeared.empty()); // existing ports arrive later, never inside subscribe()
	Ramp ramp(waiter);
	Recorder recorder(waiter);
	BlockEvents sourceEvents(waiter), sinkEvents(waiter);
	auto source = makeDriven(blocks, "ramp", Ports{{{Direction::output, "out", {"a"}}}}, ramp, sourceEvents);
	auto sink =
		makeDriven(blocks, "rec", Ports{{{Direction::input, "record", {"left", "right"}}}}, recorder, sinkEvents);

	REQUIRE(waiter.until([&] { return ports.appeared.size() >= 3; }));
	const auto out = ports.find("ramp", "out.a");
	const auto fl = ports.find("rec", "record.left");
	const auto fr = ports.find("rec", "record.right");
	REQUIRE(out);
	REQUIRE(fl);
	REQUIRE(fr);
	CHECK(out->direction == Direction::output);
	CHECK(fl->direction == Direction::input);
	CHECK(fr->direction == Direction::input);
	CHECK(fl->id != fr->id);
	CHECK(ports.appeared.size() == 3); // the dummy driver has no ports

	// A second subscriber is told the ports that already exist, later.
	PortLog late(waiter);
	live.subscribe(late);
	CHECK(late.appeared.empty());
	REQUIRE(waiter.until([&] { return late.appeared.size() >= 3; }));
	REQUIRE(late.find("rec", "record.right"));
	CHECK(late.find("rec", "record.right")->id == fr->id);
}

TEST_CASE("A link carries a source block's ramp to a sink block sample for sample", "[engine]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	LinkedPair pair(*engine, waiter);
	REQUIRE(waiter.until([&] { return pair.ports.find("ramp", "out.a") && pair.ports.find("rec", "record.left"); }));

	pair.links.createLink(kLink, pair.ports.find("ramp", "out.a")->id, pair.ports.find("rec", "record.left")->id);
	CHECK(pair.linkLog.active.empty()); // never inside createLink
	REQUIRE(waiter.until([&] { return !pair.linkLog.active.empty() || !pair.linkLog.failed.empty(); }));
	REQUIRE(pair.linkLog.failed.empty());
	CHECK(pair.linkLog.active == std::vector<LinkId>{kLink});

	// The ramp counts its own cycles, so a cycle it ran and the recorder did not (cut short between them, which a host
	// that stalls the data thread makes) moves the ramp on unheard: the recorder judges across a cycle boundary only
	// where it ran the cycle straight after its last with nothing lost (its ledger's clean cycles), to a planned count
	// of them. Within every cycle it heard, the ramp is whole.
	constexpr std::size_t kWanted = 32; // clean cycles judged, after the first sound
	const ear::test::CycleLedger& ledger = pair.recorder.ledger();
	const auto judged = [&] {
		const std::size_t first = firstSound(pair.recorder.samples());
		std::size_t clean = 0;
		for (std::size_t n = 0; n < ledger.count(); ++n)
			if (ledger[n].clean && pair.recorder.cycleStart(n) > first) ++clean;
		return clean;
	};
	REQUIRE(waiter.until([&] { return judged() >= kWanted || ledger.full(); }, 30s));
	INFO("the recorder filled its " << ledger.capacity() << " cycles before " << kWanted << " clean ones came");
	REQUIRE_FALSE(ledger.full());
	const auto samples = pair.recorder.samples();
	const std::size_t first = firstSound(samples);
	REQUIRE(first < samples.size());
	std::size_t breaks = 0, unjudged = 0;
	for (std::size_t n = 0; n < ledger.count(); ++n) {
		const std::size_t start = pair.recorder.cycleStart(n);
		const std::size_t end = start + ledger[n].frames;
		if (end <= first + 1) continue;
		for (std::size_t i = std::max(start + 1, first + 1); i < end; ++i)
			if (samples[i] != samples[i - 1] + 1.0f) ++breaks;
		if (start <= first) continue;
		if (!ledger[n].clean) ++unjudged; // a cycle lost before it: the boundary is not the ramp's to keep
		else if (samples[start] != samples[start - 1] + 1.0f) ++breaks;
	}
	INFO(unjudged << " cycle boundaries after a lost cycle, not judged");
	CHECK(samples[first] >= 1.0f);
	CHECK(breaks == 0);
}

TEST_CASE("Blocks process at the graph frequency and block size", "[engine]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	Recorder recorder(waiter);
	BlockEvents events(waiter);
	auto sink = makeDriven(blocks, "rec", Ports{{{Direction::input, "record", {"left"}}}}, recorder, events);

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
	pw_filter* probe =
		pw_filter_new(engine->core(), "clock-probe", pw_properties_new(PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
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

// docs/engine.md#the-quantum-guard: the floor and the limit are the block size, so a node that forces another quantum
// cannot change every block's cycle (a longer one would play as zeros, a shorter one break clock.duration == block).
TEST_CASE("The quantum floor and limit are the block size: a node forcing a larger or a smaller quantum leaves every "
          "cycle at the block size",
          "[engine][quantum]") {
	// What a companion must never carry (ear sets none): a forced quantum of four blocks, or of a quarter block.
	std::uint32_t forcedFrames = 0;
	SECTION("four blocks") { forcedFrames = 4 * kBlock; }
	SECTION("a quarter block") { forcedFrames = kBlock / 4; }
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	Recorder recorder(waiter);
	BlockEvents events(waiter);
	auto sink = makeDriven(blocks, "rec", Ports{{{Direction::input, "record", {"left"}}}}, recorder, events);

	struct Clock {
		Waiter* waiter;
		std::atomic<std::uint64_t> longest{0};
		std::atomic<std::uint64_t> shortest{~std::uint64_t{0}};
		std::atomic<std::uint64_t> cycles{0};
	} clock{&waiter};
	pw_filter_events probeEvents{};
	probeEvents.version = PW_VERSION_FILTER_EVENTS;
	probeEvents.process = [](void* data, spa_io_position* position) {
		auto* seen = static_cast<Clock*>(data);
		if (position->clock.duration > seen->longest.load()) seen->longest.store(position->clock.duration);
		if (position->clock.duration < seen->shortest.load()) seen->shortest.store(position->clock.duration);
		seen->cycles.fetch_add(1);
		seen->waiter->wake();
	};
	const std::string forced = std::to_string(forcedFrames);
	pw_filter* probe = pw_filter_new(engine->core(), "forcing-probe",
	                                 pw_properties_new(PW_KEY_NODE_ALWAYS_PROCESS, "true", "node.force-quantum",
	                                                   forced.c_str(), PW_KEY_NODE_GROUP, "ear.graph", nullptr));
	REQUIRE(probe != nullptr);
	spa_hook probeHook{};
	pw_filter_add_listener(probe, &probeHook, &probeEvents, &clock);
	REQUIRE(pw_filter_connect(probe, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) == 0);

	const auto before = recorder.calls();
	const bool ran = waiter.until(
		[&] { return clock.cycles.load() >= 64 && events.readyCount > 0 && recorder.calls() >= before + 64; });
	pw_filter_destroy(probe);
	INFO("forced " << forcedFrames);
	REQUIRE(ran);
	CHECK(clock.longest.load() == kBlock);
	CHECK(clock.shortest.load() == kBlock);
	CHECK(recorder.wrongFrames() == 0);
}

TEST_CASE("Destroying a sink block removes its ports and link, and it never processes again", "[engine]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	LinkedPair pair(*engine, waiter);
	REQUIRE(waiter.until([&] { return pair.ports.find("ramp", "out.a") && pair.ports.find("rec", "record.left"); }));
	const EnginePortId fl = pair.ports.find("rec", "record.left")->id;
	pair.links.createLink(kLink, pair.ports.find("ramp", "out.a")->id, fl);
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
	struct SyncState {
		int seq;
		bool* synced;
		Waiter* waiter;
	} sync{0, &synced, &waiter};
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
	const bool settled = waiter.until([&] { return synced && pair.ramp.calls() >= sourceCalls + 8; });
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

namespace {

/// Reads, inside process(), how the thread running it is scheduled.
class SchedulingProbe final : public AudioCode {
public:
	explicit SchedulingProbe(Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0f);
		if (!seen_.load(std::memory_order_acquire)) {
			policy_ = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
			sched_param param{};
			sched_getparam(0, &param);
			priority_ = param.sched_priority;
			seen_.store(true, std::memory_order_release);
			waiter_.wake();
		}
	}
	void release() noexcept override {}
	bool seen() const { return seen_.load(std::memory_order_acquire); }
	int policy() const { return policy_; }
	int priority() const { return priority_; }

private:
	Waiter& waiter_;
	std::atomic<bool> seen_{false};
	int policy_ = -1;
	int priority_ = -1;
};

} // namespace

TEST_CASE("The scheduling report is how the thread running process() is scheduled", "[engine][scheduling]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	SchedulingProbe probe(waiter);
	BlockEvents events(waiter);
	auto block = makeDriven(blocks, "probe", Ports{{{Direction::output, "out", {"a"}}}}, probe, events);
	REQUIRE(waiter.until([&] { return probe.seen(); }));

	const Scheduling scheduling = engine->dataThreadScheduling();
	CHECK(scheduling.realtime == (probe.policy() == SCHED_FIFO || probe.policy() == SCHED_RR));
	if (probe.policy() == SCHED_FIFO) {
		CHECK(scheduling.detail == "SCHED_FIFO " + std::to_string(probe.priority()));
	} else if (probe.policy() == SCHED_RR) {
		CHECK(scheduling.detail == "SCHED_RR " + std::to_string(probe.priority()));
	} else {
		REQUIRE(probe.policy() == SCHED_OTHER);
		CHECK(scheduling.detail == "SCHED_OTHER");
	}
	// The dev container and CI grant no realtime priority (no CAP_SYS_NICE, RLIMIT_RTPRIO 0).
	CHECK_FALSE(scheduling.realtime);
}

// docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog: once the data loop is judged wedged, a route's
// link is neither made nor destroyed, since either reaches the data loop through a blocking invoke (impl-port.c's
// port_set_io releases a mix with pw_loop_invoke(..., true)), and the main loop would wait on it for good.
TEST_CASE("After the data loop is judged wedged a route's link is leaked, not destroyed, and a new one is refused, "
          "and the main loop never waits",
          "[engine][wedge]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	LinkedPair pair(*engine, waiter);
	REQUIRE(waiter.until([&] { return pair.ports.find("ramp", "out.a") && pair.ports.find("rec", "record.left"); }));
	const EnginePortId out = pair.ports.find("ramp", "out.a")->id;
	const EnginePortId left = pair.ports.find("rec", "record.left")->id;
	pair.links.createLink(kLink, out, left);
	REQUIRE(waiter.until([&] { return !pair.linkLog.active.empty(); }));

	// The data loop runs no invoke from here: one of its invokes waits for the test.
	std::atomic<bool> release{false};
	pw_loop* dataLoop = pw_data_loop_get_loop(pw_context_get_data_loop(engine->context()));
	REQUIRE(dataLoop != nullptr);
	pw_loop_invoke(
		dataLoop,
		[](spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* data) -> int {
			auto* flag = static_cast<std::atomic<bool>*>(data);
			while (!flag->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return 0;
		},
		0, nullptr, 0, false, &release);
	markDataLoopWedged(*engine); // as main.cpp's glue does on ear.data-loop-wedged
	CHECK(dataLoopWedged(*engine));

	const auto before = std::chrono::steady_clock::now();
	pair.links.destroyLink(kLink);       // leaked: it returns at once
	pair.links.createLink(8, out, left); // refused later, never inside the call
	CHECK(pair.linkLog.failed.empty());
	// The main loop keeps turning: the refusal arrives, and a round of the loop later nothing has waited.
	REQUIRE(waiter.until([&] { return !pair.linkLog.failed.empty(); }));
	CHECK(pair.linkLog.failed == std::vector<std::pair<LinkId, std::string>>{{8, "the data loop is wedged"}});
	struct Later {
		bool fired = false;
		Waiter* waiter;
	} later{false, &waiter};
	spa_source* timer = pw_loop_add_timer(
		engine->mainLoop(),
		[](void* data, std::uint64_t) {
			auto* state = static_cast<Later*>(data);
			state->fired = true;
			state->waiter->wake();
		},
		&later);
	timespec value{0, 200'000'000};
	pw_loop_update_timer(engine->mainLoop(), timer, &value, nullptr, false);
	const bool turned = waiter.until([&] { return later.fired; });
	pw_loop_destroy_source(engine->mainLoop(), timer);
	CHECK(turned);
	CHECK(pair.linkLog.gone.empty()); // the link was never destroyed, so never told gone
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));

	release.store(true); // the blocks' teardown below waits on the data loop
}

namespace {

/// Writes one level on every output: a node with outputs, so every stop fades.
class LevelCode final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> output : outputs) std::ranges::fill(output, 0.25F);
	}
	void release() noexcept override {}
};

class LevelConfig final : public NodeConfig {
public:
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext&) const override { return std::make_unique<LevelCode>(); }

private:
	Ports ports_{{{Direction::output, "out", {"left", "right"}}}};
};

class HealthWake final : public NodeHealthListener {
public:
	explicit HealthWake(Waiter& waiter) : waiter_(waiter) {}
	void nodeChanged(const NodeHealth&) override { waiter_.wake(); }
	void nodeGone(const NodeName&, std::uint64_t) override { waiter_.wake(); }

private:
	Waiter& waiter_;
};

} // namespace

// Stopping many nodes at once (docs/architecture.md#teardown-by-dependency): NodeHost over the real engine posts every
// node's fade at once, then every silence once the fades are acknowledged, then one flush of destroys, so stopping many
// nodes costs about one fade, one cycle and one batch of destroys, never one per node in turn. Measured and printed;
// bounded at 2 s.
TEST_CASE("Stopping 16 nodes costs about one fade, one cycle and one batch of destroys", "[engine][fade]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	NoLevelWake wake;
	FilterPortBlocks blocks(*engine, wake);
	ThreadHandoffs handoffs(engine->mainLoop());
	AlsaSoundCards cards(*engine);
	RtpReceivers receivers(*engine);
	MainLoopExecutor mainLoop(*engine);
	ThreadSinks sinks(mainLoop);
	EventSources sources;
	ResourceRegistry resources(cards, receivers, handoffs, sinks, sources);
	NodeCatalogue catalogue{{}};
	DesiredGraph graph(catalogue);
	HealthWake health(waiter);
	{
		NodeHost host(kFormat, blocks, resources, mainLoop, mainLoop);
		host.subscribe(health);
		auto config = std::make_shared<LevelConfig>();
		for (int i = 0; i < 16; ++i) host.nodeAdded(NodeSpec{"n" + std::to_string(i), "level", {}}, config);
		REQUIRE(waiter.until(
			[&] {
				const auto all = host.health();
				return all.size() == 16 &&
			           std::ranges::all_of(all, [](const NodeHealth& h) { return h.state == NodeState::running; });
			},
			10s));
		bool gone = false;
		const auto before = std::chrono::steady_clock::now();
		host.stopAll(graph.close(), [&] {
			gone = true;
			waiter.wake();
		});
		REQUIRE(waiter.until([&] { return gone; }, 10s));
		const auto took =
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before);
		std::printf("stopping 16 nodes took %lld us\n", static_cast<long long>(took.count()));
		CHECK(took < std::chrono::seconds(2));
		CHECK(host.leaked() == 0);
	}
}
