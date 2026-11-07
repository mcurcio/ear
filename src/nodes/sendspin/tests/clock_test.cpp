// A sendspin node's clock (docs/design/sendspin.md section 7, section 14, S1 criterion 9): the real engine, a real port
// block driven by the real code, the real library behind the real worker, and the real server playing a ramp in which
// every frame names its place, so output time can be held against the time filter's prediction, cycle by cycle. Every
// wait is event-gated; a deadline only fails the test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/pass_through.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "engine_harness.hpp"
#include "feed.hpp"
#include "sendspin_code.hpp"
#include "sendspin_server.hpp"
#include "settings.hpp"
#include "temp_folder.hpp"
#include "worker.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;
using namespace ear::pipewire::testing;
using namespace ear::sendspin;
using namespace std::chrono_literals;
using ear::test::SendspinTestServer;

constexpr std::uint32_t kFeedFrames = 2048;

/// One cycle as the real code left it.
struct Cycle {
	std::int64_t startRawNs;
	std::uint64_t position;
	std::uint32_t frames;
	std::uint32_t delayFrames; // what lies downstream of the outputs (a following card's buffer), as the code was told
	std::uint32_t driverId;    // as the framework told it
	std::uint32_t got;         // frames of ramp in it (the rest zeros), if they name their place
	std::uint32_t firstAt;     // where in the cycle the first of them is, when got > 0
	std::uint32_t firstIndex;  // the ramp index that one names
	std::uint32_t lastIndex;   // the ramp index the last of them names
	bool ramp;                 // every frame in it is a zero or a ramp frame stepping by +1 from firstIndex
};

/// The ramp index a frame names, or nothing when it is no exact ramp frame (or silence).
std::optional<std::uint32_t> indexOf(float left, float right) {
	const float lo = left * 32768.0F;
	const float hi = right * 32768.0F;
	if (lo != std::trunc(lo) || hi != std::trunc(hi)) return std::nullopt;
	const auto word = [](float value) {
		return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xFFFFU;
	};
	return word(lo) | (word(hi) << 16U);
}

/// Runs the real code and records what it output, into memory made before it runs.
class Tap final : public AudioCode {
public:
	Tap(AudioCode& code, Waiter& waiter) : code_(code), waiter_(waiter), cycles_(kCapacity) {}
	void prepare(const AudioFormat& format) override { code_.prepare(format); }
	void release() noexcept override { code_.release(); }
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		code_.process(inputs, outputs, frames, cycle);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at >= kCapacity) return;
		Cycle& out = cycles_[at];
		out.startRawNs = cycle.startRawNs;
		out.position = cycle.position;
		out.frames = frames;
		out.delayFrames = cycle.downstreamOf(0).maxFrames;
		out.driverId = cycle.driverId;
		out.got = 0;
		out.firstAt = 0;
		out.firstIndex = 0;
		out.lastIndex = 0;
		out.ramp = true;
		// Zeros, ramp, zeros: silence the library primed with, or what the feed did not hold.
		for (std::uint32_t i = 0; i < frames; ++i) {
			const float left = outputs[0][i];
			const float right = outputs[1][i];
			if (left == 0.0F && right == 0.0F) continue;
			const auto index = indexOf(left, right);
			if (!index) {
				out.ramp = false;
				continue;
			}
			if (out.got == 0) {
				out.firstAt = i;
				out.firstIndex = *index;
			} else if (*index != out.firstIndex + (i - out.firstAt)) {
				out.ramp = false;
			}
			out.lastIndex = *index;
			++out.got;
		}
		count_.store(at + 1, std::memory_order_release);
		waiter_.wake();
	}
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	const Cycle& at(std::size_t index) const { return cycles_[index]; }
	static constexpr std::size_t kCapacity = 60 * kRate / kBlock;

private:
	AudioCode& code_;
	Waiter& waiter_;
	std::vector<Cycle> cycles_;
	std::atomic<std::size_t> count_{0};
};

