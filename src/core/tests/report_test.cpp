// Report<spec> and the per-node queue, the drain and the tick (docs/writing-a-node-type.md#report;
// docs/architecture.md#status-and-events), on a test type whose loop's fields and event the test writes through the
// newest configuration's Report, run by the type harness over a real NodeHost.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/core/report.hpp"
#include "ear/testing/type_harness.hpp"

namespace report_test {

using namespace ear;
using namespace std::chrono_literals;
using testing::SeenDiagnostic;
using testing::SeenStatus;

constexpr SettingsTable<> table{"reporter"};
constexpr auto out = bank::output<"out">;
struct WorkerState {};
constexpr auto worker = loop::strand<"worker", WorkerState>;
constexpr std::string_view links[] = {"down", "up"};
constexpr auto link = status::choice<"link">(writer::loop(worker), links).display(Display::box);
constexpr auto count = status::number<"count">(writer::loop(worker), 0, 5000).flapsAbove(3, 1000);
constexpr auto name = status::text<"name">(writer::loop(worker), 8);
constexpr auto rate = status::number<"rate">(writer::loop(worker), 0, 100).flapsAbove(3, 1000);
constexpr auto hello =
	event::of<"hello">(EventSeverity::warn, "hello {who} at {level}", arg::text<"who">(), arg::decimal<"level">(1));
constexpr TypeSpec spec{.settings = table,
                        .banks = Banks<out>{},
                        .loops = Loops<worker>{},
                        .status = StatusTable<link, count, name, rate>{},
                        .events = EventTable<hello>{}};

/// The newest configuration's Report, and every one made.
std::vector<Report<spec>> reports;

struct Silence {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (const auto member : io[out]) std::ranges::fill(member, 0.0F);
	}
};
struct Reporter {
	explicit Reporter(NodeOf<spec>& node) { reports.push_back(node.report()); }
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) { return cx.applied(Silence{}, cx.state(worker)); }
};
using Harness = testing::TypeHarness<spec, Reporter>;

Report<spec>& newest() { return reports.back(); }

/// A running node, its reports from this test only.
void running(Harness& h) {
	reports.clear();
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "running");
	h->clearReported();
}

std::size_t indexOf(const std::vector<std::string>& lines, std::string_view line) {
	return static_cast<std::size_t>(std::ranges::find(lines, line) - lines.begin());
}

TEST_CASE("A node's status starts at every field's initial value", "[report]") {
	Harness h;
	running(h);
	CHECK(h.status<link>() == StatusValue{});
	CHECK(h.status<count>() == StatusValue(std::int64_t{0}));
	CHECK(h.status<name>() == StatusValue(std::string()));
	CHECK(h.status().size() == 4);
}

TEST_CASE("Status written before an event reaches the wire before it, and the event's msg fills its places",
          "[report]") {
	Harness h;
	running(h);
	newest().set<link>(link("up"));
	newest().set<count>(7);
	newest().raise<hello>("ana", 1.5);
	newest().set<name>("kitchen");
	h.drain();
	CHECK(h->reported() ==
	      std::vector<std::string>{"changed link=up count=7", "event hello: hello ana at 1.5", "changed name=kitchen"});
	REQUIRE(h.events().size() == 1);
	CHECK(h.events()[0].severity == EventSeverity::warn);
	CHECK(h.events()[0].args.size() == 2);
	CHECK(h.status<link>() == StatusValue(std::string("up")));
	// Standard output's record of each change, from the last record's value.
	REQUIRE(h.statusRecord().size() == 3);
	CHECK(h.statusRecord()[0].field == "link");
	CHECK(h.statusRecord()[0].from == StatusValue{});
	CHECK(h.statusRecord()[0].to == StatusValue(std::string("up")));
	CHECK(h.statusRecord()[1].from == StatusValue(std::int64_t{0}));
}

TEST_CASE("A write of the value a field holds is no change", "[report]") {
	Harness h;
	running(h);
	newest().set<count>(0);
	h.drain();
	CHECK(h->reported().empty());
	CHECK(h.statusRecord().empty());
}

