// FilterPortBlocks on the real in-process engine (docs/engine.md#port-blocks): levels on the graph clock, complete
// snapshots, the drive barrier, a lost block, inside ports and the graph group; each cycle's clock facts, latency
// carried per bank pair, and denormals flushed on the data thread; docs/engine.md#drive-and-fade: the asynchronous
// drive applied in its invoke, the fade, settle(), one invoke in flight per block, takenAt and blockId. Every wait is
// event-gated: the main loop runs until the events a test waits for have arrived; the deadline only fails the test. The
// Waiter and makeEngine follow engine_test.cpp's (the pipewire-engine skill's pattern).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/node/command.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/testing/drive_now.hpp"
#include "probes.hpp"

// Allocations are counted on a thread that asks for it (the realtime thread, from a test code's process() on), by the
// binary's one malloc hook.
#include "allocation_count.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace std::chrono_literals;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;
constexpr AudioFormat kFormat{kRate, kBlock};
constexpr std::uint64_t kWindow = 1600; // levelWindowFrames(48000)

std::unique_ptr<Engine> makeEngine() {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
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

/// The level bell as the control thread would see it: each ring wakes the waiter.
class Bell final : public LevelWake {
public:
	explicit Bell(Waiter& waiter) : waiter_(waiter) {}
	void levelsPublished() noexcept override {
		rings_.fetch_add(1, std::memory_order_relaxed);
		waiter_.wake();
	}
	std::uint64_t rings() const { return rings_.load(std::memory_order_relaxed); }

private:
	Waiter& waiter_;
	std::atomic<std::uint64_t> rings_{0};
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

/// Counts its calls and writes `value` to every output (and can arm the allocation counter
/// on the thread that runs it).
class Constant final : public AudioCode {
public:
	Constant(Waiter& waiter, float value) : waiter_(waiter), value_(value) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		ear::pipewire::testing::countAllocationsOnThisThread(countHere.load(std::memory_order_acquire));
		for (const std::span<float> out : outputs) std::ranges::fill(out, value_);
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	std::atomic<bool> countHere{false};

private:
	Waiter& waiter_;
	float value_;
	std::atomic<std::uint64_t> calls_{0};
};

/// Writes 1, 2, 3, ... to its one output, one value per sample, across blocks.
class Ramp final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle&) noexcept override {
		for (std::uint32_t i = 0; i < frames; ++i) outputs[0][i] = static_cast<float>(++next_);
	}
	void release() noexcept override {}

private:
	std::uint32_t next_ = 0;
};

/// Copies its first input to its first output.
class PassThrough final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t, const ear::GraphCycle&) noexcept override {
		std::ranges::copy(inputs[0], outputs[0].begin());
	}
	void release() noexcept override {}
};

/// Records its first input's samples (up to a fixed capacity).
class Recorder final : public AudioCode {
public:
	explicit Recorder(Waiter& waiter) : waiter_(waiter), samples_(8 * kRate) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>>,
	             std::uint32_t frames, const ear::GraphCycle&) noexcept override {
		std::size_t at = recorded_.load(std::memory_order_relaxed);
		for (std::uint32_t i = 0; i < frames && at < samples_.size(); ++i) samples_[at++] = inputs[0][i];
		recorded_.store(at, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::span<const float> samples() const { return {samples_.data(), recorded_.load(std::memory_order_acquire)}; }

private:
	Waiter& waiter_;
	std::vector<float> samples_;
	std::atomic<std::size_t> recorded_{0};
};

class PortLog final : public LivePortListener {
public:
	explicit PortLog(Waiter& waiter) : waiter_(waiter) {}
	void portAppeared(const LivePort& port) override {
		appeared.push_back(port);
		waiter_.wake();
	}
	void portGone(EnginePortId) override { waiter_.wake(); }
	std::optional<LivePort> find(const NodeName& node, const PortName& port) const {
		for (const LivePort& live : appeared) {
			if (live.port.node == node && live.port.port == port) return live;
		}
		return std::nullopt;
	}
	std::vector<LivePort> appeared;

private:
	Waiter& waiter_;
};

class LinkLog final : public LinkListener {
public:
	explicit LinkLog(Waiter& waiter) : waiter_(waiter) {}
	void linkActive(LinkId) override {
		++active;
		waiter_.wake();
	}
	void linkFailed(LinkId, std::string) override {
		++failed;
		waiter_.wake();
	}
	void linkGone(LinkId) override { waiter_.wake(); }
	int active = 0;
	int failed = 0;

private:
	Waiter& waiter_;
};

/// Live ports and links, for tests that route between blocks.
struct Wiring {
	Wiring(Engine& engine, Waiter& waiter) : live(engine), links(engine), ports(waiter), linkLog(waiter) {
		live.subscribe(ports);
		links.subscribe(linkLog);
	}
	/// Links from.node:from.port to to.node:to.port once both ports are live.
	bool link(Waiter& waiter, const PortRef& from, const PortRef& to) {
		if (!waiter.until([&] { return ports.find(from.node, from.port) && ports.find(to.node, to.port); })) {
			return false;
		}
		const int before = linkLog.active;
		links.createLink(nextLink++, ports.find(from.node, from.port)->id, ports.find(to.node, to.port)->id);
		return waiter.until([&] { return linkLog.active > before || linkLog.failed > 0; }) && linkLog.failed == 0;
	}
	LiveGraph live;
	Links links;
	PortLog ports;
	LinkLog linkLog;
	LinkId nextLink = 1;
};

/// Engine nodes as the registry shows them, each bound for its full properties.
class EngineNodes {
public:
	EngineNodes(Engine& engine, Waiter& waiter) : waiter_(waiter) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &hook_, &registryEvents(), this);
	}
	~EngineNodes() {
		for (Node& node : nodes_) {
			spa_hook_remove(&node.hook);
			pw_proxy_destroy(node.proxy);
		}
		spa_hook_remove(&hook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	EngineNodes(const EngineNodes&) = delete;
	EngineNodes& operator=(const EngineNodes&) = delete;

	/// The node's properties, once its info has arrived.
	const std::map<std::string, std::string>* props(const NodeName& name) const {
		for (const Node& node : nodes_) {
			if (node.name == name && node.infoSeen) return &node.props;
		}
		return nullptr;
	}
	/// Destroys the node through the registry, as another client or the engine could.
	bool destroy(const NodeName& name) {
		for (const Node& node : nodes_) {
			if (node.name == name) return pw_registry_destroy(registry_, node.id) >= 0;
		}
		return false;
	}
	/// Every node of this name the registry announced, in its order.
	std::vector<std::uint32_t> ids(const NodeName& name) const {
		std::vector<std::uint32_t> found;
		for (const Node& node : nodes_) {
			if (node.name == name) found.push_back(node.id);
		}
		return found;
	}
	/// Suspends the node through its proxy, as a session manager would: the engine deactivates it, so it processes no
	/// more, and its filter goes PAUSED (impl-node.c suspend_node, filter.c impl_send_command).
	bool suspend(const NodeName& name) {
		for (const Node& node : nodes_) {
			if (node.name != name) continue;
			const spa_command command = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);
			return pw_node_send_command(reinterpret_cast<pw_node*>(node.proxy), &command) >= 0;
		}
		return false;
	}
	/// The node's state, once its info carried one.
	std::optional<pw_node_state> state(const NodeName& name) const {
		for (const Node& node : nodes_) {
			if (node.name == name && node.stateSeen) return node.state;
		}
		return std::nullopt;
	}

private:
	struct Node {
		EngineNodes* owner = nullptr;
		std::uint32_t id = 0;
		std::string name;
		pw_proxy* proxy = nullptr;
		spa_hook hook{};
		bool infoSeen = false;
		bool stateSeen = false;
		pw_node_state state = PW_NODE_STATE_CREATING;
		std::map<std::string, std::string> props;
	};

	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &EngineNodes::onGlobal;
			return events;
		}();
		return table;
	}
	static const pw_node_events& nodeEvents() {
		static const pw_node_events table = [] {
			pw_node_events events{};
			events.version = PW_VERSION_NODE_EVENTS;
			events.info = &EngineNodes::onInfo;
			return events;
		}();
		return table;
	}

	static void onGlobal(void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t,
	                     const spa_dict* props) {
		auto* self = static_cast<EngineNodes*>(data);
		if (std::string_view(type) != PW_TYPE_INTERFACE_Node || props == nullptr) return;
		const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		if (name == nullptr) return;
		Node& node = self->nodes_.emplace_back();
		node.owner = self;
		node.id = id;
		node.name = name;
		node.proxy =
			static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
		pw_proxy_add_object_listener(node.proxy, &node.hook, &nodeEvents(), &node);
		self->waiter_.wake();
	}

	static void onInfo(void* data, const pw_node_info* info) {
		auto* node = static_cast<Node*>(data);
		if (info->props != nullptr) {
			const spa_dict_item* item = nullptr;
			spa_dict_for_each(item, info->props) node->props[item->key] = item->value;
		}
		node->infoSeen = true;
		if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0) {
			node->state = info->state;
			node->stateSeen = true;
		}
		node->owner->waiter_.wake();
	}

	Waiter& waiter_;
	pw_registry* registry_ = nullptr;
	spa_hook hook_{};
	std::list<Node> nodes_;
};

const NodeLevels* find(const std::vector<NodeLevels>& snapshot, const NodeName& node) {
	const auto it = std::ranges::find(snapshot, node, &NodeLevels::node);
	return it == snapshot.end() ? nullptr : &*it;
}

std::uint64_t newestWindow(const std::vector<NodeLevels>& snapshot) {
	std::uint64_t newest = 0;
	for (const NodeLevels& node : snapshot) newest = std::max(newest, node.windowEnd);
	return newest;
}

/// Index of the first nonzero sample, or samples.size().
std::size_t firstSound(std::span<const float> samples) {
	return static_cast<std::size_t>(std::ranges::find_if(samples, [](float s) { return s != 0.0f; }) - samples.begin());
}

} // namespace

