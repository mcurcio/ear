// A sendspin node's clock (docs/node-types.md#clock): the real engine, a real port
// block driven by the real code, the real library behind the real worker, and the real server playing a ramp in which
// every frame names its place, so output time can be held against the time filter's prediction, cycle by cycle. Every
// wait is event-gated; a deadline only fails the test. Every cycle the block ran goes to the one cycle judge
// (docs/testing.md#the-cycle-judge): a cycle it excuses is excused alone, and what the host does to the graph only
// makes a test play longer, each judging a planned count of cycles. What lies past the graph (the library realigning
// itself) is judged apart, as library settles declared from their evidence.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "cycle_judge.hpp"
#include "engine_harness.hpp"
#include "feed.hpp"
#include "ear/core/latency.hpp"
#include "inducers.hpp"
#include "mailbox.hpp"
#include "sendspin_code.hpp"
#include "sendspin_server.hpp"
#include "sendspin/client.h"
#include "settings.hpp"
#include "standard_error_lines.hpp"
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
/// The cycles each timing test judges: about four seconds of whole cycles, each 3 s into the ramp and outside any
/// library settle. However often the host disturbs the clock, the stream plays on until they are.
constexpr std::size_t kPlannedCycles = 750;
/// The stream the timing tests ask for: far longer than they need on a quiet host, so a loaded one still reaches
/// kPlannedCycles before it ends. They stop judging once they have, and the rig stops the worker mid-stream.
constexpr double kStreamSeconds = 90.0;
/// The frames of one decoded chunk: what FFmpeg's FLAC encoder makes at 48000 Hz.
constexpr std::uint64_t kChunkFrames = 4608;
/// How long, in the library's own chunks, it takes to realign itself after it lost time ear did not skip
/// (docs/testing.md#the-cycle-judge): its floor puts the next chunk it syncs at now, which the block plays late by up to
/// two blocks (docs/node-types.md#library-facts-and-carried-patches), and the chunk after drops that once more, so from
/// the second chunk boundary past the first frame played after the loss the stream is on the server's stamps.
constexpr std::uint32_t kSettleChunks = 2;
/// How many of the library's soft-sync frames may follow a settle: its hard sync leaves it within its settle
/// threshold (500 us, sync_task.cpp's HARD_SYNC_SETTLE_THRESHOLD_US), and it then interpolates one frame a chunk while
/// it is off by more than 100 us (SOFT_SYNC_THRESHOLD_US), so at most (500 - 100) us of 20.8 us frames, one a chunk,
/// in the chunks after the settle. Each is one blended cycle, inside 1 ms, counted apart from the 1% bound.
constexpr std::uint32_t kSoftSyncChunks = (500 - 100) * kRate / 1000000 + 1;
/// The cycles between the judge's planned stalls, once the timing is judged: four of them land inside the planned span.
constexpr std::size_t kStallEvery = 150;

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
	std::int64_t innerStep; // the largest step between two exact ramp frames inside it (1 when it has fewer than two)
	bool ramp;              // every frame in it is a zero or a ramp frame stepping by +1 from firstIndex
	bool clean;             // the ledger's word: it followed the cycle before it with nothing lost
	CycleFacts facts;       // what the cycle stated, as the code was handed it
	std::int64_t overdueNs; // how long the next cycle had been due as it began (GraphCycle::overdueNs)
	std::uint32_t zeros;    // frames of exact silence in it
	std::uint32_t heldAtStart; // frames the feed held as the process began (SendspinCode::lastHeldAtStart)
	std::uint32_t codeSkipped; // what the code skipped of them for the graph's lost time (SendspinCode::lastSkipped)
	std::int64_t ranRawNs;     // when the block really began it (GraphCycle::ranRawNs)
	bool dry;                  // the feed's own count of short reads moved in it (Feed::dryReads)
	std::uint32_t fed;         // frames the code read from the feed: its frames less those the feed counted lacking
	double rateDiff;           // the driver's rate correction, as the code was handed it
	PlayProgress progress;     // the newest the code told the library, after it ran
	// Worked out by the Judge from the facts and what the feed held, by the code's rule (docs/node-types.md#clock): the
	// frames it skips in it for the graph's lost time, whether it tells the library of this cycle, and whether that
	// report is one the bound forced from an overdue cycle.
	std::uint32_t skipped;
	bool published;
	bool forced;
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

/// Runs the real code and records what it output, what it told the library and each cycle in a ledger, into memory
/// made before it runs.
class Tap final : public AudioCode {
public:
	Tap(SendspinCode& code, const Feed& feed, const Mailbox<PlayProgress>& progress, Waiter& waiter)
		: code_(code), feed_(feed), progress_(progress), waiter_(waiter), ledger_(kCapacity), cycles_(kCapacity) {}
	void prepare(const AudioFormat& format) override { code_.prepare(format); }
	void release() noexcept override { code_.release(); }
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		code_.process(inputs, outputs, frames, cycle);
		const std::size_t at = count_.load(std::memory_order_relaxed);
		if (at >= kCapacity) return;
		// The one planned stall, once armed. Made by a StallProbe upstream, it stalls a graph cycle the block never runs,
		// so the cycle before this one is the last before the stall and this one states the catch-up. Made here, in the
		// block's own process, this cycle is the stalled one, and what follows is the burst in whatever shape the host
		// gives it.
		if (const int mode = mode_.load(std::memory_order_acquire); mode != kUnarmed) {
			if (stalledAt_.load(std::memory_order_relaxed) == kNone) {
				if (mode == kProbe) {
					const std::uint64_t stalledIn = watched_->load(std::memory_order_acquire);
					if (stalledIn != 0 && cycle.facts.graphCycle > stalledIn)
						stalledAt_.store(at - 1, std::memory_order_release);
				} else {
					(mode == kOwnShort ? ownShort_ : ownLong_)(0, frames, kRate);
					stalledAt_.store(at, std::memory_order_release);
				}
			}
		}
		ledger_.record(cycle, frames);
		Cycle& out = cycles_[at];
		out.startRawNs = cycle.startRawNs;
		out.position = cycle.position;
		out.frames = frames;
		out.delayFrames = cycle.downstreamOf(0).maxFrames;
		out.driverId = cycle.driverId;
		out.got = 0;
		out.zeros = 0;
		out.facts = cycle.facts;
		out.overdueNs = cycle.overdueNs;
		out.heldAtStart = code_.lastHeldAtStart();
		out.codeSkipped = code_.lastSkipped();
		out.ranRawNs = cycle.ranRawNs();
		// The feed's own word on its read this cycle: the code reads it once a cycle, on this thread.
		const std::uint64_t dryReads = feed_.dryReads();
		const std::uint64_t dryFrames = feed_.dryFrames();
		out.dry = dryReads != dryReads_;
		out.fed = frames - static_cast<std::uint32_t>(dryFrames - dryFrames_);
		dryReads_ = dryReads;
		dryFrames_ = dryFrames;
		out.rateDiff = cycle.rateDiff;
		out.skipped = 0;
		out.published = false;
		out.forced = false;
		// The code publishes only on this thread, so the newest value is this cycle's when it told one.
		out.progress = progress_.read().value_or(PlayProgress{});
		out.firstAt = 0;
		out.firstIndex = 0;
		out.lastIndex = 0;
		out.ramp = true;
		out.innerStep = 1;
		out.clean = ledger_[at].whole();
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
			} else {
				if (*index != out.firstIndex + (i - out.firstAt)) out.ramp = false;
				out.innerStep = std::max<std::int64_t>(out.innerStep, static_cast<std::int64_t>(*index) -
				                                                          static_cast<std::int64_t>(out.lastIndex));
			}
			out.lastIndex = *index;
			++out.got;
		}
		count_.store(at + 1, std::memory_order_release);
		waiter_.wake();
	}
	std::size_t count() const { return count_.load(std::memory_order_acquire); }
	/// Main thread, once, before the stall is armed: watches for the graph cycle a StallProbe stalls in (`stalledIn`, 0
	/// until it has).
	void watchStall(const std::atomic<std::uint64_t>& stalledIn) {
		watched_ = &stalledIn;
		mode_.store(kProbe, std::memory_order_release);
	}
	/// Main thread, once: the block's next cycle stalls the data thread itself, for StallProbe::kShortStall or
	/// kLongStall.
	void stallOwn(bool longStall) { mode_.store(longStall ? kOwnLong : kOwnShort, std::memory_order_release); }
	/// The last cycle the block ran before the stall, once the next has run.
	std::optional<std::size_t> stalledAt() const {
		const std::size_t at = stalledAt_.load(std::memory_order_acquire);
		return at == kNone ? std::nullopt : std::optional<std::size_t>(at);
	}
	/// The count itself, for what stamps an event with the cycles recorded when it came.
	const std::atomic<std::size_t>& counter() const { return count_; }
	const Cycle& at(std::size_t index) const { return cycles_[index]; }
	static constexpr std::size_t kCapacity = 150 * kRate / kBlock;

private:
	SendspinCode& code_;
	const Feed& feed_;
	const Mailbox<PlayProgress>& progress_;
	Waiter& waiter_;
	std::uint64_t dryReads_ = 0; // the data thread's: the feed's counts at the last cycle
	std::uint64_t dryFrames_ = 0;
	CycleLedger ledger_; // the data thread's alone; each Cycle carries its word
	std::vector<Cycle> cycles_;
	std::atomic<std::size_t> count_{0};
	static constexpr std::size_t kNone = ~std::size_t{0};
	static constexpr int kUnarmed = 0;
	static constexpr int kProbe = 1;
	static constexpr int kOwnShort = 2;
	static constexpr int kOwnLong = 3;
	std::atomic<int> mode_{kUnarmed};
	const std::atomic<std::uint64_t>* watched_ = nullptr; // a StallProbe's stalled graph cycle; published by mode_
	ear::test::Stalls ownShort_{
		ear::test::StallPlan::at({{0, ear::test::StallLength::of(std::chrono::milliseconds(200))}})};
	ear::test::Stalls ownLong_{
		ear::test::StallPlan::at({{0, ear::test::StallLength::of(std::chrono::milliseconds(1000))}})};
	std::atomic<std::size_t> stalledAt_{kNone};
};

/// What the library says of how its sync task re-aligned, read as events: it has no log hook (the sendspin skill), so
/// its standard error goes through a pipe for the rig's life (StandardErrorLines), its verbose, debug and info lines
/// kept from the real one, and the lines below recorded with the tap's cycle count when each was read (sendspin-cpp's
/// sync_task.cpp, handle_synchronize_audio): `Hard sync: dropping <n> frames` (a late chunk's front dropped), `Hard
/// sync: adding <n> frames of silence` (silence for a chunk due later than the audio before it), both verbose lines,
/// and `Lost sync (<n>us off)`, a warning said once as a loss of sync begins, and `Regained sync`, an info line, as it
/// ends; and `Buffered frames underflow` (sync_task.cpp, process_playback_progress), the warning the library gives when
/// it is told more played than it wrote, which ear must never make. The rig asks for verbose once the worker has set
/// the library's level (setVerbose).
class LibraryLog {
public:
	enum class Kind { drop, silence, lostSync, regained, underflow };
	struct Line {
		std::size_t cycle; // the tap's count when the line was read
		Kind kind;
		std::int64_t value; // frames dropped or added, or us off
	};