TEST_CASE("A loss lands after the status written before it, and fails the node", "[report]") {
	Harness h;
	running(h);
	newest().set<count>(3);
	newest().handle().lost("the server went away");
	h.drain();
	const auto& lines = h->reported();
	REQUIRE(indexOf(lines, "changed count=3") < lines.size());
	REQUIRE(indexOf(lines, "node failed") < lines.size());
	CHECK(indexOf(lines, "changed count=3") < indexOf(lines, "node failed"));
	// Its configuration closed after its last values: the reset follows them.
	REQUIRE(indexOf(lines, "reset link=unknown count=0 name= rate=0") < lines.size());
	CHECK(indexOf(lines, "changed count=3") < indexOf(lines, "reset link=unknown count=0 name= rate=0"));
	CHECK(h.health().reason->detail == "the server went away");
}

TEST_CASE("A restart resets the status after the old configuration's last values, between stopping and configuring",
          "[report]") {
	Harness h;
	running(h);
	newest().set<link>(link("up"));
	h.drain();
	newest().set<count>(1); // written after the last tick: drained as the configuration closes, before its reset
	REQUIRE(h.restart());
	const auto& lines = h->reported();
	const std::size_t last = indexOf(lines, "changed link=up");
	const std::size_t stopping = indexOf(lines, "node stopping");
	const std::size_t reset = indexOf(lines, "reset link=unknown count=0 name= rate=0");
	const std::size_t configuring = indexOf(lines, "node configuring");
	REQUIRE(configuring < lines.size());
	CHECK(last < stopping);
	CHECK(stopping < indexOf(lines, "changed count=1"));
	CHECK(indexOf(lines, "changed count=1") < reset);
	CHECK(stopping < reset);
	CHECK(reset < configuring);
	CHECK(h.status<link>() == StatusValue{});
	// The new configuration's Report writes the new view.
	REQUIRE(reports.size() == 2);
	newest().set<count>(4);
	h.drain();
	CHECK(h.status<count>() == StatusValue(std::int64_t{4}));
}

TEST_CASE("A stop and a start keep the status and send no reset", "[report]") {
	Harness h;
	running(h);
	newest().set<link>(link("up"));
	h.drain();
	REQUIRE(h.stop());
	REQUIRE(h.phase() == "stopped");
	newest().set<count>(2); // a stopped node's loops still run, and its writes land
	REQUIRE(h.start());
	REQUIRE(h.phase() == "running");
	CHECK(h.status<link>() == StatusValue(std::string("up")));
	CHECK(h.status<count>() == StatusValue(std::int64_t{2}));
	CHECK(std::ranges::none_of(h->reported(), [](const std::string& line) { return line.starts_with("reset"); }));
}

TEST_CASE("A text is made safe and cut at its bound on the writer's thread", "[report]") {
	Harness h;
	running(h);
	newest().set<name>("a\tb\x01"
	                   "cdefghij");
	h.drain();
	// 8 bytes at most: "a b" then U+FFFD (3 bytes), then "…" (3 bytes) leaves room for none of the rest.
	CHECK(h.status<name>() == StatusValue(std::string("a b\xE2\x80\xA6")));
	CHECK(safeText("h\xC3\xA9llo", 5) == "h\xE2\x80\xA6");
	CHECK(safeText("h\xC3\xA9llo!", 6) == "h\xC3\xA9\xE2\x80\xA6");
	CHECK(safeText("bad \xFF byte", 64) == "bad \xEF\xBF\xBD byte");
	CHECK(safeText("ok", 2) == "ok");
	// C1 controls are controls; the line and paragraph separators break lines.
	CHECK(safeText("csi\xC2\x9B[2J nel\xC2\x85 nbsp\xC2\xA0", 64) == "csi\xEF\xBF\xBD[2J nel\xEF\xBF\xBD nbsp\xC2\xA0");
	CHECK(safeText("ls\xE2\x80\xA8ps\xE2\x80\xA9 dash\xE2\x80\x94", 64) == "ls ps  dash\xE2\x80\x94");
}

