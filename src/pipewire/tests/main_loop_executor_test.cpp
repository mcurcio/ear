// MainLoopExecutor on the real engine's main loop (docs/engine.md): post order per thread, the loop thread, never
// inline, and work left at destruction destroyed unrun. Every wait runs the main loop until the awaited work has run;
// the deadline only fails the test. Also the main-loop watchdog
// (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog) beaten by its timers, as main.cpp wires it,
// reporting a real stall of the loop with where it was.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/output/stall_watch.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "sleeps.hpp"

namespace {
using namespace ear;
using namespace ear::pipewire;

std::unique_ptr<Engine> makeEngine() {
	for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "DBUS_SESSION_BUS_ADDRESS",
	                         "DBUS_SYSTEM_BUS_ADDRESS"}) {
		unsetenv(name);
	}
	auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, ear::SampleRate::fixed<48000>(), 256});
	if (!engine) FAIL("Engine::create: " << engine.error().detail);
	return std::move(*engine);
}

/// Runs the main loop until posted work calls engine.quit(); a timer only fails the test.
bool runUntilQuit(Engine& engine, std::chrono::seconds deadline = std::chrono::seconds(5)) {
	bool timedOut = false;
	std::pair<Engine*, bool*> state{&engine, &timedOut};
	spa_source* timer = pw_loop_add_timer(
		engine.mainLoop(),
		[](void* data, std::uint64_t) {
			auto* s = static_cast<std::pair<Engine*, bool*>*>(data);
			*s->second = true;
			s->first->quit();
		},
		&state);
	timespec value{static_cast<time_t>(deadline.count()), 0};
	pw_loop_update_timer(engine.mainLoop(), timer, &value, nullptr, false);
	engine.run();
	pw_loop_destroy_source(engine.mainLoop(), timer);
	return !timedOut;
}