/// What the worker says, as a node's insides would be told; `stream-ended` also records the tap's cycle.
class Notes final : public InsidesEvents {
public:
	explicit Notes(Waiter& waiter) : waiter_(waiter) {}
	void connectTap(const Tap& tap) { tap_ = &tap; }
	void say(std::string text) override {
		{
			const std::scoped_lock lock(mutex_);
			if (text == "stream-ended" && tap_ != nullptr) endedAt_ = tap_->count();
			said_.push_back(std::move(text));
		}
		waiter_.wake();
	}
	void lost(std::string detail) override { say("LOST " + detail); }
	bool heard(std::string_view prefix) { return lineStarting(prefix).has_value(); }
	std::optional<std::string> lineStarting(std::string_view prefix) {
		const std::scoped_lock lock(mutex_);
		const auto line =
			std::ranges::find_if(said_, [&](const std::string& text) { return text.starts_with(prefix); });
		return line == said_.end() ? std::nullopt : std::optional<std::string>(*line);
	}
	std::optional<std::size_t> endedAt() {
		const std::scoped_lock lock(mutex_);
		return endedAt_;
	}
	std::string all() {
		const std::scoped_lock lock(mutex_);
		std::string text;
		for (const std::string& line : said_) text += line + "\n";
		return text;
	}

private:
	Waiter& waiter_;
	std::mutex mutex_;
	std::vector<std::string> said_;
	const Tap* tap_ = nullptr;
	std::optional<std::size_t> endedAt_;
};

/// The handoff's realtime end, where these tests need none: only the audio from the server is timed.
class DropWriter final : public RtWriter {
public:
	std::uint32_t write(std::span<const std::span<const float>>, std::uint32_t frames) noexcept override {
		return frames;
	}
};

/// The folder of the node's key file.
struct KeyFolder {
	KeyFolder() {
		path = dir.path();
		auto opened = folder::ConfinedFolder::open(path);
		REQUIRE(opened.has_value());
		held = std::make_shared<const folder::ConfinedFolder>(std::move(*opened));
	}
	ear::test::TempFolder dir{"sendspin-keys"}; // first, so it is removed last
	std::filesystem::path path;
	std::shared_ptr<const folder::ConfinedFolder> held;
};

/// An output block with the real insides, as NodeHost would run them, dialling the real server.
struct Rig {
	Rig()
		: engine(makeEngine()), waiter(*engine), blocks(*engine, bell), feed(2, kFeedFrames), notes(waiter),
		  worker(keys.held, "clock", SendspinSettings{ServerUrl::parse(server.url()), defaultListenPort, "clock"},
	             kFormat, feed, notes),
		  code(feed, worker.progress(), worker.bell(), kRate, toServer, worker.capture()), tap(code, waiter),
		  blockEvents(waiter) {
		notes.connectTap(tap);
		block = blocks.make(
			"clock",
			Ports{{{Direction::input, "to", {"left", "right"}}, {Direction::output, "from", {"left", "right"}}}},
			kFormat, blockEvents);
		tap.prepare(kFormat);
		block->drive(&tap);
		REQUIRE(waiter.until([&] { return blockEvents.isReady; }));
	}
	~Rig() {
		block->drive(nullptr);
		block.reset();
		tap.release();
		std::atomic<bool> stopped{false};
		worker.stop([&] {
			stopped = true;
			waiter.wake();
		});
		CHECK(waiter.until([&] { return stopped.load(); }, 10s));
	}

	/// Connected and paired, then the server's clock has a play time for the ramp's first frame.
	std::int64_t play(double seconds) {
		INFO("the node said:\n" << notes.all());
		REQUIRE(waiter.until([&] { return notes.heard("connected "); }, 20s));
		server.send("pair");
		REQUIRE(waiter.until([&] { return notes.heard("pairing-code "); }, 20s));
		std::string code = notes.lineStarting("pairing-code ")->substr(13);
		std::erase(code, '-');
		server.send("code " + code);
		REQUIRE(waiter.until([&] { return notes.heard("connected \"ear-test-server\" paired"); }, 20s));
		// Not a check: on a loaded host the data thread's stalls make the clock resync, which restarts its loop, and the
		// test then judges what it can (the rtp.md erratum 3 trap).
		if (!waiter.until([&] { return tracksRawClock(); }, 20s))
			WARN("the graph did not settle on the raw clock's rate within 20 s");
		server.send("play " + std::to_string(seconds));
		const auto start = server.waitForEvent("play-start ", 20s);
		REQUIRE(start.has_value());
		return std::stoll(start->substr(11));
	}

