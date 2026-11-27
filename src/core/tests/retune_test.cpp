// Retunes (docs/architecture.md#retunes): a change of live keys moves the node's live values and its slots at once, in
// every phase, beside whatever outcome its other keys decide; a retune never supersedes a change in flight, never
// retries a failed node and leaves a disabled node stored; every configuration is seeded from the live values. By a
// test type in the type harness over a real NodeHost, its code writing each live view into an output, read as a ratio
// to the `one` bank (the fade-in after a drive scales every output alike).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/gestures.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

namespace retune_test {

using namespace ear;

inline constexpr auto tagRow = setting::defaulted("tag", setting::number(0, 100), 0u).inPlace();
inline constexpr auto sizeRow = setting::defaulted("size", setting::number(1, 4), 1u).restarts("a test type restarts");
inline constexpr auto failRow =
	setting::defaulted("fail", setting::boolean(), false).restarts("a test type restarts on it");
inline constexpr auto levelRow =
	setting::defaulted("level", setting::decimal(-90.0, 24.0, "dB"), -6.0).live(setting::ramp(20));
inline constexpr auto bandsRow = setting::defaulted("bands", setting::number(1, 4), 1u).live(setting::step);
inline constexpr auto bandRow =
	setting::indexed("band", bandsRow, setting::decimal(-24.0, 24.0, "dB"), 0.0).live(setting::ramp(20));
inline constexpr SettingsTable<tagRow, sizeRow, failRow, levelRow, bandsRow, bandRow> table{"tuner"};

inline constexpr auto one = bank::output<"one">;
inline constexpr auto level = bank::output<"level">;
inline constexpr auto band = bank::output<"band">; // each member's view while active, 99 while inactive
inline constexpr auto held = bank::output<"held">; // what a change version holds
inline constexpr TypeSpec spec{.settings = table, .banks = Banks<one, level, band, held>{}};

/// What the change version does, set by each test.
struct Script {
	enum class Answer { applied, appliedCode } answer = Answer::applied;
	bool hold = true;
};
Script script;
int configures = 0;
int changes = 0;
double changeSawLevel = 0.0; // what the last change version read of level through liveValue, in its new settings

struct Code {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		const Smoothed levelView = io.param<levelRow>();
		const std::uint32_t active = io.count<bandRow>();
		const auto bands = io[band];
		for (std::uint32_t i = 0; i < io.frames(); ++i) {
			io[one][0][i] = 1.0F;
			io[level][0][i] = levelView[i];
			io[held][0][i] = 1.0F;
		}
		for (std::size_t c = 0; c < bands.size(); ++c) {
			const Smoothed view = io.param<bandRow>(c);
			for (std::uint32_t i = 0; i < io.frames(); ++i) bands[c][i] = c < active ? view[i] : 99.0F;
		}
	}
};
struct Tuner {
	static Members<spec> ports(const CheckedOf<spec>&) {
		return {one(members(1)), level(members(1)), band(members(4)), held(members(1))};
	}
	/// A rule across rows, judged on what will play: at four bands, level is at most 12.
	static std::expected<void, std::string> check(const CheckedOf<spec>& s) {
		if (s.liveCount<bandRow>() == 4 && s.liveValue<levelRow>() > 12.0)
			return std::unexpected("level: must be 12 dB or less with four bands");
		return {};
	}
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>& s) {
		++configures;
		if (s.get<failRow>()) throw std::runtime_error("told to fail");
		return cx.applied(Code{});
	}
	Changed configure(ChangeOf<spec>& cx, const CheckedOf<spec>&, const CheckedOf<spec>& now) {
		++changes;
		changeSawLevel = now.liveValue<levelRow>();
		if (script.hold) cx.hold(held);
		if (script.answer == Script::Answer::appliedCode) return cx.applied(Code{});
		return cx.applied();
	}
};
using Harness = testing::TypeHarness<spec, Tuner>;

void reset(Script asked = {}) {
	script = asked;
	configures = 0;
	changes = 0;
	changeSawLevel = 0.0;
}

testing::HarnessOptions manual() { return testing::HarnessOptions{.autoComplete = false}; }

/// With autoComplete off: every fade and barrier the node waits for completed, until it runs.
void toRunning(Harness& h) {
	for (int turn = 0; turn < 16 && h.phase() != "running"; ++turn) {
		try {
			h->acknowledgeFade();
		} catch (const std::logic_error&) {
		}
		if (h.phase() == "running") break;
		try {
			h->completeBarrier();
		} catch (const std::logic_error&) {
		}
	}
	REQUIRE(h.phase() == "running");
}

/// Added with `settings` and running, its drive's fade-in over (two blocks of 256 at 48 kHz pass its 5 ms).
void running(Harness& h, const NodeSettings& settings = NodeSettings::object()) {
	REQUIRE_FALSE(h.parse(settings));
	h.add();
	h.blockReady();
	toRunning(h);
	h.process(256);
	h.process(256);
}