TEST_CASE("A linked output and input carry the source's level and an unlinked input is silent", "[blocks][levels]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Constant quarter(waiter, 0.25F);
	Constant half(waiter, 0.5F);
	BlockEvents srcEvents(waiter), dstEvents(waiter);
	auto src = blocks.make("src", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, srcEvents);
	REQUIRE(ear::testing::driveNow(*src, &quarter));
	auto dst = blocks.make("dst", Ports{{{Direction::input, "in", {"a", "b"}}, {Direction::output, "thru", {"a"}}}},
	                       kFormat, dstEvents);
	REQUIRE(ear::testing::driveNow(*dst, &half));
	REQUIRE(wiring.link(waiter, {"src", "out.a"}, {"dst", "in.a"}));

	// Two whole windows after the first window published once the link was active.
	std::uint64_t linkedBy = 0;
	REQUIRE(waiter.until([&] { return (linkedBy = newestWindow(blocks.latestLevels())) > 0; }));
	std::vector<NodeLevels> snapshot;
	REQUIRE(waiter.until([&] {
		snapshot = blocks.latestLevels();
		return find(snapshot, "src") && find(snapshot, "dst") &&
		       find(snapshot, "dst")->windowEnd >= linkedBy + 2 * kWindow;
	}));
	CHECK(bell.rings() > 0);
	const NodeLevels& srcLevels = *find(snapshot, "src");
	const NodeLevels& dstLevels = *find(snapshot, "dst");
	REQUIRE(srcLevels.ports.size() == 1);
	CHECK(srcLevels.ports[0].port == "out.a");
	CHECK(srcLevels.ports[0].level.peak == 0.25F);
	CHECK(srcLevels.ports[0].level.rms == Approx(0.25F).margin(1e-6));
	// Inputs then outputs, declaration order.
	REQUIRE(dstLevels.ports.size() == 3);
	CHECK(dstLevels.ports[0].port == "in.a");
	CHECK(dstLevels.ports[0].level.peak == 0.25F);
	CHECK(dstLevels.ports[0].level.rms == Approx(0.25F).margin(1e-6));
	CHECK(dstLevels.ports[1].port == "in.b");
	CHECK(dstLevels.ports[1].level.peak == 0.0F);
	CHECK(dstLevels.ports[1].level.rms == 0.0F);
	CHECK(dstLevels.ports[2].port == "thru.a");
	CHECK(dstLevels.ports[2].level.peak == 0.5F);
	CHECK(dstLevels.ports[2].level.rms == Approx(0.5F).margin(1e-6));
}

TEST_CASE("Blocks made in different cycles publish the same windows of the graph clock", "[blocks][levels]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant a(waiter, 0.0F), b(waiter, 0.0F), c(waiter, 0.0F);
	BlockEvents aEvents(waiter), bEvents(waiter), cEvents(waiter);
	auto blockA = blocks.make("a", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, aEvents);
	REQUIRE(ear::testing::driveNow(*blockA, &a));
	REQUIRE(waiter.until([&] { return a.calls() >= 3; }));
	auto blockB = blocks.make("b", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, bEvents);
	REQUIRE(ear::testing::driveNow(*blockB, &b));
	REQUIRE(waiter.until([&] { return b.calls() >= 5; }));
	auto blockC = blocks.make("c", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, cEvents);
	REQUIRE(ear::testing::driveNow(*blockC, &c));
	// Each started in its own cycle: a ran before b existed, b before c.
	CHECK(a.calls() > b.calls());

	std::vector<NodeLevels> snapshot;
	REQUIRE(waiter.until([&] {
		snapshot = blocks.latestLevels();
		return snapshot.size() == 3;
	}));
	// Every snapshot from here on holds all three at one window, a multiple of the window size.
	// (The predicate runs inside a main-loop callback, so it records rather than throws.)
	std::uint64_t last = 0;
	int snapshots = 0;
	std::optional<std::string> wrong;
	REQUIRE(waiter.until([&] {
		snapshot = blocks.latestLevels();
		if (snapshot.size() != 3) wrong = "a snapshot holds " + std::to_string(snapshot.size());
		for (const NodeLevels& node : snapshot) {
			if (node.windowEnd != snapshot[0].windowEnd || node.windowEnd % kWindow != 0) {
				wrong = node.node + " at " + std::to_string(node.windowEnd) + ", " + snapshot[0].node + " at " +
				        std::to_string(snapshot[0].windowEnd);
			}
		}
		if (!snapshot.empty() && snapshot[0].windowEnd != last) {
			last = snapshot[0].windowEnd;
			++snapshots;
		}
		return wrong.has_value() || snapshots >= 10;
	}));
	CHECK(wrong.value_or("") == "");
	CHECK(snapshots >= 10);
}

TEST_CASE("latestLevels holds only blocks that exist and have published", "[blocks][levels]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	BlockEvents keepEvents(waiter), goEvents(waiter);
	auto keep = blocks.make("keep", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, keepEvents);
	REQUIRE(waiter.until([&] { return find(blocks.latestLevels(), "keep") != nullptr; }));

	auto go = blocks.make("go", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, goEvents);
	CHECK(find(blocks.latestLevels(), "go") == nullptr); // no window yet
	REQUIRE(waiter.until([&] { return find(blocks.latestLevels(), "go") != nullptr; }));
	go.reset();
	const auto after = blocks.latestLevels();
	CHECK(find(after, "go") == nullptr);
	CHECK(find(after, "keep") != nullptr);
}

// The control port answers a ring without reading the levels while nobody has subscribed to
// them; the bell must still ring for the windows after that.
TEST_CASE("The bell rings for later windows when nobody reads the levels", "[blocks][levels]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	BlockEvents firstEvents(waiter), secondEvents(waiter);
	auto first = blocks.make("first", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, firstEvents);
	auto second = blocks.make("second", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, secondEvents);
	REQUIRE(waiter.until([&] { return bell.rings() >= 3; }));

	// Rung once per window, not once per block: over the next windows, at most one ring each.
	const auto ringsBefore = bell.rings();
	const auto windowBefore = newestWindow(blocks.latestLevels());
	REQUIRE(waiter.until([&] { return bell.rings() >= ringsBefore + 6; }));
	const auto windowAfter = newestWindow(blocks.latestLevels());
	const auto rings = bell.rings() - ringsBefore;
	INFO(rings << " rings from window end " << windowBefore << " to " << windowAfter);
	CHECK(rings <= (windowAfter - windowBefore) / kWindow + 1);
}

TEST_CASE("Processing, metering and publishing allocate nothing on the realtime thread", "[blocks][levels]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Constant source(waiter, 0.5F);
	BlockEvents srcEvents(waiter), dstEvents(waiter);
	auto src = blocks.make("src", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, srcEvents);
	REQUIRE(ear::testing::driveNow(*src, &source));
	auto dst = blocks.make("dst", Ports{{{Direction::input, "in", {"a", "b"}}}}, kFormat, dstEvents);
	REQUIRE(wiring.link(waiter, {"src", "out.a"}, {"dst", "in.a"}));
	REQUIRE(waiter.until([&] { return blocks.latestLevels().size() == 2; }));

	// From the source's next process() on, the realtime thread counts its allocations, over
	// several published windows.
	const std::uint64_t before = ear::pipewire::testing::countedAllocations();
	source.countHere.store(true);
	const std::uint64_t armedAt = source.calls();
	std::uint64_t firstWindow = 0;
	REQUIRE(waiter.until([&] {
		const auto snapshot = blocks.latestLevels();
		if (source.calls() <= armedAt || snapshot.empty()) return false;
		if (firstWindow == 0) firstWindow = newestWindow(snapshot);
		return newestWindow(snapshot) >= firstWindow + 4 * kWindow;
	}));
	source.countHere.store(false);
	const std::uint64_t disarmedAt = source.calls();
	REQUIRE(waiter.until([&] { return source.calls() > disarmedAt; }));
	CHECK(ear::pipewire::testing::countedAllocations() - before == 0);
}

// The blocking drive is gone; its two cases ("drive returns only when the old code can no longer run" and
// "drive on a block whose engine node is gone returns") are the asynchronous drive's: "drive returns at once and its
// completion arrives on the main loop" and "drive and fadeOut on a block whose engine node is gone complete by signal",
// below. What the first also showed of a silence stays here.
TEST_CASE("A drive of no code leaves the linked sink exact zeros", "[blocks][drive]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Ramp ramp;
	Recorder recorder(waiter);
	BlockEvents srcEvents(waiter), recEvents(waiter);
	auto src = blocks.make("ramp", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, srcEvents);
	REQUIRE(ear::testing::driveNow(*src, &ramp));
	auto rec = blocks.make("rec", Ports{{{Direction::input, "record", {"left"}}}}, kFormat, recEvents);
	REQUIRE(ear::testing::driveNow(*rec, &recorder));
	REQUIRE(wiring.link(waiter, {"ramp", "out.a"}, {"rec", "record.left"}));
	REQUIRE(waiter.until([&] {
		const auto samples = recorder.samples();
		return firstSound(samples) + 4 * kBlock <= samples.size();
	}));
	REQUIRE(ear::testing::driveNow(*src, nullptr));
	// The sink may still run once in the cycle the barrier landed in.
	const std::size_t from = recorder.samples().size() + kBlock;
	REQUIRE(waiter.until([&] { return recorder.samples().size() >= from + 8 * kBlock; }));
	const auto samples = recorder.samples().subspan(from);
	CHECK(firstSound(samples) == samples.size());
}

