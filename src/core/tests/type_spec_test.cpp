// The type SPI (docs/writing-a-node-type.md#the-class; docs/writing-a-node-type.md): a spec, a class with configure and
// its hooks, makeNodeType, Io by bank and the harness that runs a type over a real NodeHost without PipeWire. What must
// not compile is compile_fail/type_spec_bad.cpp's.
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace type_spec_test {

using namespace ear;

std::optional<std::string> anyText(std::string_view text) { return std::string(text); }

constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 8), 2);
constexpr auto modeRow = setting::optional("mode", setting::text<anyText>("anything"));
constexpr SettingsTable<channelsRow, modeRow> table{"probe"};
constexpr auto in = bank::input<"in">;
constexpr auto out = bank::output<"out">;
constexpr auto toDevice = bank::insideOutput<"to_device">;
constexpr auto fromDevice = bank::insideInput<"from_device">;
constexpr TypeSpec spec{.settings = table, .banks = Banks<in, out, toDevice, fromDevice>{}};
using Checked = CheckedOf<spec>;

/// out = 2 * in, to_device = in; counts its prepare and release.
struct Doubler {
	int* prepared;
	void prepare(const AudioFormat&) { ++*prepared; }
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (std::size_t member = 0; member < io[in].size(); ++member) {
			for (std::uint32_t frame = 0; frame < io.frames(); ++frame) {
				io[out][member][frame] = 2.0F * io[in][member][frame];
				io[toDevice][member][frame] = io[in][member][frame];
			}
		}
	}
};

int prepares = 0;
/// The upstreamFrames of every chunk the sink's consumer took, in order.
std::vector<std::uint32_t> upstreams;

/// Keeps each chunk's upstreamFrames.
struct Upstreams final : SinkConsumer {
	void consume(const Chunk& chunk) override { upstreams.push_back(chunk.upstreamFrames); }
	void finish() override {}
};
/// What every Probe instance did, in order: "made <node>", "configure", "start", "stop", "destroyed".
std::vector<std::string> calls;

/// What the "clocked" mode's source clock heard, in order: each tick's jump and playback latency, what it was told, and
/// whether it was freed.
struct ClockHeard {
	std::vector<std::int64_t> jumps;
	std::vector<Latency> playbacks;
	std::vector<SourcePlayed> played;
	bool freed = false;
};
ClockHeard clockHeard;

/// A source's clock that skips a catch-up's whole jump and asks after every tick that read frames.
struct JumpClock final : SourceClock {
	~JumpClock() override { clockHeard.freed = true; }
	std::uint64_t skipBefore(const SourceTick& tick, SourceHeld) noexcept override {
		clockHeard.jumps.push_back(tick.cycle.facts.jumpFrames);
		clockHeard.playbacks.push_back(tick.playback);
		return tick.cycle.facts.causes.has(CycleCause::catchUp)
		           ? static_cast<std::uint64_t>(tick.cycle.facts.jumpFrames)
		           : 0;
	}
	bool played(const SourceTick&, const SourcePlayed& played) noexcept override {
		clockHeard.played.push_back(played);
		return played.read > 0;
	}
};