/// What the code wrote to `bank`'s member over the last process, as a ratio to the `one` bank's (NaN where the
/// fade-in's gain is 0).
template <class Bank> std::vector<double> heard(Harness& h, Bank bank, std::size_t member = 0) {
	const auto ones = h.output(one, 0);
	const auto values = h.output(bank, member);
	std::vector<double> out;
	for (std::size_t i = 0; i < values.size(); ++i)
		out.push_back(ones[i] == 0.0F ? std::nan("") : static_cast<double>(values[i]) / static_cast<double>(ones[i]));
	return out;
}

/// Whether every frame heard of `bank`'s member over the last process is `value` (frames the fade-in silenced
/// skipped).
template <class Bank> bool allAt(Harness& h, Bank bank, double value, std::size_t member = 0) {
	return std::ranges::all_of(heard(h, bank, member),
	                           [&](double frame) { return std::isnan(frame) || std::abs(frame - value) < 1e-5; });
}

/// Five blocks of 256: a 20 ms ramp (960 frames) ends within them.
void pastRamp(Harness& h) {
	for (int block = 0; block < 5; ++block) h.process(256);
}

std::uint64_t liveLevel(Harness& h) { return h->liveValues().at(table.liveIndexOf("level").value()); }
std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

} // namespace retune_test
using namespace retune_test;

TEST_CASE("A retune moves the slot at once: no hook called, no reconfiguring, no health told", "[retune]") {
	reset();
	Harness h;
	running(h);
	const std::size_t told = h->healths().size();
	REQUIRE(h.change({{"level", -12.0}}) == NodeOutcome::retune);
	CHECK(liveLevel(h) == bits(-12.0));
	CHECK(h->healths().size() == told);
	CHECK(changes == 0);
	CHECK(configures == 1);
	CHECK(h.swaps() == 0);
	h.process(256);
	CHECK(heard(h, level)[0] == Approx(-6.0 - 6.0 / 960.0).margin(1e-5)); // ramping from its first frame
	pastRamp(h);
	CHECK(allAt(h, level, -12.0));
}

TEST_CASE("A mixed change is the outcome its other keys ask, its live keys retuned beside it, never in its keys",
          "[retune]") {
	reset({.hold = false});
	Harness h;
	running(h);
	REQUIRE(h.change({{"tag", 1}, {"level", -12.0}}) == NodeOutcome::reconfigure);
	CHECK(changes == 1);
	CHECK(changeSawLevel == -12.0); // the change version reads the live values, already moved
	CHECK(liveLevel(h) == bits(-12.0));
	CHECK(std::ranges::none_of(
		h->healths(), [](const NodeHealth& one) { return std::ranges::find(one.keys, "level") != one.keys.end(); }));
	REQUIRE(h.change({{"tag", 1}, {"level", -3.0}, {"size", 2}}) == NodeOutcome::restart);
	CHECK(liveLevel(h) == bits(-3.0));
	pastRamp(h);
	CHECK(allAt(h, level, -3.0));
}

TEST_CASE("A retune during a change before its commit point moves the slots, and the change goes on untouched",
          "[retune]") {
	reset();
	Harness h(manual());
	running(h);
	REQUIRE(h.change({{"tag", 1}}) == NodeOutcome::reconfigure);
	REQUIRE(h.phase() == "reconfiguring"); // its hold waits for its acknowledgement
	REQUIRE(changes == 1);
	REQUIRE(h.change({{"tag", 1}, {"level", -12.0}}) == NodeOutcome::retune);
	CHECK(h.phase() == "reconfiguring");
	CHECK(changes == 1); // never superseded
	CHECK(liveLevel(h) == bits(-12.0));
	SECTION("the change commits, and the slots keep the retune") {
		h->acknowledgeHolds();
		CHECK(h.phase() == "running");
		pastRamp(h);
		CHECK(allAt(h, level, -12.0));
		CHECK(liveLevel(h) == bits(-12.0));
	}
	SECTION("a newer change supersedes it, and its change version reads the retuned value") {
		REQUIRE(h.change({{"tag", 2}, {"level", -12.0}}) == NodeOutcome::reconfigure);
		CHECK(changes == 2);
		CHECK(changeSawLevel == -12.0);
		h->acknowledgeHolds();
		CHECK(h.phase() == "running");
		pastRamp(h);
		CHECK(allAt(h, level, -12.0));
		CHECK(liveLevel(h) == bits(-12.0));
	}
	SECTION("a stop ends the change, and the start plays the retuned value from its first block") {
		h.stop();
		h->acknowledgeFade();
		h->completeBarrier();
		REQUIRE(h.phase() == "stopped");
		CHECK(liveLevel(h) == bits(-12.0));
		h.start();
		toRunning(h);
		h.process(256);
		CHECK(allAt(h, level, -12.0));
	}
	SECTION("a fallback restarts it, the new configuration seeded with the retuned value") {
		h.restart(); // during a change: it falls back to a restart
		toRunning(h);
		CHECK(configures == 2);
		h.process(256);
		CHECK(allAt(h, level, -12.0));
		CHECK(liveLevel(h) == bits(-12.0));
	}
}

