// The module-run converter's shared body on the real in-process engine (docs/engine.md#module-run-converters), apart
// from RTP: a test-only module-run shape whose module is PipeWire's libpipewire-module-pipe-tunnel in source mode. Like
// rtp-source it owns its stream, its buffer and the loop that steers its resampler, it reads from outside the graph (a
// FIFO, written here, where rtp-source reads a socket) and it makes its node on its own connection, so ear can only
// find it by name; unlike rtp-source it needs no network and no sender thread keeping time, its node's ports have
// another prefix ("capture_", not "receive_") and a bad pipe.filename makes it refuse to load. What each test proves
// goes through the shared code with the module's own words: ready and channel order, the stale-name rule, a module
// that will not load, a node that never runs, a node removed, a module destroyed, and a fault named by the module's
// body. Every wait is event-gated: the main loop runs until the events a test waits for have arrived (a converter
// event, a registry change, the block's cycles); the deadline only fails the test.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "../callback_boundary.hpp"
#include "../converter_modules.hpp"
#include "../module_converter_body.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "temp_folder.hpp"

namespace ear::pipewire::testing {

/// The stand-in's shape: the FIFO its module reads, the rate it reads at, and whether its node runs before it is
/// linked (node.always-process, as RTP's stream sets it; without it the node never runs, as an RTP receiver whose port
/// is held never does).
struct PipeTunnelShape {
	std::filesystem::path fifo;
	std::uint32_t rate = kRate;
	bool runs = true;
};

} // namespace ear::pipewire::testing

namespace ear::pipewire {

/// The stand-in's module: pipe-tunnel in source mode, 32-bit float interleaved from the FIFO. Its stream is an
/// Audio/Source that is not node.network, so its output ports are "capture_<position>" (impl-port.c).
template <> struct ModuleRun<testing::PipeTunnelShape> {
	static std::string port(std::size_t channel) { return "capture_AUX" + std::to_string(channel); }
	static constexpr ConverterModule module{.kind = "pipe tunnel",
	                                        .word = "pipe-tunnel",
	                                        .body = "PipeTunnelBody",
	                                        .flow = ChannelFlow::nodeToBlock,
	                                        .nodePort = &port};
	using Arguments = std::string;
	static Arguments arguments(const NodeName& engineNode, const testing::PipeTunnelShape& shape,
	                           std::size_t channels) {
		std::string stream = "{ adapter.auto-port-config = { mode = dsp monitor = false control = false position = "
							 "preserve }";
		if (shape.runs) stream += " node.always-process = true";
		stream += " }";
		return "{ remote.name = \"internal\" tunnel.mode = source pipe.filename = \"" + shape.fifo.string() +
		       "\" audio.format = F32LE audio.rate = " + std::to_string(shape.rate) +
		       " audio.channels = " + std::to_string(channels) + " audio.position = " + auxPositions(channels) +
		       " node.name = \"" + engineNode + "\" stream.props = " + stream + " }";
	}
	/// A test drives PipeWire itself (the property guard leaves tests alone): its stand-in loads its own module.
	static pw_impl_module* load(pw_context* context, const Arguments& arguments) {
		return pw_context_load_module(context, "libpipewire-module-pipe-tunnel", arguments.c_str(), nullptr);
	}
};

} // namespace ear::pipewire

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace std::chrono_literals;

using PipeTunnels = ModuleConverters<PipeTunnelShape>;

constexpr float kLeft = 0.25F;
constexpr float kRight = -0.5F;

/// Counts the block's runs, and the frames of the longest run in which its inside input 1 held kLeft and input 2
/// kRight (within the resampler's ripple on a constant), and in which they held each other's.
class Listener final : public AudioCode {
public:
	explicit Listener(Waiter& waiter) : waiter_(waiter) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& /*cycle*/) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		for (std::uint32_t i = 0; i < frames; ++i) {
			const auto near = [](float sample, float wanted) { return std::abs(sample - wanted) < 0.01F; };
			run_ = near(inputs[0][i], kLeft) && near(inputs[1][i], kRight) ? run_ + 1 : 0;
			swapped_ = near(inputs[0][i], kRight) && near(inputs[1][i], kLeft) ? swapped_ + 1 : 0;
			longest_.store(std::max(longest_.load(std::memory_order_relaxed), run_), std::memory_order_relaxed);
			longestSwapped_.store(std::max(longestSwapped_.load(std::memory_order_relaxed), swapped_),
			                      std::memory_order_relaxed);
		}
		runs_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}

	std::uint64_t runs() const { return runs_.load(std::memory_order_acquire); }
	std::uint64_t longest() const { return longest_.load(std::memory_order_relaxed); }
	std::uint64_t longestSwapped() const { return longestSwapped_.load(std::memory_order_relaxed); }