TEST_CASE("A value that breaks its declaration fails the node with the rule's text, and writes nothing", "[report]") {
	SECTION("a choice's text not among its values") {
		Harness h;
		running(h);
		newest().set<link>("sideways");
		h.drain();
		CHECK(h.phase() == "failed");
		CHECK(h.health().reason->detail == "status link: \"sideways\" is not one of its values");
	}
	SECTION("a number outside its range") {
		Harness h;
		running(h);
		newest().set<count>(5001);
		h.drain();
		CHECK(h.health().reason->detail == "status count: 5001 is outside 0 to 5000");
	}
}

TEST_CASE("The queue holds 1024 records: more are dropped and counted, a loss never is", "[report]") {
	Harness h({.holdStatus = true});
	running(h);
	for (std::int64_t i = 1; i <= 1100; ++i) newest().set<count>(i % 2 == 0 ? 2 : 1);
	newest().handle().lost("gone");
	h.drain();
	CHECK(h.phase() == "failed");
	const auto dropped = std::ranges::find_if(h->diagnostics(), [](const auto& seen) {
		return seen.code == "ear.records-dropped" && seen.subject == "queue";
	});
	REQUIRE(dropped != h->diagnostics().end());
	CHECK(dropped->fields == std::vector<std::pair<std::string, std::int64_t>>{{"count", 76}});
}

TEST_CASE("The tick drains at most once per 33 ms, armed only when a node wrote", "[report]") {
	Harness h({.holdStatus = true});
	running(h);
	newest().set<count>(1);
	h->settle();
	h->advance(0ms); // the first tick is due at once
	CHECK(h.status<count>() == StatusValue(std::int64_t{1}));
	newest().set<count>(2);
	h->settle();
	h->advance(32ms);
	CHECK(h.status<count>() == StatusValue(std::int64_t{1}));
	h->advance(1ms);
	CHECK(h.status<count>() == StatusValue(std::int64_t{2}));
	CHECK(h->reported() == std::vector<std::string>{"changed count=1", "changed count=2"});
}

/// The node's diagnostics of `code` and `subject`, in order.
std::vector<SeenDiagnostic> diagnosed(const Harness& h, std::string_view code, std::string_view subject = {}) {
	std::vector<SeenDiagnostic> found;
	for (const SeenDiagnostic& seen : const_cast<Harness&>(h)->diagnostics())
		if (seen.code == code && seen.subject == subject) found.push_back(seen);
	return found;
}

TEST_CASE("A field that changes more than its flapsAbove within the window flaps, and settles after a quiet window",
          "[report][pace]") {
	Harness h;
	running(h);
	for (std::int64_t value = 1; value <= 3; ++value) newest().set<count>(value);
	h.drain();
	CHECK(diagnosed(h, "ear.status-flapping", "count").empty()); // 3 is not above 3
	newest().set<count>(4);
	h.drain();
	auto flapping = diagnosed(h, "ear.status-flapping", "count");
	REQUIRE(flapping.size() == 1);
	CHECK(flapping[0].active);
	CHECK(flapping[0].count == 1);
	CHECK(flapping[0].fields ==
	      std::vector<std::pair<std::string, std::int64_t>>{{"transitions", 4}, {"windowMs", 1000}});
	h->advance(999ms);
	CHECK(diagnosed(h, "ear.status-flapping", "count").size() == 1);
	h->advance(1ms); // a whole window since it was last above
	flapping = diagnosed(h, "ear.status-flapping", "count");
	REQUIRE(flapping.size() == 2);
	CHECK_FALSE(flapping[1].active);
	CHECK(flapping[1].count == 1);
}

