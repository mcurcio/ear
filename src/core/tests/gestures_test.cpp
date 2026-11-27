// Gestures (docs/architecture.md#gestures): begin, ticks, end, abort and release on the main loop, the silence timer on
// the main loop's clock moved by the test. Over a fake live target for the rules, and over a real NodeHost in the type
// harness for the type's check with two hands in place and for the slots an end or an abort leaves.
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/decimal_text.hpp"
#include "ear/core/gestures.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

namespace gestures_test {

using namespace ear;
using namespace std::chrono_literals;

/// Main-loop timers on a clock the test moves.
struct FakeTimers final : Timers {
	struct Armed {
		std::chrono::milliseconds at;
		std::move_only_function<void()> due;
		bool cancelled = false;
	};
	struct Handle final : Timer {
		std::shared_ptr<Armed> armed;
		explicit Handle(std::shared_ptr<Armed> a) : armed(std::move(a)) {}
		~Handle() override { armed->cancelled = true; }
	};
	std::chrono::milliseconds clock{0};
	std::vector<std::shared_ptr<Armed>> armed;
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override {
		auto timer = std::make_shared<Armed>(Armed{clock + delay, std::move(due)});
		armed.push_back(timer);
		return std::make_unique<Handle>(timer);
	}
	std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::time_point{clock}; }
	void advance(std::chrono::milliseconds by) {
		clock += by;
		auto due = armed;
		for (const auto& timer : due)
			if (!timer->cancelled && timer->due && timer->at <= clock) {
				auto run = std::move(timer->due);
				timer->due = nullptr;
				run();
			}
	}
};

/// Live values per node, their committed values, and a refusal of any value above `ceiling`.
struct FakeLive final : LiveTarget {
	std::map<NodeName, std::vector<std::uint64_t>> live;
	std::map<NodeName, std::vector<std::uint64_t>> committed;
	double ceiling = 1e9;
	int writes = 0;
	void setLive(const NodeName& node, std::span<const LiveWrite> w) override {
		++writes;
		for (const LiveWrite& write : w) live.at(node).at(write.member) = write.bits;
	}
	std::expected<void, LiveRefusal> checkLive(const NodeName&, std::span<const LiveWrite> w) const override {
		for (const LiveWrite& write : w)
			if (std::bit_cast<double>(write.bits) > ceiling)
				return std::unexpected(LiveRefusal{"gainDb", "gainDb: must be at most the ceiling"});
		return {};
	}
	std::optional<std::uint64_t> committedLive(const NodeName& node, std::size_t member) const override {
		const auto found = committed.find(node);
		if (found == committed.end() || member >= found->second.size()) return std::nullopt;
		return found->second[member];
	}
	double at(const NodeName& node, std::size_t member = 0) const {
		return std::bit_cast<double>(live.at(node).at(member));
	}
};

struct Notes final : GestureListener {
	std::vector<GestureNote> told;
	void gestureChanged(const GestureNote& note) override { told.push_back(note); }
	std::size_t count(GesturePhase phase) const {
		return static_cast<std::size_t>(std::ranges::count(told, phase, &GestureNote::phase));
	}
	const GestureNote& last() const { return told.back(); }
};

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

struct Fixture {
	FakeTimers timers;
	FakeLive live;
	Notes notes;
	std::vector<GestureEnding> releases;
	Gestures gestures{live, timers};
	Fixture() {
		live.live["amp"] = {bits(-6.0), bits(0.0)};
		live.committed["amp"] = {bits(-6.0), bits(0.0)};
		live.live["amp2"] = {bits(-6.0)};
		live.committed["amp2"] = {bits(-6.0)};
		gestures.subscribe(notes);
		gestures.releaseThrough([this](GestureEnding ending) { releases.push_back(std::move(ending)); });
	}
	std::string begin(SessionToken session = 1, std::vector<GesturePair> reach = {{{"amp", "gainDb"}, 0}}) {
		return gestures.open(GestureOpen{session, std::move(reach), -90.0, 24.0, 1, -6.0});
	}
};

} // namespace gestures_test
using namespace gestures_test;