TEST_CASE("A block with inside ports: the inside output is live and routable, levels list "
          "only public ports",
          "[blocks][inside]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Ramp ramp;
	PassThrough through;
	Recorder recorder(waiter);
	BlockEvents srcEvents(waiter), cardEvents(waiter), recEvents(waiter);
	auto src = blocks.make("ramp", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, srcEvents);
	REQUIRE(ear::testing::driveNow(*src, &ramp));
	auto card = blocks.make(
		"card",
		Ports{{{Direction::input, "in", {"a"}}, {Direction::output, "to_card", {"1"}, PortBank::Reach::inside}}},
		kFormat, cardEvents);
	REQUIRE(ear::testing::driveNow(*card, &through));
	auto rec = blocks.make("rec", Ports{{{Direction::input, "record", {"left"}}}}, kFormat, recEvents);
	REQUIRE(ear::testing::driveNow(*rec, &recorder));

	REQUIRE(wiring.link(waiter, {"ramp", "out.a"}, {"card", "in.a"}));
	REQUIRE(wiring.link(waiter, {"card", "to_card.1"}, {"rec", "record.left"}));
	CHECK(wiring.ports.find("card", "to_card.1")->direction == Direction::output);

	constexpr std::size_t kWanted = 32 * kBlock;
	REQUIRE(waiter.until([&] {
		const auto samples = recorder.samples();
		return samples.size() - std::min(firstSound(samples), samples.size()) >= kWanted;
	}));
	const auto samples = recorder.samples();
	const std::size_t first = firstSound(samples);
	std::size_t breaks = 0;
	for (std::size_t i = first + 1; i < samples.size(); ++i) {
		if (samples[i] != samples[i - 1] + 1.0f) ++breaks;
	}
	CHECK(samples[first] >= 1.0f);
	CHECK(breaks == 0);

	std::vector<NodeLevels> snapshot;
	REQUIRE(waiter.until([&] {
		snapshot = blocks.latestLevels();
		return find(snapshot, "card") != nullptr;
	}));
	const NodeLevels& cardLevels = *find(snapshot, "card");
	REQUIRE(cardLevels.ports.size() == 1);
	CHECK(cardLevels.ports[0].port == "in.a");
}

TEST_CASE("Every block is in the ear.graph group and processes on the dummy driver alone", "[blocks][group]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Constant one(waiter, 0.0F), two(waiter, 0.0F);
	BlockEvents oneEvents(waiter), twoEvents(waiter);
	auto first = blocks.make("one", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, oneEvents);
	REQUIRE(ear::testing::driveNow(*first, &one));
	auto second = blocks.make("two", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, twoEvents);
	REQUIRE(ear::testing::driveNow(*second, &two));

	REQUIRE(
		waiter.until([&] { return nodes.props("one") && nodes.props("two") && one.calls() >= 8 && two.calls() >= 8; }));
	for (const char* name : {"one", "two"}) {
		const auto* props = nodes.props(name);
		const auto group = props->find(PW_KEY_NODE_GROUP);
		REQUIRE(group != props->end());
		CHECK(group->second == "ear.graph");
	}
	// The internal clock is in the group too, so it collects the blocks and whatever is linked to them first
	// (docs/engine.md#the-internal-clock), and it tracks the raw clock.
	REQUIRE(waiter.until([&] { return nodes.props("Dummy-Driver") != nullptr; }));
	const auto* dummy = nodes.props("Dummy-Driver");
	CHECK(dummy->at(PW_KEY_NODE_GROUP) == "ear.graph");
	CHECK(dummy->at("clock.name") == "clock.system.monotonic-raw");
}

namespace {

/// Records each cycle as process() was told it, and what time it was then, up to a fixed count.
class CycleLog final : public AudioCode {
public:
	explicit CycleLog(Waiter& waiter) : waiter_(waiter), seen_(kCapacity) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at < kCapacity) {
			timespec now{};
			timespec raw{};
			clock_gettime(CLOCK_MONOTONIC, &now);
			clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
			seen_[at] = {cycle, frames, static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec,
			             static_cast<std::int64_t>(raw.tv_sec) * 1000000000 + raw.tv_nsec, cycle.downstream.size()};
			seen_[at].cycle.downstream = {}; // the block's own memory: not kept past the cycle
			count_.store(at + 1, std::memory_order_release);
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	struct Seen {
		ear::GraphCycle cycle;
		std::uint32_t frames;
		std::int64_t nowNs;
		std::int64_t rawNs;
		std::size_t downstreamSize;
	};
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	const Seen& at(std::size_t index) const { return seen_[index]; }
	static constexpr std::size_t kCapacity = 64;

private:
	Waiter& waiter_;
	std::vector<Seen> seen_;
	std::atomic<std::size_t> count_{0};
};

} // namespace

TEST_CASE("Each cycle's position advances by its frames and its start is no later than now", "[port_blocks]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	CycleLog log(waiter);
	BlockEvents events(waiter);
	auto block = blocks.make("timed", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &log));
	REQUIRE(waiter.until([&] { return log.count() == CycleLog::kCapacity; }));
	REQUIRE(ear::testing::driveNow(*block, nullptr));

	std::size_t advancedByFrames = 0;
	for (std::size_t i = 1; i < CycleLog::kCapacity; ++i) {
		const auto& before = log.at(i - 1);
		const auto& now = log.at(i);
		// A loaded host's skipped cycle is a jump of whole blocks, the very thing the position tells.
		if (now.cycle.position == before.cycle.position + before.frames) ++advancedByFrames;
		CHECK((now.cycle.position - before.cycle.position) % kBlock == 0);
		CHECK(now.cycle.startNs > before.cycle.startNs);
	}
	for (std::size_t i = 0; i < CycleLog::kCapacity; ++i) {
		CHECK(log.at(i).frames == kBlock);
		CHECK(log.at(i).cycle.startNs <= log.at(i).nowNs);
		CHECK(log.at(i).nowNs - log.at(i).cycle.startNs <
		      1000000000); // the same clock: within a second, not an epoch away
	}
	CHECK(advancedByFrames >= (CycleLog::kCapacity - 1) / 2);
}

TEST_CASE("Each cycle carries its start on both clocks, the internal clock as its driver, its rate and the next "
          "cycle's start, and only the first is a discontinuity",
          "[port_blocks][clock]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	CycleLog log(waiter);
	BlockEvents events(waiter);
	auto block = blocks.make("timed", Ports{{{Direction::input, "in", {"a"}}, {Direction::output, "out", {"a", "b"}}}},
	                         kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &log));
	REQUIRE(waiter.until([&] { return log.count() == CycleLog::kCapacity && nodes.props("Dummy-Driver"); }));
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	const auto dummy = static_cast<std::uint32_t>(std::stoul(nodes.props("Dummy-Driver")->at(PW_KEY_OBJECT_ID)));

	const std::int64_t cycleNs = std::int64_t{kBlock} * 1000000000 / kRate;
	CHECK(log.at(0).cycle.discontinuity); // the first cycle the block runs
	std::size_t jumps = 0;
	std::size_t forward = 0;
	for (std::size_t i = 0; i < CycleLog::kCapacity; ++i) {
		const auto& seen = log.at(i);
		INFO("cycle " << i);
		CHECK(seen.cycle.driverId == dummy);
		// Both clocks read together: the raw start sits as far before the raw now as the start before now.
		CHECK(std::abs((seen.rawNs - seen.cycle.startRawNs) - (seen.nowNs - seen.cycle.startNs)) < 1000000);
		CHECK(seen.cycle.rateDiff == Approx(1.0).margin(0.01));
		CHECK(seen.cycle.nextNs - seen.cycle.startNs == Approx(cycleNs).epsilon(0.1));
		CHECK(seen.downstreamSize == 2); // one per output: out.a, out.b
		if (i > 0 && seen.cycle.discontinuity) ++jumps;
		if (i > 0 && seen.cycle.position > log.at(i - 1).cycle.position + log.at(i - 1).frames) ++forward;
		if (i > 0) CHECK(seen.cycle.xruns >= log.at(i - 1).cycle.xruns);
	}
	// A loaded host's stall makes the internal clock skip blocks to catch up (each a jump forward, and an xrun); a quiet
	// one makes none. The position never steps back (engine.conf's resync.ms; the stall test below).
	CHECK(forward <= log.at(CycleLog::kCapacity - 1).cycle.xruns);
	CHECK(jumps <= 2);

	// A new code's first cycle is a discontinuity to it, though the clock ran on.
	CycleLog next(waiter);
	REQUIRE(ear::testing::driveNow(*block, &next));
	REQUIRE(waiter.until([&] { return next.count() >= 4; }));
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	CHECK(next.at(0).cycle.discontinuity);
}

