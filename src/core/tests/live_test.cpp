// Live rows (docs/writing-a-node-type.md#live-rows; docs/architecture.md#the-realtime-channels): their slots, the
// advance before process, ramps and steps as io.param gives them, the snap at a drive, and the node's live values, on a
// test type whose code writes each view into an output, by the type harness over a real NodeHost. The fade-in after a
// drive scales every output alike, so each value is read as a ratio to the `one` bank, which writes 1.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/realtime_channels.hpp"
#include "ear/testing/type_harness.hpp"

namespace live_test {

using namespace ear;

enum class Mode { soft, hard };

inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 4, "channels"), 1u).shapesPorts().inPlace();
inline constexpr auto tagRow = setting::defaulted("tag", setting::number(0, 100), 0u).inPlace();
inline constexpr auto levelRow =
	setting::defaulted("level", setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db}), -6.0)
		.live(setting::ramp(20));
inline constexpr auto freqRow =
	setting::defaulted("freq", setting::decimal(20.0, 20000.0, "Hz", {.taper = setting::Taper::log}), 1000.0)
		.live(setting::ramp(20));
inline constexpr auto onRow = setting::defaulted("on", setting::boolean(), true).live(setting::ramp(10));
inline constexpr auto pickRow = setting::defaulted("pick", setting::number(1, 8), 1u).live(setting::step);
inline constexpr auto modeRow =
	setting::defaulted("mode", setting::choice<Mode>({{"soft", Mode::soft}, {"hard", Mode::hard}}), Mode::soft)
		.live(setting::step);
inline constexpr auto trimRow =
	setting::indexed("trim", channelsRow, setting::decimal(-24.0, 24.0, "dB"), 0.0).live(setting::ramp(20));
inline constexpr SettingsTable<channelsRow, tagRow, levelRow, freqRow, onRow, pickRow, modeRow, trimRow> table{"liver"};

inline constexpr auto one = bank::output<"one">;
inline constexpr auto level = bank::output<"level">;
inline constexpr auto freq = bank::output<"freq">;
inline constexpr auto on = bank::output<"on">;
inline constexpr auto trim = bank::output<"trim">;
inline constexpr auto pick = bank::output<"pick">; // value, before, changed, framesSince, mode, count, steady
inline constexpr TypeSpec spec{.settings = table, .banks = Banks<one, level, freq, on, trim, pick>{}};

struct Code {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		const Smoothed levelView = io.param<levelRow>();
		const Smoothed freqView = io.param<freqRow>();
		const Smoothed onView = io.param<onRow>();
		const Stepped<std::uint32_t> pickView = io.param<pickRow>();
		const Stepped<Mode> modeView = io.param<modeRow>();
		const auto picks = io[pick];
		for (std::uint32_t i = 0; i < io.frames(); ++i) {
			io[one][0][i] = 1.0F;
			io[level][0][i] = levelView[i];
			io[freq][0][i] = freqView[i];
			io[on][0][i] = onView[i];
			picks[0][i] = static_cast<float>(pickView.value());
			picks[1][i] = static_cast<float>(pickView.before());
			picks[2][i] = pickView.changed() ? 1.0F : 0.0F;
			picks[3][i] = static_cast<float>(std::min<std::uint32_t>(pickView.framesSince(), 1U << 20));
			picks[4][i] = modeView.value() == Mode::hard ? 1.0F : 0.0F;
			picks[5][i] = static_cast<float>(io.count<trimRow>());
			picks[6][i] = levelView.steady() ? 1.0F : 0.0F;
		}
		const auto trims = io[trim];
		for (std::size_t c = 0; c < trims.size(); ++c) {
			const Smoothed view = io.param<trimRow>(c);
			for (std::uint32_t i = 0; i < io.frames(); ++i) trims[c][i] = view[i];
		}
	}
};
/// What the last change version read of `level` and `trim.3` through liveValue, in its new settings.
double changeSawLevel = 0.0;
double changeSawTrim = 0.0;
struct Liver {
	static Members<spec> ports(const CheckedOf<spec>& s) {
		return {one(members("1")),
		        level(members("1")),
		        freq(members("1")),
		        on(members("1")),
		        trim(members(s.get<channelsRow>())),
		        pick(members(7))};
	}
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) { return cx.applied(Code{}); }
	/// Every change in place answers with a new code: swapped in at the commit point.
	Changed configure(ChangeOf<spec>& cx, const CheckedOf<spec>&, const CheckedOf<spec>& now) {
		changeSawLevel = now.liveValue<levelRow>();
		changeSawTrim = now.liveValue<trimRow>(2);
		return cx.applied(Code{});
	}
};
using Harness = testing::TypeHarness<spec, Liver>;