class Probe {
public:
	explicit Probe(NodeOf<spec>& node) : node_(node) { calls.push_back("made " + node.node()); }
	~Probe() { calls.emplace_back("destroyed"); }
	Probe(const Probe&) = delete;
	Probe& operator=(const Probe&) = delete;
	static Members<spec> ports(const Checked& settings) {
		const std::uint32_t n = settings.get<channelsRow>();
		return {in(members(n)), out(members(n)), toDevice(members(n)), fromDevice(members(n))};
	}
	static std::expected<void, std::string> check(const Checked& settings) {
		if (settings.get<modeRow>() == "bad") return std::unexpected(std::string("mode: not bad"));
		return {};
	}
	Configured configure(CreateOf<spec>& cx, const Checked& settings) {
		calls.emplace_back("configure");
		const auto mode = settings.get<modeRow>();
		if (mode == "card") {
			cx.soundCard(toDevice, SoundCardShape{.card = *CardId::parse("Probe")});
			cx.use(passThrough({bankPair(in, toDevice), bankPair(fromDevice, out)}));
			return Configured::applied();
		}
		if (mode == "sink") {
			cx.sink(toDevice, SinkShape{.frames = 1024}).consumedBy(std::make_unique<Upstreams>());
			cx.use(passThrough({bankPair(in, toDevice), bankPair(fromDevice, out)}));
			cx.declareLatency(in, toDevice, Latency{30, 40});
			return Configured::applied();
		}
		if (mode == "source") {
			cx.source(fromDevice, SourceShape{.frames = 1000});
			cx.use(passThrough({bankPair(in, toDevice), bankPair(fromDevice, out)}));
			return Configured::applied();
		}
		if (mode == "clocked") {
			cx.source(fromDevice, SourceShape{.frames = 1000}, std::make_unique<JumpClock>());
			cx.use(passThrough({bankPair(in, toDevice), bankPair(fromDevice, out)}));
			return Configured::applied();
		}
		if (mode == "fail") return Configured::failed("asked to");
		if (mode == "say") node_.handle().say("configured at " + std::to_string(cx.format().rate));
		cx.use(Doubler{&prepares});
		return Configured::applied();
	}
	void start() { calls.emplace_back("start"); }
	void stop() { calls.emplace_back("stop"); }

private:
	NodeOf<spec>& node_;
};

using Harness = testing::TypeHarness<spec, Probe>;

// Another type's spec. Its bank "out" has this spec's name, direction and reach, so it is the same handle: Io takes it
// and resolves it by name against the node's own ports (sound, and accepted). A bank this spec lacks does not compile
// (compile_fail/type_spec_bad.cpp, io-bank-the-spec-lacks).
constexpr auto otherOut = bank::output<"out">;
constexpr TypeSpec otherSpec{.settings = SettingsTable<>{"other"}, .banks = Banks<otherOut>{}};
static_assert(std::same_as<decltype(otherOut), decltype(out)>);
static_assert(requires(Io<spec> io) { io[otherOut]; });

/// A code written against the other spec.
struct OtherCode {
	void process(Io<otherSpec>, const GraphCycle&) noexcept {}
};
/// A class of this spec whose configure gives the other spec's code; constructed from nothing.
struct Mixed {
	static Members<spec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), toDevice(members(1)), fromDevice(members(1))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		cx.use(OtherCode{});
		return Configured::applied();
	}
};

/// A class that writes the change version: it compiles, and 3a never calls it.
struct Changing {
	static Members<spec> ports(const Checked&) {
		return {in(members(1)), out(members(1)), toDevice(members(1)), fromDevice(members(1))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		calls.emplace_back("configure");
		cx.use(Doubler{&prepares});
		return Configured::applied();
	}
	Changed configure(ChangeOf<spec>&, const Checked&, const Checked&) { return Changed::applied(); }
};

} // namespace type_spec_test
using namespace type_spec_test;

TEST_CASE("A spec's banks become the node's ports, in the spec's order", "[type-spec]") {
	Harness harness;
	CHECK(harness.type().name() == "probe");
	REQUIRE_FALSE(harness.parse({{"channels", 2}}));
	CHECK(harness->ports().inputs() == std::vector<PortName>{"in.1", "in.2"});
	CHECK(harness->ports().outputs() == std::vector<PortName>{"out.1", "out.2"});
	CHECK(harness->ports().insideOutputs() == std::vector<PortName>{"to_device.1", "to_device.2"});
	CHECK(harness->ports().insideInputs() == std::vector<PortName>{"from_device.1", "from_device.2"});
	CHECK(harness->ports().bank("from_device").first == 2);
}

TEST_CASE("check runs after the table's walk, and its refusal is the parse's", "[type-spec]") {
	Harness harness;
	CHECK(harness.parse({{"channels", 9}}) == "channels: must be a whole number, 1-8");
	CHECK(harness.parse({{"mode", "bad"}}) == "mode: not bad");
	const auto parsed = harness.type().parse("p", {{"mode", "bad"}});
	REQUIRE_FALSE(parsed);
	CHECK(parsed.error().node == "p");
	CHECK(parsed.error().detail == "mode: not bad");
}

