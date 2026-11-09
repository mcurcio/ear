// RtpReceivers on the real in-process engine (docs/engine.md#rtp-receivers): PipeWire's rtp-source module loaded into
// EAR's own engine, no daemon, joined to a port block by the one engine-companion lifecycle. The sender is the tests'
// own thread writing RTP to 127.0.0.1 (test/support/rtp_tone.hpp). Every wait is event-gated: the main loop runs until
// the events a test waits for have arrived (a receiver event, a registry change, the block's cycles); the deadline only
// fails the test.
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
#include <spa/param/latency-utils.h>

#include "catch2/catch.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/testing/drive_now.hpp"
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

/// Records the block's inside inputs (from_rtp.1, from_rtp.2) into buffers made before it runs,
/// up to kCapacity frames, and notes the first frame that is not silent and every onset: a frame
/// that is not silent after at least kSilentRun silent frames (every channel exactly zero). Writes
/// zeros to every output.
class Recorder final : public AudioCode {
public:
	static constexpr std::size_t kCapacity = 20 * kRate;
	static constexpr std::size_t kSilentRun = kRate / 20; // 50 ms

	Recorder(Waiter& waiter, std::size_t channels)
		: waiter_(waiter), channels_(channels, std::vector<float>(kCapacity, 0.0F)) {}
	void prepare(const AudioFormat&) override {}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const ear::GraphCycle&) noexcept override {
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
			const bool silent =
				std::ranges::all_of(channels_, [&](const auto& channel) { return channel[at + i] == 0.0F; });
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
		count(); // acquire: first_ was stored before count_
		return first_.load(std::memory_order_relaxed);
	}
	/// The first onset at or after frame `from`; kNone if none yet.
	std::size_t onsetFrom(std::size_t from) const {
		count(); // acquire: the onsets were stored before count_
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
	std::size_t silentRun_ = kSilentRun; // the data thread's only; the very first sound is an onset
	std::array<std::size_t, 64> onsets_{};
	std::atomic<std::size_t> onsetCount_{0};
};

/// What a receiver tells, with what the registry showed at the moment it was told.
class ReceiverEvents final : public EngineCompanionEvents {
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

	std::function<void()> onReady; // told inside ready(), to read the engine at that moment
	int readyCount = 0;
	bool insideMake = false;
	std::optional<std::uint32_t> nodeId; // set by the test once known, for linksAtLost
	std::optional<std::string> lostDetail;
	bool lostInsideMake = false;
	bool nodePresentAtLost = false;
	std::size_t linksAtLost = 0;

private:
	Waiter& waiter_;
	Registry& registry_;
};

/// One port block "fpp" (outputs stream.1, stream.2; the inside input bank from_rtp)
/// driven by a Recorder, running on the dummy driver, and the receivers' factory.
struct Graph {
	Graph()
		: engine(makeEngine()), waiter(*engine), registry(*engine, waiter), blocks(*engine, bell), receivers(*engine),
		  recorder(waiter, 2), blockEvents(waiter) {
		block = blocks.make("fpp",
		                    Ports{{{Direction::output, "stream", {"1", "2"}},
		                           {Direction::input, "from_rtp", {"1", "2"}, PortBank::Reach::inside}}},
		                    kFormat, blockEvents);
		REQUIRE(ear::testing::driveNow(*block, &recorder));
		REQUIRE(
			waiter.until([&] { return blockEvents.isReady && registry.id("Dummy-Driver") && registry.props("fpp"); }));
		dummy = *registry.id("Dummy-Driver");
	}
	~Graph() { ear::testing::silenceOrAbort(*block); }

	std::unique_ptr<EngineCompanion> makeReceiver(std::uint16_t port, RtpEncoding encoding, std::uint32_t rate,
	                                              ReceiverEvents& events) {
		events.insideMake = true;
		auto receiver = receivers.make(
			CompanionTarget{
				"fpp.rtp", "fpp", Direction::input, {"from_rtp.1", "from_rtp.2"}, kFormat, block->blockId()},
			RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), port, encoding, rate, 100}, events);
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
	INFO("from_rtp." << channel + 1 << ": " << wanted << " Hz, frames " << from << "+" << frames);
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
		if (std::abs(fit.amplitude - kAmplitude) > std::abs(worstAmplitude - kAmplitude))
			worstAmplitude = fit.amplitude;
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

} // namespace

TEST_CASE("A receiver is ready once, every channel's link active, and it and the block follow the "
          "Dummy-Driver",
          "[rtp][clock]") {
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
	CHECK_FALSE(events.settled()); // never inside make()
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	CHECK(events.readyCount == 1);
	CHECK(activeAtReady == std::optional<std::size_t>(1)); // of 2
	REQUIRE(graph.registry.reread({"fpp.rtp", "fpp"}));
	CHECK(graph.registry.driverId("fpp.rtp") == graph.dummy);
	CHECK(graph.registry.driverId("fpp") == graph.dummy);
	CHECK(graph.runs(16));
	CHECK(events.readyCount == 1);
}