private:
	Waiter& waiter_;
	std::uint64_t run_ = 0; // the data thread's only
	std::uint64_t swapped_ = 0;
	std::atomic<std::uint64_t> longest_{0};
	std::atomic<std::uint64_t> longestSwapped_{0};
	std::atomic<std::uint64_t> runs_{0};
};

/// What a converter tells, with what the registry showed at the moment it was told.
class ConverterEvents final : public EngineCompanionEvents {
public:
	ConverterEvents(Waiter& waiter, Registry& registry) : waiter_(waiter), registry_(registry) {}
	void ready() override {
		++readyCount;
		waiter_.wake();
	}
	void lost(std::string detail) override {
		lostDetail = std::move(detail);
		lostInsideMake = insideMake;
		nodePresentAtLost = registry_.present("fpp.pipe");
		linksAtLost = nodeId ? registry_.linksOf(*nodeId).size() : 0;
		waiter_.wake();
	}
	bool settled() const { return readyCount > 0 || lostDetail.has_value(); }

	int readyCount = 0;
	bool insideMake = false;
	std::optional<std::uint32_t> nodeId;
	std::optional<std::string> lostDetail;
	bool lostInsideMake = false;
	bool nodePresentAtLost = false;
	std::size_t linksAtLost = 0;

private:
	Waiter& waiter_;
	Registry& registry_;
};

/// One port block "fpp" (outputs stream.1, stream.2; the inside input bank from_pipe) driven by a Listener on the
/// dummy driver, the converters' factory, and a folder for the FIFOs.
struct Graph {
	explicit Graph(Faults* faults = nullptr)
		: engine(makeEngine(faults)), waiter(*engine), registry(*engine, waiter), blocks(*engine, bell),
		  converters(*engine), listener(waiter), blockEvents(waiter), folder("module-run") {
		block = blocks.make("fpp",
		                    Ports{{{Direction::output, "stream", {"1", "2"}},
		                           {Direction::input, "from_pipe", {"1", "2"}, PortBank::Reach::inside}}},
		                    kFormat, blockEvents);
		REQUIRE(ear::testing::driveNow(*block, &listener));
		REQUIRE(waiter.until([&] { return blockEvents.isReady && registry.id("Dummy-Driver"); }));
		dummy = *registry.id("Dummy-Driver");
	}
	~Graph() { ear::testing::silenceOrAbort(*block); }

	PipeTunnelShape shape(std::string_view fifo = "in.fifo") const {
		return PipeTunnelShape{.fifo = folder.path() / fifo};
	}

	std::unique_ptr<EngineCompanion> make(const PipeTunnelShape& shape, ConverterEvents& events) {
		events.insideMake = true;
		auto converter = converters.make(
			CompanionTarget{
				"fpp.pipe", "fpp", Direction::input, {"from_pipe.1", "from_pipe.2"}, kFormat, block->blockId()},
			shape, events);
		events.insideMake = false;
		return converter;
	}

	/// A ready converter whose two links are up; its node id is in `events`.
	std::unique_ptr<EngineCompanion> ready(ConverterEvents& events) {
		auto converter = make(shape(), events);
		REQUIRE(waiter.until([&] { return events.settled(); }));
		REQUIRE(events.lostDetail.value_or("") == "");
		const std::uint32_t node = *registry.id("fpp.pipe");
		events.nodeId = node;
		REQUIRE(waiter.until([&] { return registry.linksOf(node).size() == 2; }));
		return converter;
	}

	/// The block's code ran `cycles` more times.
	bool runs(std::uint64_t cycles, std::chrono::seconds deadline = 5s) {
		const std::uint64_t until = listener.runs() + cycles;
		return waiter.until([&] { return listener.runs() >= until; }, deadline);
	}

	std::unique_ptr<Engine> engine;
	Waiter waiter;
	Registry registry;
	NoBell bell;
	FilterPortBlocks blocks;
	PipeTunnels converters;
	Listener listener;
	BlockEvents blockEvents;
	ear::test::TempFolder folder;
	std::unique_ptr<PortBlock> block;
	std::uint32_t dummy = 0;
};