/// Sets a flag when destroyed (so a test sees work destroyed unrun).
struct DestroyFlag {
	explicit DestroyFlag(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}
	DestroyFlag(DestroyFlag&& other) noexcept : flag_(std::move(other.flag_)) {}
	~DestroyFlag() {
		if (flag_) flag_->store(true);
	}
	std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace

TEST_CASE("Work posted from four threads runs on the main loop's thread in each thread's order", "[executor]") {
	auto engine = makeEngine();
	MainLoopExecutor executor(*engine);
	constexpr int kThreads = 4;
	constexpr int kPosts = 1000;
	const std::thread::id loopThread = std::this_thread::get_id(); // run() runs here
	// Touched only on the loop thread.
	std::array<std::vector<int>, kThreads> seen;
	int run = 0;
	int offLoop = 0;

	std::vector<std::thread> posters;
	for (int t = 0; t < kThreads; ++t) {
		posters.emplace_back([&, t] {
			for (int i = 0; i < kPosts; ++i) {
				executor.post([&, t, i] {
					if (std::this_thread::get_id() != loopThread) ++offLoop;
					seen[t].push_back(i);
					if (++run == kThreads * kPosts) engine->quit();
				});
			}
		});
	}
	const bool done = runUntilQuit(*engine);
	for (std::thread& poster : posters) poster.join();
	REQUIRE(done);
	CHECK(offLoop == 0);
	for (int t = 0; t < kThreads; ++t) {
		REQUIRE(seen[t].size() == kPosts);
		for (int i = 0; i < kPosts; ++i) {
			if (seen[t][i] != i) FAIL("thread " << t << ": post " << i << " ran as " << seen[t][i]);
		}
	}
}

TEST_CASE("Work posted from the main loop runs after post() returns, never inside it", "[executor]") {
	auto engine = makeEngine();
	MainLoopExecutor executor(*engine);
	bool returned = false;
	bool ranAfterReturn = false;
	bool ran = false;
	executor.post([&] {
		executor.post([&] {
			ran = true;
			ranAfterReturn = returned;
			engine->quit();
		});
		returned = true;
	});
	REQUIRE(runUntilQuit(*engine));
	REQUIRE(ran);
	CHECK(ranAfterReturn);
}

TEST_CASE("Work still queued when the executor is destroyed is destroyed unrun", "[executor]") {
	auto engine = makeEngine();
	auto destroyed = std::make_shared<std::atomic<bool>>(false);
	bool ran = false;
	{
		MainLoopExecutor executor(*engine);
		executor.post([&ran, flag = DestroyFlag(destroyed)] { ran = true; });
		CHECK_FALSE(destroyed->load());
	}
	CHECK(destroyed->load());
	CHECK_FALSE(ran);

	// The loop runs on without it: a later executor's work runs, the old one's never does.
	MainLoopExecutor next(*engine);
	next.post([&] { engine->quit(); });
	REQUIRE(runUntilQuit(*engine));
	CHECK_FALSE(ran);
}

TEST_CASE("A timer runs once on the main loop after its delay, never once destroyed, and may be destroyed inside "
          "its own call",
          "[executor][timers]") {
	auto engine = makeEngine();
	MainLoopExecutor executor(*engine);
	Timers& timers = executor;
	const std::thread::id loopThread = std::this_thread::get_id();
	int cancelledRan = 0;
	int ownRan = 0;
	bool onLoop = false;
	std::unique_ptr<Timer> own;
	const auto armed = std::chrono::steady_clock::now();
	std::chrono::steady_clock::duration waited{};

	auto cancelled = timers.after(std::chrono::milliseconds(20), [&] { ++cancelledRan; });
	cancelled.reset();
	own = timers.after(std::chrono::milliseconds(50), [&] {
		++ownRan;
		onLoop = std::this_thread::get_id() == loopThread;
		waited = std::chrono::steady_clock::now() - armed;
		own.reset(); // the timer goes inside its own call
		executor.post([&] { engine->quit(); });
	});
	auto zero = timers.after(std::chrono::milliseconds(0), [] {}); // a zero delay still fires, later
	REQUIRE(runUntilQuit(*engine));
	CHECK(ownRan == 1);
	CHECK(onLoop);
	CHECK(waited >= std::chrono::milliseconds(50));
	CHECK(cancelledRan == 0);
	CHECK(own == nullptr);
}

TEST_CASE("The main-loop watchdog reports a stall of the engine's main loop with where it was, then its resumption",
          "[executor][stall_watch]") {
	auto engine = makeEngine();
	MainLoopExecutor executor(*engine);
	struct Report {
		bool stalled;
		std::int64_t ms;
		std::string where;
	};
	std::mutex mutex;
	std::vector<Report> reports;
	ear::output::StallWatch watch(
		[] { return MainLoopActivity::current(); },
		[&](std::int64_t ms, const std::string& where) {
			const std::scoped_lock lock(mutex);
			reports.push_back({true, ms, where});
		},
		[&](std::int64_t ms) {
			bool planned = false;
			{
				const std::scoped_lock lock(mutex);
				// A loaded host may stall the loop unplanned first: only the planned stall's resumption ends the run.
				planned = !reports.empty() && reports.back().stalled && reports.back().where == "configure desk";
				reports.push_back({false, ms, ""});
			}
			if (planned)
				executor.post([&] { engine->quit(); }); // the watchdog's thread posts; it never touches the loop
		});
	// main.cpp's heartbeat: a 100 ms timer armed again each time it beats.
	std::unique_ptr<Timer> beat;
	std::function<void()> arm = [&] {
		beat = executor.after(std::chrono::milliseconds(100), [&] {
			watch.beat();
			arm();
		});
	};
	arm();
	// After a few beats, one main-loop turn runs 900 ms inside a named call.
	auto stall = executor.after(std::chrono::milliseconds(350), [] {
		const MainLoopActivity::Scope scope("configure", "desk");
		ear::sleeps::induce(std::chrono::milliseconds(900), "the main-loop stall the watchdog must see");
	});
	REQUIRE(runUntilQuit(*engine, std::chrono::seconds(10)));
	beat.reset();
	watch.stop();
	const std::scoped_lock lock(mutex);
	// The planned stall and the resumption after it; other reports, if any, were the host's.
	const auto planned = std::ranges::find_if(
		reports, [](const Report& report) { return report.stalled && report.where == "configure desk"; });
	REQUIRE(planned != reports.end());
	REQUIRE(planned + 1 != reports.end());
	const Report& stalled = *planned;
	const Report& resumed = *(planned + 1);
	CHECK(stalled.ms > 500);
	CHECK(stalled.where == "configure desk");
	CHECK_FALSE(resumed.stalled);
	// The gap from the last beat before the stall to the first after it: the 900 ms turn, plus at most one 100 ms beat
	// interval and the loop's dispatch.
	CHECK(resumed.ms >= 900);
	CHECK(resumed.ms < 1500);
}