// docs/architecture.md#phases-states-and-what-is-wanted: the join names the block by its global id, so beside an older
// block of the same name (one still going) it reaches only the block it was given.
TEST_CASE("A receiver joins its block by the block's id: beside an older block of the same name it reaches only the "
          "newer",
          "[rtp][join]") {
	Graph graph;
	Recorder newerRecorder(graph.waiter, 2);
	BlockEvents newerEvents(graph.waiter);
	auto newer = graph.blocks.make("fpp",
	                               Ports{{{Direction::output, "stream", {"1", "2"}},
	                                      {Direction::input, "from_rtp", {"1", "2"}, PortBank::Reach::inside}}},
	                               kFormat, newerEvents);
	REQUIRE(ear::testing::driveNow(*newer, &newerRecorder));
	REQUIRE(graph.waiter.until([&] { return newerEvents.isReady && graph.registry.ids("fpp").size() == 2; }));
	const std::uint32_t olderId = graph.block->blockId();
	const std::uint32_t newerId = newer->blockId();
	REQUIRE(olderId != newerId);
	ReceiverEvents events(graph.waiter, graph.registry);
	const auto receiver = graph.receivers.make(
		CompanionTarget{"fpp.rtp", "fpp", Direction::input, {"from_rtp.1", "from_rtp.2"}, kFormat, newerId},
		RtpReceiverShape{*RtpAddress::parse("0.0.0.0"), ear::test::freeUdpPort(), RtpEncoding::l24, kRate, 100},
		events);
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	CHECK(graph.registry.activeLinksOf(newerId) == 2);
	CHECK(graph.registry.linksOf(olderId).empty());
	ear::testing::silenceOrAbort(*newer);
}

TEST_CASE("L24 at the graph rate: channel 1 reaches from_rtp.1 and channel 2 from_rtp.2", "[rtp][channels]") {
	checkTwoTones(RtpPcm::l24, RtpEncoding::l24, kRate);
}

TEST_CASE("L16 sent at 44100 into the 48000 graph is resampled", "[rtp][channels]") {
	checkTwoTones(RtpPcm::l16, RtpEncoding::l16, 44100);
}

// docs/engine.md#the-quantum-guard: the quantum limit is the block size, so a receiver whose sender runs faster than
// the graph must still fit a whole cycle's input in its buffers.
TEST_CASE("L24 sent at 96000 into the 48000 graph is resampled with the quantum limit at the block size",
          "[rtp][channels][quantum]") {
	checkTwoTones(RtpPcm::l24, RtpEncoding::l24, 96000);
}

TEST_CASE("A receiver's engine node carries no property that forces or locks the graph's quantum or rate",
          "[rtp][quantum]") {
	Graph graph;
	ReceiverEvents events(graph.waiter, graph.registry);
	const auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, 44100, events);
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	REQUIRE(graph.registry.reread({"fpp.rtp"}));
	const auto* props = graph.registry.props("fpp.rtp");
	REQUIRE(props != nullptr);
	for (const char* key : {"node.force-quantum", "node.force-rate", "node.lock-quantum", "node.lock-rate"}) {
		INFO(key);
		CHECK_FALSE(props->contains(key));
	}
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
          "and the block keeps running on the dummy",
          "[rtp][lost]") {
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
	CHECK(receiver != nullptr); // nobody destroyed it
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
std::unique_ptr<EngineCompanion> readyReceiver(Graph& graph, ReceiverEvents& events) {
	auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	const std::uint32_t node = *graph.registry.id("fpp.rtp");
	events.nodeId = node;
	REQUIRE(graph.waiter.until([&] { return graph.registry.linksOf(node).size() == 2; }));
	return receiver;
}

} // namespace