/// The engine's one module of this name, from its own objects (it runs in this process).
pw_impl_module* moduleNamed(Engine& engine, std::string_view name) {
	struct Find {
		std::string_view name;
		pw_impl_module* found = nullptr;
	} find{name};
	pw_context_for_each_global(
		engine.context(),
		[](void* data, pw_global* global) {
			if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Module)) return 0;
			auto* module = static_cast<pw_impl_module*>(pw_global_get_object(global));
			const pw_module_info* info = pw_impl_module_get_info(module);
			auto* wanted = static_cast<Find*>(data);
			if (info->name == nullptr || std::string_view(info->name) != wanted->name) return 0;
			wanted->found = module;
			return 1;
		},
		&find);
	return find.found;
}

/// Writes `seconds` of kLeft and kRight, interleaved 32-bit float, into a FIFO the module made, from a thread of its
/// own: the module's data loop drains the FIFO as it reads, so a blocking write paces itself on it.
class FifoWriter {
public:
	FifoWriter(const std::filesystem::path& fifo, std::size_t seconds) {
		fd_ = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
		REQUIRE(fd_ >= 0);
		thread_ = std::thread([this, seconds] {
			std::vector<float> second(2 * kRate);
			for (std::size_t i = 0; i < kRate; ++i) {
				second[2 * i] = kLeft;
				second[2 * i + 1] = kRight;
			}
			const auto* bytes = reinterpret_cast<const char*>(second.data());
			const std::size_t size = second.size() * sizeof(float);
			for (std::size_t n = 0; n < seconds && !stop_.load(); ++n) {
				for (std::size_t at = 0; at < size && !stop_.load();) {
					const ssize_t wrote = ::write(fd_, bytes + at, size - at);
					if (wrote < 0 && errno == EINTR) continue;
					if (wrote <= 0) return;
					at += static_cast<std::size_t>(wrote);
				}
			}
		});
	}
	~FifoWriter() {
		stop_.store(true);
		::close(fd_); // a write held on a full FIFO whose reader went fails, and the thread ends
		thread_.join();
	}
	FifoWriter(const FifoWriter&) = delete;
	FifoWriter& operator=(const FifoWriter&) = delete;

private:
	int fd_ = -1;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

} // namespace

TEST_CASE("A module-run converter is ready once its module's node runs, every channel linked by the node's and the "
          "block's ids, and channel n of its module reaches the bank's port n",
          "[module_run]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);
	CHECK(events.readyCount == 1);
	CHECK(graph.registry.activeLinksOf(*events.nodeId) == 2);
	CHECK(graph.registry.linksOf(*graph.registry.id("fpp")).size() == 2);

	FifoWriter writer(graph.shape().fifo, 30);
	// A tenth of a second of both channels in their places, through the module's buffer and its resampler.
	REQUIRE(graph.waiter.until([&] { return graph.listener.longest() >= kRate / 10; }, 20s));
	CHECK(graph.listener.longestSwapped() == 0);
	CHECK(events.readyCount == 1);
	CHECK_FALSE(events.lostDetail.has_value());
}

TEST_CASE("A module-run converter made while another node of its name is still going binds and links its own node, "
          "never the stale one",
          "[module_run][restart]") {
	Graph graph;
	// A stale fpp.pipe that stays: a pipe-tunnel module the test loads itself, on another FIFO.
	ConverterEvents unused(graph.waiter, graph.registry);
	const std::unique_ptr<pw_impl_module, void (*)(pw_impl_module*)> staleModule(
		ModuleRun<PipeTunnelShape>::load(
			graph.engine->context(), ModuleRun<PipeTunnelShape>::arguments("fpp.pipe", graph.shape("stale.fifo"), 2)),
		&pw_impl_module_destroy);
	REQUIRE(staleModule != nullptr);
	REQUIRE(graph.waiter.until([&] { return graph.registry.id("fpp.pipe").has_value(); }));
	const std::uint32_t stale = *graph.registry.id("fpp.pipe");

	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.make(graph.shape(), events);
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	CHECK(events.readyCount == 1);
	REQUIRE(graph.waiter.until([&] { return graph.registry.ids("fpp.pipe").size() == 2; }));
	const std::uint32_t own = graph.registry.ids("fpp.pipe").back();
	REQUIRE(own != stale);
	REQUIRE(graph.waiter.until([&] { return graph.registry.linksOf(own).size() == 2; }));
	CHECK(graph.registry.linksOf(stale).empty());
	CHECK(graph.registry.linksOf(*graph.registry.id("fpp")).size() == 2);
}