TEST_CASE("Two fields flap at once: one settles while the other keeps changing", "[report][pace]") {
	Harness h;
	running(h);
	for (std::int64_t value = 1; value <= 4; ++value) {
		newest().set<count>(value);
		newest().set<rate>(value);
	}
	h.drain();
	REQUIRE(diagnosed(h, "ear.status-flapping", "count").size() == 1);
	REQUIRE(diagnosed(h, "ear.status-flapping", "rate").size() == 1);
	for (std::int64_t value = 5; value <= 12; ++value) { // rate: 5 changes in each window of 1 s
		h->advance(200ms);
		newest().set<rate>(value);
		h.drain();
	}
	const auto count = diagnosed(h, "ear.status-flapping", "count");
	REQUIRE(count.size() == 2);
	CHECK_FALSE(count[1].active);
	const auto rates = diagnosed(h, "ear.status-flapping", "rate");
	CHECK(rates.back().active);
	CHECK(std::ranges::none_of(rates, [](const SeenDiagnostic& seen) { return !seen.active; }));
}

TEST_CASE("A field's records pass 32 at once, then a trailing record within 1 s matches the view", "[report][pace]") {
	Harness h;
	running(h);
	for (std::int64_t value = 1; value <= 40; ++value) newest().set<count>(value);
	h.drain();
	REQUIRE(h.statusRecord().size() == 32);
	CHECK(h.statusRecord().back().to == StatusValue(std::int64_t{32}));
	CHECK(h.status<count>() == StatusValue(std::int64_t{40})); // the wire's view is never held back
	h->advance(1000ms);
	REQUIRE(h.statusRecord().size() == 33);
	const SeenStatus& trailing = h.statusRecord().back();
	CHECK(trailing.field == "count");
	CHECK(trailing.from == StatusValue(std::int64_t{32}));
	CHECK(trailing.to == StatusValue(std::int64_t{40}));
	CHECK(trailing.heldBack == 8);
	const auto held = diagnosed(h, "ear.records-dropped", "status");
	REQUIRE(held.size() == 1);
	CHECK(held[0].fields == std::vector<std::pair<std::string, std::int64_t>>{{"count", 8}});
}

TEST_CASE("A node's events pass 32 at once, then one a second, and those held back are said at most once a second",
          "[report][pace]") {
	Harness h;
	running(h);
	for (int i = 0; i < 40; ++i) newest().raise<hello>("ana", 1.0);
	h.drain();
	CHECK(h.events().size() == 32);
	auto dropped = diagnosed(h, "ear.events-dropped");
	REQUIRE(dropped.size() == 1);
	CHECK(dropped[0].fields == std::vector<std::pair<std::string, std::int64_t>>{{"heldBack", 8}});
	for (int i = 0; i < 5; ++i) newest().raise<hello>("bo", 2.0);
	h.drain();
	CHECK(diagnosed(h, "ear.events-dropped").size() == 1);
	h->advance(999ms);
	CHECK(diagnosed(h, "ear.events-dropped").size() == 1);
	h->advance(1ms);
	dropped = diagnosed(h, "ear.events-dropped");
	REQUIRE(dropped.size() == 2);
	CHECK(dropped[1].fields == std::vector<std::pair<std::string, std::int64_t>>{{"heldBack", 5}});
	CHECK(dropped[1].count == 2);
	newest().raise<hello>("cy", 3.0); // a token refilled
	h.drain();
	CHECK(h.events().back().msg == "hello cy at 3.0");
}

/// A type of the same spec whose configure fails.
struct Refuser {
	static Members<spec> ports(const CheckedOf<spec>&) { return {out(members("mono"))}; }
	Configured configure(CreateOf<spec>&, const CheckedOf<spec>&) { return Configured::failed("nothing to report"); }
};

TEST_CASE("A configuration whose configure failed resets the status once", "[report]") {
	testing::TypeHarness<spec, Refuser> h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	h.add();
	h.blockReady();
	REQUIRE(h.phase() == "failed");
	h.drain();
	CHECK(std::ranges::count_if(h->reported(), [](const std::string& line) { return line.starts_with("reset"); }) == 1);
	CHECK(std::ranges::count_if(h.statusRecord(), [](const SeenStatus& record) { return record.reset; }) == 1);
}

} // namespace report_test