/// What the code wrote to `bank`'s member over the last process, each frame as a ratio to the `one` bank's (NaN where
/// the fade-in's gain is 0, the first frame after a drive).
template <class Bank> std::vector<double> heard(Harness& h, Bank bank, std::size_t member = 0) {
	const auto ones = h.output(one, 0);
	const auto values = h.output(bank, member);
	std::vector<double> out;
	for (std::size_t i = 0; i < values.size(); ++i)
		out.push_back(ones[i] == 0.0F ? std::nan("") : static_cast<double>(values[i]) / static_cast<double>(ones[i]));
	return out;
}
/// What the code wrote to `bank`'s member over the last process, as written (the pick bank's, which a test reads
/// only once the fade-in is over).
template <class Bank> float written(Harness& h, Bank bank, std::size_t member, std::size_t frame = 0) {
	return h.output(bank, member)[frame];
}

/// A running node with `settings`, its drive's fade-in over (two blocks of 256 at 48 kHz pass its 5 ms).
void running(Harness& h, const NodeSettings& settings = NodeSettings::object()) {
	REQUIRE_FALSE(h.parse(settings));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "running");
	h.process(256);
	h.process(256);
}

/// Runs `total` frames in blocks of `block`, collecting the ratio `level` heard, frame by frame.
std::vector<double> levelOver(Harness& h, std::uint32_t total, std::uint32_t block) {
	std::vector<double> frames;
	for (std::uint32_t done = 0; done < total; done += block) {
		const std::uint32_t size = std::min(block, total - done);
		h.process(size);
		const auto part = heard(h, level);
		frames.insert(frames.end(), part.begin(), part.begin() + size);
	}
	return frames;
}

TEST_CASE("A ramp follows the worked vector frame by frame at blocks of 1, 7 and 256", "[live]") {
	const std::uint32_t block = GENERATE(1U, 7U, 256U);
	CAPTURE(block);
	Harness h;
	running(h);
	h.live<levelRow>(-3.5); // stored before frame 0
	// Up to the first block that starts at or after frame 512.
	const std::uint32_t first = (512 + block - 1) / block * block;
	std::vector<double> frames = levelOver(h, first, block);
	CHECK(frames[0] == Approx(-5.9973958).margin(1e-6));
	CHECK(frames[255] == Approx(-5.3333333).margin(1e-6));
	h.live<levelRow>(-12.0); // moved mid-ramp: a new ramp of the whole length from the last frame's value
	const std::vector<double> after = levelOver(h, 1536 - first, block);
	frames.insert(frames.end(), after.begin(), after.end());
	const double from = -6.0 + 2.5 * static_cast<double>(first) / 960.0;
	CHECK(frames[first - 1] == Approx(from).margin(1e-6));
	CHECK(frames[first] == Approx(from + (-12.0 - from) / 960.0).margin(1e-6));
	CHECK(frames[first + 958] != Approx(-12.0).margin(1e-6));
	CHECK(frames[first + 959] == Approx(-12.0).margin(1e-6));
	CHECK(frames[first + 960] == Approx(-12.0).margin(1e-6));
	for (std::uint32_t k = 0; k < first; ++k) {
		CAPTURE(k);
		REQUIRE(frames[k] == Approx(-6.0 + 2.5 * static_cast<double>(std::min(k + 1, 960U)) / 960.0).margin(1e-5));
	}
	if (block == 7) {
		CHECK(first == 518);
		CHECK(frames[517] == Approx(-4.6510417).margin(1e-6));
	} else {
		CHECK(first == 512);
		CHECK(frames[511] == Approx(-4.6666667).margin(1e-6));
		CHECK(frames[1471] == Approx(-12.0).margin(1e-6));
	}
}