namespace {

/// Records the latency downstream of each of its outputs, latest cycle.
class LatencyProbe final : public AudioCode {
public:
	LatencyProbe(Waiter& waiter, std::size_t outputs) : waiter_(waiter), seen_(outputs) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		for (std::size_t i = 0; i < seen_.size(); ++i) {
			const Latency latency = cycle.downstreamOf(i);
			seen_[i].store((std::uint64_t{latency.minFrames} << 32U) | latency.maxFrames, std::memory_order_relaxed);
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	Latency downstream(std::size_t output) const {
		const std::uint64_t word = seen_[output].load(std::memory_order_relaxed);
		return Latency{static_cast<std::uint32_t>(word >> 32U), static_cast<std::uint32_t>(word)};
	}

private:
	Waiter& waiter_;
	std::vector<std::atomic<std::uint64_t>> seen_;
};

} // namespace

TEST_CASE("Latency is carried per bank pair: through a pass-through's pair to the block upstream, never across "
          "banks it does not pair, plus what a run declared",
          "[port_blocks][latency]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	// source:out -> mid:in, mid's pair in -> thru; mid:thru.1 -> sink:in (thru.2 is linked to nothing), and mid's
	// unpaired bank side -> other:in. sink declares 300 frames on its input (a following card would report its buffer
	// there), other 900.
	LatencyProbe sourceProbe(waiter, 2), midProbe(waiter, 3), sinkProbe(waiter, 0), otherProbe(waiter, 0);
	BlockEvents sourceEvents(waiter), midEvents(waiter), sinkEvents(waiter), otherEvents(waiter);
	auto source = blocks.make("source", Ports{{{Direction::output, "out", {"1"}}, {Direction::output, "spare", {"1"}}}},
	                          kFormat, sourceEvents);
	REQUIRE(ear::testing::driveNow(*source, &sourceProbe));
	auto mid = blocks.make("mid",
	                       Ports{{{Direction::input, "in", {"1"}},
	                              {Direction::output, "thru", {"1", "2"}},
	                              {Direction::output, "side", {"1"}}}},
	                       kFormat, midEvents);
	mid->carryLatency(LatencyPlan{{{"in", "thru"}}, {{"in", Latency{10, 20}}}});
	REQUIRE(ear::testing::driveNow(*mid, &midProbe));
	auto sink = blocks.make("sink", Ports{{{Direction::input, "in", {"1", "2"}}}}, kFormat, sinkEvents);
	sink->carryLatency(LatencyPlan{{}, {{"in", Latency{300, 300}}}});
	REQUIRE(ear::testing::driveNow(*sink, &sinkProbe));
	auto other = blocks.make("other", Ports{{{Direction::input, "in", {"1"}}}}, kFormat, otherEvents);
	other->carryLatency(LatencyPlan{{}, {{"in", Latency{900, 900}}}});
	REQUIRE(ear::testing::driveNow(*other, &otherProbe));
	// side is linked first, so whatever source hears after thru is linked already holds side's 900 if it leaked.
	REQUIRE(wiring.link(waiter, {"source", "out.1"}, {"mid", "in.1"}));
	REQUIRE(wiring.link(waiter, {"mid", "side.1"}, {"other", "in.1"}));
	REQUIRE(waiter.until([&] { return midProbe.downstream(2) == Latency{900, 900}; }));
	REQUIRE(wiring.link(waiter, {"mid", "thru.1"}, {"sink", "in.1"}));

	// mid's thru sees the sink's 300 (its unlinked thru.2 is no path, so the least stays 300), and side the other's
	// 900; mid's input tells thru's 300 plus its own 10..20 upstream, and side's 900 does not reach it (a leak would
	// tell 310..920). source's spare output is linked to nothing.
	CHECK(waiter.until([&] { return midProbe.downstream(0) == Latency{300, 300}; }));
	CHECK(waiter.until([&] { return midProbe.downstream(1) == Latency{300, 300}; }));
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{310, 320}; }));
	CHECK(midProbe.downstream(2) == Latency{900, 900});
	CHECK(sourceProbe.downstream(1) == Latency{});

	// A new plan replaces the last at once: mid unpaired carries nothing through, only its own.
	mid->carryLatency(LatencyPlan{{}, {{"in", Latency{10, 20}}}});
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{10, 20}; }));
	mid->carryLatency({});
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{}; }));

	REQUIRE(ear::testing::driveNow(*source, nullptr));
	REQUIRE(ear::testing::driveNow(*mid, nullptr));
	REQUIRE(ear::testing::driveNow(*sink, nullptr));
	REQUIRE(ear::testing::driveNow(*other, nullptr));
}

namespace {

/// A sink's realtime end that keeps only the last stamp's upstreamFrames.
class UpstreamTap final : public SinkTap {
public:
	explicit UpstreamTap(Waiter& waiter) : waiter_(waiter) {}
	void write(std::span<const std::span<const float>>, std::uint32_t, const ChunkStamp& stamp) noexcept override {
		upstream_.store(stamp.upstreamFrames, std::memory_order_relaxed);
		written_.store(true, std::memory_order_release);
		waiter_.wake();
	}
	std::optional<std::uint32_t> upstream() const {
		if (!written_.load(std::memory_order_acquire)) return std::nullopt;
		return upstream_.load(std::memory_order_relaxed);
	}

private:
	Waiter& waiter_;
	std::atomic<std::uint32_t> upstream_{0};
	std::atomic<bool> written_{false};
};

} // namespace

TEST_CASE("A companion's latency beyond an inside bank is one more peer of that bank: told on the paired output to "
          "the next block, and downstream of an inside output to its code and to what feeds the paired input",
          "[port_blocks][latency]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	// rx: a receiver's 480 to 960 beyond its inside input from_rtp, paired with stream; rx:stream.1 -> rec:in.1, and
	// rec's sink on writer, paired with in, stamps what rec tells there. Capture runs rx -> rec.
	LatencyProbe rxProbe(waiter, 1), recProbe(waiter, 1);
	BlockEvents rxEvents(waiter), recEvents(waiter);
	auto rx = blocks.make(
		"rx",
		Ports{{{Direction::input, "from_rtp", {"1"}, PortBank::Reach::inside}, {Direction::output, "stream", {"1"}}}},
		kFormat, rxEvents);
	rx->carryLatency(LatencyPlan{{{"from_rtp", "stream"}}, {}, {{"from_rtp", Latency{480, 960}}}});
	REQUIRE(ear::testing::driveNow(*rx, &rxProbe));
	auto rec = blocks.make(
		"rec", Ports{{{Direction::input, "in", {"1"}}, {Direction::output, "writer", {"1"}, PortBank::Reach::inside}}},
		kFormat, recEvents);
	rec->carryLatency(LatencyPlan{{{"in", "writer"}}, {}, {}});
	UpstreamTap tap(waiter);
	const std::vector<SinkBinding> bindings{SinkBinding{&tap, 0, 1}};
	bool driven = false;
	rec->drive(DriveRequest{&recProbe, {}, bindings}, [&](std::uint64_t) {
		driven = true;
		waiter.wake();
	});
	REQUIRE(waiter.until([&] { return driven; }));
	CHECK(waiter.until([&] { return tap.upstream() == 0U; })); // nothing linked to rec:in yet
	REQUIRE(wiring.link(waiter, {"rx", "stream.1"}, {"rec", "in.1"}));
	CHECK(waiter.until([&] { return tap.upstream() == 960U; }));

	// Playback: card's 256 beyond card's inside output to_card, paired with play; feed:out.1 -> card:play.1. card's
	// code sees 256 downstream of to_card, and feed sees it downstream of out.
	LatencyProbe feedProbe(waiter, 1), cardProbe(waiter, 1);
	BlockEvents feedEvents(waiter), cardEvents(waiter);
	auto feed = blocks.make("feed", Ports{{{Direction::output, "out", {"1"}}}}, kFormat, feedEvents);
	REQUIRE(ear::testing::driveNow(*feed, &feedProbe));
	auto card = blocks.make(
		"card",
		Ports{{{Direction::input, "play", {"1"}}, {Direction::output, "to_card", {"1"}, PortBank::Reach::inside}}},
		kFormat, cardEvents);
	card->carryLatency(LatencyPlan{{{"play", "to_card"}}, {}, {{"to_card", Latency{256, 256}}}});
	REQUIRE(ear::testing::driveNow(*card, &cardProbe));
	CHECK(waiter.until([&] { return cardProbe.downstream(0) == Latency{256, 256}; }));
	REQUIRE(wiring.link(waiter, {"feed", "out.1"}, {"card", "play.1"}));
	CHECK(waiter.until([&] { return feedProbe.downstream(0) == Latency{256, 256}; }));
	// A plan without it carries nothing.
	card->carryLatency(LatencyPlan{{{"play", "to_card"}}, {}, {}});
	CHECK(waiter.until([&] { return feedProbe.downstream(0) == Latency{}; }));
	CHECK(cardProbe.downstream(0) == Latency{});

	REQUIRE(ear::testing::driveNow(*rx, nullptr));
	REQUIRE(ear::testing::driveNow(*rec, nullptr));
	REQUIRE(ear::testing::driveNow(*feed, nullptr));
	REQUIRE(ear::testing::driveNow(*card, nullptr));
}