	/// Whether the graph has run at its rate on the raw clock, within 20 ppm, over the last two seconds of cycles. The
	/// internal clock starts at the rate of CLOCK_MONOTONIC and its loop takes seconds to reach the raw clock's
	/// (docs/design/node-framework.md, slice 7 as built); until it has, the library sees the graph off by however far
	/// NTP slews CLOCK_MONOTONIC on the host (140 ppm seen) and corrects by interpolating frames.
	bool tracksRawClock() const {
		constexpr std::size_t window = 2 * kRate / kBlock;
		const std::size_t count = tap.count();
		if (count < window + 1) return false;
		const Cycle& first = tap.at(count - 1 - window);
		const Cycle& last = tap.at(count - 1);
		if (last.position - first.position != std::uint64_t{window} * kBlock) return false; // a jump: look again later
		const double frames = static_cast<double>(last.position - first.position);
		const double seconds = static_cast<double>(last.startRawNs - first.startRawNs) / 1e9;
		return std::abs(frames / seconds / kRate - 1.0) < 20e-6;
	}

	/// The ramp reached the output.
	std::size_t firstRampCycle(std::size_t from = 0) const {
		for (std::size_t i = from; i < tap.count(); ++i) {
			if (tap.at(i).got > 0) return i;
		}
		return tap.count();
	}

	// The test ALSA configuration must be in place before the engine looks at ALSA.
	struct AlsaFirst {
		AlsaFirst() {
			setenv("ALSA_CONFIG_PATH", EAR_ALSA_TEST_DIR "/alsa.conf", 1);
			setenv("EAR_ALSA_OUT", out.path().c_str(), 1);
		}
		ear::test::TempFolder out{"sendspin-card"}; // last to go: Rig's first member
	} alsa;
	SendspinTestServer server;
	KeyFolder keys;
	std::unique_ptr<Engine> engine;
	Waiter waiter;
	NoBell bell;
	FilterPortBlocks blocks;
	Feed feed;
	Notes notes;
	DropWriter toServer;
	SendspinWorker worker;
	SendspinCode code;
	Tap tap;
	BlockEvents blockEvents;
	std::unique_ptr<PortBlock> block;
};

class CardEvents final : public OutsideNodeEvents {
public:
	explicit CardEvents(Waiter& waiter) : waiter_(waiter) {}
	void ready() override {
		isReady = true;
		waiter_.wake();
	}
	void lost(std::string detail) override {
		lostDetail = std::move(detail);
		waiter_.wake();
	}
	bool isReady = false;
	std::optional<std::string> lostDetail;

private:
	Waiter& waiter_;
};

/// Card "Test" of the test ALSA configuration behind its own block, as a card node runs: the framework's pass-through
/// from its public "play" bank to the inside bank the card joins, carrying the card's latency back. The node's "from"
/// outputs are linked to "play", so the card follows the internal clock and plays what the node plays. The test card
/// has no clock, so it is made clockless (its rate matching off): docs/design/node-framework.md section 10.
struct TestCard {
	explicit TestCard(Rig& rig)
		: engine(*rig.engine), cards(*rig.engine, clocklessCardProperties()), blockEvents(rig.waiter),
		  events(rig.waiter), registry(*rig.engine, rig.waiter) {
		const Ports ports{{{Direction::input, "play", {"1", "2"}},
		                   {Direction::output, "to_card", {"1", "2"}, PortBank::Reach::inside}}};
		const std::vector<BankPair> pairs{{"play", "to_card"}};
		passThrough = makePassThrough(ports, pairs);
		passThrough->prepare(kFormat);
		block = rig.blocks.make("card", ports, kFormat, blockEvents);
		block->carryLatency(LatencyPlan{pairs, {}});
		block->drive(passThrough.get());
		REQUIRE(rig.waiter.until([&] { return blockEvents.isReady; }));
		card = cards.make(OutsideTarget{"card.playback", "card", {"to_card.1", "to_card.2"}, kFormat},
		                  SoundCardRequest{*CardId::parse("Test"), CardDirection::playback, "to_card"}, events);
		REQUIRE(rig.waiter.until([&] { return events.isReady || events.lostDetail.has_value(); }));
		REQUIRE(events.lostDetail.value_or("") == "");
		for (const char* side : {"left", "right"})
			link(std::string("from.") + side, side[0] == 'l' ? "play.1" : "play.2");
		REQUIRE(rig.waiter.until([&] {
			const auto node = registry.id("clock");
			return node && registry.activeLinksOf(*node) == 2;
		}));
	}
	~TestCard() {
		for (pw_proxy* made : links) pw_proxy_destroy(made);
		block->drive(nullptr);
	}
	/// A route's link, by node and port name, as the reconciler would make by port id.
	void link(const std::string& from, const std::string& to) {
		pw_properties* props =
			pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, "clock", PW_KEY_LINK_OUTPUT_PORT, from.c_str(),
		                      PW_KEY_LINK_INPUT_NODE, "card", PW_KEY_LINK_INPUT_PORT, to.c_str(), nullptr);
		links.push_back(static_cast<pw_proxy*>(pw_core_create_object(
			engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0)));
		pw_properties_free(props);
		REQUIRE(links.back() != nullptr);
	}
	Engine& engine;
	AlsaSoundCards cards;
	std::unique_ptr<AudioCode> passThrough;
	BlockEvents blockEvents;
	CardEvents events;
	Registry registry;
	std::unique_ptr<PortBlock> block;
	std::unique_ptr<OutsideNode> card;
	std::vector<pw_proxy*> links;
};

