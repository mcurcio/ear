// What a node's code reports from process (docs/writing-a-node-type.md#reporting-from-the-code;
// docs/architecture.md#the-realtime-channels): rt fields, events and log codes through Io, into the configuration's
// realtime channels, drained on the main loop, on a test type whose code runs what each test scripts, by the type
// harness over a real NodeHost.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "allocation_count.hpp"
#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

namespace realtime_test {

using namespace ear;
using namespace std::chrono_literals;
using testing::SeenDiagnostic;

constexpr auto gainRow = setting::optional("gain", setting::number(0, 100)).inPlace();
constexpr SettingsTable<gainRow> table{"realtimer"};
constexpr auto out = bank::output<"out">;
constexpr std::string_view modes[] = {"quiet", "loud"};
constexpr auto level = status::number<"level">(writer::rt, 0, 100).flapsAbove(3, 1000);
constexpr auto clipping = status::flag<"clipping">(writer::rt);
constexpr auto mode = status::choice<"mode">(writer::rt, modes);
constexpr auto gainDb = status::decimal<"gainDb">(writer::rt, -60, 12, "dB", 1);
constexpr auto modeArg = arg::choice<"mode">(modes);
constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "clipped {samples} samples in {mode}",
                                              arg::number<"samples">(0, 100000), modeArg);
constexpr auto overrun = log::code<"overrun">(LogLevel::debug, "overrun by {frames} frames at {load}",
                                              arg::number<"frames">(0, 1 << 20), arg::decimal<"load">(2));
constexpr TypeSpec spec{.settings = table,
                        .banks = Banks<out>{},
                        .status = StatusTable<level, clipping, mode, gainDb>{},
                        .events = EventTable<clipped>{},
                        .logCodes = LogCodes<overrun>{}};

/// What the code does in each cycle, after it wrote its outputs: set by each test, run on the realtime thread (here
/// the test's).
using Script = void (*)(const Io<spec>& io);
Script script = nullptr;
/// Whether the code counts its own allocations while it runs the script (allocation_count.hpp).
bool counting = false;
/// The start of the last cycle the code ran.
std::int64_t lastStart = 0;

struct Code {
	void process(Io<spec> io, const GraphCycle& cycle) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
		lastStart = cycle.startNs;
		if (counting) ear::testing::countAllocationsOnThisThread(true);
		if (script != nullptr) script(io);
		if (counting) ear::testing::countAllocationsOnThisThread(false);
	}
};
struct Realtimer {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) { return cx.applied(Code{}); }
	/// A change of gain in place, answered with a new code: swapped in at the commit point.
	Changed configure(ChangeOf<spec>& cx, const CheckedOf<spec>&, const CheckedOf<spec>&) { return cx.applied(Code{}); }
};
using Harness = testing::TypeHarness<spec, Realtimer>;

/// A running node, nothing scripted yet.
void running(Harness& h) {
	script = nullptr;
	counting = false;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "running");
	h->clearReported();
}

std::vector<SeenDiagnostic> diagnosed(Harness& h, std::string_view code, std::string_view subject) {
	std::vector<SeenDiagnostic> found;
	for (const SeenDiagnostic& seen : h->diagnostics())
		if (seen.code == code && seen.subject == subject) found.push_back(seen);
	return found;
}

std::vector<LogEntry> logged(Harness& h) {
	std::vector<LogEntry> lines;
	for (const LogEntry& entry : h->logs())
		if (entry.topic == "overrun") lines.push_back(entry);
	return lines;
}

TEST_CASE("Status set and events raised in process reach the wire in the order the code wrote them", "[realtime]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) {
		io.set<level>(5);
		io.set<mode>(mode("loud"));
		io.raise<clipped>(3, modeArg("loud"));
		io.set<clipping>(true);
		io.set<gainDb>(-3.5);
	};
	h.process();
	CHECK(h->reported() == std::vector<std::string>{"changed level=5 mode=loud",
	                                                "event clipped: clipped 3 samples in loud",
	                                                "changed clipping=true gainDb=-3.50"});
	CHECK(h.status<level>() == StatusValue(std::int64_t{5}));
	CHECK(h.status<gainDb>() == StatusValue(-3.5));
	REQUIRE(h.events().size() == 1);
	CHECK(h.events()[0].args == std::vector<std::pair<std::string, StatusValue>>{{"samples", std::int64_t{3}},
	                                                                             {"mode", std::string("loud")}});
	// Each change once, as standard output writes it.
	REQUIRE(h.statusRecord().size() == 4);
	CHECK(h.statusRecord()[0].field == "level");
	CHECK(h.statusRecord()[0].from == StatusValue(std::int64_t{0}));
	CHECK(h.statusRecord()[0].to == StatusValue(std::int64_t{5}));
}