TEST_CASE("The class is made from NodeOf for each configuration, configured, started once driven and stopped before "
          "its silence",
          "[type-spec]") {
	calls.clear();
	prepares = 0;
	Harness harness({.format = {.rate = 48000, .maxBlock = 64}, .node = "probe"});
	REQUIRE_FALSE(harness.parse({{"channels", 2}}));
	harness.add();
	CHECK(calls == std::vector<std::string>{"made probe", "configure"});
	CHECK(prepares == 1); // no resource to wait for: prepared at once
	harness.blockReady();
	CHECK(harness.phase() == "running");
	CHECK(calls.back() == "start");
	REQUIRE(harness.restart());
	CHECK(calls == std::vector<std::string>{"made probe", "configure", "start", "stop", "destroyed", "made probe",
	                                        "configure", "start"});
	harness.remove();
	CHECK(calls.back() == "destroyed");
	CHECK(harness->gone());
}

TEST_CASE("A code reads and writes by bank through Io, bound by use to the node's ports", "[type-spec]") {
	prepares = 0;
	Harness harness({.format = {.rate = 48000, .maxBlock = 64}});
	REQUIRE_FALSE(harness.parse({{"channels", 2}}));
	harness.add();
	harness.blockReady();
	REQUIRE(harness.phase() == "running");
	CHECK(harness->runObject() == nullptr);
	harness.process(); // the drive's 5 ms fade-in (240 frames)
	harness.process();
	harness.process();
	harness.process();
	std::ranges::fill(harness.input(in, 0), 0.25F);
	std::ranges::fill(harness.input(in, 1), -0.5F);
	harness.process(32);
	CHECK(harness->unwritten() == 0);
	CHECK(harness.output(out, 0)[31] == 0.5F);
	CHECK(harness.output(out, 1)[0] == -1.0F);
	CHECK(harness.output(toDevice, 1)[5] == -0.5F);
}

TEST_CASE("NodeOf's handle says a note from configure", "[type-spec]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"mode", "say"}}));
	harness.add();
	CHECK(harness->notes() == std::vector<std::string>{"configured at 48000"});
}

TEST_CASE("The harness readies and loses a sound card configure asked for", "[type-spec]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"mode", "card"}, {"channels", 1}}));
	harness.add();
	harness.blockReady();
	CHECK(harness.phase() == "configuring");
	const testing::CompanionFake& card = harness.companion(toDevice);
	CHECK(card.name == "node.playback");
	CHECK(card.kind == CompanionKind::cardPlayback); // the bank's direction is the card's
	CHECK(card.bank == "to_device");
	CHECK(std::get<SoundCardShape>(card.shape).card.text() == "Probe");
	harness.ready(toDevice);
	CHECK(harness.phase() == "running");
	harness.process(); // the fade-in
	std::ranges::fill(harness.input(in, 0), 0.75F);
	std::ranges::fill(harness.input(fromDevice, 0), 0.125F);
	harness.process(16);
	CHECK(harness->unwritten() == 0);
	CHECK(harness.output(toDevice, 0)[15] == 0.75F);
	CHECK(harness.output(out, 0)[0] == 0.125F);
	harness.lose(toDevice, "card gone");
	CHECK(harness.phase() == "failed");
	CHECK(harness->health().reason->code == FailureReason::Code::engineRefused);
	CHECK(harness->health().reason->detail == "card gone");
	CHECK(harness.companion(toDevice).destroyed);
}

