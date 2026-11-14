// A type's loops through a real NodeHost, by the harness (docs/writing-a-node-type.md#loops): each loop started with
// its state at readiness step `loops`, what is sent to a loop staged until the node is configured, start() and stop()
// posting to the state, the state destroyed on its own loop at teardown step 4 (after the sinks drained, before the
// instance), timers and wakes through the loop, a source a state left armed caught, and the rules configure must keep.
// The loops on real threads are ear_runtime's (loops_test.cpp); NodeHost's own order of steps, with a scripted adapter,
// node_host_test.cpp's.
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/lifecycle.hpp"
#include "ear/testing/type_harness.hpp"

// Named, never unnamed: every table, row and spec a template is keyed on (settings_table.hpp, linkage).
namespace loop_test {

using namespace ear;
using namespace std::chrono_literals;

/// What the type did, in order (the harness runs everything on the test's thread).
std::vector<std::string> events;
/// What each test asks of the type.
struct Behaviour {
	bool giveNoState = false;    // configure gives the worker no state
	bool giveStateTwice = false; // configure gives it two
	bool failConfigure = false;  // configure answers failed after posting
	bool leaveSource = false;    // begin() leaves a timer armed past the state, in `left`
	bool throwInBegin = false;   // begin() throws
};
Behaviour behaviour;
/// Where a careless state leaves a source.
std::unique_ptr<Timer> left;

void said(std::string what) { events.push_back(std::move(what)); }
std::ptrdiff_t at(const std::string& what) { return std::ranges::find(events, what) - events.begin(); }
std::size_t count(const std::string& what) { return static_cast<std::size_t>(std::ranges::count(events, what)); }

/// A clock that asks after every tick: the source's clock wake rings each cycle.
struct AskingClock final : SourceClock {
	std::uint64_t skipBefore(const SourceTick&, SourceHeld) noexcept override { return 0; }
	bool played(const SourceTick&, const SourcePlayed&) noexcept override { return true; }
};

/// The worker loop's state: its loop, the helper strand and the source's writer, taken in configure; armed by begin().
class Worker {
public:
	Worker(Loop& self, Loop& helper, SourceWriter& played) : self_(self), helper_(helper), played_(played) {
		said("worker made");
		staged_ = self_.after(5ms, [] { said("staged timer"); }); // armed while staged: armed at the release
	}
	~Worker() { said(self_.inLoop() ? "worker destroyed on its loop" : "worker destroyed on the main loop"); }
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	void begin() {
		said("begin");
		said(onStrand() ? "worker on a strand" : "worker off a strand");
		if (behaviour.throwInBegin) throw std::runtime_error("no server");
		tick_ = self_.every(10ms, [] { said("tick"); });
		rung_ = self_.onWake(played_.clockWake(), [] { said("rung"); });
		helper_.post([] { said(onStrand() ? "helper on a strand" : "helper off a strand"); });
		if (behaviour.leaveSource) left = self_.every(10ms, [] { said("left tick"); });
	}
	void end() {
		said("goodbye");
		tick_.reset();
		rung_.reset();
	}

private:
	Loop& self_;
	Loop& helper_;
	SourceWriter& played_;
	std::unique_ptr<Timer> staged_;
	std::unique_ptr<Timer> tick_;
	std::unique_ptr<Timer> rung_;
};

/// The sink's consumer: it says when it finished.
struct Tap final : SinkConsumer {
	void begin() override {}
	void consume(const Chunk&) override {}
	void finish() override { said("sink finished"); }
};

constexpr SettingsTable<> table{"looped"};
constexpr auto out = bank::output<"out">;
constexpr auto tap = bank::insideOutput<"tap">;
constexpr auto feed = bank::insideInput<"feed">;
constexpr auto worker = loop::dedicated<"worker", Worker>;
constexpr auto helper = loop::strand<"helper">;
constexpr TypeSpec spec{.settings = table, .banks = Banks<out, tap, feed>{}, .loops = Loops<worker, helper>{}};
using Checked = CheckedOf<spec>;

struct Silence {
	void process(Io<spec> io, const GraphCycle&) noexcept {
		for (auto member : io[out]) std::ranges::fill(member, 0.0F);
		for (auto member : io[tap]) std::ranges::fill(member, 0.0F);
	}
};

class Looped {
public:
	explicit Looped(NodeOf<spec>& node) : node_(node) {}
	~Looped() { said("instance destroyed"); }
	Looped(const Looped&) = delete;
	Looped& operator=(const Looped&) = delete;
	static Members<spec> ports(const Checked&) { return {out(members(1)), tap(members(1)), feed(members(1))}; }
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		said("configure");
		SourceWriter& played = cx.source(feed, {.frames = 256}, std::make_unique<AskingClock>());
		if (!behaviour.giveNoState)
			cx.loop(worker, std::make_unique<Worker>(cx.executor(worker), cx.executor(helper), played));
		if (behaviour.giveStateTwice)
			cx.loop(worker, std::make_unique<Worker>(cx.executor(worker), cx.executor(helper), played));
		cx.post(worker, [](Worker&) { said("staged post"); });
		cx.post(helper, [] { said("staged helper post"); });
		cx.sink(tap, {.frames = 1024}).consumedBy(std::make_unique<Tap>());
		cx.use(Silence{});
		if (behaviour.failConfigure) return Configured::failed("nope");
		return Configured::applied();
	}
	void start() {
		node_.post(worker, [](Worker& w) { w.begin(); });
	}
	void stop() {
		node_.post(worker, [](Worker& w) { w.end(); });
	}

private:
	NodeOf<spec>& node_;
};