	void connectTap(const std::atomic<std::size_t>& cycles) { said_.stampFrom(cycles); }
	/// The library's verbose lines, which say each hard sync's frames. Its level is one for the process, and the worker
	/// sets it as it makes its client, so this comes after the worker has connected.
	static void setVerbose() { ::sendspin::SendspinClient::set_log_level(::sendspin::LogLevel::VERBOSE); }
	std::vector<Line> lines() const {
		std::vector<Line> lines;
		for (const ear::test::StandardErrorLines::Line& said : said_.lines()) {
			const auto after = [&](std::string_view marker) -> std::optional<std::int64_t> {
				const std::size_t at = said.text.find(marker);
				if (at == std::string::npos) return std::nullopt;
				return std::strtoll(said.text.c_str() + at + marker.size(), nullptr, 10);
			};
			if (const auto frames = after(kDrop)) lines.push_back({said.stamp, Kind::drop, *frames});
			else if (const auto added = after(kSilence)) lines.push_back({said.stamp, Kind::silence, *added});
			else if (const auto off = after(kLost)) lines.push_back({said.stamp, Kind::lostSync, *off});
			else if (said.text.find(kUnderflow) != std::string::npos) lines.push_back({said.stamp, Kind::underflow, 0});
			else lines.push_back({said.stamp, Kind::regained, 0});
		}
		return lines;
	}

private:
	static constexpr std::string_view kDrop = "Hard sync: dropping ";
	static constexpr std::string_view kSilence = "Hard sync: adding ";
	static constexpr std::string_view kLost = "Lost sync (";
	static constexpr std::string_view kRegained = "Regained sync";
	static constexpr std::string_view kUnderflow = "Buffered frames underflow";
	ear::test::StandardErrorLines said_{{std::string(kDrop), std::string(kSilence), std::string(kLost),
	                                     std::string(kRegained), std::string(kUnderflow)},
	                                    {"V ", "D ", "I "}};
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
		  code(feed, worker.progress(), worker.bell(), kRate, toServer, worker.capture()),
		  tap(code, feed, worker.progress(), waiter), pulse(kRate), blockEvents(waiter), pulseEvents(waiter),
		  registry(*engine, waiter) {
		notes.connectTap(tap);
		log.connectTap(tap.counter());
		block = blocks.make(
			"clock",
			Ports{{{Direction::input, "to", {"left", "right"}}, {Direction::output, "from", {"left", "right"}}}},
			kFormat, blockEvents);
		tap.prepare(kFormat);
		REQUIRE(ear::testing::driveNow(*block, &tap));
		REQUIRE(waiter.until([&] { return blockEvents.isReady; }));
		// The judge's stalls are made upstream of the judged block, so each cuts a cycle the block then misses
		// (docs/testing.md#the-cycle-judge): a block "pulse" linked into the node's "to", which runs first in each cycle.
		pulseBlock = blocks.make("pulse", Ports{{{Direction::output, "out", {"1"}}}}, kFormat, pulseEvents);
		REQUIRE(ear::testing::driveNow(*pulseBlock, &pulse));
		REQUIRE(waiter.until([&] { return pulseEvents.isReady; }));
		pw_properties* props =
			pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, "pulse", PW_KEY_LINK_OUTPUT_PORT, "out.1",
		                      PW_KEY_LINK_INPUT_NODE, "clock", PW_KEY_LINK_INPUT_PORT, "to.left", nullptr);
		pulseLink = static_cast<pw_proxy*>(pw_core_create_object(engine->core(), "link-factory", PW_TYPE_INTERFACE_Link,
		                                                         PW_VERSION_LINK, &props->dict, 0));
		pw_properties_free(props);
		REQUIRE(pulseLink != nullptr);
		REQUIRE(waiter.until([&] {
			const auto node = registry.id("pulse");
			return node && registry.activeLinksOf(*node) == 1;
		}));
	}
	~Rig() {
		if (pulseLink != nullptr) pw_proxy_destroy(pulseLink);
		ear::testing::silenceOrAbort(*pulseBlock);
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
		LibraryLog::setVerbose();
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
	LibraryLog log; // before the library's threads start, and after they stop
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
	ear::test::ArmedStall pulse;
	BlockEvents blockEvents;
	BlockEvents pulseEvents;
	Registry registry;
	std::unique_ptr<PortBlock> block;
	std::unique_ptr<PortBlock> pulseBlock;
	pw_proxy* pulseLink = nullptr;
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
		// The node's links: these two and the rig's pulse into its "to".
		REQUIRE(rig.waiter.until([&] {
			const auto node = registry.id("clock");
			return node && registry.activeLinksOf(*node) == 3;
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

/// The stalls a test makes: 200 ms and 1 s, past the feed (43 ms) and past a decoded chunk (96 ms), and a hiccup of
/// 1.5 cycles, which the feed holds whole.
enum class StallKind { shortStall, longStall, hiccup };

/// A stall of the data thread whose lost cycles the node's block certainly misses, on any host. A block of its own,
/// linked into the node's `to` inputs so it runs first in every cycle, stalls once inside its process when armed: the
/// driver's timer becomes ready during the sleep, before the eventfd the probe writes as it finishes, so that cycle is
/// cut short straight after the probe and the node's block never runs it (docs/engine.md#the-one-data-loop-and-invokes).
/// The driver then catches the stalled time up in a burst of timeouts (docs/engine.md#the-internal-clock), and whether
/// the node's block runs a burst's cycle is the host's: it runs it when its own eventfd is dispatched before the
/// timeout the driver has already re-armed in the past, so one host runs it every other timeout and another every
/// timeout, losing nothing. So the probe makes the loss certain the same way: while a cycle's next timeout is already
/// due (`GraphCycle::nextNs` at or before now), it holds the cycle 1 ms, so that cycle too is cut short after it and
/// the block misses it. 1 ms is far past any wait for a due timer's interrupt and well under a block (5.3 ms), so the
/// burst still closes on raw time, by 4.3 ms a cycle; its first cycle whose next timeout is not yet due runs whole, and
/// the node's block states one catch-up of the whole stall and burst. The probe writes silence, as an unlinked input
/// reads.
struct StallProbe {
	/// The planned stalls: 200 ms, past the feed's 2048 frames (43 ms) and past a decoded chunk (96 ms), and 1 s.
	static constexpr std::chrono::milliseconds kShortStall{200};
	static constexpr std::chrono::milliseconds kLongStall{1000};
	static constexpr std::chrono::milliseconds kBurstHold{1};

	class Code final : public AudioCode {
	public:
		void prepare(const AudioFormat&) override {}
		void release() noexcept override {}
		void process(std::span<const std::span<const float>>, std::span<const std::span<float>> outputs,
		             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
			for (const std::span<float>& out : outputs) std::ranges::fill(out.first(frames), 0.0F);
			const int armed = armed_.load(std::memory_order_acquire);
			if (armed == 0 || caughtUp_) return;
			if (!stalled_) {
				(armed == 1 ? shortStall_ : armed == 2 ? longStall_ : hiccup_)(0, frames, kRate);
				stalled_ = true;
				stalledIn.store(cycle.facts.graphCycle, std::memory_order_release);
				return;
			}
			const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
										 std::chrono::steady_clock::now().time_since_epoch())
			                             .count(); // CLOCK_MONOTONIC on Linux, as nextNs
			if (cycle.nextNs <= now) {
				burstHolds_(held_++, frames, kRate);
				return;
			}
			caughtUp_ = true;
		}
		/// Main thread, once: the next cycle stalls, for kShortStall, kLongStall or a hiccup of 1.5 cycles.
		void arm(StallKind kind) { armed_.store(static_cast<int>(kind) + 1, std::memory_order_release); }
		/// The graph cycle it stalled in, once it has; 0 before.
		std::atomic<std::uint64_t> stalledIn{0};
		/// Cycles it held in the burst, once caught up; any thread after stalledIn, read once the node ran whole.
		std::size_t burstHolds() const { return burstHolds_.made(); }

	private:
		std::atomic<int> armed_{0}; // 0, or 1 for the short stall, 2 for the long and 3 for the hiccup
		ear::test::Stalls shortStall_{ear::test::StallPlan::at({{0, ear::test::StallLength::of(kShortStall)}})};
		ear::test::Stalls longStall_{ear::test::StallPlan::at({{0, ear::test::StallLength::of(kLongStall)}})};
		ear::test::Stalls hiccup_{ear::test::StallPlan::at({{0, ear::test::StallLength::cycles(1.5)}})};
		ear::test::Stalls burstHolds_{
			ear::test::StallPlan::every(1, 0, ear::test::Stalls::kKept, ear::test::StallLength::of(kBurstHold))};
		bool stalled_ = false;  // the data thread's
		bool caughtUp_ = false; // the data thread's
		std::size_t held_ = 0;  // the data thread's: the burst's holds so far, the plan's index
	};

	explicit StallProbe(Rig& rig) : engine(*rig.engine), blockEvents(rig.waiter), registry(*rig.engine, rig.waiter) {
		block = rig.blocks.make("probe", Ports{{{Direction::output, "out", {"left", "right"}}}}, kFormat, blockEvents);
		code.prepare(kFormat);
		REQUIRE(ear::testing::driveNow(*block, &code));
		REQUIRE(rig.waiter.until([&] { return blockEvents.isReady; }));
		for (const char* side : {"left", "right"}) link(std::string("out.") + side, std::string("to.") + side);
		REQUIRE(rig.waiter.until([&] {
			const auto node = registry.id("probe");
			return node && registry.activeLinksOf(*node) == 2;
		}));
	}
	~StallProbe() {
		for (pw_proxy* made : links) pw_proxy_destroy(made);
		ear::testing::silenceOrAbort(*block);
	}
	void link(const std::string& from, const std::string& to) {
		pw_properties* props =
			pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, "probe", PW_KEY_LINK_OUTPUT_PORT, from.c_str(),
		                      PW_KEY_LINK_INPUT_NODE, "clock", PW_KEY_LINK_INPUT_PORT, to.c_str(), nullptr);
		links.push_back(static_cast<pw_proxy*>(pw_core_create_object(
			engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0)));
		pw_properties_free(props);
		REQUIRE(links.back() != nullptr);
	}
	Engine& engine;
	Code code;
	BlockEvents blockEvents;
	Registry registry;
	std::unique_ptr<PortBlock> block;
	std::vector<pw_proxy*> links;
};

struct Errors {
	std::size_t judged = 0;
	std::size_t afterFirst3s = 0; // the cycles that could have been judged, those disturbed among them
	std::size_t rampCycles = 0;   // cycles since the ramp began
	std::uint32_t mostDelayFrames = 0;
	std::uint32_t leastDelayFrames = UINT32_MAX;
	std::size_t driverChanges = 0;
	std::size_t within = 0;
	double worstUs = 0.0;
	std::size_t skips = 0;          // catch-ups, which the code skips as far as the feed holds them
	std::size_t shortSkips = 0;     // of them, those the feed held less than: the rest the library's to correct
	std::size_t disturbances = 0;   // cycles the judge did not judge whole: what the host or the test did to the graph
	std::size_t dryCycles = 0;      // cycles the feed counted short (Feed::dryReads): what the host did to the worker
	std::size_t librarySilence = 0; // cycles fed whole that held silence: the library's own (a hard sync's)
	std::size_t badCycles = 0;
	std::size_t settleBlends = 0;     // judged cycles blended by the soft sync that follows a library settle
	std::size_t settleSteps = 0;      // forward steps inside a library settle, within its evidence
	std::size_t hardSyncSteps = 0;    // forward steps exactly a logged hard sync's drops
	std::size_t hardSyncSilence = 0;  // frames of library silence a logged hard sync added
	std::size_t lateChunks = 0;       // whole chunks the library skipped unlogged as too late, inside a logged step
	std::size_t skipsOverBlend = 0;   // skips that took a soft-sync frame with them
	std::uint64_t mostSettleLost = 0; // the most frames a settle's evidence said the library lost
	// The code's reports on consecutive whole cycles fed whole: how far each report's finish time moved from the last
	// one's plus a cycle's frames, the worst seen and those of a millisecond or more, with the cycle. Ear's own reports
	// stepping (a block's 5.3 ms, say) would be ear's bug, whatever the library then did.
	double worstProgressDriftUs = 0.0;
	std::vector<std::string> progressSteps;
	std::vector<std::string> steps; // every ramp step but +1 and the rule that took it, with the cycle (the first 64)
	std::vector<std::string> bad;   // every step, skip or report that broke a rule, with its cycle, so a red names it
	std::vector<std::string> unexplainedSilence; // library silence no logged hard sync accounts for
	std::uint64_t contiguousFrames = 0;          // and how long they took: the graph's real rate
	std::int64_t contiguousNs = 0;
	std::vector<std::string> filterSteps; // the time filter moving a fixed server time by 1 ms or more, with the cycle
	std::vector<std::string> lostSync;    // the library's losses of sync, with the cycle each was read at
	std::vector<std::size_t> underflows;  // the cycles each `Buffered frames underflow` was read at: ear told too much
	std::size_t forcedReports = 0;        // reports the bound forced from an overdue cycle
	std::size_t clamps = 0;               // of the reports after them, those the clamp moved
	std::vector<std::string> logged;      // each hard sync the library logged, with the cycle its line was read at
	std::size_t loggedDrops = 0;          // hard syncs the library logged dropping frames, and adding silence
	std::size_t loggedSilences = 0;
	std::vector<std::string> settles; // each library settle: its cause, the cycle, the frames its evidence says lost
	ear::test::JudgeReport graph;     // the cycle judge's word on every cycle of the block
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
		 << " us, worst " << errors.worstUs << " us, skips " << errors.skips << " (short " << errors.shortSkips
		 << "), disturbed cycles " << errors.disturbances << ", dry cycles " << errors.dryCycles << " of "
		 << errors.rampCycles << " ramp cycles ("
		 << (errors.rampCycles > 0
	             ? 100.0 * static_cast<double>(errors.dryCycles) / static_cast<double>(errors.rampCycles)
	             : 0.0)
		 << "%), library silence " << errors.librarySilence << ", bad cycles " << errors.badCycles << " (and "
		 << errors.settleBlends << " blended after a settle)"
		 << ", steps inside a library settle " << errors.settleSteps << " (the most it lost " << errors.mostSettleLost
		 << "), steps of a logged hard sync " << errors.hardSyncSteps << " (of " << errors.loggedDrops
		 << " logged drops), silence a logged hard sync added " << errors.hardSyncSilence << " frames (of "
		 << errors.loggedSilences << " logged), whole late chunks " << errors.lateChunks
		 << ", skips over a soft-sync frame " << errors.skipsOverBlend << ", reports the bound forced "
		 << errors.forcedReports << " (clamped after them " << errors.clamps << "), worst drift of the code's reports "
		 << errors.worstProgressDriftUs << " us, the graph's rate "
		 << static_cast<double>(errors.contiguousFrames) / seconds << " Hz\nthe cycle judge: " << errors.graph;
	const auto list = [&](const char* title, const std::vector<std::string>& items) {
		text << "\n" << title << ":";
		for (const std::string& item : items) text << " " << item;
	};
	list("broken", errors.bad);
	list("unexplained silence", errors.unexplainedSilence);
	list("steps (rule: step at cycle)", errors.steps);
	list("the code's reports stepping", errors.progressSteps);
	list("library settles (cause at cycle: frames lost)", errors.settles);
	list("lost sync (cycle: us off)", errors.lostSync);
	text << "\nunderflows told by the library: " << errors.underflows.size();
	list("hard syncs logged", errors.logged);
	list("time filter steps (cycle: us)", errors.filterSteps);
	return text.str();
}

/// Reads cycles as they arrive and holds each ramp cycle's output time against the time filter's prediction, at that
/// moment, for the stamp the server sent its frames with. Every cycle goes to the one cycle judge
/// (docs/testing.md#the-cycle-judge), which judges it whole, excuses it alone (a catch-up the driver cut) or fails it;
/// the code's skips and reports are modelled by its own rule and every one held against it. What lies past the graph is
/// the library's: where it realigns itself (a dry cycle, a catch-up's jump the feed did not hold whole, or a hard sync or
/// loss it logged) a library settle is declared with its evidence, kSettleChunks of its own chunks long. A cycle's
/// timing is judged when the judge judged it whole, 3 s into the ramp, outside any settle and any loss of sync the
/// library logged. Every step is accounted for by a rule, and every excuse is counted and bounded by its evidence.
class Judge {
public:
	/// The stalls the steady tests make: the judge's four 1.5-cycle stalls, upstream of the block, one every
	/// kStallEvery cycles once the timing is judged, so each run proves that a catch-up the feed holds, skipped exactly,
	/// excuses its own cycles and nothing after.
	static ear::test::StallPlan inducedStalls() {
		return ear::test::StallPlan::every(1, 0, 4, ear::test::StallLength::cycles(1.5));
	}

	/// `induce`: inducedStalls(), or none for a test whose own disturbance is what it judges (it says why).
	Judge(Rig& rig, const LibraryLog& log, ear::test::StallPlan induce)
		: rig_(rig), log_(log),
		  cycles_(
			  ear::test::JudgePlan{.judged = kPlannedCycles, .induce = std::move(induce), .expected = {.drives = 1}},
			  FrameClock(kRate), &rig.engine->cuts()) {}

	/// Every cycle not read yet but the newest kLogLag, whose log lines may still be on their way, and the time filter
	/// as it is now; arms the next planned stall when it is due.
	void read() {
		readLog();
		const std::size_t count = rig_.tap.count();
		for (; next_ + kLogLag < count; ++next_) one(next_, rig_.tap.at(next_));
		sampleFilter();
		errors.graph = cycles_.report();
		const std::size_t made = cycles_.stalls().made();
		if (errors.afterFirst3s > 0 && made < cycles_.stalls().plan().size() && !rig_.pulse.armed() &&
		    next_ >= lastStallAt_ + kStallEvery) {
			lastStallAt_ = next_;
			rig_.pulse.arm(cycles_.stalls());
		}
	}
	/// The cycle judge is done: its planned boundaries judged and every planned stall made and excused.
	bool graphDone() const { return cycles_.done(); }

	/// How many whole cycles in a row have held the ramp exact, each stepping by +1 from the last.
	std::size_t plusOneRun() const { return plusOneRun_; }
	/// The cycle at which plusOneRun() last reached `run`.
	std::optional<std::size_t> runReachedAt(std::size_t run) const {
		return plusOneRun_ >= run ? std::optional<std::size_t>(lastRampCycle_ - (plusOneRun_ - run)) : std::nullopt;
	}
	/// Counts runs again from cycle `from` on: those before it, read later, count for nothing.
	void restartRun(std::size_t from) noexcept {
		plusOneRun_ = 0;
		runFrom_ = from;
	}
	/// The newest ramp index out.
	std::uint32_t lastIndex() const { return lastIndex_.value_or(0); }
	/// One cycle that played ramp frames, as judged: how far its first frame was from the time filter's prediction, and
	/// its step from the last ramp frame out, less what the code skipped since (so 0 is the next frame; anything else is
	/// the library's own step).
	struct RampSeen {
		std::size_t index;
		double errorUs;
		std::int64_t libraryStep;
		bool broken;            // not every frame of it stepped by +1 (a soft-sync blend, or a step inside it)
		std::int64_t innerStep; // the largest step between exact ramp frames inside it
	};
	/// Every ramp cycle read so far, in order.
	const std::vector<RampSeen>& ramp() const { return ramp_; }
	/// Cycles read so far (all of them before this index).
	std::size_t readThrough() const { return next_; }
	/// Cycles from `from` up to and not including `to` the judge did not judge whole, read so far: what the host did to
	/// the graph.
	std::size_t disturbedBetween(std::size_t from, std::size_t to) const {
		return static_cast<std::size_t>(
			std::ranges::count_if(disturbed_, [&](std::size_t at) { return at >= from && at < to; }));
	}
	/// The first cycle from `from` on that the host disturbed (the ledger did not call it clean: cycles missed, cut or
	/// not, or any other jump) in a way the library could see, read so far: a catch-up the feed held whole is skipped
	/// from audio the library already wrote and told exactly, so it is not one. Nor is a dry cycle: what the feed holds
	/// is the product's doing.
	std::optional<std::size_t> firstHostDisturbanceFrom(std::size_t from) const {
		const auto found = std::ranges::find_if(unabsorbed_, [&](std::size_t at) { return at >= from; });
		return found != unabsorbed_.end() ? std::optional<std::size_t>(*found) : std::nullopt;
	}
	/// Dry cycles from cycle `from` up to and not including `to`, read so far.
	std::size_t dryBetween(std::size_t from, std::size_t to) const {
		return static_cast<std::size_t>(
			std::ranges::count_if(dry_, [&](std::size_t at) { return at >= from && at < to; }));
	}

	Errors errors;

private:
	// The library's lines are stamped with the tap's count when standard error was read, not with where their audio
	// plays, so a line is matched within reach of the cycle it was read at: what it did is heard within the feed's 2048
	// frames and the chunk it was writing after the line was read, give or take the cycles the reading thread lagged;
	// silence it added lasts its own frames more. Interim, and used for nothing but the library's own lines, until its
	// log reaches ear stamped with the feed's write position (docs/testing.md#the-librarys-own-word).
	static constexpr std::size_t kLogReach = (kFeedFrames + kChunkFrames) / kBlock + 16;
	static constexpr std::size_t kLogLag = 32;

	void one(std::size_t index, Cycle cycle) {
		const ear::test::Verdict verdict = cycles_.at(cycle.facts, cycle.frames);
		// What the code skips for the graph's lost time and when it tells, by its rule (docs/node-types.md#clock): on a
		// catch-up's or a pause's cycle, overdue or not, its jump as far as the feed holds it; what the feed did not hold
		// is the library's to correct, a settle with that as its evidence. It tells from a cycle that began on time, or
		// from any once untoldBound of the graph's nominal time has passed untold. No clear comes while cycles are judged.
		// Its progress and the ramp are judged against it.
		const bool caughtUp = cycle.facts.causes.has(CycleCause::catchUp) || cycle.facts.causes.has(CycleCause::absent);
		const std::uint64_t lostNow =
			caughtUp && cycle.facts.jumpFrames > 0 ? static_cast<std::uint64_t>(cycle.facts.jumpFrames) : 0;
		cycle.skipped = static_cast<std::uint32_t>(std::min<std::uint64_t>(lostNow, cycle.heldAtStart));
		if (cycle.codeSkipped != cycle.skipped)
			errors.bad.push_back("skipped " + std::to_string(cycle.codeSkipped) + " of a jump of " +
			                     std::to_string(lostNow) + " with " + std::to_string(cycle.heldAtStart) +
			                     " held, for " + std::to_string(cycle.skipped) + " at " + std::to_string(index));
		if (lostNow > cycle.skipped) settle(ear::test::LibrarySettle::unskipped, index, lostNow - cycle.skipped);
		pendingTold_ += cycle.skipped + cycle.fed;
		if (pendingTold_ > 0 && !untoldSince_) untoldSince_ = cycle.startRawNs;
		cycle.forced = cycle.overdueNs > 0 && untoldSince_ && cycle.startRawNs - *untoldSince_ >= untoldBound.count();
		cycle.published = pendingTold_ > 0 && (cycle.overdueNs == 0 || cycle.forced);
		if (cycle.published) untoldSince_.reset();
		if (verdict != ear::test::Verdict::judge && previous_) {
			++errors.disturbances;
			disturbed_.push_back(index);
			// A catch-up the feed held whole is skipped from audio the library already wrote and told exactly: nothing
			// the library could see, so nothing it may answer.
			if (!caughtUp || cycle.skipped < lostNow) unabsorbed_.push_back(index);
		}
		if (caughtUp && previous_) {
			// The code skips a catch-up as far as the feed holds it; what the feed did not hold is the library's.
			++errors.skips;
			if (cycle.skipped < lostNow) ++errors.shortSkips;
		}
		if (previous_ && cycle.driverId != previous_->driverId) ++errors.driverChanges;
		if (previous_ && cycle.facts.whole()) {
			errors.contiguousFrames += previous_->frames;
			errors.contiguousNs += cycle.startRawNs - previous_->startRawNs;
		}
		checkProgress(index, cycle);
		if (previous_ && cycle.facts.whole()) checkProgressStep(index, *previous_, cycle);
		previous_ = cycle;
		pendingSkipped_ += cycle.skipped; // a skip in a cycle that played nothing moves the next ramp frame on

		if (lastIndex_) {
			++errors.rampCycles;
			if (cycle.dry != (cycle.fed < cycle.frames))
				errors.bad.push_back("the feed's dry count and the frames it lacked disagree at " +
				                     std::to_string(index));
			if (cycle.dry) {
				// The feed's own word: it was read short. The code tells the library what it read, which then sees its
				// next chunk late by the zeros and realigns.
				++errors.dryCycles;
				dry_.push_back(index);
				settle(ear::test::LibrarySettle::dry, index, cycle.frames - cycle.fed);
			} else if (cycle.zeros > 0) {
				// Fed whole, and silence in it: the library's own, a hard sync adding it, which it logged with its frames.
				++errors.librarySilence;
				if (takeLogged(LibraryLog::Kind::silence, index, cycle.zeros, false)) {
					errors.hardSyncSilence += cycle.zeros;
					settle(ear::test::LibrarySettle::logged, index, 0);
				} else
					errors.unexplainedSilence.push_back(std::to_string(cycle.zeros) + " at " + std::to_string(index));
			}
		}
		// While the library has lost sync, by its own word, what it plays is its re-alignment, not its timing: one
		// settle, which runs on to kSettleChunks past the first ramp frame out after the loss ends.
		const bool lossOfSync = inLoss(index);
		if (lossOfSync && !wasInLoss_) settle(ear::test::LibrarySettle::logged, index, 0);
		if (!lossOfSync && wasInLoss_) settleFromNext_ = true;
		wasInLoss_ = lossOfSync;
		if (cycle.got == 0) {
			plusOneRun_ = 0;
			return;
		}
		if (!firstFrameNs_) firstFrameNs_ = cycle.startRawNs;
		// The chunks the server sent up to this cycle's first frame, which the step and the timing are judged by.
		const std::int64_t rampUs = sentUs(cycle.firstIndex);
		const std::int64_t libraryStep = lastIndex_ ? static_cast<std::int64_t>(cycle.firstIndex) -
		                                                  static_cast<std::int64_t>(*lastIndex_) - 1 - pendingSkipped_
		                                            : 0;
		if (lastIndex_) checkStep(index, cycle);
		// A settle opened before this ramp cycle ends kSettleChunks of the library's chunks past its first frame.
		if (settleFromNext_ && !lossOfSync) {
			settleFromNext_ = false;
			settleEnd_ = std::max(settleEnd_.value_or(0), chunkStartAfter(cycle.firstIndex, kSettleChunks));
			blendTailEnd_ =
				std::max(blendTailEnd_.value_or(0), chunkStartAfter(cycle.firstIndex, kSettleChunks + kSoftSyncChunks));
			tailBlends_ += kSoftSyncChunks;
		}
		const bool settling = lossOfSync || settleFromNext_ || (settleEnd_ && cycle.firstIndex < *settleEnd_);
		if (!settling) { // a settle's evidence ends with it
			settleLost_ = 0;
			realigned_ = false;
			settleStepped_ = false;
		}
		lastIndex_ = cycle.lastIndex;
		lastCycleRamp_ = cycle.ramp;
		lastRampCycle_ = index;
		pendingSkipped_ = 0;

		// When the first of them is heard: at the block's port, plus what lies downstream of it.
		const double outputUs = static_cast<double>(cycle.startRawNs) / 1000.0 +
		                        static_cast<double>(cycle.firstAt + cycle.delayFrames) * 1e6 / kRate;
		errors.mostDelayFrames = std::max(errors.mostDelayFrames, cycle.delayFrames);
		errors.leastDelayFrames = std::min(errors.leastDelayFrames, cycle.delayFrames);
		const double errorUs = outputUs - static_cast<double>(rig_.worker.clientTimeOf(rampUs));
		ramp_.push_back({index, errorUs, libraryStep, !cycle.ramp, cycle.innerStep});
		if (cycle.startRawNs - *firstFrameNs_ <= 3000000000) return;
		++errors.afterFirst3s;
		if (verdict != ear::test::Verdict::judge || settling) return;
		++errors.judged;
		if (!cycle.ramp) {
			// The soft sync that follows a settle, as far as its residual allows; any other blend counts against 1%.
			if (blendTailEnd_ && cycle.firstIndex < *blendTailEnd_ && tailBlends_ > 0) {
				--tailBlends_;
				++errors.settleBlends;
			} else ++errors.badCycles;
		}
		if (blendTailEnd_ && cycle.firstIndex >= *blendTailEnd_) tailBlends_ = 0;
		if (std::abs(errorUs) < 1000.0) ++errors.within;
		if (std::abs(errorUs) > std::abs(errors.worstUs)) errors.worstUs = errorUs;
		errors.sorted.insert(std::ranges::lower_bound(errors.sorted, errorUs), errorUs);
	}

	/// The library realigns itself after `cause`, having lost `lost` frames by its evidence: declared to the judge in its
	/// chunks, and open from now to kSettleChunks past the first ramp frame played after it. Inside it the library's own
	/// forward steps are excused as checkStep says, by what was lost.
	void settle(ear::test::LibrarySettle cause, std::size_t index, std::uint64_t lost) {
		if (!lastIndex_) return; // before the ramp: nothing of it to realign
		cycles_.librarySettle(cause, kSettleChunks);
		settleFromNext_ = true;
		settleLost_ += lost;
		errors.mostSettleLost = std::max(errors.mostSettleLost, lost);
		if (errors.settles.size() < 64) {
			static constexpr const char* names[] = {"dry", "unskipped", "logged"};
			errors.settles.push_back(std::string(names[static_cast<std::size_t>(cause)]) + " at " +
			                         std::to_string(index) + ": " + std::to_string(lost));
		}
	}

	/// The first frame of the chunk `chunks` past the one the server sent frame `index` in: the library's own units.
	std::uint32_t chunkStartAfter(std::uint32_t index, std::uint32_t chunks) {
		std::uint32_t at = index;
		for (std::uint32_t i = 0; i < chunks; ++i) {
			sentUs(at); // the chunk that carries it, once the server has said it
			const auto chunk = std::ranges::find_if(
				sent_, [&](const SentChunk& sent) { return at >= sent.first && at - sent.first < sent.frames; });
			at = chunk->first + chunk->frames;
		}
		return at;
	}

	/// Each step from the last ramp frame out to this cycle's first is accounted for by exactly one rule.
	void checkStep(std::size_t index, const Cycle& cycle) {
		const std::int64_t step = static_cast<std::int64_t>(cycle.firstIndex) - static_cast<std::int64_t>(*lastIndex_);
		const auto broken = [&](const char* what) {
			errors.bad.push_back(std::string(what) + " " + std::to_string(step) + " at " + std::to_string(index));
		};
		const auto note = [&](const char* rule) {
			if (errors.steps.size() < 64)
				errors.steps.push_back(std::string(rule) + ": " + std::to_string(step) + " at " +
				                       std::to_string(index));
		};
		// The library's dropped frame: frame n goes and n+1 is blended, so no ramp frame names n or n+1. Inside a cycle
		// that is only a bad cycle; when the blend falls on the cycle's edge, the indices either side of it are 3 apart.
		const bool blendOnEdge = step == 3 && !(cycle.ramp && lastCycleRamp_);
		// A skip moves the ramp on by exactly the frames the code skipped for the graph's lost time.
		const bool skip = pendingSkipped_ > 0 && step == 1 + pendingSkipped_;
		if (skip) note("skip");
		if (blendOnEdge) note("blend on the edge");
		if (step == 1 || skip || blendOnEdge) {
			plusOneRun_ =
				step == 1 && cycle.ramp && cycle.got == cycle.frames && index >= runFrom_ ? plusOneRun_ + 1 : 0;
			return;
		}
		plusOneRun_ = 0;
		// A skip that took one of the library's soft-sync frames with it (at most one a chunk, and a skip is less than a
		// chunk): an inserted frame holds no new index, so the ramp moves one less than the frames skipped; a dropped
		// one holds two, so one more.
		if (pendingSkipped_ > 0 && (step == pendingSkipped_ || step == pendingSkipped_ + 2)) {
			++errors.skipsOverBlend;
			note("skip over a soft-sync frame");
			return;
		}
		if (step <= 0 || step - 1 < pendingSkipped_) return broken("backward step");
		// Past what the code skipped: what the library dropped or caught up.
		const auto forward = static_cast<std::uint64_t>(step - 1 - pendingSkipped_);
		const bool settling = settleFromNext_ || (settleEnd_ && cycle.firstIndex < *settleEnd_);
		// A hard sync the library logged dropping frames moves the ramp on by exactly those, and the whole chunks it
		// skips unlogged as too late to play (sync_task.cpp, "This chunk will arrive too late to be played, skip it!")
		// by whole chunks more: the frame after them begins a chunk the server sent, less the prefix the last logged
		// drop took of it.
		if (takeDrops(index, cycle.firstIndex, forward, static_cast<std::uint64_t>(pendingSkipped_),
		              inLoss(index) || settling)) {
			++errors.hardSyncSteps;
			settle(ear::test::LibrarySettle::logged, index, 0);
			note("logged hard sync");
			return;
		}
		// Inside a settle the library realigns itself once by what its evidence says it lost and up to its floor's two
		// blocks more (the floor places its next chunk at now, which the block plays up to two blocks later), then may
		// drop what is left of those two blocks once more at the next chunk: each give or take one soft-sync frame (an
		// inserted one names no index, a dropped one two), in that order. A step of less than was lost, or a second
		// realignment, is no realignment the library made. It makes it by a hard sync, so a line of its own lies within
		// reach.
		constexpr std::uint64_t floorFrames = 2 * kBlock + 1;
		const bool realigns = !realigned_ && forward + 1 >= settleLost_ && forward <= settleLost_ + floorFrames;
		const bool settleStep = realigned_ && !settleStepped_ && forward <= floorFrames;
		if (settling && (realigns || settleStep)) {
			(realigns ? realigned_ : settleStepped_) = true;
			++errors.settleSteps;
			note(realigns ? "a library settle's realignment" : "a library settle's step");
			if (!loggedNear(index))
				errors.bad.push_back("a step of " + std::to_string(forward) + " inside a library settle at " +
				                     std::to_string(index) + " with nothing the library logged near it");
			return;
		}
		std::string where = " (skipped " + std::to_string(pendingSkipped_) +
		                    (settling ? ", inside a settle that lost " + std::to_string(settleLost_) : std::string()) +
		                    (realigned_ ? ", realigned" : "");
		for (const SentChunk& chunk : sent_)
			if (cycle.firstIndex >= chunk.first && cycle.firstIndex - chunk.first < chunk.frames)
				where += ", " + std::to_string(cycle.firstIndex - chunk.first) + " into a chunk of " +
				         std::to_string(chunk.frames);
		errors.bad.push_back("step " + std::to_string(step) + " at " + std::to_string(index) + where + ")");
	}

	/// What the code told the library on a cycle is that cycle: the frames counted played grow by exactly what it skipped
	/// for the graph's lost time and read since it last told, told only on a cycle that began on time (or one the bound
	/// forced), and the library's next frame plays at that cycle's end and what lies downstream, the cycle dated by when
	/// it ran, nominally when on time (docs/node-types.md#clock); a cycle that told nothing leaves the count as it was.
	/// No finish is ever behind the last one: a step back fails, naming both cycles' facts. Any other value is ear's
	/// bug, whatever the host did.
	void checkProgress(std::size_t index, const Cycle& cycle) {
		if (!cycle.published) {
			if (cycle.progress.totalFrames != told_)
				errors.bad.push_back("progress " + std::to_string(cycle.progress.totalFrames) +
				                     " told for nothing at " + std::to_string(index));
			return;
		}
		const std::uint64_t total = told_ + pendingTold_;
		pendingTold_ = 0;
		const std::int64_t heard =
			FrameClock(kRate).heardRawNs(cycle.ranRawNs, cycle.frames, Latency{cycle.delayFrames, cycle.delayFrames});
		std::int64_t finish = heard;
		if (afterForced_ && heard < lastFinish_) {
			++errors.clamps;
			finish = lastFinish_;
		}
		if (cycle.forced) ++errors.forcedReports;
		afterForced_ = cycle.forced;
		if (cycle.progress.totalFrames != total || cycle.progress.finishRawNs != finish)
			errors.bad.push_back("progress " + std::to_string(cycle.progress.totalFrames) + "/" +
			                     std::to_string(cycle.progress.finishRawNs) + " for " + std::to_string(total) + "/" +
			                     std::to_string(finish) + " at " + std::to_string(index));
		if (lastTold_ && cycle.progress.finishRawNs < lastFinish_) {
			std::ostringstream text;
			text << "a finish " << lastFinish_ - cycle.progress.finishRawNs << " ns behind the last told, at " << index
				 << " (told at " << lastTold_->first << ": start " << lastTold_->second.startRawNs << " ns, ran "
				 << lastTold_->second.ranRawNs << " ns, overdue " << lastTold_->second.overdueNs << " ns, "
				 << ear::test::describe(lastTold_->second.facts) << "; now: start " << cycle.startRawNs << " ns, ran "
				 << cycle.ranRawNs << " ns, overdue " << cycle.overdueNs << " ns, " << ear::test::describe(cycle.facts)
				 << ")";
			errors.bad.push_back(text.str());
		}
		lastFinish_ = cycle.progress.finishRawNs;
		lastTold_ = {index, cycle};
		told_ = cycle.progress.totalFrames;
	}

	/// Between two whole cycles fed whole, with nothing skipped and the same downstream, the code's report moves by one
	/// cycle: its frames, and its finish by their time give or take what the graph's clock did between the two starts
	/// (the internal clock's loop: microseconds). Measured from the reports themselves, not the cycles: a report a block
	/// off, or one that skipped a cycle, shows here as about 5.3 ms. A step names both cycles' starts, facts and rate
	/// correction, so a recurrence shows whether the engine's dating jumped.
	void checkProgressStep(std::size_t index, const Cycle& before, const Cycle& cycle) {
		if (!before.published || !cycle.published || cycle.skipped != 0 || before.fed != before.frames ||
		    cycle.fed != cycle.frames || before.delayFrames != cycle.delayFrames)
			return;
		const auto both = [&] {
			std::ostringstream text;
			text << " (cycle " << index - 1 << ": start " << before.startRawNs << " ns, "
				 << ear::test::describe(before.facts) << ", rateDiff " << before.rateDiff << "; cycle " << index
				 << ": start " << cycle.startRawNs << " ns, " << ear::test::describe(cycle.facts) << ", rateDiff "
				 << cycle.rateDiff << ")";
			return text.str();
		};
		if (cycle.progress.totalFrames != before.progress.totalFrames + before.frames)
			errors.progressSteps.push_back(std::to_string(index) + ": " +
			                               std::to_string(cycle.progress.totalFrames - before.progress.totalFrames) +
			                               " frames" + both());
		const double driftUs = static_cast<double>(cycle.progress.finishRawNs - before.progress.finishRawNs -
		                                           FrameClock(kRate).ns(before.frames)) /
		                       1000.0;
		if (std::abs(driftUs) > std::abs(errors.worstProgressDriftUs)) errors.worstProgressDriftUs = driftUs;
		if (std::abs(driftUs) >= 1000.0 && errors.progressSteps.size() < 64)
			errors.progressSteps.push_back(std::to_string(index) + ": " + std::to_string(driftUs) + " us" + both());
	}

	/// Takes logged drops (oldest first, within reach of cycle `index`) and whole late chunks that together make
	/// `frames`, the ramp then going on at frame `next`: true if some prefix of the drops leaves a whole number of chunks,
	/// and `next` lies that drop's frames into a chunk the server sent (none when it dropped a whole chunk), or up to
	/// `skipped` more, the frames the code skipped in the same step that fell after the drop. Whole chunks the library
	/// skips unlogged are taken only when `chunksMayGo`: inside a loss it logged, or after frames were lost, the only
	/// times a chunk can come too late to play.
	bool takeDrops(std::size_t index, std::uint32_t next, std::uint64_t frames, std::uint64_t skipped,
	               bool chunksMayGo) {
		std::erase_if(drops_, [&](const Logged& line) { return line.cycle + kLogReach < index; });
		const auto into = [&]() -> std::optional<std::uint64_t> {
			for (const SentChunk& chunk : sent_)
				if (next >= chunk.first && next - chunk.first < chunk.frames) return next - chunk.first;
			return std::nullopt;
		}();
		std::uint64_t sum = 0;
		for (std::size_t used = 0; used <= drops_.size(); ++used) {
			if (used > 0) {
				if (drops_[used - 1].cycle > index + kLogLag) break;
				sum += drops_[used - 1].left;
			}
			if (sum > frames) break;
			const std::uint64_t lastDrop = used > 0 ? drops_[used - 1].left : 0;
			const std::uint64_t prefix = lastDrop % kChunkFrames;
			const std::uint64_t chunks = (frames - sum) / kChunkFrames;
			if ((frames - sum) % kChunkFrames != 0 || (used == 0 && chunks == 0) || !into || *into < prefix ||
			    *into - prefix > skipped || (chunks > 0 && !chunksMayGo))
				continue;
			errors.lateChunks += chunks;
			drops_.erase(drops_.begin(), drops_.begin() + static_cast<std::ptrdiff_t>(used));
			return true;
		}
		return false;
	}

	/// Takes `frames` from the hard syncs of `kind` the library logged within reach of cycle `index`, oldest first: drops
	/// whose frames sum to `frames` exactly (`exact`), or silence that still holds `frames` more. Lines out of reach are
	/// let go. Nothing is taken unless all of it is there.
	bool takeLogged(LibraryLog::Kind kind, std::size_t index, std::uint64_t frames, bool exact) {
		std::vector<Logged>& logged = kind == LibraryLog::Kind::drop ? drops_ : silences_;
		std::erase_if(logged, [&](const Logged& line) {
			return line.cycle + kLogReach + (kind == LibraryLog::Kind::silence ? line.frames / kBlock : 0) < index;
		});
		std::uint64_t sum = 0;
		std::size_t used = 0;
		for (; used < logged.size() && logged[used].cycle <= index + kLogLag && sum < frames; ++used)
			sum += logged[used].left;
		if (exact ? sum != frames : sum < frames) return false;
		if (exact) {
			logged.erase(logged.begin(), logged.begin() + static_cast<std::ptrdiff_t>(used));
			return true;
		}
		for (std::uint64_t want = frames; want > 0;) {
			const std::uint64_t take = std::min(want, logged.front().left);
			logged.front().left -= take;
			want -= take;
			if (logged.front().left == 0) logged.erase(logged.begin());
		}
		return true;
	}

	/// Whether the library logged anything of its sync task (a hard sync, a loss or its end) within reach of cycle `index`.
	bool loggedNear(std::size_t index) const {
		return std::ranges::any_of(heard_,
		                           [&](std::size_t at) { return at <= index + kLogLag && index <= at + kLogReach; });
	}

	/// New lines of the library's log, into the drops and silences still to be matched.
	void readLog() {
		const std::vector<LibraryLog::Line> lines = log_.lines();
		for (; logRead_ < lines.size(); ++logRead_) {
			const LibraryLog::Line& line = lines[logRead_];
			heard_.push_back(line.cycle);
			const auto frames = static_cast<std::uint64_t>(std::max<std::int64_t>(line.value, 0));
			if (line.kind == LibraryLog::Kind::drop) {
				drops_.push_back({line.cycle, frames, frames});
				++errors.loggedDrops;
				errors.logged.push_back("drop " + std::to_string(frames) + " at " + std::to_string(line.cycle));
			} else if (line.kind == LibraryLog::Kind::silence) {
				silences_.push_back({line.cycle, frames, frames});
				++errors.loggedSilences;
				errors.logged.push_back("silence " + std::to_string(frames) + " at " + std::to_string(line.cycle));
			} else if (line.kind == LibraryLog::Kind::lostSync) {
				errors.lostSync.push_back(std::to_string(line.cycle) + ": " + std::to_string(line.value));
				losses_.push_back({line.cycle, std::nullopt});
			} else if (line.kind == LibraryLog::Kind::underflow) {
				errors.underflows.push_back(line.cycle);
			} else if (!losses_.empty() && !losses_.back().regained) {
				losses_.back().regained = line.cycle;
				errors.lostSync.push_back(std::to_string(line.cycle) + ": regained");
			}
		}
	}

	/// The time filter's prediction for one fixed server time, sampled as cycles are read: a step of a millisecond or
	/// more in it is the library's clock moving, which a hard sync then follows.
	void sampleFilter() {
		if (sent_.empty()) return;
		const std::int64_t now = rig_.worker.clientTimeOf(sent_.front().us);
		if (lastFilterUs_ && std::abs(now - *lastFilterUs_) >= 1000)
			errors.filterSteps.push_back(std::to_string(next_) + ": " + std::to_string(now - *lastFilterUs_));
		lastFilterUs_ = now;
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

	struct Logged {
		std::size_t cycle;    // when its line was read
		std::uint64_t frames; // what it said
		std::uint64_t left;   // its frames not yet matched
	};

	Rig& rig_;
	const LibraryLog& log_;
	std::size_t logRead_ = 0;
	std::vector<Logged> drops_;
	std::vector<Logged> silences_;
	struct Loss {
		std::size_t lost;                    // when the library said it lost sync, by the tap's count
		std::optional<std::size_t> regained; // when it said it regained it
	};
	std::vector<Loss> losses_;
	/// Whether cycle `index` lies in a loss of sync the library logged: from its line, less the reading's lag, to the
	/// line that ends it, and the lag after. Interim, as kLogLag is (docs/testing.md#the-librarys-own-word).
	bool inLoss(std::size_t index) const {
		return std::ranges::any_of(losses_, [&](const Loss& loss) {
			return index + kLogLag >= loss.lost && (!loss.regained || index <= *loss.regained + kLogLag);
		});
	}
	std::vector<SentChunk> sent_;
	ear::test::CycleJudge cycles_; // the one judge of every cycle the block ran
	std::size_t lastStallAt_ = 0;  // the cycle read when the last planned stall was armed
	std::size_t next_ = 0;
	std::optional<Cycle> previous_;
	std::optional<std::uint32_t> lastIndex_;
	bool lastCycleRamp_ = true;
	std::size_t lastRampCycle_ = 0;
	std::optional<std::int64_t> firstFrameNs_;
	std::optional<std::int64_t> lastFilterUs_;
	std::size_t plusOneRun_ = 0;
	std::size_t runFrom_ = 0;
	std::int64_t pendingSkipped_ = 0;        // skipped since the last ramp frame out
	bool settleFromNext_ = false;            // a library settle opened, its end to be placed at the next ramp frame out
	std::optional<std::uint32_t> settleEnd_; // the ramp index from which the library is back on the stamps
	std::uint64_t settleLost_ = 0;           // what the open settles' evidence says the library lost
	bool realigned_ = false;                 // the open settle's realignment came
	bool settleStepped_ = false;             // and its step after
	std::optional<std::uint32_t> blendTailEnd_; // the ramp index to which the settles' soft sync may still blend
	std::size_t tailBlends_ = 0;                // the blends it may still make
	bool wasInLoss_ = false;                    // the last cycle lay in a loss of sync the library logged
	std::uint64_t told_ = 0;                    // the frames the code last told the library were played
	std::uint64_t pendingTold_ = 0;             // skipped and read since, not told yet (overdue cycles tell nothing)
	std::optional<std::int64_t> untoldSince_;   // the start of the first cycle not yet told
	std::int64_t lastFinish_ = 0;               // the finish the code last told
	bool afterForced_ = false;                  // the last report was one the bound forced
	std::optional<std::pair<std::size_t, Cycle>> lastTold_; // the cycle that told it, and its index
	std::vector<std::size_t> dry_;
	std::vector<std::size_t> disturbed_;  // cycles the judge did not judge whole
	std::vector<std::size_t> unabsorbed_; // of them, all but a catch-up the feed held whole
	std::vector<std::size_t> heard_;      // the cycle each line of the library's log was read at
	std::vector<RampSeen> ramp_;
};

/// Judges cycles as they come until the planned count is judged and the cycle judge is done (its planned stalls made
/// and excused): however often the host disturbs the clock, the stream plays on until then. The deadline only fails
/// the test.
void judgePlanned(Rig& rig, Judge& judge) {
	const bool judged = rig.waiter.until(
		[&] {
			judge.read();
			return (judge.errors.judged >= kPlannedCycles && judge.graphDone()) || rig.notes.heard("stream-ended");
		},
		150s);
	INFO(describe(judge.errors));
	REQUIRE(judged);
	REQUIRE(judge.errors.judged >= kPlannedCycles);
	REQUIRE(judge.graphDone());
}

void checkSync(const Errors& errors) {
	INFO(describe(errors));
	// Every cycle of the block was judged whole, excused alone as a catch-up the driver cut, or a cause the test made.
	CHECK(errors.graph.failed == 0);
	CHECK(static_cast<double>(errors.within) >= 0.99 * static_cast<double>(errors.judged));
	CHECK(errors.bad.empty());
	CHECK(errors.unexplainedSilence.empty());
	// The library was never told more played than it wrote.
	CHECK(errors.underflows.empty());
	// Every report came from a cycle that began on time: none was forced by the bound, so the clamp never acted.
	CHECK(errors.forcedReports == 0);
	CHECK(errors.clamps == 0);
	// What the code told the library moved by one cycle between whole cycles fed whole: a report a block off is ear's.
	CHECK(errors.progressSteps.empty());
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
	Judge judge(rig, rig.log, Judge::inducedStalls());
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
	Judge judge(rig, rig.log, Judge::inducedStalls());
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
	// The card's loss is the event this test judges, so it makes no stall of its own: the steady tests prove the judge's
	// excuse path through the same rig and judge.
	Judge judge(rig, rig.log, ear::test::StallPlan{});
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
	judge.restartRun(cut);

	// Losing the card cuts the audio path but leaves its latency (PipeWire never tells the block that the last link went,
	// so the downstream latency the node's frames are dated by stays, stale). Any correction the library makes comes from
	// the cut, a frame per chunk: a chunk is about five blocks, so three in a row hold it. The 3 s are the graph's,
	// counted in the cycles from the cut to the run, less those the ledger says the host disturbed (each its own cycle,
	// no more): a host that keeps disturbing the clock only makes the wait longer. A cycle the starved worker left dry is
	// counted like any other, and reported.
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.plusOneRun() >= 3;
		},
		60s));
	const std::size_t reached = *judge.runReachedAt(3);
	const std::size_t disturbed = judge.disturbedBetween(cut, reached);
	INFO(describe(judge.errors));
	INFO("the run of three came at cycle " << reached << ", the cut at " << cut << ", the host disturbed " << disturbed
	                                       << " cycles between, dry cycles between " << judge.dryBetween(cut, reached));
	CHECK(reached - cut - disturbed <= 3 * kRate / kBlock);
	// Every step before and after the cut was one a rule accounts for.
	CHECK(judge.errors.bad.empty());
	// The internal clock drove all along: neither the card's links nor its loss moved the clock.
	CHECK(judge.errors.driverChanges == 0);
}

