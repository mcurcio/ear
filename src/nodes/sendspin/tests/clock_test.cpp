// A sendspin node's clock (docs/node-types.md#clock): the real engine, a real port
// block driven by the real code, the real library behind the real worker, and the real server playing a ramp in which
// every frame names its place, so output time can be held against the time filter's prediction, cycle by cycle. Every
// wait is event-gated; a deadline only fails the test. What the host does (a stalled data thread, a jump of the clock) is
// recorded in a cycle ledger and only makes a test play longer: each judges a planned count of cycles the host left alone
// (docs/testing.md#making-the-rare-event).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
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
#include "ear/testing/drive_now.hpp"
#include "engine_harness.hpp"
#include "feed.hpp"
#include "inducers.hpp"
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
using ear::test::CycleLedger;
using ear::test::SendspinTestServer;

constexpr std::uint32_t kFeedFrames = 2048;
/// The cycles each timing test judges: about four seconds of cycles the host left alone, each 3 s into the ramp and
/// 1.5 s past anything the host did. However often the host disturbs the clock, the stream plays on until they are.
constexpr std::size_t kPlannedCycles = 750;
/// The stream the timing tests ask for: far longer than they need on a quiet host, so a loaded one still reaches
/// kPlannedCycles before it ends. They stop judging once they have, and the rig stops the worker mid-stream.
constexpr double kStreamSeconds = 90.0;
/// How long a disturbance of the clock (a skip, a jump, a stall) excuses what follows it: the library catching up with
/// its clock after the feed ran dry, and the internal clock catching up by whole blocks (engine.conf's resync.ms keeps
/// its phase to the raw clock).
constexpr std::int64_t kSettleNs = 1500000000;

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
	bool clean;                // the ledger's word: it followed the cycle before it with nothing lost
	std::uint32_t zeros;       // frames of exact silence in it: once the ramp has begun, what the feed did not hold
	std::uint32_t skipped;     // frames the code skipped in it for the graph's jump (SendspinCode::lastSkipped)
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