TEST_CASE("A gesture's id is 32 hex digits, never reused, and its begin is told with its reach and value",
          "[gestures]") {
	Fixture f;
	const std::string a = f.begin();
	const std::string b = f.begin(2, {{{"amp2", "gainDb"}, 0}});
	CHECK(a.size() == 32);
	CHECK(std::ranges::all_of(a, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }));
	CHECK(a != b);
	REQUIRE(f.notes.count(GesturePhase::begin) == 2);
	CHECK(f.notes.told[0].value == -6.0);
	CHECK(f.notes.told[0].reach == std::vector<GestureKey>{{"amp", "gainDb"}});
	CHECK(f.live.writes == 0); // a begin writes nothing
}

TEST_CASE("Ticks move every reached slot, a stale seq or an equal value writes nothing, and moves are paced",
          "[gestures]") {
	Fixture f;
	const std::string g = f.begin(1, {{{"amp", "gainDb"}, 0}, {{"amp2", "gainDb"}, 0}});
	f.gestures.move(g, 1, 1, -4.0);
	CHECK(f.live.at("amp") == -4.0);
	CHECK(f.live.at("amp2") == -4.0);
	CHECK(f.notes.count(GesturePhase::change) == 1);
	f.gestures.move(g, 1, 1, -3.0); // not above the last seq taken
	CHECK(f.live.at("amp") == -4.0);
	const int writes = f.live.writes;
	f.gestures.move(g, 1, 2, -4.0); // equal
	CHECK(f.live.writes == writes);
	f.gestures.move(g, 2, 3, -1.0); // another session's ticks are never taken
	CHECK(f.live.at("amp") == -4.0);
	f.gestures.move(g, 1, 4, -3.0); // within the pace: the slots move, the change is told later, the newest
	f.gestures.move(g, 1, 5, -2.0);
	CHECK(f.live.at("amp") == -2.0);
	CHECK(f.notes.count(GesturePhase::change) == 1);
	f.timers.advance(gestureChangePace);
	REQUIRE(f.notes.count(GesturePhase::change) == 2);
	CHECK(f.notes.last().value == -2.0);
}

TEST_CASE("A refused tick keeps the last accepted value and is told to its owner at most once a second", "[gestures]") {
	Fixture f;
	f.live.ceiling = 0.0;
	const std::string g = f.begin();
	f.gestures.move(g, 1, 1, -1.0);
	f.gestures.move(g, 1, 2, 3.0);
	f.gestures.move(g, 1, 3, 4.0);
	CHECK(f.live.at("amp") == -1.0);
	REQUIRE(f.notes.count(GesturePhase::tickRefused) == 1);
	CHECK(f.notes.last().code == "invalid-settings");
	CHECK(f.notes.last().key == "gainDb");
	f.timers.advance(gestureRefusalPace);
	f.gestures.move(g, 1, 4, 5.0);
	CHECK(f.notes.count(GesturePhase::tickRefused) == 2);
}

TEST_CASE("A silent gesture is released stale at staleAfter with its last accepted value, re-armed by an equal tick",
          "[gestures]") {
	Fixture f;
	f.live.ceiling = 0.0;
	const std::string g = f.begin();
	f.gestures.move(g, 1, 1, -3.46);
	f.timers.advance(20s);
	f.gestures.move(g, 1, 2, -3.46); // equal: writes nothing, re-arms
	f.timers.advance(20s);
	CHECK(f.releases.empty());
	CHECK(f.gestures.heldAt({"amp", "gainDb"})->silent == 20s);
	f.gestures.move(g, 1, 3, 9.0); // refused, but it is a message: re-armed
	f.timers.advance(29s);
	CHECK(f.releases.empty());
	f.timers.advance(1s);
	REQUIRE(f.releases.size() == 1);
	CHECK(f.releases[0].gesture == g);
	CHECK(f.releases[0].reason == "stale");
	CHECK(f.releases[0].value == -3.5); // the last accepted, never the refused one, rounded to one place
	CHECK(f.releases[0].at == GestureKey{"amp", "gainDb"});
	f.gestures.move(g, 1, 4, -1.0); // ending: no tick taken again
	CHECK(f.live.at("amp") == -3.46);
	f.gestures.close(g, -3.5);
	CHECK(f.notes.last().phase == GesturePhase::end);
	CHECK(f.notes.last().reason == "stale");
	CHECK_FALSE(f.gestures.heldAt({"amp", "gainDb"}));
}

