// Pipe receivers on the real in-process engine (docs/engine.md#pipe-receivers): PipeWire's pipe-tunnel module loaded
// into ear's own engine through PipeReceivers, the shipped module-run converter with no network, so the shared body
// (docs/engine.md#module-run-converters) is proved here in a module's own words: ready and channel order, the
// stale-name rule, a module that will not load, a node removed, a module destroyed and a fault named by the module's
// body. One test-only stand-in shape remains, for a node that never runs, which the shipped arguments cannot express
// (they always set node.always-process). Then what is the pipe module's own: its FIFO, a tone written at another rate,
// its latency, that it never drives, and a writer that stops. The FIFO is written here, from a thread of its own paced
// as a real writer is. Every wait is event-gated: the main loop runs until the events a test waits for have arrived (a
// converter event, a registry change, the block's cycles); the deadline only fails the test.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "../callback_boundary.hpp"
#include "../converter_modules.hpp"
#include "../module_converter_body.hpp"
#include "catch2/catch.hpp"
#include "ear/pipewire/pipe_receivers.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "port_latency.hpp"
#include "rtp_tone.hpp"
#include "sleeps.hpp"
#include "temp_folder.hpp"

namespace ear::pipewire::testing {

/// The stand-in's shape, for the one case the shipped arguments cannot express: a pipe-tunnel module whose node never
/// runs before it is linked (no node.always-process, as an RTP receiver whose port is held never runs).
struct NeverRunsShape {};

} // namespace ear::pipewire::testing