/// Where what the library does after a planned event (a stall, a network gap) stops being judged as its answer to it,
/// from `back`, the cycle the stream came back on the server's stamps, up to `cap`: the first cycle after `back` that
/// the host disturbed in a way the library could see (the ledger did not call it clean, and it was no catch-up the feed
/// held whole: that one is skipped from audio the library already wrote and told exactly), after which the library may answer the host in its own steps
/// and silence, which the run's rules judge (checkSync) and the event did not cause. A host that skipped 117 ms 1600
/// cycles after a stall made the library add 2269 frames of silence 107 cycles later. A dry cycle ends nothing: what
/// the feed holds is the product's doing. The window never ends before its floor: the library answers what it was told
/// at its next chunk's sync, at most a chunk's frames after the return, and what it changed reaches the outputs within
/// two cycles more (one for its write to reach the feed, one for the cycle that reads it). A host that disturbs a cycle
/// inside the floor leaves the two answers inseparable, so the run fails naming it, to be rerun alone
/// (docs/testing.md); a window shortened past the floor says the host event that ended it.
struct Reach {
	std::size_t until;
	std::string note;
};
static Reach responseReach(const Judge& judge, const Rig& rig, std::size_t back, std::size_t cap) {
	const std::size_t floor = back + (kChunkFrames + kBlock - 1) / kBlock + 2;
	const std::optional<std::size_t> host = judge.firstHostDisturbanceFrom(back + 1);
	if (!host || *host >= cap) return {cap, "the library's answer judged to cycle " + std::to_string(cap)};
	const CycleFacts& facts = rig.tap.at(*host).facts;
	const std::string event = "cycle " + std::to_string(*host) + " (causes " + std::to_string(facts.causes.bits()) +
	                          ", jump " + std::to_string(facts.jumpFrames) + " frames, missed " +
	                          std::to_string(facts.missed) + ", " + std::to_string(facts.missedCut) + " cut)";
	if (*host < floor)
		FAIL("the host disturbed " << event << " inside the response window's floor (to cycle " << floor
		                           << "): the library's answer to it cannot be told from its answer to the planned "
		                              "event; rerun alone (docs/testing.md)");
	return {*host, "the library's answer judged to cycle " + std::to_string(*host) + ", not " + std::to_string(cap) +
	                   ": the host disturbed " + event};
}