TEST_CASE("A value the code sets again is no change, and no record", "[realtime]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) {
		io.set<level>(0); // its initial value
		io.set<clipping>(false);
	};
	h.process();
	h.process();
	CHECK(h->reported().empty());
	CHECK(h.realtimeRings() == 0);
	script = [](const Io<spec>& io) { io.set<level>(9); };
	h.process();
	h.process();
	CHECK(h->reported() == std::vector<std::string>{"changed level=9"});
	CHECK(h.statusRecord().size() == 1);
}

TEST_CASE("An event's time is its cycle's start", "[realtime]") {
	Harness h;
	running(h);
	for (int i = 0; i < 3; ++i) h.process();
	script = [](const Io<spec>& io) { io.raise<clipped>(1, modeArg("quiet")); };
	h.process();
	REQUIRE(h.events().size() == 1);
	CHECK(lastStart > 0);
	CHECK(h.events()[0].at == std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastStart)));
}

TEST_CASE("A log code is a log line of the node, its source the type and its topic the code", "[realtime]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) { io.log<overrun>(12, 0.5); };
	h.process();
	const std::vector<LogEntry> lines = logged(h);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].level == LogLevel::debug);
	CHECK(lines[0].source == "realtimer");
	CHECK(lines[0].text == "overrun by 12 frames at 0.50");
	CHECK(lines[0].node == NodeName("node"));
}

TEST_CASE("The ring holds 256 records: the rest are dropped and counted, and a field's word still lands",
          "[realtime][bound]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) {
		for (int i = 0; i < 300; ++i) io.raise<clipped>(i, modeArg("quiet"));
		io.set<level>(7); // its record has no room; its word is sampled
	};
	h.process();
	CHECK(h.status<level>() == StatusValue(std::int64_t{7}));
	const auto dropped = diagnosed(h, "ear.records-dropped", "realtime");
	REQUIRE(dropped.size() == 1);
	CHECK(dropped[0].fields ==
	      std::vector<std::pair<std::string, std::int64_t>>{{"count", 45}, {"status", 1}, {"events", 44}, {"logs", 0}});
	// The events that had room are paced as any node's: 32 at once.
	CHECK(h.events().size() == 32);
	CHECK(h.events()[0].msg == "clipped 0 samples in quiet");
}

TEST_CASE("Log records are refused once 192 records are in use, while status and events still pass",
          "[realtime][bound]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) {
		for (int i = 0; i < 250; ++i) io.log<overrun>(i, 1.0);
		for (int i = 0; i < 10; ++i) io.raise<clipped>(i, modeArg("loud"));
		io.set<clipping>(true);
	};
	h.process();
	CHECK(logged(h).size() == 192);
	CHECK(h.events().size() == 10);
	CHECK(h.status<clipping>() == StatusValue(true));
	const auto dropped = diagnosed(h, "ear.records-dropped", "realtime");
	REQUIRE(dropped.size() == 1);
	CHECK(dropped[0].fields ==
	      std::vector<std::pair<std::string, std::int64_t>>{{"count", 58}, {"status", 0}, {"events", 0}, {"logs", 58}});
}

TEST_CASE("A field's transitions are counted between samples, those the ring had no room for too",
          "[realtime][flapping]") {
	Harness h({.holdStatus = true});
	running(h);
	// The ring full of events, then level changes five times: none of its records has room, and its counter holds
	// every change, so the field is judged flapping as its fourth change crosses three a second.
	script = [](const Io<spec>& io) {
		for (int i = 0; i < 256; ++i) io.raise<clipped>(i, modeArg("quiet"));
		for (int value = 1; value <= 5; ++value) io.set<level>(value);
	};
	h.process();
	script = nullptr;
	h.drain();
	CHECK(h.status<level>() == StatusValue(std::int64_t{5}));
	const auto flapping = diagnosed(h, "ear.status-flapping", "level");
	REQUIRE(flapping.size() == 1);
	CHECK(flapping[0].active);
	CHECK(flapping[0].fields ==
	      std::vector<std::pair<std::string, std::int64_t>>{{"transitions", 4}, {"windowMs", 1000}});
}