struct Errors {
	std::size_t judged = 0;
	std::size_t afterFirst3s = 0; // the cycles that could have been judged, skipped ones' 1.5 s among them
	std::uint32_t mostDelayFrames = 0;
	std::uint32_t leastDelayFrames = UINT32_MAX;
	std::size_t driverChanges = 0;
	std::size_t within = 0;
	double worstUs = 0.0;
	std::size_t skips = 0;
	std::size_t badCycles = 0;
	std::vector<std::uint32_t> badSteps; // each step's size, so a red names it
	std::uint64_t contiguousFrames = 0;  // and how long they took: the graph's real rate
	std::int64_t contiguousNs = 0;
	std::vector<double> sorted;
	double percentile(double fraction) const {
		return sorted.empty() ? 0.0
		                      : sorted[static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1))];
	}
};

/// Reads cycles as they arrive and holds each ramp cycle's output time against the time filter's prediction, at that
/// moment.
class Judge {
public:
	Judge(Rig& rig, std::int64_t playStartUs) : rig_(rig), playStartUs_(playStartUs) {}

	/// Every cycle not read yet.
	void read() {
		for (; next_ < rig_.tap.count(); ++next_) one(rig_.tap.at(next_));
	}

	/// How many whole cycles in a row have held the ramp exact, each stepping by +1 from the last.
	std::size_t plusOneRun() const { return plusOneRun_; }
	void restartRun() noexcept { plusOneRun_ = 0; }
	/// The newest ramp index out.
	std::uint32_t lastIndex() const { return lastIndex_.value_or(0); }