TEST_CASE("The harness stamps each sink chunk with the most capture latency the plan gives the sink's bank",
          "[type-spec]") {
	upstreams.clear();
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"mode", "sink"}, {"channels", 1}}));
	harness.add();
	harness.blockReady();
	REQUIRE(harness.phase() == "running");
	// Nothing peers in the harness, so to_device tells what configure declared on it: 30 to 40 frames.
	harness.process(16);
	harness.process(16);
	REQUIRE(harness.companion(toDevice).chunks.size() == 2);
	CHECK(harness.companion(toDevice).chunks[0].stamp.upstreamFrames == 40);
	CHECK(upstreams == std::vector<std::uint32_t>{40, 40}); // the consumer's chunks carry the stamp's
}

TEST_CASE("The harness's cycles are discontinuities where the real block's are: the first, and each drive's first",
          "[type-spec]") {
	upstreams.clear();
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"mode", "sink"}, {"channels", 1}}));
	harness.add();
	harness.blockReady();
	harness.process(16);
	harness.process(16);
	REQUIRE(harness.stop());
	harness.process(16); // stopped: the sink is unbound
	REQUIRE(harness.start());
	harness.process(16);
	harness.process(16);
	const auto& chunks = harness.companion(toDevice).chunks; // a stop closes nothing: the same sink
	REQUIRE(chunks.size() == 4);
	CHECK(chunks[0].stamp.discontinuity());
	CHECK_FALSE(chunks[1].stamp.discontinuity());
	CHECK(chunks[2].stamp.causes == CycleCause::drive); // the start's drive
	CHECK(chunks[2].stamp.position == 48);
	CHECK_FALSE(chunks[3].stamp.discontinuity());
}

TEST_CASE("The harness's fade-out ramps down from the next cycle and a cycle that ends it at zero acknowledges it, as "
          "the real block's does",
          "[type-spec]") {
	// At 1000 Hz a fade is 5 frames. out = 2 * in, faded.
	Harness harness(testing::HarnessOptions{.format = {.rate = 1000, .maxBlock = 16}, .autoComplete = false});
	REQUIRE_FALSE(harness.parse({{"channels", 1}}));
	harness.add();
	harness.blockReady();
	harness->completeBarrier(); // the drive
	REQUIRE(harness.phase() == "running");
	std::ranges::fill(harness.input(in, 0), 1.0F);
	harness.process(16); // in over 5 frames, then full
	CHECK(harness->lastFadeGain("out.1") == 1.0F);
	REQUIRE(harness.stop());
	harness.process(2); // 5/5 then 4/5
	CHECK(harness.output(out, 0)[0] == 2.0F);
	CHECK(harness.output(out, 0)[1] == Approx(1.6F));
	CHECK(harness->lastFadeGain("out.1") == Approx(0.8F));
	harness.process(4); // 3/5, 2/5, 1/5 and zero: acknowledged at this cycle's end
	CHECK(harness.output(out, 0)[2] == Approx(0.4F));
	CHECK(harness.output(out, 0)[3] == 0.0F);
	CHECK_THROWS_AS(harness->acknowledgeFade(), std::logic_error); // nothing left to acknowledge
	harness->completeBarrier(); // the silence the acknowledgement let NodeHost ask for
	CHECK(harness.phase() == "stopped");
}

TEST_CASE("A drive applied while the harness's fade-out waits cancels it, as the real block's does, so a fade left "
          "unacknowledged for 100 ms, then a start, fades in from zero and leaves nothing to acknowledge",
          "[type-spec]") {
	// At 1000 Hz a fade is 5 frames. out = 2 * in, faded.
	Harness harness(testing::HarnessOptions{.format = {.rate = 1000, .maxBlock = 16}, .autoComplete = false});
	REQUIRE_FALSE(harness.parse({{"channels", 1}}));
	harness.add();
	harness.blockReady();
	harness->completeBarrier(); // the drive
	REQUIRE(harness.phase() == "running");
	std::ranges::fill(harness.input(in, 0), 1.0F);
	harness.process(16);
	REQUIRE(harness.stop()); // the fade-out waits: no cycle comes to end it
	harness->advance(std::chrono::milliseconds{100});
	// NodeHost took the silence unfaded: the drive cancelled the fade-out, whose acknowledgement never comes.
	REQUIRE_FALSE(harness->diagnostics().empty());
	CHECK(harness->diagnostics().back().code == "ear.fade-unacknowledged");
	CHECK_THROWS_AS(harness->acknowledgeFade(), std::logic_error);
	harness->completeBarrier(); // the silence
	CHECK(harness.phase() == "stopped");
	REQUIRE(harness.start());
	harness->completeBarrier(); // the start's drive
	REQUIRE(harness.phase() == "running");
	harness.process(8);
	// The engine's rule: the new code ramps in from zero over 5 frames, exact at both ends.
	for (std::uint32_t i = 0; i < 8; ++i) {
		INFO("sample " << i);
		CHECK(harness.output(out, 0)[i] == Approx(2.0F * static_cast<float>(std::min(i, 5U)) / 5.0F));
	}
	CHECK_THROWS_AS(harness->acknowledgeFade(), std::logic_error); // still nothing waits
}