namespace ear::pipewire {

/// The stand-in's module: pipe-tunnel in source mode on the target's FIFO, its stream without node.always-process.
/// Its words are the pipe receiver's, so what it proves reads as the shipped one's.
template <> struct ModuleRun<testing::NeverRunsShape> {
	static std::string port(std::size_t channel) { return "capture_AUX" + std::to_string(channel); }
	static constexpr ConverterModule module{.kind = "pipe receiver",
	                                        .word = "pipe-tunnel",
	                                        .body = "NeverRunsBody",
	                                        .flow = ChannelFlow::nodeToBlock,
	                                        .nodePort = &port};
	using Arguments = std::string;
	static Arguments arguments(const CompanionTarget& target, const testing::NeverRunsShape& /*shape*/) {
		const std::size_t channels = target.blockPorts.size();
		return "{ remote.name = \"internal\" tunnel.mode = source pipe.filename = \"" + target.fifo +
		       "\" audio.format = S16LE audio.rate = 48000 audio.channels = " + std::to_string(channels) +
		       " audio.position = " + auxPositions(channels) + " node.name = \"" + target.engineNode +
		       "\" stream.props = { adapter.auto-port-config = { mode = dsp monitor = false control = false position = "
		       "preserve } } }";
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

using NeverRuns = ModuleConverters<NeverRunsShape>;

constexpr float kLeft = 0.25F;
constexpr float kRight = -0.5F;
/// The writer's rate in the tests that resample: 8192 frames of it are 8916 graph frames, to the nearest.
constexpr std::uint32_t kWriterRate = 44100;
constexpr double kAmplitude = 0.5;

/// Records the block's inside inputs 1 and 2 (from_pipe.1, from_pipe.2) up to kCapacity frames and where each cycle that
/// followed lost ones began (its facts not whole: the recording jumps there, docs/writing-a-node-type.md#lost-cycles),
/// counts its runs and the cycles some other node than `expectedDriver` drove, and keeps the frames of the longest run in which input 1
/// held kLeft and input 2 kRight (within the resampler's ripple on a constant), and in which they held each other's.
class Listener final : public AudioCode {
public:
	static constexpr std::size_t kCapacity = 40 * kRate;

	explicit Listener(Waiter& waiter) : waiter_(waiter), channels_(2, std::vector<float>(kCapacity, 0.0F)) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		for (const std::span<float> out : outputs) std::ranges::fill(out, 0.0F);
		const std::uint32_t expected = expectedDriver.load(std::memory_order_relaxed);
		if (expected != 0 && cycle.driverId != expected) otherDriver_.fetch_add(1, std::memory_order_relaxed);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		const std::size_t take = std::min<std::size_t>(frames, kCapacity - at);
		if (const std::size_t n = jumps_.load(std::memory_order_relaxed); !cycle.facts.whole() && n < jumpAt_.size()) {
			jumpAt_[n] = at;
			jumps_.store(n + 1, std::memory_order_relaxed);
		}
		for (std::size_t channel = 0; channel < 2; ++channel)
			std::copy_n(inputs[channel].begin(), take, channels_[channel].begin() + static_cast<std::ptrdiff_t>(at));
		for (std::uint32_t i = 0; i < frames; ++i) {
			const auto near = [](float sample, float wanted) { return std::abs(sample - wanted) < 0.01F; };
			run_ = near(inputs[0][i], kLeft) && near(inputs[1][i], kRight) ? run_ + 1 : 0;
			swapped_ = near(inputs[0][i], kRight) && near(inputs[1][i], kLeft) ? swapped_ + 1 : 0;
			longest_.store(std::max(longest_.load(std::memory_order_relaxed), run_), std::memory_order_relaxed);
			longestSwapped_.store(std::max(longestSwapped_.load(std::memory_order_relaxed), swapped_),
			                      std::memory_order_relaxed);
		}
		count_.store(at + take, std::memory_order_release);
		runs_.fetch_add(1, std::memory_order_release);
		waiter_.wake();
	}
	void release() noexcept override {}

	std::uint64_t runs() const { return runs_.load(std::memory_order_acquire); }
	std::uint64_t longest() const { return longest_.load(std::memory_order_relaxed); }
	std::uint64_t longestSwapped() const { return longestSwapped_.load(std::memory_order_relaxed); }
	std::uint64_t otherDriver() const { return otherDriver_.load(std::memory_order_relaxed); }
	/// Frames recorded so far.
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	/// Channel `channel`'s frames [from, from + frames); only frames already counted.
	std::span<const float> frames(std::size_t channel, std::size_t from, std::size_t frames) const {
		return std::span<const float>(channels_[channel]).subspan(from, frames);
	}
	/// The first recorded frame at or after `from` where either channel is not silent; nothing if none yet.
	std::optional<std::size_t> soundFrom(std::size_t from) const {
		const std::size_t until = count();
		for (std::size_t i = from; i < until; ++i)
			if (channels_[0][i] != 0.0F || channels_[1][i] != 0.0F) return i;
		return std::nullopt;
	}
	/// Whether the recording jumps inside frames (from, from + frames): a cycle after lost ones began there.
	bool jumpsWithin(std::size_t from, std::size_t frames) const {
		count(); // acquire: the jumps were stored before count_
		const std::size_t n = jumps_.load(std::memory_order_relaxed);
		for (std::size_t i = 0; i < n; ++i)
			if (jumpAt_[i] > from && jumpAt_[i] < from + frames) return true;
		return false;
	}
	/// Whether both channels were exactly silent for `frames` frames running, somewhere at or after `from`.
	bool silentFor(std::size_t from, std::size_t frames) const {
		const std::size_t until = count();
		std::size_t run = 0;
		for (std::size_t i = from; i < until; ++i) {
			run = channels_[0][i] == 0.0F && channels_[1][i] == 0.0F ? run + 1 : 0;
			if (run >= frames) return true;
		}
		return false;
	}

	std::atomic<std::uint32_t> expectedDriver{0}; // 0: not judged

private:
	Waiter& waiter_;
	std::vector<std::vector<float>> channels_;
	std::atomic<std::size_t> count_{0};
	std::uint64_t run_ = 0; // the data thread's only
	std::uint64_t swapped_ = 0;
	std::atomic<std::uint64_t> longest_{0};
	std::atomic<std::uint64_t> longestSwapped_{0};
	std::atomic<std::uint64_t> runs_{0};
	std::atomic<std::uint64_t> otherDriver_{0};
	std::array<std::size_t, 256> jumpAt_{};
	std::atomic<std::size_t> jumps_{0};
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
/// dummy driver, the pipe receivers and the stand-in's adapter, and a folder for the FIFOs.
struct Graph {
	explicit Graph(Faults* faults = nullptr)
		: engine(makeEngine(faults)), waiter(*engine), registry(*engine, waiter), blocks(*engine, bell),
		  receivers(*engine), neverRuns(*engine), listener(waiter), blockEvents(waiter), folder("pipe-receiver") {
		block = blocks.make("fpp",
		                    Ports{{{Direction::output, "stream", {"1", "2"}},
		                           {Direction::input, "from_pipe", {"1", "2"}, PortBank::Reach::inside}}},
		                    kFormat, blockEvents);
		REQUIRE(ear::testing::driveNow(*block, &listener));
		REQUIRE(waiter.until([&] { return blockEvents.isReady && registry.id("Dummy-Driver"); }));
		dummy = *registry.id("Dummy-Driver");
	}
	~Graph() { ear::testing::silenceOrAbort(*block); }

	CompanionTarget target(std::string_view fifo = "fpp.1.from_pipe.fifo") const {
		return CompanionTarget{.engineNode = "fpp.pipe",
		                       .blockNode = "fpp",
		                       .bank = Direction::input,
		                       .blockPorts = {"from_pipe.1", "from_pipe.2"},
		                       .format = kFormat,
		                       .blockId = block->blockId(),
		                       .insideBank = "from_pipe",
		                       .fifo = (folder.path() / fifo).string()};
	}

	std::unique_ptr<EngineCompanion> make(const CompanionTarget& at, const PipeReceiverShape& shape,
	                                      ConverterEvents& events) {
		events.insideMake = true;
		auto converter = receivers.make(at, shape, events);
		events.insideMake = false;
		return converter;
	}

	/// A ready receiver of `shape` whose two links are up; its node id is in `events`.
	std::unique_ptr<EngineCompanion>
	ready(ConverterEvents& events, PipeReceiverShape shape = {PipeFormat::s16le, ear::SampleRate::fixed<kRate>()}) {
		auto converter = make(target(), shape, events);
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
	PipeReceivers receivers;
	NeverRuns neverRuns;
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

/// Sample `frame` of channel `channel` (0 or 1), as a float in [-1, 1].
using Samples = std::function<double(std::size_t frame, std::size_t channel)>;

Samples constants() {
	return [](std::size_t, std::size_t channel) { return channel == 0 ? double{kLeft} : double{kRight}; };
}

/// 1 kHz on channel 1 and 1.5 kHz on channel 2 at kAmplitude, at `rate`.
Samples tones(std::uint32_t rate) {
	return [rate](std::size_t frame, std::size_t channel) {
		const double hz = channel == 0 ? 1000.0 : 1500.0;
		return kAmplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(frame) / rate);
	};
}

/// Writes `seconds` of interleaved stereo S16LE at `rate` into a FIFO the module made, from a thread of its own, in
/// hundredths of a second, each when it is due: the stand-in for a program writing in real time (its pace is
/// ear::sleeps::pace's), in pieces as small as a real writer's, since the module steers its rate by its ring's fill and
/// a writer's lumps swing that fill. It closes the FIFO when it goes.
class FifoWriter {
public:
	FifoWriter(const std::string& fifo, std::uint32_t rate, Samples samples, std::size_t seconds = 30) {
		fd_ = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
		REQUIRE(fd_ >= 0);
		thread_ = std::thread([this, rate, samples = std::move(samples), seconds] {
			const std::size_t piece = rate / 100;
			std::vector<std::int16_t> chunk(2 * piece);
			const auto start = std::chrono::steady_clock::now();
			for (std::size_t n = 0; n < seconds * 100 && !stop_.load(); ++n) {
				for (std::size_t i = 0; i < piece; ++i)
					for (std::size_t channel = 0; channel < 2; ++channel)
						chunk[2 * i + channel] =
							static_cast<std::int16_t>(std::lround(samples(n * piece + i, channel) * 32767.0));
				const auto* bytes = reinterpret_cast<const char*>(chunk.data());
				const std::size_t size = chunk.size() * sizeof(std::int16_t);
				for (std::size_t at = 0; at < size && !stop_.load();) {
					const ssize_t wrote = ::write(fd_, bytes + at, size - at);
					if (wrote < 0 && errno == EINTR) continue;
					if (wrote <= 0) return;
					at += static_cast<std::size_t>(wrote);
				}
				ear::sleeps::pace(start + (n + 1) * 10ms, "a writer in real time writes each piece when it is due");
			}
		});
	}
	~FifoWriter() {
		stop_.store(true);
		thread_.join();
		::close(fd_);
	}
	FifoWriter(const FifoWriter&) = delete;
	FifoWriter& operator=(const FifoWriter&) = delete;

private:
	int fd_ = -1;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

} // namespace

TEST_CASE("A pipe receiver is ready once its module's node runs, every channel linked by the node's and the block's "
          "ids, and channel n of its FIFO reaches the bank's port n",
          "[module_run][pipe]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);
	CHECK(events.readyCount == 1);
	CHECK(graph.registry.activeLinksOf(*events.nodeId) == 2);
	CHECK(graph.registry.linksOf(*graph.registry.id("fpp")).size() == 2);

	FifoWriter writer(graph.target().fifo, kRate, constants());
	// A tenth of a second of both channels in their places, through the module's buffer and its resampler.
	REQUIRE(graph.waiter.until([&] { return graph.listener.longest() >= kRate / 10; }, 20s));
	CHECK(graph.listener.longestSwapped() == 0);
	CHECK(events.readyCount == 1);
	CHECK_FALSE(events.lostDetail.has_value());
}

TEST_CASE("A pipe receiver made while another node of its name is still going binds and links its own node, never "
          "the stale one",
          "[module_run][pipe][restart]") {
	Graph graph;
	// A stale fpp.pipe that stays: a pipe-tunnel module the test loads itself, on another FIFO.
	const std::unique_ptr<pw_impl_module, void (*)(pw_impl_module*)> staleModule(
		ModuleRun<NeverRunsShape>::load(graph.engine->context(),
	                                    ModuleRun<NeverRunsShape>::arguments(graph.target("stale.fifo"), {})),
		&pw_impl_module_destroy);
	REQUIRE(staleModule != nullptr);
	REQUIRE(graph.waiter.until([&] { return graph.registry.id("fpp.pipe").has_value(); }));
	const std::uint32_t stale = *graph.registry.id("fpp.pipe");

	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter =
		graph.make(graph.target(), PipeReceiverShape{PipeFormat::s16le, ear::SampleRate::fixed<kRate>()}, events);
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

TEST_CASE("A pipe receiver whose module will not load is lost after make returns, in its module's words, and the "
          "block keeps running",
          "[module_run][pipe][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	// pipe-tunnel makes its FIFO when it loads: in a folder that is not there, mkfifo fails and so does the load.
	const auto converter = graph.make(graph.target("no-such-folder/in.fifo"),
	                                  PipeReceiverShape{PipeFormat::s16le, ear::SampleRate::fixed<kRate>()}, events);
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
	auto converter = graph.neverRuns.make(graph.target(), NeverRunsShape{}, events);
	REQUIRE(graph.waiter.until([&] { return graph.registry.state("fpp.pipe").has_value(); }));
	const std::uint32_t node = *graph.registry.id("fpp.pipe");
	// As long as a pipe receiver's deadline, counted in the block's cycles: what would have told it is not coming.
	REQUIRE(graph.runs(FrameClock(ear::SampleRate::fixed<kRate>()).frames(pipeReceiverReadinessBound) / kBlock, 30s));
	CHECK_FALSE(events.settled());
	CHECK(graph.registry.state("fpp.pipe") != PW_NODE_STATE_RUNNING);
	CHECK(graph.registry.linksOf(node).empty());

	converter.reset();
	REQUIRE(graph.waiter.until([&] { return !graph.registry.present("fpp.pipe"); }));
	CHECK(moduleNamed(*graph.engine, "libpipewire-module-pipe-tunnel") == nullptr);
	CHECK_FALSE(events.settled()); // destroying it tells nothing
	CHECK(graph.runs(16));
}

TEST_CASE("A pipe receiver whose node goes is lost in its module's words, its node and links already gone, and the "
          "block keeps running on the dummy",
          "[module_run][pipe][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);

	REQUIRE(graph.registry.destroy(*events.nodeId));
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK_FALSE(events.lostInsideMake);
	CHECK(*events.lostDetail == "the engine removed the pipe receiver");
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(events.linksAtLost == 0);
	REQUIRE(graph.registry.reread({"fpp"}));
	CHECK(graph.registry.driverId("fpp") == graph.dummy);
	CHECK(graph.runs(16));
}

TEST_CASE("A pipe receiver whose module is destroyed is lost in its module's words, its node and links already gone",
          "[module_run][pipe][lost]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);

	// No client may destroy a module through the registry; a module destroys itself (pipe-tunnel does when its stream
	// errors or its connection fails). The test does that last step in its place.
	pw_impl_module* module = moduleNamed(*graph.engine, "libpipewire-module-pipe-tunnel");
	REQUIRE(module != nullptr);
	pw_impl_module_destroy(module);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(*events.lostDetail == "the engine removed the pipe receiver's module");
	CHECK_FALSE(events.nodePresentAtLost);
	CHECK(events.linksAtLost == 0);
	CHECK(graph.runs(16));
}

// docs/architecture.md#faults-by-owner: the boundary names a module-run converter's callbacks by its module's body, so
// a report says which module's converter threw (RTP's are RtpReceiverBody's, the pipe receiver's PipeReceiverBody's).
TEST_CASE("A pipe receiver's body's callback that throws is reported by its module's body name, and the receiver is "
          "torn down and told faulted",
          "[module_run][pipe][faults]") {
	FaultLog faults;
	Graph graph(&faults);
	faults.onCaught = [&] { graph.waiter.wake(); };
	ConverterEvents events(graph.waiter, graph.registry);
	boundary::throwInCallback("PipeReceiverBody::onCoreDone");
	const auto converter =
		graph.make(graph.target(), PipeReceiverShape{PipeFormat::s16le, ear::SampleRate::fixed<kRate>()}, events);
	REQUIRE(graph.waiter.until([&] { return events.lostDetail.has_value(); }));
	CHECK(*events.lostDetail == "internal error in PipeReceiverBody::onCoreDone: injected");
	REQUIRE(faults.reports.size() == 1);
	CHECK(faults.reports[0].callback == "PipeReceiverBody::onCoreDone");
	CHECK(faults.reports[0].node == "fpp");
	CHECK(faults.reports[0].owned);
	CHECK(events.readyCount == 0);
	CHECK(graph.runs(16));
}

TEST_CASE("A pipe receiver's FIFO is at its target's path once it is ready, open to every writer, and gone once it is "
          "destroyed",
          "[module_run][pipe][fifo]") {
	Graph graph;
	const std::string fifo = graph.target().fifo;
	REQUIRE_FALSE(std::filesystem::exists(fifo)); // the path is the module's to make
	ConverterEvents events(graph.waiter, graph.registry);
	auto converter = graph.ready(events);
	struct stat made{};
	REQUIRE(::lstat(fifo.c_str(), &made) == 0);
	CHECK(S_ISFIFO(made.st_mode));
	CHECK((made.st_mode & 07777) == 0666);

	converter.reset();
	REQUIRE(graph.waiter.until([&] { return !graph.registry.present("fpp.pipe"); }));
	CHECK_FALSE(std::filesystem::exists(fifo));
	CHECK_FALSE(events.lostDetail.has_value()); // destroying it tells nothing
}

// docs/engine.md#pipe-receivers: the module steers its resampler by its ring's fill, from a loop that settles over
// seconds and starts again at every resync, so its pitch wanders by tenths of a percent, and a writer's host that falls
// behind leaves an underrun's silence. So each tenth of a second is judged on its own, against the best fit within 1% of
// each tone: 1% holds the module's wander, while a stream not resampled (44.1 kHz played as 48 kHz, 8.8% sharp) or read
// in the wrong format fits nowhere near. A tenth holding an underrun's silence is the writer's host's, and one the
// recording jumps in (a cycle the graph lost on a loaded host, its facts not whole) the graph's: neither is judged.
TEST_CASE("S16LE stereo written at 44.1 kHz reaches the 48 kHz block resampled: 1 kHz on from_pipe.1, 1.5 kHz on "
          "from_pipe.2",
          "[module_run][pipe][tone]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events, PipeReceiverShape{PipeFormat::s16le, *SampleRate::of(kWriterRate)});
	FifoWriter writer(graph.target().fifo, kWriterRate, tones(kWriterRate));

	REQUIRE(graph.waiter.until([&] { return graph.listener.soundFrom(0).has_value(); }, 10s));
	// Half a second past the first sound, then twenty tenths judged.
	constexpr std::size_t tenth = kRate / 10;
	constexpr std::size_t planned = 20;
	const auto near = [](std::span<const float> window, double hz) {
		ear::test::SineFit best{0.0, INFINITY};
		for (int step = -40; step <= 40; ++step) {
			const auto fit = ear::test::fitSine(window, hz * (1.0 + step * 0.00025), kRate);
			if (fit.amplitude > best.amplitude) best = fit;
		}
		return best;
	};
	std::string profile;
	std::size_t judged = 0;
	std::size_t fitted = 0;
	std::size_t excused = 0;
	for (std::size_t at = *graph.listener.soundFrom(0) + kRate / 2; judged < planned; at += tenth) {
		REQUIRE(at + tenth <= decltype(graph.listener)::kCapacity);
		REQUIRE(graph.waiter.until([&] { return graph.listener.count() >= at + tenth; }, 30s));
		// An underrun plays exact zeros on both channels: a millisecond of them running is one.
		const auto left = graph.listener.frames(0, at, tenth);
		const auto right = graph.listener.frames(1, at, tenth);
		bool underrun = false;
		for (std::size_t i = 0, run = 0; i < tenth && !underrun; ++i) {
			run = left[i] == 0.0F && right[i] == 0.0F ? run + 1 : 0;
			underrun = run >= kRate / 1000;
		}
		if (underrun || graph.listener.jumpsWithin(at, tenth)) {
			++excused;
			continue;
		}
		++judged;
		bool both = true;
		for (std::size_t channel = 0; channel < 2; ++channel) {
			const auto window = graph.listener.frames(channel, at, tenth);
			const double wanted = channel == 0 ? 1000.0 : 1500.0;
			const double other = channel == 0 ? 1500.0 : 1000.0;
			const auto fit = near(window, wanted);
			const double crossed = near(window, other).amplitude;
			profile += std::to_string(fit.amplitude) + "/" + std::to_string(fit.residual) + "/" +
			           std::to_string(crossed) + (channel == 0 ? "," : " ");
			both = both && std::abs(fit.amplitude - kAmplitude) < 0.01 && fit.residual < 0.1 && crossed < 0.01;
		}
		if (both) ++fitted;
	}
	INFO("amplitude/residual/other per judged tenth, from_pipe.1,from_pipe.2: "
	     << profile << "(" << excused << " tenths held an underrun or a lost cycle)");
	CHECK(fitted == planned);
	CHECK_FALSE(events.lostDetail.has_value());
}

// docs/engine.md#latency-per-bank-pair: the pipe module's ring lies behind its own ports, which tell no latency, so the
// block's plan enters it once, by moduleRunDelay; told on `stream` it is exactly PipeWire's 8192 frames at the writer's
// rate, in graph frames.
TEST_CASE("A pipe receiver's ring beyond from_pipe reaches stream whole: its ports tell 0 and the block tells exactly "
          "moduleRunDelay",
          "[module_run][pipe][latency]") {
	Graph graph;
	const PipeReceiverShape shape{PipeFormat::s16le, *SampleRate::of(kWriterRate)};
	const Delay delay = moduleRunDelay(shape, FrameClock(ear::SampleRate::fixed<kRate>()));
	REQUIRE(delay.latency == Latency{8916, 8916});
	graph.block->carryLatency(LatencyPlan{.pairs = {{"from_pipe", "stream"}}, .beyond = {{"from_pipe", delay}}});
	PortLatency stream(*graph.engine, graph.waiter, graph.block->blockId(), "stream.1");
	REQUIRE(graph.waiter.until([&] { return stream.told() == delay.latency; }));

	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events, shape);
	FifoWriter writer(graph.target().fifo, kWriterRate, tones(kWriterRate));
	PortLatency receiverPort(*graph.engine, graph.waiter, *events.nodeId, "capture_AUX0");
	REQUIRE(graph.waiter.until(
		[&] { return receiverPort.told().has_value() && graph.listener.soundFrom(0).has_value(); }, 10s));
	REQUIRE(graph.runs(64)); // the main loop turned meanwhile, with any retell among it
	INFO("the receiver's port tells " << receiverPort.told()->minFrames << "-" << receiverPort.told()->maxFrames);
	CHECK(receiverPort.told() == Latency{0, 0});
	CHECK(stream.told() == delay.latency);
}

TEST_CASE("A pipe receiver never drives: every cycle's driver is the internal clock while its writer stops and starts "
          "again",
          "[module_run][pipe][clock]") {
	Graph graph;
	graph.listener.expectedDriver.store(graph.dummy);
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events, PipeReceiverShape{PipeFormat::s16le, *SampleRate::of(kWriterRate)});
	for (int round = 0; round < 2; ++round) {
		INFO("writer " << round + 1);
		const std::size_t at = graph.listener.count();
		{
			FifoWriter writer(graph.target().fifo, kWriterRate, tones(kWriterRate));
			REQUIRE(graph.waiter.until([&] { return graph.listener.soundFrom(at).has_value(); }, 10s));
			REQUIRE(graph.runs(kRate / kBlock)); // a second of it playing
		}
		// Stopped: the ring runs dry and the receiver plays silence, still on the internal clock.
		REQUIRE(graph.waiter.until([&] { return graph.listener.silentFor(at, kRate / 2); }, 10s));
	}
	CHECK(graph.listener.otherDriver() == 0);
	CHECK(events.readyCount == 1);
	CHECK_FALSE(events.lostDetail.has_value());
}

TEST_CASE("A pipe receiver whose writer stops stays ready and its channels go silent", "[module_run][pipe][writer]") {
	Graph graph;
	ConverterEvents events(graph.waiter, graph.registry);
	const auto converter = graph.ready(events);
	std::size_t stoppedAt = 0;
	{
		FifoWriter writer(graph.target().fifo, kRate, constants());
		REQUIRE(graph.waiter.until([&] { return graph.listener.longest() >= kRate / 10; }, 20s));
		stoppedAt = graph.listener.count();
	}
	// A whole second of silence on both channels once the ring ran dry, and the receiver neither lost nor told again.
	REQUIRE(graph.waiter.until([&] { return graph.listener.silentFor(stoppedAt, kRate); }, 15s));
	CHECK(events.readyCount == 1);
	CHECK_FALSE(events.lostDetail.has_value());
	CHECK(graph.registry.state("fpp.pipe") == PW_NODE_STATE_RUNNING);
}