TEST_CASE("A receiver that loses a link while its node still runs is lost with its node and links "
          "already gone, and the block keeps running on the dummy",
          "[rtp][lost]") {
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

TEST_CASE("A receiver whose module is destroyed is lost with its node and links already gone", "[rtp][lost]") {
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
          "node, never the stale one",
          "[rtp][restart]") {
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
          "own node's id",
          "[rtp][restart]") {
	Graph graph;
	const std::uint16_t port = ear::test::freeUdpPort();
	ReceiverEvents firstEvents(graph.waiter, graph.registry);
	auto first = graph.makeReceiver(port, RtpEncoding::l24, kRate, firstEvents);
	REQUIRE(graph.waiter.until([&] { return firstEvents.settled(); }));
	REQUIRE(firstEvents.lostDetail.value_or("") == "");

	first.reset(); // a restart: the old receiver goes and the new one is made in the same turn
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

namespace {

/// The capture (upstream) latency the engine holds on one port, as any client reads it: a registry binding finds the
/// port by its node's global id and its name, binds it, and keeps its newest Latency param of direction output.
class CaptureLatencyWatch {
public:
	CaptureLatencyWatch(Engine& engine, Waiter& waiter, std::uint32_t nodeId, std::string portName)
		: waiter_(waiter), nodeId_(nodeId), portName_(std::move(portName)) {
		registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
	}
	~CaptureLatencyWatch() {
		if (port_ != nullptr) {
			spa_hook_remove(&portHook_);
			pw_proxy_destroy(port_);
		}
		spa_hook_remove(&registryHook_);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
	}
	CaptureLatencyWatch(const CaptureLatencyWatch&) = delete;
	CaptureLatencyWatch& operator=(const CaptureLatencyWatch&) = delete;

	/// The newest capture latency told on the port, in frames; nothing before the first.
	std::optional<Latency> told() const { return told_; }

private:
	static const pw_registry_events& registryEvents() {
		static const pw_registry_events table = [] {
			pw_registry_events events{};
			events.version = PW_VERSION_REGISTRY_EVENTS;
			events.global = &CaptureLatencyWatch::onGlobal;
			return events;
		}();
		return table;
	}
	static const pw_port_events& portEvents() {
		static const pw_port_events table = [] {
			pw_port_events events{};
			events.version = PW_VERSION_PORT_EVENTS;
			events.param = &CaptureLatencyWatch::onParam;
			return events;
		}();
		return table;
	}
	static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/, const char* type,
	                     std::uint32_t /*version*/, const spa_dict* props) {
		auto* self = static_cast<CaptureLatencyWatch*>(data);
		if (self->port_ != nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Port || props == nullptr) return;
		const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
		if (node == nullptr || name == nullptr || std::to_string(self->nodeId_) != node || self->portName_ != name)
			return;
		self->port_ =
			static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, 0));
		pw_proxy_add_object_listener(self->port_, &self->portHook_, &portEvents(), self);
		std::uint32_t wanted = SPA_PARAM_Latency;
		pw_port_subscribe_params(reinterpret_cast<pw_port*>(self->port_), &wanted, 1);
	}
	static void onParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/, std::uint32_t /*next*/,
	                    const spa_pod* param) {
		auto* self = static_cast<CaptureLatencyWatch*>(data);
		spa_latency_info info{};
		if (id != SPA_PARAM_Latency || param == nullptr || spa_latency_parse(param, &info) < 0 ||
		    info.direction != SPA_DIRECTION_OUTPUT)
			return;
		self->told_ = Latency{static_cast<std::uint32_t>(info.min_rate), static_cast<std::uint32_t>(info.max_rate)};
		self->waiter_.wake();
	}

	Waiter& waiter_;
	std::uint32_t nodeId_;
	std::string portName_;
	pw_registry* registry_ = nullptr;
	spa_hook registryHook_{};
	pw_proxy* port_ = nullptr;
	spa_hook portHook_{};
	std::optional<Latency> told_;
};

} // namespace

// A receiver's buffer lies behind its own ports, which report a latency of zero once linked, so the plan's `beyond`
// entry for its bank is added in series to what its peers report, never combined with them as a path of its own: told
// on `stream`, the capture latency's least and most are both the buffer (docs/engine.md#latency-per-bank-pair).
TEST_CASE("A real receiver's buffer beyond from_rtp reaches stream whole: the told capture latency's least and most "
          "are both the buffer once the receiver is linked and reports its own",
          "[rtp][latency]") {
	Graph graph;
	constexpr std::uint32_t buffered = 100 * kRate / 1000; // latencyMs 100
	graph.block->carryLatency(LatencyPlan{{{"from_rtp", "stream"}}, {}, {{"from_rtp", Latency{buffered, buffered}}}});
	CaptureLatencyWatch stream(*graph.engine, graph.waiter, graph.block->blockId(), "stream.1");
	REQUIRE(graph.waiter.until([&] { return stream.told() == Latency{buffered, buffered}; }));

	ReceiverEvents events(graph.waiter, graph.registry);
	const auto receiver = graph.makeReceiver(ear::test::freeUdpPort(), RtpEncoding::l24, kRate, events);
	REQUIRE(graph.waiter.until([&] { return events.settled(); }));
	REQUIRE(events.lostDetail.value_or("") == "");
	// The receiver's own output port reports its capture latency once linked; the block hears it and tells again.
	REQUIRE(graph.registry.id("fpp.rtp"));
	CaptureLatencyWatch receiverPort(*graph.engine, graph.waiter, *graph.registry.id("fpp.rtp"), "receive_AUX0");
	REQUIRE(graph.waiter.until([&] { return receiverPort.told().has_value(); }));
	INFO("the receiver's port reports " << receiverPort.told()->minFrames << "-" << receiverPort.told()->maxFrames);
	REQUIRE(graph.runs(32)); // the main loop turned meanwhile, with the block's retell among it
	REQUIRE(stream.told().has_value());
	CHECK(stream.told()->minFrames == buffered);
	CHECK(stream.told()->maxFrames == buffered);
}