TEST_CASE("The harness plays what a test feeds a source into its bank before the code, by the source's own rules",
          "[type-spec]") {
	// At 1000 Hz a fade is 5 frames and a frame lasts 1 ms.
	Harness harness(testing::HarnessOptions{.format = {.rate = 1000, .maxBlock = 16}});
	REQUIRE_FALSE(harness.parse({{"mode", "source"}, {"channels", 2}}));
	harness.add();
	CHECK(harness.companion(fromDevice).kind == CompanionKind::source);
	CHECK(harness.companion(fromDevice).ready);
	harness.blockReady();
	REQUIRE(harness.phase() == "running");
	REQUIRE(harness->latencyPlan().beyond.size() == 1);
	CHECK(harness->latencyPlan().beyond[0] == std::pair<std::string, Delay>{"from_device", Delay{{0, 1024}}});
	std::vector<float> interleaved;
	for (int frame = 0; frame < 24; ++frame) {
		interleaved.push_back(static_cast<float>(frame + 1));
		interleaved.push_back(-static_cast<float>(frame + 1));
	}
	CHECK(harness.feed(fromDevice, interleaved) == 24);
	harness.process(16);
	// The source fades in over 5 frames (and the drive fades `out` in over the same 5), then exact, through the
	// pass-through.
	CHECK(harness.output(out, 0)[0] == 0.0F);
	for (std::uint32_t i = 5; i < 16; ++i) {
		CHECK(harness.output(out, 0)[i] == static_cast<float>(i + 1));
		CHECK(harness.output(out, 1)[i] == -static_cast<float>(i + 1));
	}
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.read = 16});
	harness.process(16); // 8 frames, then dry
	CHECK(harness.output(out, 0)[7] == 24.0F);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.read = 8, .zeros = 8});
	CHECK(harness.shortfall(fromDevice) == 8);
	// No clock: a catch-up skips nothing, the ring plays on in order.
	CHECK(harness.feed(fromDevice, interleaved) == 24);
	harness->loseCycles(1, testing::Lost::cut);
	harness.process(16);
	REQUIRE(harness->facts().causes == CycleCause::catchUp);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.read = 16});
	CHECK(harness.output(out, 0)[15] == 16.0F);
	// A stop drops what the ring holds; the writer waits as at a full ring until the next cycle after a start.
	REQUIRE(harness.stop());
	CHECK(harness.feed(fromDevice, interleaved) == 0);
	REQUIRE(harness.start());
	harness.process(16);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.zeros = 16});
	CHECK(harness.feed(fromDevice, interleaved) == 24);
	harness.remove();
	CHECK(harness.companion(fromDevice).destroyed);
}