/// Runs the real code and records what it output, and each cycle in a ledger, into memory made before it runs.
class Tap final : public AudioCode {
public:
	Tap(SendspinCode& code, Waiter& waiter) : code_(code), waiter_(waiter), ledger_(kCapacity), cycles_(kCapacity) {}
	void prepare(const AudioFormat& format) override { code_.prepare(format); }
	void release() noexcept override { code_.release(); }
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		code_.process(inputs, outputs, frames, cycle);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at >= kCapacity) return;
		ledger_.record(cycle, frames);
		Cycle& out = cycles_[at];
		out.startRawNs = cycle.startRawNs;
		out.position = cycle.position;
		out.frames = frames;
		out.delayFrames = cycle.downstreamOf(0).maxFrames;
		out.driverId = cycle.driverId;
		out.got = 0;
		out.zeros = 0;
		out.skipped = code_.lastSkipped();
		out.firstAt = 0;
		out.firstIndex = 0;
		out.lastIndex = 0;
		out.ramp = true;
		out.clean = ledger_[at].clean;
		// Zeros, ramp, zeros: silence the library primed with, or what the feed did not hold.
		for (std::uint32_t i = 0; i < frames; ++i) {
			const float left = outputs[0][i];
			const float right = outputs[1][i];
			if (left == 0.0F && right == 0.0F) {
				++out.zeros;
				continue;
			}
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
	static constexpr std::size_t kCapacity = 150 * kRate / kBlock;

private:
	SendspinCode& code_;
	Waiter& waiter_;
	CycleLedger ledger_; // the data thread's alone; each Cycle carries its word
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
		REQUIRE(ear::testing::driveNow(*block, &tap));
		REQUIRE(waiter.until([&] { return blockEvents.isReady; }));
	}
	~Rig() {
		ear::testing::silenceOrAbort(*block);
		block.reset();
		tap.release();
		std::atomic<bool> stopped{false};
		worker.stop([&] {
			stopped = true;
			waiter.wake();
		});
		CHECK(waiter.until([&] { return stopped.load(); }, 10s));
	}

	/// Connected and paired, then the server has committed the ramp's first frames. Their play time is not taken from
	/// here: a Judge reads each frame's from the chunk that carried it.
	void play(double seconds) {
		INFO("the node said:\n" << notes.all());
		REQUIRE(waiter.until([&] { return notes.heard("connected "); }, 20s));
		server.send("pair");
		REQUIRE(waiter.until([&] { return notes.heard("pairing-code "); }, 20s));
		std::string code = notes.lineStarting("pairing-code ")->substr(13);
		std::erase(code, '-');
		server.send("code " + code);
		REQUIRE(waiter.until([&] { return notes.heard("connected \"ear-test-server\" paired"); }, 20s));
		// ear's own event: the internal clock's loop reaching the raw clock's rate. Measured over the cycles the host
		// left alone, so a loaded host only delays it; the deadline only fails the test.
		const bool tracking = waiter.until([&] { return tracksRawClock(); }, 60s);
		INFO("the graph's rate over its clean cycles never came within 20 ppm of the raw clock's");
		REQUIRE(tracking);
		server.send("play " + std::to_string(seconds));
		REQUIRE(server.waitForEvent("play-start ", 20s).has_value());
	}

	/// Whether the graph has run at its rate on the raw clock, within 20 ppm, over the clean cycles of the last two
	/// seconds, which must be most of them. The internal clock starts at the rate of CLOCK_MONOTONIC and its loop takes
	/// seconds to reach the raw clock's (docs/engine.md#the-internal-clock); until it has, the library sees the graph off
	/// by however far NTP slews CLOCK_MONOTONIC on the host (140 ppm seen) and corrects by interpolating frames. Each
	/// clean cycle counts against the one before it: a jump the host made says nothing of the loop's rate, and a window
	/// with one in it is still read.
	bool tracksRawClock() const {
		constexpr std::size_t window = 2 * kRate / kBlock;
		const std::size_t count = tap.count();
		if (count < window + 1) return false;
		std::uint64_t frames = 0;
		std::int64_t ns = 0;
		for (std::size_t i = count - window; i < count; ++i) {
			if (!tap.at(i).clean) continue;
			frames += tap.at(i - 1).frames;
			ns += tap.at(i).startRawNs - tap.at(i - 1).startRawNs;
		}
		if (frames < std::uint64_t{window} * kBlock * 3 / 4 || ns <= 0) return false; // too few: look again later
		return std::abs(static_cast<double>(frames) / (static_cast<double>(ns) / 1e9) / kRate - 1.0) < 20e-6;
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

class CardEvents final : public EngineCompanionEvents {
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
/// has no clock, so it is made clockless (its rate matching off): docs/engine.md#the-internal-clock.
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
		block->carryLatency(LatencyPlan{.pairs = pairs});
		REQUIRE(ear::testing::driveNow(*block, passThrough.get()));
		REQUIRE(rig.waiter.until([&] { return blockEvents.isReady; }));
		card = cards.make(
			CompanionTarget{
				"card.playback", "card", Direction::output, {"to_card.1", "to_card.2"}, kFormat, block->blockId()},
			SoundCardShape{*CardId::parse("Test")}, events);
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
		ear::testing::silenceOrAbort(*block);
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
	std::unique_ptr<EngineCompanion> card;
	std::vector<pw_proxy*> links;
};

struct Errors {
	std::size_t judged = 0;
	std::size_t afterFirst3s = 0; // the cycles that could have been judged, those the host disturbed among them
	std::uint32_t mostDelayFrames = 0;
	std::uint32_t leastDelayFrames = UINT32_MAX;
	std::size_t driverChanges = 0;
	std::size_t within = 0;
	double worstUs = 0.0;
	std::size_t skips = 0;        // forward jumps within the feed, which the code skips
	std::size_t shortSkips = 0;   // of them, those the feed held less than: the code skipped all it held
	std::size_t disturbances = 0; // cycles the ledger did not call clean: what the host did to the graph
	std::size_t dryCycles = 0;    // cycles mid-ramp the feed did not fill: what the host did to the worker
	std::size_t badCycles = 0;
	std::size_t excusedSteps = 0;        // steps the library took catching up after a jump past the feed
	std::vector<std::uint32_t> badSteps; // each step's size, so a red names it
	std::uint64_t contiguousFrames = 0;  // and how long they took: the graph's real rate
	std::int64_t contiguousNs = 0;
	std::vector<double> sorted;
	double percentile(double fraction) const {
		return sorted.empty() ? 0.0
		                      : sorted[static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1))];
	}
};