/// How a stall test stalls the data thread: by a StallProbe upstream of the block, which cuts the burst's cycles too, so
/// the block misses them all on any host; or inside the block's own process, so the burst takes whatever shape the
/// host gives it (docs/engine.md#the-internal-clock): the block missing some of its cycles (each a catch-up), or running
/// every one, losing nothing, while the next is already due (overdueNs).
enum class StallMade { byProbe, inBlock };

/// How far the library's one correction after a stall may be from what its stream fell behind by, in frames: what the
/// clock could not skip, and the zeros ear played meanwhile (time that passed while the stream did not move). Its hard
/// sync leaves it within its settle threshold (sync_task.cpp's HARD_SYNC_SETTLE_THRESHOLD_US, 500 us), so its step
/// may differ from that by as much.
constexpr std::uint64_t kCorrectionTolerance = 500 * kRate / 1000000;

// docs/node-types.md#clock: the clock plays every frame in order; on the cycle that states a catch-up it skips the jump
// as far as the feed holds it, and the library is told only from a cycle that began on time, every report true when
// told. So after a stall the library corrects exactly once what the feed did not hold (the stall's lost time less the
// skip) and the zeros ear played meanwhile, and ear's own silence after the burst is at most a block (the library
// refilling the feed a skip emptied); a stall the feed holds is skipped exactly, with no correction; a burst that
// loses nothing (the block ran every cycle, overdue) needs no correction and plays no silence, and tells nothing until
// its end. Judged from the tap's ramp against the server's stamps through the time filter: back within the feed, a
// chunk and two blocks of the burst's end, the library never told more than it wrote, every report exact, never
// behind the last, then 750 cycles within 1 ms. The test appends the burst's shape it judged to a file beside ctest's
// records (EAR_STALL_SHAPES_FILE), which CI prints, so a passing run shows which shapes its host gave.
static void judgeStall(StallKind kind, StallMade made) {
	Rig rig;
	std::optional<StallProbe> probe;
	if (made == StallMade::byProbe) probe.emplace(rig);
	rig.play(kStreamSeconds);
	// The stall is the test's own, and what it judges: no planned stall besides.
	Judge judge(rig, rig.log, ear::test::StallPlan{});
	INFO("the node said:\n" << rig.notes.all());
	// Steady first: a hundred cycles judged within the run's rules.
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.errors.judged >= 100;
		},
		60s));
	if (probe) {
		rig.tap.watchStall(probe->code.stalledIn);
		probe->code.arm(kind);
	} else rig.tap.stallOwn(kind == StallKind::longStall);
	REQUIRE(rig.waiter.until([&] { return rig.tap.stalledAt().has_value(); }, 10s));
	const std::size_t stalled = *rig.tap.stalledAt();
	const std::size_t judgedBefore = judge.errors.judged;
	// The 750 after it, each judged by the run's rules (whole, outside any library settle).
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.errors.judged >= judgedBefore + kPlannedCycles || rig.notes.heard("stream-ended");
		},
		150s));
	INFO(describe(judge.errors));
	REQUIRE(judge.errors.judged >= judgedBefore + kPlannedCycles);

	// The burst: the driver catches the stalled time up in a burst of timeouts (docs/engine.md#the-internal-clock), each
	// cycle the block runs in it a catch-up or overdue; it ends at the first cycle that lost nothing and ran on time.
	std::size_t end = stalled + 1;
	std::uint64_t lostFrames = 0;
	std::uint64_t skipped = 0;
	std::size_t overdue = 0;
	std::size_t toldInBurst = 0;
	std::uint64_t dryInBurst = 0;
	std::string burst;
	for (; end < rig.tap.count() && (!rig.tap.at(end).facts.whole() || rig.tap.at(end).overdueNs > 0); ++end) {
		const Cycle& c = rig.tap.at(end);
		burst += " " + std::to_string(c.facts.causes.bits()) + ":" + std::to_string(c.facts.jumpFrames) + ":" +
		         std::to_string(c.overdueNs / 1000);
		if (c.facts.causes.has(CycleCause::catchUp) || c.facts.causes.has(CycleCause::absent))
			lostFrames += static_cast<std::uint64_t>(std::max<std::int64_t>(c.facts.jumpFrames, 0));
		skipped += c.codeSkipped;
		dryInBurst += c.frames - c.fed;
		if (c.overdueNs > 0) ++overdue;
		if (c.progress.totalFrames != rig.tap.at(end - 1).progress.totalFrames) ++toldInBurst;
	}
	REQUIRE(end < rig.tap.count());
	const bool cut = lostFrames > 0;
	const std::uint64_t unskipped = lostFrames - std::min(lostFrames, skipped);
	const char* const what = kind == StallKind::hiccup      ? "a 1.5-cycle hiccup"
	                         : kind == StallKind::longStall ? "a 1 s stall"
	                                                        : "a 200 ms stall";
	const std::string shape = std::string(what) + (probe ? " upstream" : " in the block") + ": the burst's shape " +
	                          (cut ? "cut (the block missed some of its cycles)"
	                               : "uncut (the block ran every cycle, overdue, nothing lost)") +
	                          ", " + std::to_string(lostFrames) + " frames lost, " + std::to_string(skipped) +
	                          " skipped, " + std::to_string(unskipped) + " left to the library, " +
	                          std::to_string(end - stalled - 1) + " cycles in the burst, " + std::to_string(overdue) +
	                          " overdue";
	INFO(shape);
	INFO("stalled in cycle " << stalled << "; the burst after it (causes:jump:overdue us):" << burst
	                         << (probe ? "; the probe held " + std::to_string(probe->code.burstHolds()) +
	                                         " of the burst's cycles"
	                                   : std::string()));
	REQUIRE(end > stalled + 1);
	if (probe && kind != StallKind::hiccup) REQUIRE(lostFrames > static_cast<std::uint64_t>(kFeedFrames));
	// The hiccup's one lost cycle is in the feed, which holds 2048 frames: the clock skips all of it.
	if (kind == StallKind::hiccup) REQUIRE(cut);

	// The first report after the stall comes from a cycle that began on time, dated by its own start.
	const Cycle& caughtUp = rig.tap.at(end);
	std::size_t firstTold = stalled + 1;
	while (firstTold < rig.tap.count() &&
	       rig.tap.at(firstTold).progress.totalFrames == rig.tap.at(firstTold - 1).progress.totalFrames)
		++firstTold;
	REQUIRE(firstTold < rig.tap.count());
	const Cycle& firstReport = rig.tap.at(firstTold);
	INFO("the first report after the stall at cycle " << firstTold << ", overdue " << firstReport.overdueNs << " ns");
	CHECK(firstReport.overdueNs == 0);
	CHECK(firstReport.progress.finishRawNs ==
	      FrameClock(kRate).heardRawNs(firstReport.startRawNs, firstReport.frames,
	                                   Latency{firstReport.delayFrames, firstReport.delayFrames}));

	// Back on the server's stamps: the first ramp cycle from the burst's end on whose first frame left within the band
	// the library leaves hard sync at (sync_task.cpp's settle threshold, 500 us), inside which only its soft sync acts.
	// The bound, in blocks from the burst's end: the library answers what it was told at the next chunk it syncs, which
	// follows what it has already written, the feed's frames and the rest of the chunk it is writing (a 200 ms stall
	// upstream came back 26 blocks after the burst once), and two cycles more bring the frame it changed to the
	// outputs.
	constexpr double kBandUs = 500.0;
	const std::uint64_t boundBlocks = (kFeedFrames + kChunkFrames + kBlock - 1) / kBlock + 2;
	const auto& ramp = judge.ramp();
	const auto back = std::ranges::find_if(
		ramp, [&](const Judge::RampSeen& seen) { return seen.index >= end && std::abs(seen.errorUs) <= kBandUs; });
	REQUIRE(back != ramp.end());
	const Reach reach = responseReach(judge, rig, back->index, end + kPlannedCycles);
	INFO(reach.note);
	// A correction is a step of the library's own at a cycle's edge or inside one; its soft sync's blends are not (a
	// step of at most 3, at most once a chunk, as checkStep counts them).
	std::size_t corrections = 0;
	std::uint64_t corrected = 0;
	std::vector<std::uint64_t> sizes; // each correction's frames, in order
	std::size_t blends = 0;
	std::string steps;
	std::string correctionCycles;
	for (const Judge::RampSeen& seen : ramp) {
		if (seen.index <= stalled || seen.index >= reach.until) continue;
		// A soft-sync frame on the edge: a drop moves it by 2, and under a skip a drop by 1 and an insert by -1
		// (checkStep's "skip over a soft-sync frame").
		const bool edgeBlend = seen.libraryStep == 2 || seen.libraryStep == 1 || seen.libraryStep == -1;
		const bool innerBlend = seen.broken && seen.innerStep <= 3;
		if (seen.libraryStep == 0 && (!seen.broken || innerBlend)) {
			blends += seen.broken ? 1 : 0;
			continue;
		}
		if (edgeBlend && (!seen.broken || innerBlend)) {
			++blends;
			continue;
		}
		const std::int64_t edge = seen.libraryStep == 0 || edgeBlend ? 0 : seen.libraryStep;
		const std::int64_t inner = seen.innerStep > 3 ? seen.innerStep - 1 : 0;
		sizes.push_back(static_cast<std::uint64_t>(std::abs(edge) + inner));
		corrected += sizes.back();
		steps += " " + std::to_string(seen.libraryStep) + "/" + std::to_string(seen.innerStep) + "@" +
		         std::to_string(seen.index) + " (" + std::to_string(seen.errorUs) + " us)";
		// And each cycle of the chunk and two cycles before it, so a correction says what led to it: the cycle's causes,
		// its jump, how long the next was overdue, what the feed held as it began, what the code skipped and read, and
		// whether it told the library.
		const std::size_t reachBack = (kChunkFrames + kBlock - 1) / kBlock + 2;
		correctionCycles += "\n  before the correction at " + std::to_string(seen.index) + ":";
		for (std::size_t i = seen.index > reachBack ? seen.index - reachBack : 1; i <= seen.index; ++i) {
			const Cycle& c = rig.tap.at(i);
			const bool told = c.progress.totalFrames != rig.tap.at(i - 1).progress.totalFrames;
			correctionCycles += "\n    " + std::to_string(i) + " causes " + std::to_string(c.facts.causes.bits()) +
			                    " jump " + std::to_string(c.facts.jumpFrames) + " overdue " +
			                    std::to_string(c.overdueNs / 1000) + " us held " + std::to_string(c.heldAtStart) +
			                    " skipped " + std::to_string(c.codeSkipped) + " read " + std::to_string(c.fed) +
			                    (told ? " told" : "");
		}
		++corrections;
	}
	INFO("the cycles before each correction (index, causes, jump, overdue, held at start, skipped, read, told):"
	     << correctionCycles);
	std::string library;
	std::size_t silenceAfter = 0;
	for (const LibraryLog::Line& line : rig.log.lines()) {
		if (line.cycle <= stalled) continue;
		library += " " + std::to_string(static_cast<int>(line.kind)) + ":" + std::to_string(line.value) + "@" +
		           std::to_string(line.cycle);
		if (line.kind == LibraryLog::Kind::silence && line.cycle < reach.until) ++silenceAfter;
	}
	// Ear's own silence after the burst: the zeros its cycles played for frames the feed lacked, to the return.
	std::uint64_t silenceFrames = 0;
	std::string after;
	for (std::size_t i = end; i < back->index && i < rig.tap.count(); ++i) {
		const Cycle& c = rig.tap.at(i);
		after += " " + std::to_string(c.heldAtStart) + "/" + std::to_string(c.fed);
		silenceFrames += c.frames - c.fed;
	}
	// Ear's silence after the stall in all: the zeros of the burst's dry cycles (a catch-up that skipped all the feed
	// held reads nothing) and after it.
	const std::uint64_t zeros = dryInBurst + silenceFrames;
	const std::uint64_t blocksToBack = (rig.tap.at(back->index).position - caughtUp.position) / kBlock;
	INFO("from the burst's end to the return (held at start/read):"
	     << after << "; ear's silence " << silenceFrames << " frames after the burst, " << dryInBurst << " in it");
	INFO("back at cycle " << back->index << ", " << blocksToBack << " blocks after the burst's end at " << end
	                      << " (bound " << boundBlocks << "); the library's corrections after the stall:" << steps
	                      << " (" << corrected << " frames in all, " << unskipped
	                      << " left to it); its lines after the stall (kind:value@cycle):" << library);
	// The shape judged, pass or fail, appended to a file beside ctest's records, which CI prints: a passing test's
	// output reaches no log, and a node's tests write no standard output (ear_nodes_never_write_standard_output).
	std::ostringstream judged;
	judged << "sendspin stall judged: " << shape << "; the library corrected " << corrections << " times, " << corrected
		   << " frames; ear's silence after the stall " << zeros << " frames; back " << blocksToBack
		   << " blocks after it\n";
	INFO(judged.str());
	std::ofstream(EAR_STALL_SHAPES_FILE, std::ios::app) << judged.str();
	if (unskipped > 0) {
		// What the clock could not skip, the library corrects once, with the zeros ear played meanwhile: the stream fell
		// behind by both. After that hard sync its next write can land up to two blocks late, which the next chunk's
		// sync drops: one settle step, as after a network gap (docs/testing.md#the-cycle-judge), never more. Seen under
		// load: a block, when the burst's end played a block of zeros the correction did not yet count, and two blocks
		// with none.
		REQUIRE(corrections >= 1);
		CHECK(corrections <= 2);
		const std::uint64_t first = sizes.front();
		CHECK(first + kCorrectionTolerance >= unskipped + dryInBurst);
		CHECK(first <= unskipped + zeros + kCorrectionTolerance);
		if (corrections == 2) CHECK(sizes[1] <= 2 * kBlock + kCorrectionTolerance);
	} else CHECK(corrections == 0);
	// Ear's own silence after the burst: at most the block the library takes to refill the feed a skip emptied.
	if (cut) CHECK(silenceFrames <= kBlock);
	else {
		// Nothing lost: no correction (above), no silence, and nothing told until the burst had ended.
		CHECK(silenceFrames == 0);
		CHECK(dryInBurst == 0);
		CHECK(toldInBurst == 0);
	}
	CHECK(blocksToBack <= boundBlocks);
	CHECK(blends <= kPlannedCycles * kBlock / kChunkFrames + 1);
	CHECK(silenceAfter == 0);
	// The library was never told more than it wrote, every report was exact, came from an on-time cycle and was never
	// behind the last, and 750 cycles after the stall are within 1 ms.
	checkSync(judge.errors);
}