TEST_CASE("The bell rings after a cycle the code wrote in, at most once until the drain, and never for an idle code",
          "[realtime][bell]") {
	Harness h({.holdStatus = true});
	running(h);
	for (int i = 0; i < 3; ++i) h.process();
	CHECK(h.realtimeRings() == 0);
	script = [](const Io<spec>& io) {
		static std::int64_t value = 0;
		io.set<level>(++value % 100);
	};
	for (int i = 0; i < 3; ++i) h.process();
	CHECK(h.realtimeRings() == 1);
	h.drain();
	h.process();
	CHECK(h.realtimeRings() == 2);
}

TEST_CASE("A code swapped in writes to the same channels: an rt field keeps its last value", "[realtime][swap]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) { io.set<level>(9); };
	h.process();
	REQUIRE(h.status<level>() == StatusValue(std::int64_t{9}));
	h.change({{"gain", 10}});
	REQUIRE(h->swaps() == 1);
	REQUIRE(h.phase() == "running");
	// The new code writes the value the old one last wrote: no change.
	h.process();
	h.process();
	CHECK(h.status<level>() == StatusValue(std::int64_t{9}));
	CHECK(h.statusRecord().size() == 1);
	script = [](const Io<spec>& io) { io.set<level>(10); };
	h.process();
	REQUIRE(h.statusRecord().size() == 2);
	CHECK(h.statusRecord()[1].from == StatusValue(std::int64_t{9}));
	CHECK(h.statusRecord()[1].to == StatusValue(std::int64_t{10}));
}

TEST_CASE("A restart's new configuration starts its rt fields at their initial values", "[realtime]") {
	Harness h;
	running(h);
	script = [](const Io<spec>& io) { io.set<level>(9); };
	h.process();
	REQUIRE(h.status<level>() == StatusValue(std::int64_t{9}));
	script = nullptr;
	REQUIRE(h.restart());
	REQUIRE(h.phase() == "running");
	CHECK(h.status<level>() == StatusValue(std::int64_t{0}));
	// The new configuration's first write of 9 is a change again.
	script = [](const Io<spec>& io) { io.set<level>(9); };
	h.process();
	CHECK(h.status<level>() == StatusValue(std::int64_t{9}));
}

TEST_CASE("A value outside its declaration fails the node with the rule, and nothing is written", "[realtime]") {
	Harness h;
	running(h);
	SECTION("a field's number outside its range") {
		script = [](const Io<spec>& io) { io.set<level>(101); };
		h.process();
		REQUIRE(h.health().reason);
		CHECK(h.health().reason->detail == "status level: 101 is outside 0 to 100");
	}
	SECTION("a decimal outside its range") {
		script = [](const Io<spec>& io) { io.set<gainDb>(12.5); };
		h.process();
		REQUIRE(h.health().reason);
		CHECK(h.health().reason->detail == "status gainDb: 12.5 is outside -60.0 to 12.0");
	}
	SECTION("an event's number outside its range") {
		script = [](const Io<spec>& io) { io.raise<clipped>(-1, modeArg("loud")); };
		h.process();
		REQUIRE(h.health().reason);
		CHECK(h.health().reason->detail == "event clipped: samples -1 is outside 0 to 100000");
		CHECK(h.events().empty());
	}
	CHECK(h.health().state == NodeState::failed);
}

TEST_CASE("Process allocates nothing while it writes status, events and log lines", "[realtime][allocation]") {
	Harness h({.holdStatus = true});
	running(h);
	script = [](const Io<spec>& io) {
		static std::int64_t value = 0;
		value = (value + 1) % 100;
		io.set<level>(value);
		io.set<clipping>(value % 2 == 0);
		io.set<mode>(value % 2 == 0 ? mode("loud") : mode("quiet"));
		io.set<gainDb>(-1.5);
		io.raise<clipped>(value, modeArg("loud"));
		io.log<overrun>(value, 0.25);
	};
	counting = true;
	const std::uint64_t before = ear::testing::countedAllocations();
	for (int i = 0; i < 300; ++i) h.process(); // the ring fills: puts that are refused allocate nothing either
	const std::uint64_t allocations = ear::testing::countedAllocations() - before;
	counting = false;
	CHECK(allocations == 0);
	h.drain();
	CHECK(h.status<gainDb>() == StatusValue(-1.5));
}

} // namespace realtime_test