TEST_CASE("A module-run converter whose module will not load is lost after make returns, in its module's words, and "
          "the block keeps running",
          "[module_run][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	// pipe-tunnel makes its FIFO when it loads: in a folder that is not there, mkfifo fails and so does the load.
	const auto converter = graph.make(graph.shape("no-such-folder/in.fifo"), events);
	CHECK_FALSE(events.lostDetail.has_value()); // never inside make()
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.lostInsideMake);
	CHECK(*events.lostDetail == "cannot load PipeWire's pipe-tunnel module: No such file or directory");
	CHECK(events.readyCount == 0);
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(moduleNamed(*graph.engine, "libpipewire-module-pipe-tunnel") == nullptr);
	CHECK(graph.runs(16));
}

TEST_CASE("A module-run converter whose node never runs tells nothing while its block runs on, and goes when it is "
          "destroyed, so its kind's readiness deadline in NodeHost is what fails it",
          "[module_run][deadline]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	PipeTunnelShape shape = graph.shape();
	shape.runs = false;
	auto converter = graph.make(shape, events);
	REQUIRE(graph.waiter.until([&] { return graph.registry.state("fpp.pipe").has_value(); }));
	const std::uint32_t node = *graph.registry.id("fpp.pipe");
	// As long as an RTP receiver's deadline, counted in the block's cycles: what would have told it is not coming.
	REQUIRE(graph.runs(FrameClock(ear::SampleRate::fixed<kRate>()).frames(rtpReceiverReadinessBound) / kBlock, 30s));
	CHECK_FALSE(events.settled());
	CHECK(graph.registry.state("fpp.pipe") != PW_NODE_STATE_RUNNING);
	CHECK(graph.registry.linksOf(node).empty());

	converter.reset();
	REQUIRE(graph.waiter.until([&] { return !graph.registry.present("fpp.pipe"); }));
	CHECK(moduleNamed(*graph.engine, "libpipewire-module-pipe-tunnel") == nullptr);
	CHECK_FALSE(events.settled()); // destroying it tells nothing
	CHECK(graph.runs(16));
}

TEST_CASE("A module-run converter whose node goes is lost in its module's words, its node and links already gone, "
          "and the block keeps running on the dummy",
          "[module_run][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);

	REQUIRE(graph.registry.destroy(*events.nodeId));
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.lostInsideMake);
	CHECK(*events.lostDetail == "the engine removed the pipe tunnel");
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(events.linksAtLost == 0);
	REQUIRE(graph.registry.reread({"fpp"}));
	CHECK(graph.registry.driverId("fpp") == graph.dummy);
	CHECK(graph.runs(16));
}

TEST_CASE("A module-run converter whose module is destroyed is lost in its module's words, its node and links "
          "already gone",
          "[module_run][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);

	// No client may destroy a module through the registry; a module destroys itself (pipe-tunnel does when its stream
	// errors or its connection fails). The test does that last step in its place.
	pw_impl_module* module = moduleNamed(*graph.engine, "libpipewire-module-pipe-tunnel");
	REQUIRE(module != nullptr);
	pw_impl_module_destroy(module);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(*events.lostDetail == "the engine removed the pipe tunnel's module");
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(events.linksAtLost == 0);
	CHECK(graph.runs(16));
}

// docs/architecture.md#faults-by-owner: the boundary names a module-run converter's callbacks by its module's body, so
// a report says which module's converter threw (RTP's are RtpReceiverBody's, as before the body was shared).
TEST_CASE("A module-run converter's body's callback that throws is reported by its module's body name, and the "
          "converter is torn down and told faulted",
          "[module_run][faults]") {
	FaultLog faults;
	Graph graph(&faults);
	faults.onCaught = [&] { graph.waiter.wake(); };
	ConverterEvents events(graph.waiter, graph.registry);
	boundary::throwInCallback("PipeTunnelBody::onCoreDone");
	const auto converter = graph.make(graph.shape(), events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(*events.lostDetail == "internal error in PipeTunnelBody::onCoreDone: injected");
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "PipeTunnelBody::onCoreDone");
	CHECK(faults.reports[0].node == "fpp");
	CHECK(faults.reports[0].owned);
	CHECK(events.readyCount == 0);
	CHECK(graph.runs(16));
}