TEST_CASE("A closed session releases its gestures not already ending", "[gestures]") {
	Fixture f;
	const std::string a = f.begin(1);
	const std::string b = f.begin(1, {{{"amp2", "gainDb"}, 0}});
	const std::string c = f.begin(2, {{{"amp", "trimDb.1"}, 1}});
	REQUIRE(f.gestures.end(b, -2.0, 1));
	f.gestures.sessionClosed(1);
	REQUIRE(f.releases.size() == 1);
	CHECK(f.releases[0].gesture == a);
	CHECK(f.releases[0].reason == "session-closed");
	CHECK(f.gestures.openGestures().size() == 3); // released, still open until committed or aborted
	static_cast<void>(c);
}

TEST_CASE("An end rounds to the reach's decimals and marks it ending, and a second end is unknown-gesture",
          "[gestures]") {
	Fixture f;
	const std::string g = f.begin();
	f.gestures.move(g, 1, 1, -6.04);
	auto ending = f.gestures.end(g, std::nullopt, 1);
	REQUIRE(ending);
	CHECK(ending->value == -6.0); // one decimal: -6.04 commits -6, which ear.conf already holds
	CHECK_FALSE(f.gestures.end(g, -1.0, 1));
	CHECK(f.gestures.end("0123", -1.0, 1).error() == GestureRefusal::unknownGesture);
	f.gestures.close(g, -6.0);
	CHECK(f.live.at("amp") == -6.0); // the slot's target back on the committed value, not the last tick
	CHECK(f.notes.last().phase == GesturePhase::end);
	CHECK(f.notes.last().value == -6.0);
}

TEST_CASE("An end by another session is told to both, and a refused release aborts to the committed value",
          "[gestures]") {
	Fixture f;
	const std::string g = f.begin(1);
	f.gestures.move(g, 1, 1, -2.0);
	REQUIRE(f.gestures.end(g, -2.0, 7));
	f.gestures.abort(g, "refused");
	CHECK(f.notes.last().phase == GesturePhase::aborted);
	CHECK(f.notes.last().reason == "refused");
	CHECK(f.notes.last().by == SessionToken{7});
	CHECK(f.notes.last().value == -6.0);
	CHECK(f.notes.last().handValue == -2.0);
	CHECK(f.live.at("amp") == -6.0);
}

TEST_CASE("A release whose commit is refused tells no end, then its abort refused at the committed value, and is gone",
          "[gestures]") {
	Fixture f;
	const std::string g = f.begin(1);
	f.gestures.move(g, 1, 1, -2.0);
	REQUIRE(f.live.at("amp") == -2.0);
	f.notes.told.clear();
	f.gestures.sessionClosed(1);
	REQUIRE(f.releases.size() == 1);
	CHECK(f.releases[0].reason == "session-closed");
	CHECK(f.releases[0].value == -2.0);
	// Released, its commit not yet answered: nothing told, the hand still playing and held.
	CHECK(f.notes.told.empty());
	CHECK(f.live.at("amp") == -2.0);
	CHECK(f.gestures.heldAt({"amp", "gainDb"}));
	// The keeper refuses the commit and aborts it (GraphFileKeeper::commit): one note, the abort, and no end ever.
	f.gestures.abort(g, "refused");
	REQUIRE(f.notes.told.size() == 1);
	CHECK(f.notes.last().phase == GesturePhase::aborted);
	CHECK(f.notes.last().reason == "refused");
	CHECK(f.notes.last().owner == SessionToken{1});
	CHECK_FALSE(f.notes.last().by);
	CHECK(f.notes.last().value == -6.0);
	CHECK(f.notes.last().handValue == -2.0);
	CHECK(f.live.at("amp") == -6.0);
	CHECK_FALSE(f.gestures.heldAt({"amp", "gainDb"}));
	CHECK(f.gestures.openGestures().empty());
	// The commit's answer, had it come after all, closes nothing; its stale timer fires nothing.
	f.gestures.close(g, -2.0);
	f.timers.advance(gestureStaleAfter);
	CHECK(f.notes.told.size() == 1);
	CHECK(f.releases.size() == 1);
	CHECK(f.live.at("amp") == -6.0);
}

