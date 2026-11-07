// FilterPortBlocks on the real in-process engine (docs/design/batch2.md section 4.3; acceptance
// criteria E1(2)-(8)): levels on the graph clock, complete snapshots, the drive barrier, a lost
// block, inside ports and the graph group; docs/design/node-framework.md slice 7: each cycle's
// clock facts, latency carried per bank pair, and denormals flushed on the data thread. Every wait is event-gated: the main loop runs until
// the events a test waits for have arrived; the deadline only fails the test. The Waiter and
// makeEngine follow engine_test.cpp's (the pipewire-engine skill's pattern).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"

// Counts allocations made by a thread that asks for it (the realtime thread, from a test
// code's process() on). handoff_test.cpp in this binary already replaces operator new, so this
// file counts one level lower: it replaces malloc, calloc and realloc (glibc supports that) and
// forwards to glibc's own; operator new reaches malloc either way.
extern "C" {
void* __libc_malloc(std::size_t size) noexcept;
void* __libc_calloc(std::size_t count, std::size_t size) noexcept;
void* __libc_realloc(void* memory, std::size_t size) noexcept;
}
namespace {
std::atomic<std::uint64_t> realtimeAllocations{0};
thread_local bool countAllocations = false;
void counted() noexcept {
	if (countAllocations) realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
}
} // namespace
extern "C" {
void* malloc(std::size_t size) noexcept {
	counted();
	return __libc_malloc(size);
}
void* calloc(std::size_t count, std::size_t size) noexcept {
	counted();
	return __libc_calloc(count, size);
}
void* realloc(void* memory, std::size_t size) noexcept {
	counted();
	return __libc_realloc(memory, size);
}
}

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
		countAllocations = countHere.load(std::memory_order_acquire);
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

private:
	struct Node {
		EngineNodes* owner = nullptr;
		std::uint32_t id = 0;
		std::string name;
		pw_proxy* proxy = nullptr;
		spa_hook hook{};
		bool infoSeen = false;
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
	src->drive(&quarter);
	auto dst = blocks.make("dst", Ports{{{Direction::input, "in", {"a", "b"}}, {Direction::output, "thru", {"a"}}}},
	                       kFormat, dstEvents);
	dst->drive(&half);
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
	blockA->drive(&a);
	REQUIRE(waiter.until([&] { return a.calls() >= 3; }));
	auto blockB = blocks.make("b", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, bEvents);
	blockB->drive(&b);
	REQUIRE(waiter.until([&] { return b.calls() >= 5; }));
	auto blockC = blocks.make("c", Ports{{{Direction::input, "in", {"a"}}}}, kFormat, cEvents);
	blockC->drive(&c);
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
	src->drive(&source);
	auto dst = blocks.make("dst", Ports{{{Direction::input, "in", {"a", "b"}}}}, kFormat, dstEvents);
	REQUIRE(wiring.link(waiter, {"src", "out.a"}, {"dst", "in.a"}));
	REQUIRE(waiter.until([&] { return blocks.latestLevels().size() == 2; }));

	// From the source's next process() on, the realtime thread counts its allocations, over
	// several published windows.
	const std::uint64_t before = realtimeAllocations.load();
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
	CHECK(realtimeAllocations.load() - before == 0);
}

TEST_CASE("drive returns only when the old code can no longer run, and nullptr silences", "[blocks][drive]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);

	SECTION("a stops when drive(&b) returns") {
		Constant a(waiter, 0.0F), b(waiter, 0.0F);
		BlockEvents events(waiter);
		auto block = blocks.make("switch", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, events);
		block->drive(&a);
		REQUIRE(waiter.until([&] { return a.calls() >= 3; }));
		block->drive(&b);
		const std::uint64_t aCalls = a.calls();
		REQUIRE(waiter.until([&] { return b.calls() >= 3; }));
		CHECK(a.calls() == aCalls);
	}

	SECTION("drive(nullptr) leaves the linked sink exact zeros") {
		Wiring wiring(*engine, waiter);
		Ramp ramp;
		Recorder recorder(waiter);
		BlockEvents srcEvents(waiter), recEvents(waiter);
		auto src = blocks.make("ramp", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, srcEvents);
		src->drive(&ramp);
		auto rec = blocks.make("rec", Ports{{{Direction::input, "record", {"left"}}}}, kFormat, recEvents);
		rec->drive(&recorder);
		REQUIRE(wiring.link(waiter, {"ramp", "out.a"}, {"rec", "record.left"}));
		REQUIRE(waiter.until([&] {
			const auto samples = recorder.samples();
			return firstSound(samples) + 4 * kBlock <= samples.size();
		}));
		src->drive(nullptr);
		// The sink may still run once in the cycle the barrier landed in.
		const std::size_t from = recorder.samples().size() + kBlock;
		REQUIRE(waiter.until([&] { return recorder.samples().size() >= from + 8 * kBlock; }));
		const auto samples = recorder.samples().subspan(from);
		CHECK(firstSound(samples) == samples.size());
	}
}