#if defined(__x86_64__) || defined(__i386__)
namespace {

/// Reads the MXCSR of the thread that runs process().
class Mxcsr final : public AudioCode {
public:
	explicit Mxcsr(Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		value_.store(_mm_getcsr() | 0x80000000U, std::memory_order_relaxed); // the top bit: seen
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint32_t value() const { return value_.load(std::memory_order_relaxed); }

private:
	Waiter& waiter_;
	std::atomic<std::uint32_t> value_{0};
};

} // namespace

TEST_CASE("process() runs with denormals flushed to zero (FTZ and DAZ), and the main thread is left alone",
          "[port_blocks][denormals]") {
	const std::uint32_t mainBefore = _mm_getcsr();
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Mxcsr probe(waiter);
	BlockEvents events(waiter);
	auto block = blocks.make("csr", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &probe));
	REQUIRE(waiter.until([&] { return probe.value() != 0; }));
	REQUIRE(ear::testing::driveNow(*block, nullptr));
	CHECK((probe.value() & 0x8000U) != 0); // FTZ
	CHECK((probe.value() & 0x0040U) != 0); // DAZ
	CHECK(_mm_getcsr() == mainBefore);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Slice 3a's PortBlock (docs/engine.md#drive-and-fade): the asynchronous drive, applied inside
// its invoke between two cycles; fadeOut; settle(); one invoke in flight per block; takenAt; blockId.

namespace {

constexpr std::uint32_t kFade = kRate * 5 / 1000; // 240 frames: the fade's length at 48 kHz

/// The fade's gain at `at` (0 to kFade), computed as the block computes it.
float fadeGain(std::uint32_t at) { return static_cast<float>(at) / static_cast<float>(kFade); }

/// One value per frame of the graph clock, nonzero and exact as a float (as spikes_test.cpp's).
float rampAt(std::uint64_t frame) noexcept { return static_cast<float>((frame & 0xFFFFFFU) + 1U); }

/// The context's one data loop (S3), the loop of every block: a test posts its own markers there.
pw_loop* contextDataLoop(Engine& engine) { return pw_data_loop_get_loop(pw_context_get_data_loop(engine.context())); }

/// A non-blocking no-op invoke on the data loop that says when it ran. Invokes run in posting order, so once it ran,
/// every invoke posted before it ran too.
class Marker {
public:
	void post(pw_loop* dataLoop) {
		ran_.store(false, std::memory_order_release);
		pw_loop_invoke(dataLoop, &Marker::onRun, 0, nullptr, 0, false, this);
	}
	/// Spins on the main thread (its loop not run) until the marker ran; false past the deadline.
	bool spin(std::chrono::milliseconds deadline = 2000ms) const {
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (!ran_.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() > until) return false;
			std::this_thread::yield();
		}
		return true;
	}

private:
	static int onRun(spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* data) {
		static_cast<Marker*>(data)->ran_.store(true, std::memory_order_release);
		return 0;
	}
	std::atomic<bool> ran_{false};
};

/// What a block's completions did, each `done` recorded on the main loop.
struct Done {
	explicit Done(Waiter& waiter) : waiter_(waiter) {}
	std::move_only_function<void(std::uint64_t)> drive(int tag) {
		return [this, tag](std::uint64_t taken) {
			order.push_back(tag);
			takens.push_back(taken);
			waiter_.wake();
		};
	}
	std::move_only_function<void()> fade(int tag) {
		return [this, tag] {
			order.push_back(tag);
			waiter_.wake();
		};
	}
	bool has(int tag) const { return std::ranges::find(order, tag) != order.end(); }
	std::vector<int> order;            // the tags, in the order their `done` ran
	std::vector<std::uint64_t> takens; // each drive's position, in order

private:
	Waiter& waiter_;
};

/// Writes rampAt(position + i) to every output, so a sample says which frame of the graph clock wrote it; records its
/// first cycle's position and the xrun count of each cycle.
class PositionRamp final : public AudioCode {
public:
	explicit PositionRamp(Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle& cycle) noexcept override {
		if (calls_.load(std::memory_order_relaxed) == 0) {
			first_.store(cycle.position, std::memory_order_relaxed);
			firstXruns_.store(cycle.xruns, std::memory_order_relaxed);
		}
		for (const std::span<float> out : outputs) {
			for (std::uint32_t i = 0; i < frames; ++i) out[i] = rampAt(cycle.position + i);
		}
		calls_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	std::uint64_t first() const { return first_.load(std::memory_order_relaxed); }
	std::uint64_t firstXruns() const { return firstXruns_.load(std::memory_order_relaxed); }

private:
	Waiter& waiter_;
	std::atomic<std::uint64_t> calls_{0};
	std::atomic<std::uint64_t> first_{0};
	std::atomic<std::uint64_t> firstXruns_{0};
};

/// Writes `value` to every output; once armed, sleeps 150 ms inside one process(), so requests made meanwhile reach the
/// data loop together, in one invoke, before any further cycle.
class Stalling final : public AudioCode {
public:
	Stalling(Waiter& waiter, float value) : waiter_(waiter), value_(value) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, value_);
		calls_.fetch_add(1, std::memory_order_release);
		if (armed.exchange(false, std::memory_order_acq_rel)) {
			stalling.store(true, std::memory_order_release);
			waiter_.wake();
			const timespec stall{0, 150'000'000};
			nanosleep(&stall, nullptr);
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
	std::atomic<bool> armed{false};
	std::atomic<bool> stalling{false};

private:
	Waiter& waiter_;
	float value_;
	std::atomic<std::uint64_t> calls_{0};
};

/// A probe downstream of a block: records every frame its inputs received, with the frame's clock position.
class Listener final : public ear::pipewire::testing::ProbeCode {
public:
	static constexpr std::size_t kCapacity = 6 * kRate;
	Listener(Waiter& waiter, std::size_t inputs)
		: waiter_(waiter), positions_(kCapacity), samples_(inputs, std::vector<float>(kCapacity)) {}
	void cycle(const spa_io_clock& clock, std::span<const float* const> inputs, std::span<float* const>,
	           std::uint32_t frames) noexcept override {
		std::size_t n = count_.load(std::memory_order_relaxed);
		for (std::uint32_t i = 0; i < frames && n < kCapacity; ++i, ++n) {
			positions_[n] = clock.position + i;
			for (std::size_t c = 0; c < samples_.size(); ++c) samples_[c][n] = inputs[c][i];
		}
		count_.store(n, std::memory_order_release);
		waiter_.wake();
	}
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	std::uint64_t position(std::size_t n) const { return positions_[n]; }
	float sample(std::size_t input, std::size_t n) const { return samples_[input][n]; }

private:
	Waiter& waiter_;
	std::vector<std::uint64_t> positions_;
	std::vector<std::vector<float>> samples_;
	std::atomic<std::size_t> count_{0};
};

/// The first recorded frame at or after `from` whose position is `position`, or none.
std::optional<std::size_t> frameAt(const Listener& listener, std::uint64_t position, std::size_t from = 0) {
	for (std::size_t n = from; n < listener.count(); ++n) {
		if (listener.position(n) == position) return n;
	}
	return std::nullopt;
}

const Ports oneOutput{{{Direction::output, "out", {"a"}}}};

} // namespace

TEST_CASE("drive returns at once and its completion arrives on the main loop, and after the completion no process of "
          "the old code runs",
          "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant a(waiter, 0.0F), b(waiter, 0.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	auto block = blocks.make("switch", oneOutput, kFormat, events);

	block->drive(DriveRequest{&a, {}}, done.drive(1));
	CHECK(done.order.empty()); // never inside the call
	REQUIRE(waiter.until([&] { return done.has(1) && a.calls() >= 3; }));
	block->drive(DriveRequest{&b, {}}, done.drive(2));
	CHECK(done.order.size() == 1);
	REQUIRE(waiter.until([&] { return done.has(2); }));
	const std::uint64_t aCalls = a.calls();
	REQUIRE(waiter.until([&] { return b.calls() >= 3; }));
	CHECK(a.calls() == aCalls);
	CHECK(done.order == std::vector<int>{1, 2});
	CHECK(done.takens[1] > done.takens[0]);
	CHECK(block->takenAt() == done.takens[1]);

	// A silence the same way: from its completion on, b runs no more.
	block->drive(DriveRequest{nullptr, {}}, done.drive(3));
	REQUIRE(waiter.until([&] { return done.has(3); }));
	const std::uint64_t bCalls = b.calls();
	const std::uint64_t aliveCalls = a.calls();
	Constant witness(waiter, 0.0F);
	BlockEvents witnessEvents(waiter);
	auto other = blocks.make("witness", oneOutput, kFormat, witnessEvents);
	other->drive(DriveRequest{&witness, {}}, done.drive(4));
	REQUIRE(waiter.until([&] { return witness.calls() >= 8; }));
	CHECK(b.calls() == bCalls);
	CHECK(a.calls() == aliveCalls);
}

TEST_CASE("drive and fadeOut on a block whose engine node is gone complete by signal, never inline",
          "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Constant before(waiter, 0.0F), after(waiter, 0.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	auto gone = blocks.make("gone", oneOutput, kFormat, events);
	gone->drive(DriveRequest{&before, {}}, done.drive(1));
	REQUIRE(waiter.until([&] { return before.calls() > 0 && nodes.props("gone") != nullptr; }));

	REQUIRE(nodes.destroy("gone"));
	REQUIRE(waiter.until([&] { return events.lostDetail.has_value(); }));
	const bool faded[] = {true};
	gone->drive(DriveRequest{&after, faded}, done.drive(2));
	gone->fadeOut(done.fade(3));
	gone->drive(DriveRequest{nullptr, {}}, done.drive(4));
	gone->fadeOut(done.fade(5));
	CHECK(done.order == std::vector<int>{1});
	REQUIRE(waiter.until([&] { return done.has(5); }));
	// Nothing processes, so each fade-out is acknowledged in the invoke that finds it, unless that invoke also holds
	// the drive behind it (the fade-out at 3 and the drive at 4 in one invoke): then it was cancelled, never run.
	const bool cancelled = done.order == std::vector<int>{1, 2, 4, 5};
	const bool acknowledged = done.order == std::vector<int>{1, 2, 3, 4, 5};
	INFO("completed: " << done.order.size());
	CHECK((cancelled || acknowledged));
	CHECK(after.calls() == 0);
}

// Settled for slice 3b's design (docs/design/companions.md): PipeWire runs a loop's invokes while a node on it is not
// processing, so a request applied in its invoke completes without a cycle.
TEST_CASE("A block that is PAUSED takes a drive in its invoke and completes it", "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Constant a(waiter, 0.0F), b(waiter, 0.0F), witness(waiter, 0.0F);
	BlockEvents events(waiter), witnessEvents(waiter);
	Done done(waiter);
	auto other = blocks.make("witness", oneOutput, kFormat, witnessEvents);
	other->drive(DriveRequest{&witness, {}}, done.drive(0));
	auto block = blocks.make("paused", oneOutput, kFormat, events);
	block->drive(DriveRequest{&a, {}}, done.drive(1));
	REQUIRE(waiter.until([&] { return a.calls() > 0 && nodes.state("paused") == PW_NODE_STATE_RUNNING; }));

	REQUIRE(nodes.suspend("paused"));
	REQUIRE(waiter.until([&] { return nodes.state("paused") == PW_NODE_STATE_SUSPENDED; }));
	// It processes no more: the data loop runs other blocks' cycles, never this one's.
	const std::uint64_t aCalls = a.calls();
	const std::uint64_t witnessCalls = witness.calls();
	REQUIRE(waiter.until([&] { return witness.calls() >= witnessCalls + 8; }));
	CHECK(a.calls() == aCalls);

	block->drive(DriveRequest{&b, {}}, done.drive(2));
	block->fadeOut(done.fade(3)); // not processing: acknowledged in its invoke
	REQUIRE(waiter.until([&] { return done.has(2) && done.has(3); }));
	// Again after other blocks ran more cycles: the same position, since this block ran none.
	const std::uint64_t moreCalls = witness.calls();
	REQUIRE(waiter.until([&] { return witness.calls() >= moreCalls + 4; }));
	block->drive(DriveRequest{&a, {}}, done.drive(4));
	REQUIRE(waiter.until([&] { return done.has(4); }));
	CHECK(b.calls() == 0);
	CHECK(a.calls() == aCalls);
	CHECK(done.order == std::vector<int>{0, 1, 2, 3, 4});
	REQUIRE(done.takens.size() == 4);
	CHECK(done.takens[3] == done.takens[2]);
	CHECK(block->takenAt() == done.takens[3]);
	CHECK(nodes.state("paused") == PW_NODE_STATE_SUSPENDED);
}

TEST_CASE("A fade-out reaches exactly 0 over 5 ms and is acknowledged in the cycle it did", "[blocks][fade][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Constant one(waiter, 1.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	Listener listener(waiter, 2);
	ear::pipewire::testing::Probe probe(*engine, waiter, "listener", 2, 0, listener);
	auto block = blocks.make("fader", Ports{{{Direction::output, "out", {"a", "b"}}}}, kFormat, events);
	const bool faded[] = {true, false}; // out.a fades, out.b never does
	block->drive(DriveRequest{&one, faded}, done.drive(1));
	REQUIRE(wiring.link(waiter, {"fader", "out.a"}, {"listener", "in.1"}));
	REQUIRE(wiring.link(waiter, {"fader", "out.b"}, {"listener", "in.2"}));
	// Both links carry full gain for a while (the fade-in long over).
	std::size_t full = 0;
	REQUIRE(waiter.until([&] {
		while (full < listener.count() && (listener.sample(0, full) != 1.0F || listener.sample(1, full) != 1.0F)) {
			++full;
		}
		return full + 4 * kBlock <= listener.count();
	}));

	const std::uint64_t cyclesAtFade = one.calls();
	block->fadeOut(done.fade(2));
	REQUIRE(waiter.until([&] { return done.has(2); }));
	const std::uint64_t cyclesAtAck = one.calls();
	const std::size_t ackedBy = listener.count();
	REQUIRE(waiter.until([&] { return listener.count() >= ackedBy + 4 * kBlock; }));

	// out.a: 1.0, then (kFade - k) / kFade for k = 0..kFade (its first frame still at full gain), then exact zeros.
	std::size_t start = full;
	while (start < listener.count() && listener.sample(0, start) == 1.0F) ++start;
	REQUIRE(start > full);
	--start; // k = 0 of the fade
	REQUIRE(start + kFade + 2 * kBlock < listener.count());
	for (std::uint32_t k = 0; k <= kFade; ++k) {
		INFO("fade frame " << k);
		CHECK(listener.sample(0, start + k) == fadeGain(kFade - k));
	}
	std::size_t nonzero = 0;
	for (std::size_t n = start + kFade; n < listener.count(); ++n) nonzero += listener.sample(0, n) != 0.0F ? 1 : 0;
	CHECK(nonzero == 0);
	// Contiguous frames: no cycle was lost across the fade.
	for (std::size_t n = start; n < start + kFade; ++n) CHECK(listener.position(n + 1) == listener.position(n) + 1);
	// out.b untouched.
	std::size_t notFull = 0;
	for (std::size_t n = full; n < listener.count(); ++n) notFull += listener.sample(1, n) != 1.0F ? 1 : 0;
	CHECK(notFull == 0);
	// 240 frames fit one cycle and the fade starts at a cycle's start, so the first cycle of the fade ends at zero and
	// signals; the acknowledgement reaches the main loop that many cycles later as the host lets it (not judged).
	INFO("cycles from fadeOut to its acknowledgement: " << cyclesAtAck - cyclesAtFade);
	CHECK(cyclesAtAck >= cyclesAtFade + 1);
}

TEST_CASE("A fade-out on a block that is not processing, or that leaves STREAMING mid-fade, is acknowledged by signal",
          "[blocks][fade][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Done done(waiter);

	SECTION("a block not yet STREAMING") {
		BlockEvents events(waiter);
		auto block = blocks.make("young", oneOutput, kFormat, events);
		// Posted in the turn that made it, so its invoke finds the block not STREAMING (the main loop has not heard
		// so yet), whenever the block's first STREAMING is dispatched beside the completion.
		CHECK(events.readyCount == 0);
		block->fadeOut(done.fade(1));
		CHECK(done.order.empty());
		REQUIRE(waiter.until([&] { return done.has(1); }));
	}

	SECTION("a block that leaves STREAMING with the fade pending") {
		// The data thread is held in one process() while the fade is posted and the node suspended; the fade's invoke
		// then finds the block still STREAMING (as the main loop last heard), no cycle of it follows, and the
		// acknowledgement comes from the invoke posted when the block left STREAMING.
		Stalling code(waiter, 1.0F);
		BlockEvents events(waiter);
		auto block = blocks.make("leaving", oneOutput, kFormat, events);
		const bool faded[] = {true};
		block->drive(DriveRequest{&code, faded}, done.drive(1));
		REQUIRE(waiter.until([&] {
			return code.calls() > 0 && events.readyCount == 1 && nodes.state("leaving") == PW_NODE_STATE_RUNNING;
		}));
		code.armed.store(true);
		REQUIRE(waiter.until([&] { return code.stalling.load(); }));
		block->fadeOut(done.fade(2));
		REQUIRE(nodes.suspend("leaving"));
		CHECK(done.order == std::vector<int>{1});
		REQUIRE(waiter.until([&] { return done.has(2); }));
		REQUIRE(waiter.until([&] { return nodes.state("leaving") == PW_NODE_STATE_SUSPENDED; }));
		CHECK(done.order == std::vector<int>{1, 2});
	}
}

TEST_CASE("A cycle too large to run counts as one that ended at zero", "[blocks][fade][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant code(waiter, 1.0F), witness(waiter, 0.0F);
	BlockEvents events(waiter), witnessEvents(waiter);
	Done done(waiter);
	// The engine runs 256-frame cycles; a block whose largest is 128 runs none of them (its outputs zeros).
	auto block = blocks.make("small", oneOutput, AudioFormat{kRate, kBlock / 2}, events);
	auto other = blocks.make("witness", oneOutput, kFormat, witnessEvents);
	other->drive(DriveRequest{&witness, {}}, done.drive(0));
	REQUIRE(waiter.until([&] { return events.readyCount == 1 && witness.calls() >= 4; }));

	const bool faded[] = {true};
	block->drive(DriveRequest{&code, faded}, done.drive(1));
	REQUIRE(waiter.until([&] { return done.has(1); }));
	block->fadeOut(done.fade(2));
	REQUIRE(waiter.until([&] { return done.has(2); }));
	// Take-up goes on by position all the same.
	const std::uint64_t witnessCalls = witness.calls();
	REQUIRE(waiter.until([&] { return witness.calls() >= witnessCalls + 4; }));
	block->drive(DriveRequest{nullptr, {}}, done.drive(3));
	REQUIRE(waiter.until([&] { return done.has(3); }));
	CHECK(code.calls() == 0);
	REQUIRE(done.takens.size() == 3);
	CHECK(done.takens[2] > done.takens[1]);
	CHECK(done.order == std::vector<int>{0, 1, 2, 3});
}

/// Writes `value` to every output; once armed, holds the data thread inside one process() until released or for 8 ms at
/// most (under max_resync's 10 ms, so the driver does not resync and every cycle reaches the probes downstream), so
/// requests made meanwhile reach the data loop together, in one invoke, before any further cycle.
class Holding final : public AudioCode {
public:
	Holding(Waiter& waiter, float value) : waiter_(waiter), value_(value) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle&) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, value_);
		if (armed.exchange(false, std::memory_order_acq_rel)) {
			holding.store(true, std::memory_order_release);
			waiter_.wake();
			const auto until = std::chrono::steady_clock::now() + 8ms;
			while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) {
			}
			overran.store(!released.load(std::memory_order_acquire), std::memory_order_release);
			holding.store(false, std::memory_order_release);
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	std::atomic<bool> armed{false};
	std::atomic<bool> holding{false};
	std::atomic<bool> released{false};
	std::atomic<bool> overran{false}; // the hold ended before the main thread released it