TEST_CASE("A 200 ms stall upstream: the clock skips what the feed held, and the library corrects the rest once",
          "[sendspin][clock]") {
	judgeStall(StallKind::shortStall, StallMade::byProbe);
}

TEST_CASE("A 1 s stall upstream: the clock skips what the feed held, and the library corrects the rest once",
          "[sendspin][clock]") {
	judgeStall(StallKind::longStall, StallMade::byProbe);
}

// The same stalls made inside the block, so the burst takes the host's shape: on the dev host the block misses every
// other of its cycles, on a GitHub runner it ran every one, losing nothing (docs/engine.md#the-internal-clock).
TEST_CASE("A 200 ms stall in the block, whatever the burst's shape: told only once the graph runs on time, one "
          "correction of what was lost and not skipped, none when nothing was lost",
          "[sendspin][clock]") {
	judgeStall(StallKind::shortStall, StallMade::inBlock);
}

TEST_CASE("A 1 s stall in the block, whatever the burst's shape: told only once the graph runs on time, one "
          "correction of what was lost and not skipped, none when nothing was lost",
          "[sendspin][clock]") {
	judgeStall(StallKind::longStall, StallMade::inBlock);
}

// docs/node-types.md#clock: a cycle the host missed, upstream of the node, whose frames the feed holds: the clock skips
// exactly the jump, the ramp steps by exactly that, the report moves by the jump and the cycle's read, and the library
// corrects nothing.
TEST_CASE("A 1.5-cycle hiccup upstream is skipped exactly, with no correction of the library's", "[sendspin][clock]") {
	judgeStall(StallKind::hiccup, StallMade::byProbe);
}