TEST_CASE("With nobody to commit releases, a release aborts refused at once", "[gestures]") {
	FakeTimers timers;
	FakeLive live;
	live.live["amp"] = {bits(-6.0)};
	live.committed["amp"] = {bits(-6.0)};
	Notes notes;
	Gestures gestures{live, timers};
	gestures.subscribe(notes);
	const std::string g = gestures.open(GestureOpen{1, {{{"amp", "gainDb"}, 0}}, -90.0, 24.0, 1, -6.0});
	gestures.move(g, 1, 1, -2.0);
	gestures.sessionClosed(1);
	CHECK(notes.last().phase == GesturePhase::aborted);
	CHECK(notes.last().reason == "refused");
	CHECK(notes.count(GesturePhase::end) == 0);
	CHECK(live.at("amp") == -6.0);
	CHECK(gestures.openGestures().empty());
}

TEST_CASE("Cancel aborts at once, and an abort told by a change settles its slots only at settle()", "[gestures]") {
	Fixture f;
	const std::string a = f.begin(1);
	f.gestures.move(a, 1, 1, -1.0);
	REQUIRE(f.gestures.cancel(a, 1));
	CHECK(f.notes.last().reason == "cancelled");
	CHECK(f.live.at("amp") == -6.0);
	CHECK(f.gestures.cancel(a, 1).error() == GestureRefusal::unknownGesture);

	const std::string b = f.begin(1);
	f.gestures.move(b, 1, 1, -1.0);
	f.gestures.abort(b, "hand-edit", true, false);
	CHECK_FALSE(f.notes.last().value);
	CHECK(f.live.at("amp") == -1.0);
	f.live.committed["amp"][0] = bits(-10.0); // the change committed
	f.gestures.settle();
	CHECK(f.live.at("amp") == -10.0);
}

TEST_CASE("drop forgets every gesture and tells nothing, and a timer armed before fires nothing", "[gestures]") {
	Fixture f;
	f.begin();
	const std::size_t told = f.notes.told.size();
	f.gestures.drop();
	f.timers.advance(gestureStaleAfter);
	CHECK(f.notes.told.size() == told);
	CHECK(f.releases.empty());
	CHECK(f.gestures.openGestures().empty());
}

TEST_CASE("roundedDecimal rounds half away from zero on the shortest text", "[gestures]") {
	CHECK(roundedDecimal(-6.04, 1) == -6.0);
	CHECK(roundedDecimal(-6.05, 1) == -6.1);
	CHECK(roundedDecimal(2.675, 2) == 2.68); // its binary value is below 2.675
	CHECK(roundedDecimal(9.96, 1) == 10.0);
	CHECK(roundedDecimal(-0.04, 1) == 0.0);
	CHECK_FALSE(std::signbit(roundedDecimal(-0.04, 1)));
	CHECK(roundedDecimal(0.5, 0) == 1.0);
	CHECK(roundedDecimal(-3.5, 3) == -3.5);
}