TEST_CASE("At blocks of 256 a 20 ms ramp is unsteady for four blocks, steady from the fifth", "[live]") {
	Harness h;
	running(h);
	h.live<levelRow>(-3.5);
	for (int block = 0; block < 4; ++block) {
		h.process(256);
		CHECK(written(h, pick, 6) == 0.0F);
	}
	h.process(256);
	CHECK(written(h, pick, 6) == 1.0F);
	CHECK(heard(h, level).back() == Approx(-3.5).margin(1e-6));
	CHECK(heard(h, level).front() == Approx(-3.5).margin(1e-6));
}

TEST_CASE("A steady block loads no slot: only a moved generation word makes the advance read them", "[live]") {
	Harness h;
	running(h);
	const std::uint64_t before = h->liveReloads();
	for (int block = 0; block < 10; ++block) h.process(256);
	CHECK(h->liveReloads() == before); // a running ramp is computed from its ends, never reloaded
	h.live<levelRow>(-3.5);
	h.process(256);
	CHECK(h->liveReloads() == before + 1);
	for (int block = 0; block < 10; ++block) h.process(256);
	CHECK(h->liveReloads() == before + 1);
}

TEST_CASE("A log taper's row ramps on its logarithm", "[live]") {
	Harness h;
	running(h);
	h.live<freqRow>(4000.0);
	std::vector<double> frames;
	for (int block = 0; block < 4; ++block) {
		h.process(256);
		const auto part = heard(h, freq);
		frames.insert(frames.end(), part.begin(), part.end());
	}
	CHECK(frames[479] == Approx(2000.0).epsilon(1e-5)); // half way on the logarithm: the geometric mean
	CHECK(frames[959] == Approx(4000.0).epsilon(1e-6));
	CHECK(frames[0] == Approx(1000.0 * std::pow(4.0, 1.0 / 960.0)).epsilon(1e-5));
}

TEST_CASE("A boolean ramps as a weight from 1 to 0 over its ms", "[live]") {
	Harness h;
	running(h);
	h.live<onRow>(false);
	std::vector<double> frames;
	for (int block = 0; block < 3; ++block) {
		h.process(256);
		const auto part = heard(h, on);
		frames.insert(frames.end(), part.begin(), part.end());
	}
	CHECK(frames[0] == Approx(1.0 - 1.0 / 480.0).margin(1e-6));
	CHECK(frames[239] == Approx(0.5).margin(1e-6));
	CHECK(frames[479] == 0.0);
	CHECK(frames[700] == 0.0);
	for (std::size_t k = 1; k < frames.size(); ++k) REQUIRE(frames[k] <= frames[k - 1]);
}

TEST_CASE("A step is the new value for its whole first block, with changed, before and framesSince", "[live]") {
	Harness h;
	running(h);
	CHECK(written(h, pick, 1) == 1.0F); // no edge yet: before() is value()
	CHECK(written(h, pick, 3) == static_cast<float>(1U << 20));
	h.live<pickRow>(3);
	h.live<modeRow>(Mode::hard);
	h.process(256);
	CHECK(written(h, pick, 0, 0) == 3.0F);
	CHECK(written(h, pick, 0, 255) == 3.0F);
	CHECK(written(h, pick, 1) == 1.0F);
	CHECK(written(h, pick, 2) == 1.0F);
	CHECK(written(h, pick, 3) == 0.0F);
	CHECK(written(h, pick, 4) == 1.0F);
	h.process(100);
	CHECK(written(h, pick, 2) == 0.0F);
	CHECK(written(h, pick, 3) == 256.0F);
	CHECK(written(h, pick, 1) == 1.0F);
	h.process(256);
	CHECK(written(h, pick, 3) == 356.0F);
}