private:
	Waiter& waiter_;
	float value_;
};

TEST_CASE("Every drive fades in over 5 ms and cancels a pending fade-out", "[blocks][fade][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	Holding half(waiter, 0.5F);
	Constant one(waiter, 1.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	Listener listener(waiter, 1);
	ear::pipewire::testing::Probe probe(*engine, waiter, "listener", 1, 0, listener);
	auto block = blocks.make("fader", oneOutput, kFormat, events);
	const bool faded[] = {true};
	REQUIRE(wiring.link(waiter, {"fader", "out.a"}, {"listener", "in.1"}));
	REQUIRE(waiter.until([&] { return listener.count() >= 4 * kBlock; })); // silent so far

	// A drive fades in from 0: frame k after its position carries k / kFade of the code's output.
	block->drive(DriveRequest{&half, faded}, done.drive(1));
	REQUIRE(waiter.until([&] { return done.has(1); }));
	const std::uint64_t firstTaken = done.takens[0];
	REQUIRE(waiter.until([&] { return frameAt(listener, firstTaken + kFade + kBlock).has_value(); }));
	const std::size_t from = *frameAt(listener, firstTaken);
	CHECK(listener.sample(0, from - 1) == 0.0F);
	for (std::uint32_t k = 0; k <= kFade; ++k) {
		INFO("fade-in frame " << k);
		CHECK(listener.sample(0, from + k) == 0.5F * fadeGain(k));
	}
	CHECK(listener.sample(0, from + kFade + 1) == 0.5F);

	// A fade-out and then a drive reach the data loop in one invoke, while it is held in one process(): the drive
	// cancels the fade-out (its done never runs) and fades in from 0 again, whatever gain the fade-out had reached. A
	// hold the main loop was too slow to use (a loaded host) is tried again.
	int tag = 2;
	bool held = false;
	for (int attempt = 0; attempt < 10 && !held; ++attempt) {
		half.released.store(false);
		block->drive(DriveRequest{&half, faded}, done.drive(tag++));
		REQUIRE(waiter.until([&] { return done.has(tag - 1); }));
		const std::size_t before = listener.count();
		REQUIRE(waiter.until([&] { return listener.count() >= before + 2 * kBlock; })); // at full gain again
		half.armed.store(true);
		REQUIRE(waiter.until([&] { return half.holding.load(); }));
		const std::size_t heldAt = listener.count();
		const int fadeTag = tag++;
		const int driveTag = tag++;
		block->fadeOut(done.fade(fadeTag));
		block->drive(DriveRequest{&one, faded}, done.drive(driveTag));
		half.released.store(true);
		REQUIRE(waiter.until([&] { return done.has(driveTag); }));
		if (half.overran.load()) {
			WARN("the hold ended before both requests were made; again");
			continue;
		}
		held = true;
		const std::uint64_t taken = done.takens.back();
		std::size_t again = heldAt;
		REQUIRE(waiter.until([&] {
			while (again < listener.count() && listener.sample(0, again) == 0.5F) ++again;
			return again + kFade + kBlock < listener.count();
		}));
		CHECK(listener.position(again) == taken);
		CHECK(listener.sample(0, again - 1) == 0.5F); // the old code at full gain up to the drive
		for (std::uint32_t k = 0; k <= kFade; ++k) {
			INFO("second fade-in frame " << k);
			CHECK(listener.sample(0, again + k) == fadeGain(k));
		}
		// The cancelled fade-out's done is dropped; a later fade-out is acknowledged as usual.
		block->fadeOut(done.fade(tag++));
		REQUIRE(waiter.until([&] { return done.has(tag - 1); }));
		CHECK_FALSE(done.has(fadeTag));
		CHECK(done.order.back() == tag - 1);
	}
	CHECK(held);
}

TEST_CASE("The published position is where the new code's first sample is", "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Wiring wiring(*engine, waiter);
	BlockEvents events(waiter);
	Done done(waiter);
	Listener listener(waiter, 1);
	ear::pipewire::testing::Probe probe(*engine, waiter, "listener", 1, 0, listener);
	auto block = blocks.make("source", oneOutput, kFormat, events);
	REQUIRE(wiring.link(waiter, {"source", "out.a"}, {"listener", "in.1"}));
	REQUIRE(waiter.until([&] { return listener.count() >= 4 * kBlock; }));

	// Several take-ups, each a fresh code (a ramp by position), each between silences.
	for (int round = 0; round < 5; ++round) {
		INFO("round " << round);
		PositionRamp ramp(waiter);
		block->drive(DriveRequest{&ramp, {}}, done.drive(round * 2 + 1));
		REQUIRE(waiter.until([&] { return done.has(round * 2 + 1) && ramp.calls() >= 2; }));
		const std::uint64_t taken = done.takens.back();
		CHECK(block->takenAt() == taken);
		// The code's first cycle is at the published position, unless the clock jumped (an xrun) right there.
		if (ramp.firstXruns() == 0 || ramp.first() == taken) {
			CHECK(ramp.first() == taken);
		} else {
			WARN("an xrun at the take-up: first cycle at " << ramp.first() << ", published " << taken);
		}
		// Downstream, the first frame of the ramp is at the published position, and the frame before it is silent.
		std::optional<std::size_t> first;
		REQUIRE(waiter.until([&] { return (first = frameAt(listener, ramp.first())).has_value(); }));
		CHECK(listener.sample(0, *first) == rampAt(ramp.first()));
		if (*first > 0 && listener.position(*first - 1) + 1 == ramp.first()) {
			CHECK(listener.sample(0, *first - 1) == 0.0F);
		}
		block->drive(DriveRequest{nullptr, {}}, done.drive(round * 2 + 2));
		REQUIRE(waiter.until([&] { return done.has(round * 2 + 2); }));
		const std::uint64_t calls = ramp.calls();
		const std::size_t heard = listener.count();
		REQUIRE(waiter.until([&] { return listener.count() >= heard + 2 * kBlock; }));
		CHECK(ramp.calls() == calls);
	}
}

TEST_CASE("settle() runs a completion whose event has not been dispatched", "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant a(waiter, 0.0F), b(waiter, 0.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	auto block = blocks.make("settled", oneOutput, kFormat, events);
	block->drive(DriveRequest{&a, {}}, done.drive(1));
	REQUIRE(waiter.until([&] { return done.has(1) && a.calls() > 0; }));

	// One main-loop turn: post the drive, wait (without running the loop) until the data loop ran it, then settle.
	Marker marker;
	block->drive(DriveRequest{&b, {}}, done.drive(2));
	block->fadeOut(done.fade(3));
	marker.post(contextDataLoop(*engine));
	REQUIRE(marker.spin());
	CHECK(done.order == std::vector<int>{1});
	block->settle();
	REQUIRE(done.order.size() >= 2);
	CHECK(done.order[1] == 2);
	// The event, dispatched later, runs nothing twice; the fade completes in its own time.
	REQUIRE(waiter.until([&] { return done.has(3); }));
	const std::uint64_t bCalls = b.calls();
	REQUIRE(waiter.until([&] { return b.calls() >= bCalls + 4; }));
	block->settle();
	CHECK(done.order == std::vector<int>{1, 2, 3});
}

TEST_CASE("A thousand requests to one block keep one invoke in flight", "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant a(waiter, 0.0F), b(waiter, 0.0F);
	BlockEvents events(waiter);
	Done done(waiter);
	auto block = blocks.make("busy", oneOutput, kFormat, events);
	block->drive(DriveRequest{&a, {}}, done.drive(0));
	REQUIRE(waiter.until([&] { return done.has(0) && a.calls() > 0; }));

	// In one main-loop turn: 1,000 requests, then a marker invoke behind whatever the block posted. Once the marker
	// ran, every invoke the block posted before it ran: with one in flight, that is one invoke, which drained at most
	// the ring's eight requests; the next is posted only once the main loop hears the completion.
	constexpr int kRequests = 1000;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 1; i <= kRequests; ++i) {
		if (i % 10 == 0) {
			block->fadeOut(done.fade(i));
		} else {
			block->drive(DriveRequest{i % 2 == 0 ? &a : &b, {}}, done.drive(i));
		}
	}
	const double postMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	Marker marker;
	marker.post(contextDataLoop(*engine));
	REQUIRE(marker.spin());
	block->settle();
	const std::size_t byFirstInvoke = done.order.size() - 1;
	INFO("requests completed by the block's first invoke: " << byFirstInvoke << "; 1,000 requests cost " << postMs
	                                                        << " ms");
	CHECK(byFirstInvoke >= 1);
	CHECK(byFirstInvoke <= 8);
	CHECK(postMs < 100.0);

	// Every drive completes, in order; a fade-out completes unless a drive behind it reached the data loop first (then
	// it was cancelled, its done dropped). The last request is a fade-out with nothing behind it.
	REQUIRE(waiter.until([&] { return done.has(kRequests); }, 20s));
	CHECK(std::ranges::is_sorted(done.order));
	CHECK(std::ranges::adjacent_find(done.order) == done.order.end());
	int drives = 0;
	for (const int tag : done.order) drives += tag % 10 != 0 ? 1 : 0;
	CHECK(drives == kRequests - kRequests / 10); // tags 1 to 1,000 but every tenth (tag 0 counts with the fades)
	CHECK(done.takens.size() == static_cast<std::size_t>(drives + 1));
	CHECK(std::ranges::is_sorted(done.takens));
}