namespace gestures_test {

inline constexpr auto lowRow =
	setting::defaulted("lowHz", setting::decimal(20.0, 20000.0, "Hz"), 100.0).live(setting::ramp(20));
inline constexpr auto highRow =
	setting::defaulted("highHz", setting::decimal(20.0, 20000.0, "Hz"), 1000.0).live(setting::ramp(20));
inline constexpr auto midRow =
	setting::defaulted("midHz", setting::decimal(20.0, 20000.0, "Hz"), 100.0).live(setting::ramp(20));
inline constexpr SettingsTable<lowRow, highRow, midRow> bandTable{"band"};
inline constexpr auto out = bank::output<"out">;
inline constexpr TypeSpec bandSpec{.settings = bandTable, .banks = Banks<out>{}};
struct BandCode {
	void process(Io<bandSpec> io, const GraphCycle&) noexcept {
		const Smoothed low = io.param<lowRow>();
		for (std::uint32_t i = 0; i < io.frames(); ++i) io[out][0][i] = low[i];
	}
};
struct Band {
	static Members<bandSpec> ports(const CheckedOf<bandSpec>&) { return {out(members(1))}; }
	static std::expected<void, std::string> check(const CheckedOf<bandSpec>& s) {
		if (s.liveValue<lowRow>() >= s.liveValue<highRow>()) return std::unexpected("lowHz: must be below highHz");
		if (s.liveValue<midRow>() >= s.liveValue<highRow>()) return std::unexpected("midHz: must be below highHz");
		return {};
	}
	Configured configure(CreateOf<bandSpec>& cx, const CheckedOf<bandSpec>&) { return cx.applied(BandCode{}); }
};
using BandHarness = testing::TypeHarness<bandSpec, Band>;

struct OverHost {
	BandHarness h;
	Notes notes;
	std::unique_ptr<Gestures> gestures;
	OverHost() {
		REQUIRE_FALSE(h.parse(NodeSettings::object()));
		h.add();
		gestures = std::make_unique<Gestures>(h->liveTarget(), h->timers());
		gestures->subscribe(notes);
	}
	std::string begin(SessionToken session, const char* key, double committed) {
		const std::uint32_t member = bandTable.liveIndexOf(key).value();
		return gestures->open(GestureOpen{session, {{{"node", key}, member}}, 20.0, 20000.0, 0, committed});
	}
	double liveAt(const char* key) {
		return std::bit_cast<double>(h->liveValues().at(bandTable.liveIndexOf(key).value()));
	}
};

} // namespace gestures_test

TEST_CASE(
	"Two gestures on rows one check binds are each checked with the other hand in place, so the pair never crosses",
	"[gestures]") {
	OverHost f;
	const std::string low = f.begin(1, "lowHz", 100.0);
	const std::string high = f.begin(2, "highHz", 1000.0);
	f.gestures->move(high, 2, 1, 400.0);
	f.gestures->move(low, 1, 1, 300.0);
	CHECK(f.liveAt("lowHz") == 300.0);
	f.gestures->move(high, 2, 2, 250.0); // would cross the other hand's 300
	CHECK(f.liveAt("highHz") == 400.0);
	REQUIRE(f.notes.count(GesturePhase::tickRefused) == 1);
	CHECK(f.notes.last().owner == SessionToken{2});
	CHECK(f.notes.last().key == "highHz");
	CHECK(f.notes.last().detail == "lowHz: must be below highHz");
	f.gestures->move(low, 1, 2, 500.0); // the first to cross is refused
	CHECK(f.liveAt("lowHz") == 300.0);
	f.h->advance(gestureRefusalPace);
	f.gestures->move(low, 1, 3, 30000.0); // outside the row's range
	CHECK(f.notes.last().detail == "lowHz: must be a number of Hz from 20 to 20000");
}

TEST_CASE("Over NodeHost an abort returns the slot to the committed settings' value", "[gestures]") {
	OverHost f;
	const std::string low = f.begin(1, "lowHz", 100.0);
	f.gestures->move(low, 1, 1, 300.0);
	CHECK(f.liveAt("lowHz") == 300.0);
	REQUIRE(f.gestures->cancel(low, 1));
	CHECK(f.liveAt("lowHz") == 100.0);
}