TEST_CASE("The first block after the first drive is at the slot, with no ramp", "[live]") {
	Harness h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	REQUIRE(h.phase() != "running");
	h.live<levelRow>(-12.0); // its configuration made, not yet driven
	h.live<pickRow>(5);
	h.blockReady();
	REQUIRE(h.phase() == "running");
	h.process(256);
	for (const double frame : heard(h, level))
		if (!std::isnan(frame)) REQUIRE(frame == Approx(-12.0).margin(1e-6));
	CHECK(heard(h, level).back() == Approx(-12.0).margin(1e-6));
	h.process(256);
	CHECK(written(h, pick, 0) == 5.0F);
	CHECK(written(h, pick, 1) == 5.0F); // unchanged: the snap is no edge
	CHECK(written(h, pick, 2) == 0.0F);
}

TEST_CASE("A start after a stop takes what the slots were moved to while stopped, with no ramp", "[live]") {
	Harness h;
	running(h);
	REQUIRE(h.stop());
	REQUIRE(h.phase() == "stopped");
	h.live<levelRow>(-20.0);
	REQUIRE(h.start());
	REQUIRE(h.phase() == "running");
	h.process(256);
	for (const double frame : heard(h, level))
		if (!std::isnan(frame)) REQUIRE(frame == Approx(-20.0).margin(1e-6));
	CHECK(heard(h, level).back() == Approx(-20.0).margin(1e-6));
	CHECK(h->liveValues().at(table.liveIndexOf("level").value()) == std::bit_cast<std::uint64_t>(-20.0));
}

TEST_CASE("A change of a live row in place moves its slot at the commit point, and a swap keeps the ramp going",
          "[live]") {
	Harness h;
	running(h);
	REQUIRE(h.change({{"level", -3.5}}) == NodeOutcome::reconfigure);
	REQUIRE(h.swaps() == 1);
	std::vector<double> frames = levelOver(h, 256, 256);
	CHECK(frames[0] == Approx(-5.9973958).margin(1e-6));
	// A change of another key swaps in a new code; the slots are unchanged, so the ramp goes on where it was.
	REQUIRE(h.change({{"level", -3.5}, {"tag", 1}}) == NodeOutcome::reconfigure);
	REQUIRE(h.swaps() == 2);
	const std::vector<double> after = levelOver(h, 768, 256);
	frames.insert(frames.end(), after.begin(), after.end());
	for (std::uint32_t k = 0; k < frames.size(); ++k) {
		CAPTURE(k);
		REQUIRE(frames[k] == Approx(-6.0 + 2.5 * static_cast<double>(std::min(k + 1, 960U)) / 960.0).margin(1e-5));
	}
}

TEST_CASE("A swap two blocks after a step's edge sees framesSince of two blocks and the same before", "[live]") {
	Harness h;
	running(h);
	// The edge comes from a change of the setting, so the commit point that writes it is the change's.
	REQUIRE(h.change({{"pick", 4}}) == NodeOutcome::reconfigure);
	REQUIRE(h.swaps() == 1);
	h.process(256);
	CHECK(written(h, pick, 2) == 1.0F);
	h.process(256);
	REQUIRE(h.change({{"pick", 4}, {"tag", 7}}) == NodeOutcome::reconfigure);
	REQUIRE(h.swaps() == 2);
	h.process(256);
	CHECK(written(h, pick, 3) == 512.0F);
	CHECK(written(h, pick, 1) == 1.0F);
	CHECK(written(h, pick, 0) == 4.0F);
	CHECK(written(h, pick, 2) == 0.0F);
}

TEST_CASE("An inactive member is seeded at its file value and plays it from its first active sample", "[live]") {
	Harness h;
	running(h, {{"channels", 1}, {"trim.2", -6.0}});
	CHECK(written(h, pick, 5) == 1.0F);
	REQUIRE(h.change({{"channels", 2}, {"trim.2", -6.0}}) == NodeOutcome::reshape);
	h.process(256);
	CHECK(written(h, pick, 5) == 2.0F);
	for (const double frame : heard(h, trim, 1)) REQUIRE(frame == Approx(-6.0).margin(1e-6));
	for (const double frame : heard(h, trim, 0)) REQUIRE(frame == 0.0);
}