/// What a red needs to say about the judging: a report for the failure's message, never a warning of its own.
std::string describe(const Errors& errors) {
	const double seconds = static_cast<double>(errors.contiguousNs) / 1e9;
	std::ostringstream text;
	text << "judged " << errors.judged << " of " << errors.afterFirst3s << " cycles (planned " << kPlannedCycles
		 << "), downstream " << errors.leastDelayFrames << " to " << errors.mostDelayFrames << " frames, within 1 ms "
		 << errors.within << ", p50 " << errors.percentile(0.5) << " us, p99 " << errors.percentile(0.99)
		 << " us, worst " << errors.worstUs << " us, skips " << errors.skips << " (short " << errors.shortSkips << ")"
		 << ", disturbed cycles " << errors.disturbances << ", dry cycles " << errors.dryCycles << ", bad cycles "
		 << errors.badCycles << ", steps excused after a jump " << errors.excusedSteps << ", the graph's rate "
		 << static_cast<double>(errors.contiguousFrames) / seconds << " Hz, bad steps:";
	for (const std::uint32_t step : errors.badSteps) text << " " << step;
	return text.str();
}

/// Reads cycles as they arrive and holds each ramp cycle's output time against the time filter's prediction, at that
/// moment, for the stamp the server sent its frames with. What the host did is read from the ledger's word on each
/// cycle: a cycle is judged only when it is clean, 3 s into the ramp and 1.5 s past the last cycle that was not.
class Judge {
public:
	explicit Judge(Rig& rig) : rig_(rig) {}

	/// Every cycle not read yet.
	void read() {
		for (; next_ < rig_.tap.count(); ++next_) one(next_, rig_.tap.at(next_));
	}

	/// How many whole cycles in a row have held the ramp exact, each stepping by +1 from the last.
	std::size_t plusOneRun() const { return plusOneRun_; }
	/// The cycle at which plusOneRun() last reached `run`.
	std::optional<std::size_t> runReachedAt(std::size_t run) const {
		return plusOneRun_ >= run ? std::optional<std::size_t>(next_ - 1 - (plusOneRun_ - run)) : std::nullopt;
	}
	void restartRun() noexcept { plusOneRun_ = 0; }
	/// The newest ramp index out.
	std::uint32_t lastIndex() const { return lastIndex_.value_or(0); }
	/// The newest cycle the ledger did not call clean, by index.
	std::optional<std::size_t> lastDisturbed() const { return lastDisturbed_; }