	Errors errors;

private:
	void one(const Cycle& cycle) {
		if (previous_ && cycle.position != previous_->position + previous_->frames) {
			const auto gap = static_cast<std::int64_t>(cycle.position) -
			                 static_cast<std::int64_t>(previous_->position + previous_->frames);
			if (gap > 0 && gap <= kFeedFrames) {
				++errors.skips;
				lastSkipNs_ = cycle.startRawNs;
			}
		}
		if (previous_ && cycle.driverId != previous_->driverId) ++errors.driverChanges;
		if (previous_ && cycle.position == previous_->position + previous_->frames) {
			errors.contiguousFrames += previous_->frames;
			errors.contiguousNs += cycle.startRawNs - previous_->startRawNs;
		}
		previous_ = cycle;
		if (cycle.got == 0) {
			plusOneRun_ = 0;
			return;
		}
		if (!cycle.ramp) ++errors.badCycles;
		if (!firstFrameNs_) firstFrameNs_ = cycle.startRawNs;
		if (lastIndex_) {
			const auto step = cycle.firstIndex - *lastIndex_;
			// The library's dropped frame: frame n goes and n+1 is blended, so no ramp frame names n or n+1. Inside a cycle
			// that is only a bad cycle; when the blend falls on the cycle's edge, the indices either side of it are 3 apart.
			const bool blendOnEdge = step == 3 && !(cycle.ramp && lastCycleRamp_);
			if (step != 1 && (step - 1) % kBlock != 0 && !blendOnEdge) errors.badSteps.push_back(step);
			plusOneRun_ = step == 1 && cycle.ramp && cycle.got == cycle.frames ? plusOneRun_ + 1 : 0;
		}
		lastIndex_ = cycle.lastIndex;
		lastCycleRamp_ = cycle.ramp;

		const std::int64_t rampUs = playStartUs_ + static_cast<std::int64_t>(cycle.firstIndex) * 1000000 / kRate;
		// When the first of them is heard: at the block's port, plus what lies downstream of it (section 7).
		const double outputUs = static_cast<double>(cycle.startRawNs) / 1000.0 +
		                        static_cast<double>(cycle.firstAt + cycle.delayFrames) * 1e6 / kRate;
		errors.mostDelayFrames = std::max(errors.mostDelayFrames, cycle.delayFrames);
		errors.leastDelayFrames = std::min(errors.leastDelayFrames, cycle.delayFrames);
		const double errorUs = outputUs - static_cast<double>(rig_.worker.clientTimeOf(rampUs));
		if (cycle.startRawNs - *firstFrameNs_ <= 3000000000) return;
		++errors.afterFirst3s;
		if (lastSkipNs_ && cycle.startRawNs - *lastSkipNs_ <= 1500000000) return;
		++errors.judged;
		if (std::abs(errorUs) < 1000.0) ++errors.within;
		if (std::abs(errorUs) > std::abs(errors.worstUs)) errors.worstUs = errorUs;
		errors.sorted.insert(std::ranges::lower_bound(errors.sorted, errorUs), errorUs);
	}

	Rig& rig_;
	std::int64_t playStartUs_;
	std::size_t next_ = 0;
	std::optional<Cycle> previous_;
	std::optional<std::uint32_t> lastIndex_;
	bool lastCycleRamp_ = true;
	std::optional<std::int64_t> firstFrameNs_;
	std::optional<std::int64_t> lastSkipNs_;
	std::size_t plusOneRun_ = 0;
};

void report(const Errors& errors) {
	const double seconds = static_cast<double>(errors.contiguousNs) / 1e9;
	WARN("judged " << errors.judged << " of " << errors.afterFirst3s << " cycles, downstream "
	               << errors.leastDelayFrames << " to " << errors.mostDelayFrames << " frames, within 1 ms "
	               << errors.within << ", p50 " << errors.percentile(0.5) << " us, p99 " << errors.percentile(0.99)
	               << " us, worst " << errors.worstUs << " us, skips " << errors.skips << ", bad cycles "
	               << errors.badCycles << ", bad steps " << errors.badSteps.size() << ", the graph's rate "
	               << static_cast<double>(errors.contiguousFrames) / seconds << " Hz");
}

void checkSync(const Errors& errors) {
	report(errors);
	// A host that skips all the time leaves little to judge: a cycle or two passing proves nothing. A tenth is about half a
	// second's worth of cycles; a host worse than that fails here, without a bug (the rtp.md erratum 3 trap).
	INFO("judged " << errors.judged << " of " << errors.afterFirst3s << ", within " << errors.within);
	REQUIRE(errors.afterFirst3s > 0);
	REQUIRE(static_cast<double>(errors.judged) >= 0.1 * static_cast<double>(errors.afterFirst3s));
	CHECK(static_cast<double>(errors.within) >= 0.99 * static_cast<double>(errors.judged));
	CHECK(errors.badSteps.empty());
	// A frame the library interpolates to correct an error past 100 us (section 7) is no exact ramp frame, and is allowed: CI's
	// amd64 host sits at -88 us and made two in one run, and at +84 us it dropped one. More than a few would be something else.
	CHECK(static_cast<double>(errors.badCycles) <= 0.01 * static_cast<double>(errors.judged));
}

} // namespace