TEST_CASE(
	"A hand valid only beside another hand is aborted crossed with it when that one goes back, so no pair crosses",
	"[gestures]") {
	OverHost f;
	const std::string low = f.begin(1, "lowHz", 100.0);
	const std::string high = f.begin(2, "highHz", 1000.0);
	f.gestures->move(high, 2, 1, 5000.0);
	f.gestures->move(low, 1, 1, 2000.0); // above the committed highHz, below the hand's
	REQUIRE(f.liveAt("lowHz") == 2000.0);
	REQUIRE(f.gestures->cancel(high, 2));
	CHECK(f.liveAt("highHz") == 1000.0);
	CHECK(f.liveAt("lowHz") == 100.0);
	REQUIRE(f.notes.count(GesturePhase::aborted) == 2);
	CHECK(f.notes.last().gesture == low);
	CHECK(f.notes.last().reason == "crossed");
	CHECK(f.notes.last().value == 100.0);
	CHECK(f.gestures->openGestures().empty());
}

TEST_CASE("A retune crossing a held hand aborts that gesture crossed before it lands, and one that does not keeps it",
          "[gestures]") {
	OverHost f;
	const std::string low = f.begin(1, "lowHz", 100.0);
	f.gestures->move(low, 1, 1, 800.0);
	const std::uint32_t highMember = bandTable.liveIndexOf("highHz").value();
	const LiveWrite clear{highMember, bits(900.0)};
	f.gestures->abortCrossing("node", std::span(&clear, 1));
	CHECK(f.gestures->openGestures().size() == 1);
	CHECK(f.liveAt("lowHz") == 800.0);
	const LiveWrite crossing{highMember, bits(500.0)};
	f.gestures->abortCrossing("node", std::span(&crossing, 1), low); // its own end's change keeps it
	CHECK(f.gestures->openGestures().size() == 1);
	f.gestures->abortCrossing("node", std::span(&crossing, 1));
	CHECK(f.gestures->openGestures().empty());
	CHECK(f.notes.last().reason == "crossed");
	CHECK(f.liveAt("lowHz") == 100.0);
}

TEST_CASE("A hand a related change aborted counts at its committed value in that change's crossing check",
          "[gestures]") {
	OverHost f;
	const std::string high = f.begin(1, "highHz", 1000.0);
	const std::string low = f.begin(2, "lowHz", 100.0);
	f.gestures->move(high, 1, 1, 900.0);
	f.gestures->move(low, 2, 1, 800.0); // below the hand's 900
	// A related change aborts the high hand, its slot settling only once the change committed, and retunes midHz to
	// 950: above the leaving hand's 900, below the committed 1000 the slot goes back to.
	f.gestures->abort(high, "hand-edit", true, false);
	REQUIRE(f.liveAt("highHz") == 900.0);
	const LiveWrite mid{bandTable.liveIndexOf("midHz").value(), bits(950.0)};
	f.gestures->abortCrossing("node", std::span(&mid, 1));
	CHECK(f.gestures->openGestures().size() == 1); // the low hand never crossed what will play
	CHECK(f.liveAt("lowHz") == 800.0);
	f.gestures->settle();
	CHECK(f.liveAt("highHz") == 1000.0);
	CHECK(f.liveAt("lowHz") == 800.0);
	CHECK(f.gestures->openGestures().size() == 1);
	// With the leaving hand's own key retuned too, the change's value is judged, never the committed one.
	const std::string again = f.begin(1, "highHz", 1000.0);
	f.gestures->move(again, 1, 1, 900.0);
	f.gestures->abort(again, "hand-edit", true, false);
	const std::array<LiveWrite, 2> both{LiveWrite{bandTable.liveIndexOf("highHz").value(), bits(700.0)},
	                                    mid}; // 800 and 950 cross 700
	f.gestures->abortCrossing("node", both);
	CHECK(f.gestures->openGestures().empty());
	CHECK(f.notes.last().reason == "crossed");
}