TEST_CASE("drive on a block whose engine node is gone returns, and its code never runs", "[blocks][drive]") {
	auto engine = makeEngine();
	Waiter waiter(*engine);
	Bell bell(waiter);
	FilterPortBlocks blocks(*engine, bell);
	EngineNodes nodes(*engine, waiter);
	Constant alive(waiter, 0.0F), before(waiter, 0.0F), after(waiter, 0.0F);
	BlockEvents aliveEvents(waiter), goneEvents(waiter);
	auto keep = blocks.make("alive", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, aliveEvents);
	keep->drive(&alive);
	auto gone = blocks.make("gone", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, goneEvents);
	gone->drive(&before);
	REQUIRE(waiter.until([&] { return before.calls() > 0 && nodes.props("gone") != nullptr; }));

	REQUIRE(nodes.destroy("gone"));
	REQUIRE(waiter.until([&] { return goneEvents.lostDetail.has_value(); }));
	gone->drive(&after);
	gone->drive(nullptr);
	gone->drive(&after);
	const std::uint64_t aliveCalls = alive.calls();
	REQUIRE(waiter.until([&] { return alive.calls() >= aliveCalls + 8; }));
	CHECK(after.calls() == 0);
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
	src->drive(&ramp);
	auto card = blocks.make(
		"card",
		Ports{{{Direction::input, "in", {"a"}}, {Direction::output, "to_card", {"1"}, PortBank::Reach::inside}}},
		kFormat, cardEvents);
	card->drive(&through);
	auto rec = blocks.make("rec", Ports{{{Direction::input, "record", {"left"}}}}, kFormat, recEvents);
	rec->drive(&recorder);

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
	first->drive(&one);
	auto second = blocks.make("two", Ports{{{Direction::output, "out", {"a"}}}}, kFormat, twoEvents);
	second->drive(&two);

	REQUIRE(
		waiter.until([&] { return nodes.props("one") && nodes.props("two") && one.calls() >= 8 && two.calls() >= 8; }));
	for (const char* name : {"one", "two"}) {
		const auto* props = nodes.props(name);
		const auto group = props->find(PW_KEY_NODE_GROUP);
		REQUIRE(group != props->end());
		CHECK(group->second == "ear.graph");
	}
	// The internal clock is in the group too, so it collects the blocks and whatever is linked to them first
	// (docs/design/node-framework.md section 10), and it tracks the raw clock.
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
	block->drive(&log);
	REQUIRE(waiter.until([&] { return log.count() == CycleLog::kCapacity; }));
	block->drive(nullptr);

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
	block->drive(&log);
	REQUIRE(waiter.until([&] { return log.count() == CycleLog::kCapacity && nodes.props("Dummy-Driver"); }));
	block->drive(nullptr);
	const auto dummy = static_cast<std::uint32_t>(std::stoul(nodes.props("Dummy-Driver")->at(PW_KEY_OBJECT_ID)));

	const std::int64_t cycleNs = std::int64_t{kBlock} * 1000000000 / kRate;
	CHECK(log.at(0).cycle.discontinuity); // the first cycle the block runs
	std::size_t jumps = 0;
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
		if (i > 0) CHECK(seen.cycle.xruns >= log.at(i - 1).cycle.xruns);
	}
	// A loaded host's stall makes the internal clock resync (a jump, counted as an xrun); a quiet one makes none.
	CHECK(jumps <= log.at(CycleLog::kCapacity - 1).cycle.xruns);
	CHECK(jumps <= 2);
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
	// source:out -> mid:in, mid's pair in -> thru, and its unpaired bank side -> sink:in; mid:thru -> sink:in.
	// sink declares 300 frames on its input (a following card would report its buffer there).
	LatencyProbe sourceProbe(waiter, 2), midProbe(waiter, 2), sinkProbe(waiter, 0);
	BlockEvents sourceEvents(waiter), midEvents(waiter), sinkEvents(waiter);
	auto source = blocks.make("source", Ports{{{Direction::output, "out", {"1"}}, {Direction::output, "spare", {"1"}}}},
	                          kFormat, sourceEvents);
	source->drive(&sourceProbe);
	auto mid = blocks.make(
		"mid",
		Ports{
			{{Direction::input, "in", {"1"}}, {Direction::output, "thru", {"1"}}, {Direction::output, "side", {"1"}}}},
		kFormat, midEvents);
	mid->carryLatency(LatencyPlan{{{"in", "thru"}}, {{"in", Latency{10, 20}}}});
	mid->drive(&midProbe);
	auto sink = blocks.make("sink", Ports{{{Direction::input, "in", {"1", "2"}}}}, kFormat, sinkEvents);
	sink->carryLatency(LatencyPlan{{}, {{"in", Latency{300, 300}}}});
	sink->drive(&sinkProbe);
	REQUIRE(wiring.link(waiter, {"source", "out.1"}, {"mid", "in.1"}));
	REQUIRE(wiring.link(waiter, {"mid", "thru.1"}, {"sink", "in.1"}));
	REQUIRE(wiring.link(waiter, {"mid", "side.1"}, {"sink", "in.2"}));

	// mid's thru and side each see the sink's 300; mid's input tells thru's 300 plus its own 10..20 upstream, and side's
	// does not reach it. source's spare output is linked to nothing.
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{310, 320}; }));
	CHECK(midProbe.downstream(0) == Latency{300, 300});
	CHECK(midProbe.downstream(1) == Latency{300, 300});
	CHECK(sourceProbe.downstream(1) == Latency{});

	// A new plan replaces the last at once: mid unpaired carries nothing through, only its own.
	mid->carryLatency(LatencyPlan{{}, {{"in", Latency{10, 20}}}});
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{10, 20}; }));
	mid->carryLatency({});
	CHECK(waiter.until([&] { return sourceProbe.downstream(0) == Latency{}; }));

	source->drive(nullptr);
	mid->drive(nullptr);
	sink->drive(nullptr);
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
	block->drive(&probe);
	REQUIRE(waiter.until([&] { return probe.value() != 0; }));
	block->drive(nullptr);
	CHECK((probe.value() & 0x8000U) != 0); // FTZ
	CHECK((probe.value() & 0x0040U) != 0); // DAZ
	CHECK(_mm_getcsr() == mainBefore);
}
#endif