TEST_CASE("The harness plays a source with a clock by the block's own play rule, and frees the clock with the source",
          "[type-spec]") {
	clockHeard = {};
	Harness harness(testing::HarnessOptions{.format = {.rate = 1000, .maxBlock = 16}});
	REQUIRE_FALSE(harness.parse({{"mode", "clocked"}, {"channels", 1}}));
	harness.add();
	harness.blockReady();
	REQUIRE(harness.phase() == "running");
	std::vector<float> stream;
	for (int frame = 0; frame < 48; ++frame) stream.push_back(static_cast<float>(frame + 1));
	CHECK(harness.feed(fromDevice, stream) == 48);
	harness.process(16);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.read = 16});
	// A cycle cut: the next states a catch-up of 16, and the clock skips it (16 of the 32 held), exactly.
	harness->loseCycles(1, testing::Lost::cut);
	harness.process(16);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.skipped = 16, .read = 16});
	for (std::uint32_t i = 0; i < 16; ++i) CHECK(harness.output(out, 0)[i] == static_cast<float>(32 + i + 1));
	// A stall past what is held: capped at nothing held.
	harness->loseCycles(4, testing::Lost::cut);
	harness.process(16);
	CHECK(harness.lastPlayed(fromDevice) == SourcePlayed{.zeros = 16});
	REQUIRE(clockHeard.jumps == std::vector<std::int64_t>{0, 16, 64});
	REQUIRE(clockHeard.played.size() == 3);
	CHECK(clockHeard.played[1] == SourcePlayed{.skipped = 16, .read = 16});
	CHECK(clockHeard.playbacks[0] == Latency{}); // `out` is linked to nothing: no playback latency is told
	CHECK_FALSE(clockHeard.freed);
	harness.remove();
	CHECK(clockHeard.freed);
}

TEST_CASE("A configure that answers failed fails the node start-failed with its detail", "[type-spec]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({{"mode", "fail"}}));
	harness.add();
	CHECK(harness.phase() == "failed");
	CHECK(harness->health().reason == FailureReason{FailureReason::Code::startFailed, "asked to"});
}

TEST_CASE("A layout that disagrees with the spec is a logic error at prepare", "[type-spec]") {
	// Missing a bank, a bank of the wrong direction, and a code run outside makeNodeType.
	const Ports missing{{{Direction::input, "in", {"1"}}, {Direction::output, "out", {"1"}}}};
	CHECK_THROWS_AS(Io<spec>::resolve(missing), std::logic_error);
	const Ports turned{{{Direction::input, "in", {"1"}},
	                    {Direction::output, "out", {"1"}},
	                    {Direction::input, "to_device", {"1"}, PortBank::Reach::inside},
	                    {Direction::input, "from_device", {"1"}, PortBank::Reach::inside}}};
	CHECK_THROWS_AS(Io<spec>::resolve(turned), std::logic_error);
	int unused = 0;
	const auto erased = eraseSpecCode(Doubler{&unused});
	CHECK_THROWS_WITH(erased->prepare({48000, 64}),
	                  "a code written against a spec runs only in a type made by makeNodeType");
}

TEST_CASE("Members are numbered or named, each once", "[type-spec]") {
	CHECK(members(3).names == std::vector<std::string>{"1", "2", "3"});
	CHECK(members("left", "right").names == std::vector<std::string>{"left", "right"});
	const Members<spec> twice{in(members("a", "a")), out(members(1)), toDevice(members(1)), fromDevice(members(1))};
	CHECK_THROWS_AS(twice.ports(), std::logic_error);
}

TEST_CASE("A configure that uses a code written against another type's spec fails the node at use", "[type-spec]") {
	testing::TypeHarness<spec, Mixed> harness;
	REQUIRE_FALSE(harness.parse({{"channels", 1}}));
	harness.add();
	CHECK(harness.phase() == "failed");
	CHECK(harness->health().reason ==
	      FailureReason{FailureReason::Code::startFailed,
	                    "configure threw: a type's configure gave a code written against another type's spec"});
}

TEST_CASE("A class that writes the change version compiles, and every change restarts the node in 3a", "[type-spec]") {
	calls.clear();
	testing::TypeHarness<spec, Changing> harness;
	REQUIRE_FALSE(harness.parse({{"channels", 1}}));
	harness.add();
	harness.blockReady();
	CHECK(harness.change({{"channels", 1}, {"mode", "x"}}) == NodeOutcome::restart);
	CHECK(harness.phase() == "running");
	CHECK(calls == std::vector<std::string>{"configure", "configure"}); // configured afresh; the change version unused
}