// docs/node-types.md#library-facts-and-carried-patches: a network that stalls past what the library holds starves it;
// it floors its playtime at now (its underrun recovery, sendspin-starved-floor.patch), and the chunks that then arrive
// at once are late: the earliest wholly, one at its front only. That one is decoded and its late front dropped
// (sendspin-late-chunk.patch), so the lost time is corrected in one step with no silence added, where skipping it
// whole overshoots by the rest of it and fills that with silence. The test server holds the audio
// it sends until the library has starved for about 300 ms and places the release inside a chunk, a fixed way past its
// start (sendspin_server.py's `gap`), so which chunk is late at its front is certain, not likely.
TEST_CASE("A starving network gap: one correction of the lost time, no silence, the late chunk's front dropped",
          "[sendspin][clock]") {
	Rig rig;
	rig.play(kStreamSeconds);
	// The gap is the test's own, and what it judges: no planned stall besides.
	Judge judge(rig, rig.log, ear::test::StallPlan{});
	INFO("the node said:\n" << rig.notes.all());
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.errors.judged >= 100;
		},
		60s));
	const std::size_t heldFrom = rig.tap.count();
	rig.server.send("gap 300");
	const std::optional<std::string> released = rig.server.waitForEvent("gap-released ", 30s);
	REQUIRE(released.has_value());
	INFO("the server said: " << *released);
	const std::size_t judgedBefore = judge.errors.judged;
	REQUIRE(rig.waiter.until(
		[&] {
			judge.read();
			return judge.errors.judged >= judgedBefore + kPlannedCycles || rig.notes.heard("stream-ended");
		},
		150s));
	INFO(describe(judge.errors));
	REQUIRE(judge.errors.judged >= judgedBefore + kPlannedCycles);

	// The library starved: the feed ran dry from some cycle after the hold, and refilled at the release.
	std::size_t starvedFrom = heldFrom;
	while (starvedFrom < rig.tap.count() && rig.tap.at(starvedFrom).fed == rig.tap.at(starvedFrom).frames)
		++starvedFrom;
	std::size_t recovered = starvedFrom + 1;
	std::uint64_t starvedFrames = rig.tap.at(starvedFrom).frames - rig.tap.at(starvedFrom).fed;
	for (; recovered < rig.tap.count() && rig.tap.at(recovered).fed == 0; ++recovered)
		starvedFrames += rig.tap.at(recovered).frames;
	INFO("starved from cycle " << starvedFrom << " to " << recovered << ", " << starvedFrames << " frames");
	REQUIRE(recovered < rig.tap.count());
	REQUIRE(starvedFrames >= kRate / 5);

	// The recovery: the library's one correction of the lost time, a forward step at the first cycle fed again (the
	// late chunk's front dropped, and any wholly late chunk before it skipped). Its floor puts the next frame at now,
	// but the block plays it from the first cycle to start after the library has written it: late by up to a block, and
	// by up to another when its decode and write outlast the cycle in which it floored (5.47 ms seen). The next chunk's
	// sync (one chunk on, its settle threshold 500 us) may drop that once more, a settle step of under two blocks, and no
	// other. Then it is back on the server's stamps, within the band that settle leaves.
	constexpr double kBandUs = 500.0;
	const double blockUs = 1e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
	const auto& ramp = judge.ramp();
	const auto back = std::ranges::find_if(ramp, [&](const Judge::RampSeen& seen) {
		return seen.index >= recovered && std::abs(seen.errorUs) <= kBandUs;
	});
	const std::uint64_t boundBlocks = (kChunkFrames + kBlock - 1) / kBlock + 2;
	// What the library does about the gap is judged up to the first cycle after the stream's return that the host
	// disturbed, and at most kPlannedCycles past the recovery (responseReach says why). A drop is counted wherever it is
	// logged: the check is that one came.
	REQUIRE(back != ramp.end());
	const Reach reach = responseReach(judge, rig, back->index, recovered + kPlannedCycles + 1);
	INFO(reach.note);
	const std::size_t gapReach = reach.until;
	std::size_t lostTimeSteps = 0;
	std::size_t settles = 0;
	std::size_t others = 0;
	double worstBeforeBackUs = 0.0;
	std::string steps;
	for (const Judge::RampSeen& seen : ramp) {
		if (seen.index <= heldFrom || seen.index >= gapReach) continue;
		if (seen.index >= recovered && (back == ramp.end() || seen.index < back->index) &&
		    std::abs(seen.errorUs) > std::abs(worstBeforeBackUs))
			worstBeforeBackUs = seen.errorUs;
		const bool edgeBlend = seen.libraryStep == 2;
		const bool innerBlend = seen.broken && seen.innerStep <= 3;
		if ((seen.libraryStep == 0 || edgeBlend) && (!seen.broken || innerBlend)) continue;
		steps += " " + std::to_string(seen.libraryStep) + "/" + std::to_string(seen.innerStep) + "@" +
		         std::to_string(seen.index) + " (" + std::to_string(seen.errorUs) + " us)";
		const std::int64_t step = std::max(std::abs(seen.libraryStep), seen.innerStep);
		if (seen.index == recovered && lostTimeSteps == 0) ++lostTimeSteps;
		else if (lostTimeSteps == 1 && settles == 0 && step <= 2 * static_cast<std::int64_t>(kBlock)) ++settles;
		else ++others;
	}
	std::string library;
	std::size_t silenceAfter = 0;
	std::size_t drops = 0;
	for (const LibraryLog::Line& line : rig.log.lines()) {
		if (line.cycle <= heldFrom) continue;
		library += " " + std::to_string(static_cast<int>(line.kind)) + ":" + std::to_string(line.value) + "@" +
		           std::to_string(line.cycle);
		if (line.kind == LibraryLog::Kind::silence && line.cycle < gapReach) ++silenceAfter;
		if (line.kind == LibraryLog::Kind::drop) ++drops;
	}
	const std::uint64_t blocksToBack =
		back != ramp.end() ? (rig.tap.at(back->index).position - rig.tap.at(recovered).position) / kBlock : 0;
	INFO("back at cycle " << (back != ramp.end() ? back->index : 0) << ", " << blocksToBack
	                      << " blocks after the recovery (bound " << boundBlocks << "), off by at most "
	                      << worstBeforeBackUs << " us before; the library's corrections after the hold:" << steps
	                      << "; its lines after the hold (kind:value@cycle):" << library);
	REQUIRE(back != ramp.end());
	CHECK(drops >= 1);
	CHECK(lostTimeSteps == 1);
	CHECK(settles <= 1);
	CHECK(others == 0);
	CHECK(silenceAfter == 0);
	CHECK(worstBeforeBackUs >= -kBandUs);
	CHECK(worstBeforeBackUs <= 2 * blockUs + kBandUs);
	CHECK(blocksToBack <= boundBlocks);
	// Every report exact, and the 750 cycles after the gap within 1 ms.
	checkSync(judge.errors);
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