	Errors errors;

private:
	void one(std::size_t index, const Cycle& cycle) {
		std::int64_t skipped = 0; // the graph's forward jump within the feed, which the code skips
		bool jumpPastFeed = false;
		if (previous_ && !cycle.clean) {
			// Whatever the host did, the time around it is not judged: a skip, a stall, a jump.
			++errors.disturbances;
			lastDisturbanceNs_ = cycle.startRawNs;
			lastDisturbed_ = index;
			const auto gap = static_cast<std::int64_t>(cycle.position) -
			                 static_cast<std::int64_t>(previous_->position + previous_->frames);
			if (gap > 0 && gap <= kFeedFrames) {
				++errors.skips;
				skipped = gap;
				// The code skips the jump, or all the feed held when that was less (a worker the host held up), and never
				// more.
				if (cycle.skipped < gap) ++errors.shortSkips;
				if (cycle.skipped > gap) errors.badSteps.push_back(cycle.skipped);
			} else if (gap != 0 || cycle.driverId != previous_->driverId) {
				// Past the feed, or backward: a new clock to the code, which skips nothing; the library then catches up
				// with its clock in its own steps (sendspin skill, threads and clock).
				jumpPastFeed = true;
				lastJumpNs_ = cycle.startRawNs;
			}
		}
		// Mid-ramp, the feed did not fill this cycle: it held less than a skip took, or the worker that fills it was held
		// up by the host. Told nothing played while it was dry, the library drops what it holds to catch up with its
		// clock, so this too starts the time that is not judged, and excuses the catching up after it.
		const bool afterDry = previousDry_;
		previousDry_ = lastIndex_ && cycle.zeros > 0;
		if (previousDry_) {
			++errors.dryCycles;
			lastDisturbanceNs_ = cycle.startRawNs;
			lastDisturbed_ = index;
		}
		if (previous_ && cycle.driverId != previous_->driverId) ++errors.driverChanges;
		if (previous_ && cycle.clean) {
			errors.contiguousFrames += previous_->frames;
			errors.contiguousNs += cycle.startRawNs - previous_->startRawNs;
		}
		previous_ = cycle;
		pendingSkipped_ += cycle.skipped; // a skip in a cycle that played nothing moves the next ramp frame on
		if (cycle.got == 0) {
			plusOneRun_ = 0;
			return;
		}
		if (!firstFrameNs_) firstFrameNs_ = cycle.startRawNs;
		const bool settling = lastDisturbanceNs_ && cycle.startRawNs - *lastDisturbanceNs_ <= kSettleNs;
		if (lastIndex_) {
			const auto step = cycle.firstIndex - *lastIndex_;
			// The library's dropped frame: frame n goes and n+1 is blended, so no ramp frame names n or n+1. Inside a cycle
			// that is only a bad cycle; when the blend falls on the cycle's edge, the indices either side of it are 3 apart.
			const bool blendOnEdge = step == 3 && !(cycle.ramp && lastCycleRamp_);
			// A skip moves the ramp on by exactly the frames the code skipped for it (lastSkipped: the jump, or all the
			// feed held). A stalled data thread makes the internal
			// clock jump by the time it lost, any number of frames (1127, 1156, 1189 seen on an idle host), not only
			// whole cycles. Resuming after the ramp ran dry is the library's own catching up, right after a disturbance
			// only.
			const bool skip = pendingSkipped_ > 0 && step == 1 + static_cast<std::uint32_t>(pendingSkipped_);
			const bool caughtUp = afterDry && settling;
			// After a jump past the feed nothing is skipped, and what the library then does to catch up is its own.
			const bool catchingUp =
				step != 1 && !jumpPastFeed && lastJumpNs_ && cycle.startRawNs - *lastJumpNs_ <= kSettleNs;
			if (catchingUp) ++errors.excusedSteps;
			if (step != 1 && !skip && !caughtUp && !blendOnEdge && !catchingUp) errors.badSteps.push_back(step);
			plusOneRun_ = step == 1 && cycle.ramp && cycle.got == cycle.frames ? plusOneRun_ + 1 : 0;
		}
		lastIndex_ = cycle.lastIndex;
		lastCycleRamp_ = cycle.ramp;
		pendingSkipped_ = 0;

		const std::int64_t rampUs = sentUs(cycle.firstIndex);
		// When the first of them is heard: at the block's port, plus what lies downstream of it.
		const double outputUs = static_cast<double>(cycle.startRawNs) / 1000.0 +
		                        static_cast<double>(cycle.firstAt + cycle.delayFrames) * 1e6 / kRate;
		errors.mostDelayFrames = std::max(errors.mostDelayFrames, cycle.delayFrames);
		errors.leastDelayFrames = std::min(errors.leastDelayFrames, cycle.delayFrames);
		const double errorUs = outputUs - static_cast<double>(rig_.worker.clientTimeOf(rampUs));
		if (cycle.startRawNs - *firstFrameNs_ <= 3000000000) return;
		++errors.afterFirst3s;
		if (!cycle.clean || settling) return;
		++errors.judged;
		if (!cycle.ramp) ++errors.badCycles;
		if (std::abs(errorUs) < 1000.0) ++errors.within;
		if (std::abs(errorUs) > std::abs(errors.worstUs)) errors.worstUs = errorUs;
		errors.sorted.insert(std::ranges::lower_bound(errors.sorted, errorUs), errorUs);
	}

	/// One chunk of the ramp as the server handed it to the client: its first frame, how many, and the server time the
	/// first is stamped to play at.
	struct SentChunk {
		std::uint32_t first;
		std::uint32_t frames;
		std::int64_t us;
	};

	/// The server time ramp frame `index` was sent to play at, from the stamp of the chunk that carried it: the time the
	/// client was asked to play it at, which is what the time filter's prediction is of. Not play-start plus the index:
	/// the server can move its timeline after a commit (on a loaded host, the second commit came late and moved it by
	/// 7 ms), and the audio its encoder held from before goes out on the moved one (sendspin_server.py, _SentRamp).
	std::int64_t sentUs(std::uint32_t index) {
		for (;;) {
			for (const SentChunk& chunk : sent_) {
				if (index >= chunk.first && index - chunk.first < chunk.frames)
					return chunk.us + static_cast<std::int64_t>(index - chunk.first) * 1000000 / kRate;
			}
			// Chunks go out hundreds of milliseconds before they play, so this one has been said by now.
			const auto said = rig_.server.waitForEvent("chunk-sent ", 5s);
			REQUIRE(said.has_value());
			SentChunk chunk{};
			REQUIRE(std::sscanf(said->c_str(), "chunk-sent %u %u %" SCNd64, &chunk.first, &chunk.frames, &chunk.us) ==
			        3);
			sent_.push_back(chunk);
		}
	}