using Harness = testing::TypeHarness<spec, Looped>;

/// A harness for "porch", the type's globals fresh.
std::unique_ptr<Harness> fresh(Harness::Options options = {}, Behaviour asked = {}) {
	events.clear();
	behaviour = asked;
	left.reset();
	options.node = "porch";
	auto harness = std::make_unique<Harness>(std::move(options));
	REQUIRE_FALSE(harness->parse({}));
	return harness;
}

/// Added, configured and running, with begin() run.
std::unique_ptr<Harness> running(Harness::Options options = {}, Behaviour asked = {}) {
	auto h = fresh(std::move(options), asked);
	h->add();
	h->blockReady();
	REQUIRE(h->phase() == "running");
	h->runLoops();
	return h;
}

std::string failure(const Harness& h) {
	const NodeHealth health = h.health();
	return health.reason ? std::string(failureCode(health.reason->code)) + ": " + health.reason->detail : "";
}

} // namespace loop_test

using namespace loop_test;

TEST_CASE("Work posted and timers armed while the node readies are staged: nothing runs before it is configured, then "
          "all of it, in the order sent",
          "[loops]") {
	auto h = fresh({.holdLoops = true});
	h->add();
	REQUIRE(h->phase() == "configuring");
	REQUIRE(h->node().loops().size() == 2); // each loop made at readiness step `loops`, neither started yet
	h->runLoops();
	h->node().advance(50ms);
	CHECK(count("staged post") == 0);
	CHECK(count("staged helper post") == 0);
	CHECK(count("staged timer") == 0);

	h->loopsReady();                  // both run: the node is configured, and what was staged is released
	CHECK(h->phase() == "starting");  // configured, then waiting for its block
	CHECK(count("staged post") == 0); // released onto its loop, run by the loop's next turn
	h->runLoops();
	CHECK(count("staged post") == 1);
	CHECK(count("staged helper post") == 1);
	CHECK(count("staged timer") == 0); // armed at the release: it counts from there
	h->node().advance(5ms);
	CHECK(count("staged timer") == 1);
	CHECK(at("worker made") < at("staged post"));
	CHECK(at("staged post") < at("staged timer"));
}

TEST_CASE("Staged work never runs after a configuration that failed first, and a state never on a loop is destroyed on "
          "the main loop",
          "[loops]") {
	SECTION("a loop that cannot start") {
		auto h = fresh({.refuseLoops = "Resource temporarily unavailable"});
		h->add();
		CHECK(h->phase() == "failed");
		CHECK(failure(*h) == "start-failed: cannot start loop worker: Resource temporarily unavailable");
	}
	SECTION("configure answered failed after it posted") {
		auto h = fresh({}, {.failConfigure = true});
		h->add();
		CHECK(h->phase() == "failed");
		CHECK(failure(*h) == "start-failed: nope");
		CHECK(h->node().loops().empty()); // no step was asked
	}
	CHECK(count("staged post") == 0);
	CHECK(count("staged helper post") == 0);
	CHECK(count("worker destroyed on the main loop") == 1);
	CHECK(at("worker destroyed on the main loop") < at("instance destroyed")); // a state may read the instance
}

TEST_CASE("start() and stop() post to the state on its loop, a stop and a start keep it, a restart runs the goodbye, "
          "destroys it on its own loop and makes the next",
          "[loops]") {
	auto h = running();
	CHECK(count("begin") == 1);
	h->node().advance(10ms);
	CHECK(count("tick") == 1);
	h->node().advance(20ms); // each tick the clock passes
	CHECK(count("tick") == 3);

	REQUIRE(h->stop());
	h->runLoops();
	CHECK(count("goodbye") == 1);
	h->node().advance(30ms);
	CHECK(count("tick") == 3);
	CHECK(count("worker made") == 1);
	CHECK(h->node().loops().at(0).closedToOthers == false); // a stop keeps the configuration, its loops open to others
	REQUIRE(h->start());
	h->runLoops();
	CHECK(count("begin") == 2);
	CHECK(count("worker destroyed on its loop") == 0);

	events.clear();
	REQUIRE(h->restart());
	CHECK(at("goodbye") < at("worker destroyed on its loop"));
	CHECK(at("worker destroyed on its loop") < at("instance destroyed"));
	CHECK(at("instance destroyed") < at("worker made")); // the next configuration's
	const auto seen = h->node().loops();
	REQUIRE(seen.size() == 4);
	CHECK(seen[0].closedToOthers);
	CHECK(seen[0].finished);
	CHECK(seen[0].closed);
	CHECK_FALSE(seen[2].closed);
}