TEST_CASE("On the dummy driver, audio leaves the block within 1 ms of the time filter's prediction",
          "[sendspin][clock]") {
	Rig rig;
	const std::int64_t playStart = rig.play(8.0);
	Judge judge(rig, playStart);
	INFO("the node said:\n" << rig.notes.all());
	const auto finished = [&] {
		judge.read();
		return rig.notes.heard("stream-ended");
	};
	REQUIRE(rig.waiter.until(finished, 30s));
	CHECK(rig.notes.heard("stream flac 48000 2 24"));
	checkSync(judge.errors);
}

// The card follows the internal clock (docs/design/node-framework.md section 10), so the graph keeps the dummy's clock
// and the library's 1 ms holds as on the dummy alone. What changes is when a frame is heard: once the card has played what
// it holds, which the card reports as its latency and the framework carries back through the card block's pass-through to
// the node's outputs (section 9.4). The test card is clockless, so it cannot show drift; that is a check on the hardware
// (the ear-on-talos skill).
TEST_CASE("Through a following test card, audio is dated by the card's latency and leaves within 1 ms of the time "
          "filter's prediction",
          "[sendspin][clock][cards]") {
	Rig rig;
	TestCard card(rig);
	const std::int64_t playStart = rig.play(8.0);
	Judge judge(rig, playStart);
	INFO("the node said:\n" << rig.notes.all());
	const auto finished = [&] {
		judge.read();
		return rig.notes.heard("stream-ended");
	};
	REQUIRE(rig.waiter.until(finished, 30s));
	CHECK(rig.notes.heard("stream flac 48000 2 24"));
	// ALSA's playback node reports at least a quantum (alsa-pcm-sink.c), and the card's latency is the same all along.
	CHECK(judge.errors.leastDelayFrames >= kBlock);
	CHECK(judge.errors.leastDelayFrames == judge.errors.mostDelayFrames);
	checkSync(judge.errors);
}

TEST_CASE("A card lost mid-stream leaves the clock alone, and within 3 s the output steps by +1 again",
          "[sendspin][clock][cards]") {
	Rig rig;
	TestCard card(rig);
	const std::int64_t playStart = rig.play(8.0);
	Judge judge(rig, playStart);
	INFO("the node said:\n" << rig.notes.all());
	// Well into the stream: two seconds of the ramp are out.
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.lastIndex() > 2 * kRate;
		},
		20s));

	const auto node = card.registry.id("card.playback");
	REQUIRE(node.has_value());
	const auto links = card.registry.linksOf(*node);
	REQUIRE_FALSE(links.empty());
	REQUIRE(card.registry.destroy(links.front()));
	REQUIRE(rig.waiter.until([&] {
		judge.read();
		return card.events.lostDetail.has_value();
	}));
	judge.restartRun();

	// Losing the card cuts the audio path but leaves its latency (PipeWire never tells the block that the last link went,
	// so the downstream latency the node's frames are dated by stays, stale). Any correction the library makes comes from
	// the cut, a frame per chunk: a chunk is about five blocks, so three in a row hold it.
	CHECK(rig.waiter.until(
		[&] {
			judge.read();
			return judge.plusOneRun() >= 3;
		},
		3s));
	report(judge.errors);
	// The internal clock drove all along: neither the card's links nor its loss moved the clock.
	CHECK(judge.errors.driverChanges == 0);
}

TEST_CASE("A stream ended mid-play leaves zeros within two blocks", "[sendspin][clock]") {
	Rig rig;
	rig.play(8.0);
	INFO("the node said:\n" << rig.notes.all());
	// The ramp is flowing.
	REQUIRE(rig.waiter.until(
		[&] {
			const std::size_t first = rig.firstRampCycle();
			return first < rig.tap.count() && rig.tap.count() > first + 32;
		},
		20s));

	rig.server.send("end");
	REQUIRE(rig.waiter.until([&] { return rig.notes.endedAt().has_value(); }, 10s));
	const std::size_t ended = *rig.notes.endedAt();
	REQUIRE(rig.waiter.until([&] { return rig.tap.count() >= ended + 32; }));
	for (std::size_t i = ended + 2; i < rig.tap.count(); ++i) {
		INFO("cycle " << i << " of those after the end at " << ended);
		CHECK(rig.tap.at(i).got == 0);
		CHECK(rig.tap.at(i).ramp);
	}
}