TEST_CASE("A retune while a change's commit is in flight moves the slots, and the commit keeps them", "[retune]") {
	reset({.answer = Script::Answer::appliedCode, .hold = false});
	Harness h(manual());
	running(h);
	REQUIRE(h.change({{"tag", 1}}) == NodeOutcome::reconfigure);
	REQUIRE(h.phase() == "reconfiguring"); // the swap's invoke in flight: committing
	REQUIRE(h.change({{"tag", 1}, {"level", -12.0}}) == NodeOutcome::retune);
	CHECK(liveLevel(h) == bits(-12.0));
	h->completeBarrier();
	CHECK(h.phase() == "running");
	CHECK(changes == 1);
	pastRamp(h);
	CHECK(allAt(h, level, -12.0));
}

TEST_CASE("A retune of a failed node moves its live values and does not retry it, and the retry is seeded from them",
          "[retune]") {
	reset();
	Harness h;
	REQUIRE_FALSE(h.parse({{"fail", true}}));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "failed");
	REQUIRE(configures == 1);
	REQUIRE(h.change({{"fail", true}, {"level", -12.0}}) == NodeOutcome::retune);
	CHECK(h.phase() == "failed");
	CHECK(configures == 1);
	CHECK(liveLevel(h) == bits(-12.0));
	REQUIRE(h.change({{"level", -12.0}}) == NodeOutcome::restart); // a key that is not live: retried
	REQUIRE(h.phase() == "running");
	CHECK(configures == 2);
	h.process(256);
	CHECK(allAt(h, level, -12.0));
}

TEST_CASE("A gesture on a failed node moves its live values without a retry, and the retry plays the hand",
          "[retune][gestures]") {
	reset();
	Harness h;
	REQUIRE_FALSE(h.parse({{"fail", true}}));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "failed");
	Gestures gestures(h->liveTarget(), h->timers());
	const std::uint32_t member = table.liveIndexOf("level").value();
	const std::string g = gestures.open(GestureOpen{1, {{{"node", "level"}, member}}, -90.0, 24.0, 1, -6.0});
	gestures.move(g, 1, 1, -12.0);
	CHECK(h.phase() == "failed");
	CHECK(configures == 1);
	CHECK(liveLevel(h) == bits(-12.0));
	// Another key's change retries it, seeded from the live values: the hand plays from the first block.
	REQUIRE(h.change(NodeSettings::object()) == NodeOutcome::restart);
	REQUIRE(h.phase() == "running");
	CHECK(configures == 2);
	h.process(256);
	CHECK(allAt(h, level, -12.0));
	gestures.move(g, 1, 2, -9.0);
	pastRamp(h);
	CHECK(allAt(h, level, -9.0));
	// Cancelled: back to the committed -6.
	REQUIRE(gestures.cancel(g, 1));
	pastRamp(h);
	CHECK(allAt(h, level, -6.0));
}

TEST_CASE("A live change of a disabled node is stored, and its enable is seeded from it", "[retune]") {
	reset();
	Harness h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add({.disabled = true});
	h.blockReady();
	REQUIRE(h.phase() == "disabled");
	REQUIRE(h.change({{"level", -12.0}}) == NodeOutcome::stored);
	CHECK(h.phase() == "disabled");
	CHECK(configures == 0);
	CHECK(liveLevel(h) == bits(-12.0));
	h.enable();
	REQUIRE(h.phase() == "running");
	h.process(256);
	CHECK(allAt(h, level, -12.0));
}

TEST_CASE("A retune raising a live count makes its member active, playing its file value from its first sample",
          "[retune]") {
	reset();
	Harness h;
	running(h, {{"bands", 1}, {"band.2", -6.0}});
	CHECK(allAt(h, band, 99.0, 1)); // inactive
	REQUIRE(h.change({{"bands", 2}, {"band.2", -6.0}}) == NodeOutcome::retune);
	h.process(256);
	CHECK(allAt(h, band, -6.0, 1));
	CHECK(allAt(h, band, 0.0, 0));
	CHECK(allAt(h, band, 99.0, 2));
}

TEST_CASE("A restart seeds its new configuration from the node's live values, not its settings'", "[retune]") {
	reset();
	Harness h;
	running(h);
	h.live<levelRow>(-20.0); // moved as a gesture's tick moves it: the settings still say -6
	REQUIRE(h.change({{"size", 2}}) == NodeOutcome::restart);
	REQUIRE(h.phase() == "running");
	CHECK(configures == 2);
	h.process(256);
	CHECK(allAt(h, level, -20.0));
}

TEST_CASE("live() runs the type's check on the live values with its move in place, and a refusal moves nothing",
          "[retune]") {
	reset();
	Harness h;
	running(h, {{"bands", 4}});
	CHECK_THROWS_WITH(h.live<levelRow>(20.0), Catch::Contains("must be 12 dB or less with four bands"));
	CHECK(liveLevel(h) == bits(-6.0));
	h.live<levelRow>(10.0);
	CHECK(liveLevel(h) == bits(10.0));
	h.live<bandsRow>(3);
	h.live<levelRow>(20.0); // three bands: allowed
	CHECK(liveLevel(h) == bits(20.0));
}