namespace {

/// A sink's realtime end that only counts its writes: the block's tap path (the binding, the told latency's read, the
/// stamp) runs in full before it.
class CountingTap final : public SinkTap {
public:
	void write(std::span<const std::span<const float>>, std::uint32_t, const ChunkStamp&) noexcept override {
		writes_.fetch_add(1, std::memory_order_relaxed);
	}
	std::uint64_t writes() const { return writes_.load(std::memory_order_relaxed); }

private:
	std::atomic<std::uint64_t> writes_{0};
};

} // namespace

TEST_CASE("Drives, fades, their completions and a bound sink's tap allocate nothing on the data thread",
          "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	Constant a(waiter, 0.5F), b(waiter, 0.25F);
	BlockEvents events(waiter);
	Done done(waiter);
	auto block = blocks.make(
		"quiet",
		Ports{{{Direction::output, "out", {"a", "b"}}, {Direction::output, "writer", {"1"}, PortBank::Reach::inside}}},
		kFormat, events);
	block->carryLatency(LatencyPlan{{}, {{"writer", Latency{3, 5}}}, {}}); // a told latency for the stamps to read
	CountingTap tap;
	const std::vector<SinkBinding> sinks{SinkBinding{&tap, 2, 1}};
	const bool faded[] = {true, false};
	block->drive(DriveRequest{&a, faded, sinks}, done.drive(0));
	REQUIRE(waiter.until([&] { return done.has(0) && a.calls() > 0; }));

	// Every process() from here on turns counting on for the data thread, where the invokes run too.
	a.countHere.store(true);
	b.countHere.store(true);
	const std::uint64_t armedAt = a.calls();
	REQUIRE(waiter.until([&] { return a.calls() > armedAt; }));
	const std::uint64_t before = ear::pipewire::testing::countedAllocations();
	int tag = 1;
	for (int round = 0; round < 50; ++round) {
		block->drive(DriveRequest{round % 2 == 0 ? &b : &a, faded, sinks}, done.drive(tag++));
		block->fadeOut(done.fade(tag++));
		block->drive(DriveRequest{round % 2 == 0 ? &a : &b, faded, sinks}, done.drive(tag++));
		const std::size_t want = done.order.size() + 2; // the fade between the two drives is cancelled, or acknowledged
		REQUIRE(waiter.until([&] { return done.order.size() >= want && done.has(tag - 1); }));
	}
	const std::uint64_t counted = ear::pipewire::testing::countedAllocations() - before;
	a.countHere.store(false);
	b.countHere.store(false);
	const std::uint64_t calls = a.calls() + b.calls();
	REQUIRE(waiter.until([&] { return a.calls() + b.calls() > calls + 1; }));
	CHECK(counted == 0);
	CHECK(tap.writes() > 0);
	REQUIRE(ear::testing::driveNow(*block, nullptr)); // unbinds the tap before it goes
}