	Rig& rig_;
	std::vector<SentChunk> sent_;
	std::size_t next_ = 0;
	std::optional<Cycle> previous_;
	std::optional<std::uint32_t> lastIndex_;
	bool lastCycleRamp_ = true;
	std::optional<std::int64_t> firstFrameNs_;
	std::optional<std::int64_t> lastDisturbanceNs_;
	std::optional<std::int64_t> lastJumpNs_;
	std::optional<std::size_t> lastDisturbed_;
	std::size_t plusOneRun_ = 0;
	bool previousDry_ = false;
	std::int64_t pendingSkipped_ = 0; // skipped since the last ramp frame out
};

/// Judges cycles as they come until the planned count is judged: however often the host disturbs the clock, the
/// stream plays on until then. The deadline only fails the test.
void judgePlanned(Rig& rig, Judge& judge) {
	const bool judged = rig.waiter.until(
		[&] {
			judge.read();
			return judge.errors.judged >= kPlannedCycles || rig.notes.heard("stream-ended");
		},
		150s);
	INFO(describe(judge.errors));
	REQUIRE(judged);
	REQUIRE(judge.errors.judged >= kPlannedCycles);
}

void checkSync(const Errors& errors) {
	INFO(describe(errors));
	CHECK(static_cast<double>(errors.within) >= 0.99 * static_cast<double>(errors.judged));
	CHECK(errors.badSteps.empty());
	// A frame the library interpolates to correct an error past 100 us is no exact ramp frame, and is allowed: CI's
	// amd64 host sits at -88 us and made two in one run, and at +84 us it dropped one. More than a few would be
	// something else.
	CHECK(static_cast<double>(errors.badCycles) <= 0.01 * static_cast<double>(errors.judged));
}

} // namespace

TEST_CASE("On the dummy driver, audio leaves the block within 1 ms of the time filter's prediction",
          "[sendspin][clock]") {
	Rig rig;
	rig.play(kStreamSeconds);
	Judge judge(rig);
	INFO("the node said:\n" << rig.notes.all());
	judgePlanned(rig, judge);
	CHECK(rig.notes.heard("stream flac 48000 2 24"));
	checkSync(judge.errors);
}

// The card follows the internal clock (docs/engine.md#the-internal-clock), so the graph keeps the dummy's clock and the
// library's 1 ms holds as on the dummy alone. What changes is when a frame is heard: once the card has played what it
// holds, which the card reports as its latency and the framework carries back through the card block's pass-through to
// the node's outputs. The test card is clockless, so it cannot show drift; that is a check on the hardware (the
// ear-on-talos skill).
TEST_CASE("Through a following test card, audio is dated by the card's latency and leaves within 1 ms of the time "
          "filter's prediction",
          "[sendspin][clock][cards]") {
	Rig rig;
	TestCard card(rig);
	rig.play(kStreamSeconds);
	Judge judge(rig);
	INFO("the node said:\n" << rig.notes.all());
	judgePlanned(rig, judge);
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
	rig.play(kStreamSeconds);
	Judge judge(rig);
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
	const std::size_t cut = rig.tap.count(); // no cycle after the cut is recorded before this
	REQUIRE(card.registry.destroy(links.front()));
	REQUIRE(rig.waiter.until([&] {
		judge.read();
		return card.events.lostDetail.has_value();
	}));
	judge.restartRun();

	// Losing the card cuts the audio path but leaves its latency (PipeWire never tells the block that the last link went,
	// so the downstream latency the node's frames are dated by stays, stale). Any correction the library makes comes from
	// the cut, a frame per chunk: a chunk is about five blocks, so three in a row hold it. The 3 s are the graph's,
	// counted in cycles from the cut, or from the last cycle the host disturbed after it (which the library recovers
	// from as it does from the cut); a host that keeps disturbing the clock only makes the wait longer.
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.plusOneRun() >= 3;
		},
		60s));
	const std::size_t reached = *judge.runReachedAt(3);
	std::size_t from = cut;
	if (const auto disturbed = judge.lastDisturbed(); disturbed && *disturbed > from && *disturbed < reached)
		from = *disturbed;
	INFO(describe(judge.errors));
	INFO("the run of three came at cycle " << reached << ", counted from cycle " << from << " (the cut at " << cut
	                                       << ")");
	CHECK(reached - from <= 3 * kRate / kBlock);
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