TEST_CASE("liveValue reads the node's live values in configure and the parsed ones in check", "[live]") {
	const auto checked = table.check({{"level", -9.0}, {"trim.3", 2.0}});
	REQUIRE(checked);
	// No hook is running: what was parsed.
	CHECK(checked->liveValue<levelRow>() == -9.0);
	CHECK(checked->liveValue<trimRow>(2) == 2.0);
	CHECK(checked->liveValue<trimRow>(0) == 0.0);
	CHECK(checked->count<trimRow>() == 1);
	STATIC_REQUIRE(table.liveMemberCount == 9); // level, freq, on, pick, mode and trim's four
	STATIC_REQUIRE(table.liveIndexOf("trim.1") == 5);
	STATIC_REQUIRE(table.liveIndexOf("trim.4") == 8);
	STATIC_REQUIRE(!table.liveIndexOf("trim.5"));
	STATIC_REQUIRE(!table.liveIndexOf("tag"));
	// In the change version: the node's live values, which a slot's move made, not the settings' -9.
	Harness h;
	running(h, {{"level", -9.0}, {"trim.3", 2.0}});
	h.live<levelRow>(-1.5);
	h.live<trimRow>(2, 7.0);
	REQUIRE(h.change({{"level", -9.0}, {"trim.3", 2.0}, {"tag", 1}}) == NodeOutcome::reconfigure);
	CHECK(changeSawLevel == -1.5);
	CHECK(changeSawTrim == 7.0);
	// Until retunes are built the commit point takes the settings' values again.
	CHECK(h->liveValues().at(table.liveIndexOf("level").value()) == std::bit_cast<std::uint64_t>(-9.0));
}

TEST_CASE("The advance and the views allocate nothing, with 130 members moving", "[live]") {
	std::vector<LiveMember> members(130, LiveMember{.move = LiveMember::Move::linear, .frames = 960});
	for (std::size_t at = 0; at < members.size(); at += 3) members[at].move = LiveMember::Move::step;
	RealtimeChannels channels({}, nullptr, 0, members);
	for (std::size_t at = 0; at < members.size(); ++at) channels.setLive(at, std::bit_cast<std::uint64_t>(-3.5));
	channels.publishLive();
	const std::uint64_t before = ear::testing::countedAllocations();
	ear::testing::countAllocationsOnThisThread(true);
	float sum = 0.0F;
	for (int block = 0; block < 8; ++block) {
		if (block == 4) {
			channels.setLive(1, std::bit_cast<std::uint64_t>(-12.0));
			channels.publishLive();
		}
		if (block == 6) channels.snapLive();
		channels.advanceLive(256);
		for (std::size_t at = 1; at < members.size(); at += 3) {
			const Smoothed view(channels.liveState(at), channels.liveMember(at), channels.livePosition(), 256);
			for (std::uint32_t i = 0; i < 256; ++i) sum += view[i];
		}
	}
	ear::testing::countAllocationsOnThisThread(false);
	CHECK(ear::testing::countedAllocations() == before);
	CHECK(std::isfinite(sum));
	CHECK(channels.liveReloads() == 3);
}

TEST_CASE("forSegments ends each piece where a ramp ends, and makes one call when every view is steady", "[live]") {
	std::vector<LiveMember> members(2, LiveMember{.move = LiveMember::Move::linear, .frames = 100});
	RealtimeChannels channels({}, nullptr, 0, members);
	channels.advanceLive(256);
	const auto pieces = [&] {
		const Smoothed a(channels.liveState(0), channels.liveMember(0), channels.livePosition(), 256);
		const Smoothed b(channels.liveState(1), channels.liveMember(1), channels.livePosition(), 256);
		std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
		forSegments(256, 32, [&](std::uint32_t begin, std::uint32_t end) { calls.emplace_back(begin, end); }, a, b);
		return calls;
	};
	CHECK(pieces() == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0, 256}});
	channels.setLive(0, std::bit_cast<std::uint64_t>(1.0));
	channels.publishLive();
	channels.advanceLive(256);
	// The ramp's last frame is 99: from frame 99 on every frame is its target.
	CHECK(pieces() ==
	      std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0, 32}, {32, 64}, {64, 96}, {96, 99}, {99, 256}});
}

} // namespace live_test