// Settled for slice 3b's design (docs/design/companions.md): pw_filter_get_node_id gives the block's global id once
// STREAMING, and an engine companion's link made by it (as joinEngineCompanion writes link.output.node) reaches that
// block even beside an older block of the same name.
TEST_CASE("A block's id is its engine node's global id once STREAMING, and a link made by it reaches that block, not "
          "another of its name",
          "[blocks][drive][async]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Constant older(waiter, 0.25F), newer(waiter, 0.5F);
	BlockEvents olderEvents(waiter), newerEvents(waiter);
	Done done(waiter);
	auto first = blocks.make("twin", oneOutput, kFormat, olderEvents);
	CHECK(first->blockId() == SPA_ID_INVALID);
	first->drive(DriveRequest{&older, {}}, done.drive(1));
	auto second = blocks.make("twin", oneOutput, kFormat, newerEvents);
	second->drive(DriveRequest{&newer, {}}, done.drive(2));
	Listener listener(waiter, 1);
	ear::pipewire::testing::Probe probe(*engine, waiter, "listener", 1, 0, listener);
	REQUIRE(waiter.until([&] {
		return olderEvents.readyCount == 1 && newerEvents.readyCount == 1 && probe.streaming() &&
		       nodes.ids("twin").size() == 2;
	}));
	const std::vector<std::uint32_t> ids = nodes.ids("twin");
	CHECK(first->blockId() != SPA_ID_INVALID);
	CHECK(second->blockId() != SPA_ID_INVALID);
	CHECK(first->blockId() != second->blockId());
	CHECK(std::ranges::find(ids, first->blockId()) != ids.end());
	CHECK(std::ranges::find(ids, second->blockId()) != ids.end());

	// The link as link-factory makes it for an engine companion: each end by node id, each port by name.
	const std::string output = std::to_string(second->blockId());
	const std::string input = std::to_string(probe.nodeId());
	pw_properties* props =
		pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, output.c_str(), PW_KEY_LINK_OUTPUT_PORT, "out.a",
	                      PW_KEY_LINK_INPUT_NODE, input.c_str(), PW_KEY_LINK_INPUT_PORT, "in.1", nullptr);
	auto* link = static_cast<pw_proxy*>(pw_core_create_object(engine->core(), "link-factory", PW_TYPE_INTERFACE_Link,
	                                                          PW_VERSION_LINK, &props->dict, 0));
	pw_properties_free(props);
	REQUIRE(link != nullptr);
	std::size_t heard = 0;
	REQUIRE(waiter.until([&] {
		while (heard < listener.count() && listener.sample(0, heard) == 0.0F) ++heard;
		return heard + 4 * kBlock <= listener.count();
	}));
	std::size_t notNewer = 0;
	for (std::size_t n = heard; n < listener.count(); ++n) notNewer += listener.sample(0, n) != 0.5F ? 1 : 0;
	CHECK(notNewer == 0);
	pw_proxy_destroy(link);
}

namespace {

/// Records each cycle's position and raw start, and stalls the data thread once for each of `stalls` (cycle, ms).
class StallingLog final : public AudioCode {
public:
	struct Stall {
		std::size_t atCycle;
		std::chrono::milliseconds length;
	};
	StallingLog(Waiter& waiter, std::size_t capacity, std::vector<Stall> stalls)
		: waiter_(waiter), position_(capacity), startRawNs_(capacity), rateDiff_(capacity), stalls_(std::move(stalls)) {
	}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs, std::uint32_t,
	             const ear::GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at < position_.size()) {
			position_[at] = cycle.position;
			startRawNs_[at] = cycle.startRawNs;
			rateDiff_[at] = cycle.rateDiff;
			count_.store(at + 1, std::memory_order_release);
			for (const Stall& stall : stalls_) {
				if (stall.atCycle == at) std::this_thread::sleep_for(stall.length); // the data thread stalls
			}
		}
		waiter_.wake();
	}
	void release() noexcept override {}
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	std::size_t capacity() const { return position_.size(); }
	/// Where the graph's position lies against the raw clock, in frames, at cycle `i` (a constant while it tracks it).
	double phase(std::size_t i) const {
		return static_cast<double>(position_[i]) - static_cast<double>(startRawNs_[i]) * kRate / 1e9;
	}
	/// The phase averaged over cycles [from, to).
	double phase(std::size_t from, std::size_t to) const {
		double sum = 0.0;
		for (std::size_t i = from; i < to; ++i) sum += phase(i) - phase(0);
		return sum / static_cast<double>(to - from);
	}
	std::vector<std::uint64_t> position_;
	std::vector<std::int64_t> startRawNs_;
	std::vector<double> rateDiff_;

private:
	Waiter& waiter_;
	std::vector<Stall> stalls_;
	std::atomic<std::size_t> count_{0};
};

} // namespace

// node-driver.c (PipeWire 1.4.2), tracking the raw clock, resets clock.position to raw time past resync.ms, but leaves
// its CLOCK_MONOTONIC timer where it was: the timeouts it then fires back to back, to catch up, each add a block while
// raw time barely moves, stepping the position back by the next reset, and leave it up to resync.ms ahead of the raw
// clock. There its error is clamped to 1 ms a cycle, and the loop slews the whole graph by hundreds of ppm for tens of
// seconds to work it off (-230 ppm 9 s after one 20 ms stall). engine.conf sets resync.ms beyond any stall, so the
// driver never resets: it catches up on its own timer, by whole blocks, and after stalls of up to a few seconds the
// phase it held against the raw clock is kept (docs/engine.md#the-internal-clock; longer pauses: the next test).
TEST_CASE("After data-thread stalls past 10 ms the internal clock catches up in whole blocks, never steps back, and "
          "keeps its phase to the raw clock",
          "[port_blocks][clock]") {
	using Stall = StallingLog::Stall;
	const std::vector<Stall> stalls{{200, 30ms}, {600, 60ms}, {1000, 120ms}};
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	StallingLog log(waiter, 1400, stalls);
	BlockEvents events(waiter);
	auto block = blocks.make("stalling", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &log));
	REQUIRE(waiter.until([&] { return log.count() == log.capacity(); }, 30s));
	REQUIRE(ear::testing::driveNow(*block, nullptr));

	std::size_t back = 0, notWhole = 0, jumps = 0;
	for (std::size_t i = 1; i < log.capacity(); ++i) {
		const std::uint64_t before = log.position_[i - 1], now = log.position_[i];
		if (now <= before) ++back;
		else if ((now - before) % kBlock != 0) ++notWhole;
		else if (now - before > kBlock) ++jumps;
	}
	// Whether the catching up runs every missed cycle or skips some (whole-block jumps) depends on how fast the host is:
	// a fast one runs them all, so `jumps` is reported, never required. What matters is checked: never back, whole
	// blocks only, and the phase to the raw clock kept (below), which shows every stall's lost time was caught up.
	INFO("jumps " << jumps);
	CHECK(back == 0);
	CHECK(notWhole == 0);
	for (const Stall& stall : stalls) {
		// Before: the cycles just ahead of the stall. After: a second on, when the catching up is long over. The loop's own
		// wander over that second is a few frames (140 ppm at worst, before it has settled: 7 frames).
		const double before = log.phase(stall.atCycle - 60, stall.atCycle - 10);
		const double after = log.phase(stall.atCycle + 190, stall.atCycle + 240);
		INFO("stall of " << stall.length.count() << " ms at cycle " << stall.atCycle << ": phase " << before
		                 << " frames before, " << after << " after");
		CHECK(std::abs(after - before) < 48.0); // 1 ms: the driver's own error clamp, well under one block
	}
}

// A stall of seconds is caught up the same way, by a burst of about half its timeouts' blocks, but the loop is fed its
// clamped 1 ms error at every one of the burst's timeouts, so it winds up and slews the graph for a while after (the
// limit of resync.ms beyond any stall; docs/engine.md#the-internal-clock). For a 3 s stall the simulated loop
// (node-driver.c's DLL, cycle for cycle) is within 80 ppm of the raw clock from 5 s on, and the real one measured 38 to
// 95 ppm in ten runs; 150 ppm bounds it here.
TEST_CASE(
	"After a 3 s data-thread stall the internal clock never steps back, and 5 s on runs within 150 ppm of the raw "
	"clock",
	"[port_blocks][clock]") {
	using Stall = StallingLog::Stall;
	constexpr std::size_t stallAt = 200;
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	StallingLog log(waiter, 2400, {Stall{stallAt, 3000ms}});
	BlockEvents events(waiter);
	auto block = blocks.make("stalling", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, events);
	REQUIRE(ear::testing::driveNow(*block, &log));
	REQUIRE(waiter.until([&] { return log.count() == log.capacity(); }, 60s));
	REQUIRE(ear::testing::driveNow(*block, nullptr));

	std::size_t back = 0;
	for (std::size_t i = 1; i < log.capacity(); ++i) back += log.position_[i] <= log.position_[i - 1] ? 1 : 0;
	CHECK(back == 0);
	// From 5 s after the stall ended to the log's end.
	const std::int64_t stallEndNs = log.startRawNs_[stallAt] + 3'000'000'000;
	double worstPpm = 0.0;
	std::size_t judged = 0;
	for (std::size_t i = stallAt + 1; i < log.capacity(); ++i) {
		if (log.startRawNs_[i] < stallEndNs + 5'000'000'000) continue;
		++judged;
		worstPpm = std::max(worstPpm, std::abs(log.rateDiff_[i] - 1.0) * 1e6);
	}
	INFO("judged " << judged << " cycles, worst rate " << worstPpm << " ppm off the raw clock");
	REQUIRE(judged > 100);
	CHECK(worstPpm < 150.0);
}