TEST_CASE("Teardown: loops closed to others at step 1, the sink drained before any state goes, each state destroyed on "
          "its loop before the instance",
          "[loops]") {
	auto h = running();
	events.clear();
	h->remove();
	CHECK(h->gone());
	CHECK(at("sink finished") < at("worker destroyed on its loop"));
	CHECK(at("goodbye") < at("worker destroyed on its loop"));
	CHECK(at("worker destroyed on its loop") < at("instance destroyed"));
	for (const auto& seen : h->node().loops()) {
		INFO(seen.name);
		CHECK(seen.closedToOthers);
		CHECK(seen.finished);
		CHECK(seen.closed);
	}
}

TEST_CASE("A source a state left armed is cancelled at the close and said, and the harness fails the call that tore it "
          "down",
          "[loops]") {
	SECTION("said: ear.loop-source-left, the source cancelled") {
		auto h = running({.allowSourcesLeft = true}, {.leaveSource = true});
		CHECK(h->armed(worker) ==
		      4); // the staged timer (fired, its handle kept), the tick, the wake's watch, the left one
		h->remove();
		const auto diagnostics = h->node().diagnostics();
		const auto found = std::ranges::find_if(
			diagnostics, [](const auto& diagnostic) { return diagnostic.code == "ear.loop-source-left"; });
		REQUIRE(found != diagnostics.end());
		CHECK(found->detail == "1 source was still armed on loop worker when the node's configuration closed");
		CHECK(found->fields == std::vector<std::pair<std::string, std::int64_t>>{{"sources", 1}});
		const std::size_t ticks = count("left tick");
		h->node().advance(100ms);
		CHECK(count("left tick") == ticks); // cancelled by its loop's close
	}
	SECTION("failed: the teardown's call throws, naming the loop") {
		auto h = running({}, {.leaveSource = true});
		CHECK_THROWS_WITH(h->remove(), Catch::Contains("still armed on loop worker"));
	}
	left.reset(); // after the close, on any thread
}

TEST_CASE("A source's clock wake through the loop: rung before it was watched it runs once, each ring after the answer "
          "runs it again",
          "[loops]") {
	auto h = fresh();
	h->add();
	h->blockReady(); // start() posted begin(); it has not run
	h->process();    // the clock asks: the wake is rung before anything watches it
	h->process();
	h->runLoops(); // begin() watches it: the pending ring runs the handler once
	CHECK(count("rung") == 1);
	h->runLoops();
	CHECK(count("rung") == 1);
	h->process();
	h->runLoops();
	CHECK(count("rung") == 2);
}

TEST_CASE("A strand's handlers run with onStrand() true, a dedicated loop's with it false", "[loops]") {
	auto h = running();
	CHECK(count("worker off a strand") == 1);
	CHECK(count("helper on a strand") == 1);
	CHECK_FALSE(onStrand());
}

TEST_CASE("Configure's loop rules: a loop declared with a state is given exactly one", "[loops]") {
	SECTION("none") {
		auto h = fresh({}, {.giveNoState = true});
		h->add();
		CHECK(failure(*h) == "start-failed: configure gave loop worker no state");
	}
	SECTION("two") {
		auto h = fresh({}, {.giveStateTwice = true});
		h->add();
		CHECK(failure(*h) == "start-failed: configure threw: loop worker given a state twice");
	}
}

TEST_CASE("A loop not yet running is what the node waits for, after 2 s, by name", "[loops]") {
	auto h = fresh({.holdLoops = true});
	h->add();
	h->node().advance(2s);
	CHECK(h->health().waitingFor == std::vector<WaitingFor>{{WaitingFor::Kind::loop, "worker"},
	                                                        {WaitingFor::Kind::loop, "helper"},
	                                                        {WaitingFor::Kind::consumer}}); // its step is later
	h->loopsReady();
	CHECK(h->phase() == "starting"); // configured, then waiting for its block
}

TEST_CASE("A loop handler's throw fails the node as a hook's does", "[loops]") {
	auto h = running({}, {.throwInBegin = true});
	CHECK(failure(*h) == "lost: loop worker's handler threw: no server");
}
