// The main-loop watchdog (docs/design/node-lifecycle.md 5.12, 11.6; step L-7): its judgement on a clock stepped by hand
// (armed by the first beat, a stall past the bound reported once, its resumption once with the stall's length), and the
// thread over real short stalls: reported after the bound and resumed once, nothing under steady beats, nothing before
// the first beat, nothing once stopped, and stop joining at once.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/output/stall_watch.hpp"

using ear::output::StallJudge;
using ear::output::StallWatch;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t ms = 1'000'000; // nanoseconds

/// What a watch reported, in order, waited on by the test.
struct Reports {
	struct Report {
		bool stalled;
		std::int64_t ms;
		std::string where;
		std::thread::id thread;
	};
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<Report> seen;

	StallWatch make(StallWatch::Options options, std::string where = "configure kitchen") {
		return {
			[where] { return where; },
			[this](std::int64_t length, const std::string& in) { add({true, length, in, std::this_thread::get_id()}); },
			[this](std::int64_t length) { add({false, length, "", std::this_thread::get_id()}); }, options};
	}
	void add(Report report) {
		{
			const std::scoped_lock lock(mutex);
			seen.push_back(std::move(report));
		}
		changed.notify_all();
	}
	/// Waits until `count` reports were seen; a deadline only fails the test.
	bool waitFor(std::size_t count) {
		std::unique_lock lock(mutex);
		return changed.wait_for(lock, 10s, [&] { return seen.size() >= count; });
	}
	std::vector<Report> taken() {
		const std::scoped_lock lock(mutex);
		return seen;
	}
};

/// Beats `watch` every 10 ms for `span` on this thread.
void beatFor(StallWatch& watch, std::chrono::milliseconds span) {
	const auto end = std::chrono::steady_clock::now() + span;
	while (std::chrono::steady_clock::now() < end) {
		watch.beat();
		std::this_thread::sleep_for(10ms);
	}
}

} // namespace

TEST_CASE("The stall judge is armed by the first beat", "[stall_watch]") {
	StallJudge judge(500ms);
	CHECK_FALSE(judge.look(0, 10'000 * ms));
	CHECK_FALSE(judge.look(0, 20'000 * ms));
	CHECK_FALSE(judge.stalled());
}

TEST_CASE("The stall judge reports a heartbeat older than its bound once, then its resumption once", "[stall_watch]") {
	StallJudge judge(500ms);
	const std::int64_t beat = 1'000 * ms;
	CHECK_FALSE(judge.look(beat, beat + 250 * ms));
	CHECK_FALSE(judge.look(beat, beat + 500 * ms)); // at the bound, not past it
	const auto stalled = judge.look(beat, beat + 750 * ms);
	REQUIRE(stalled);
	CHECK(stalled->kind == StallJudge::Verdict::Kind::stalled);
	CHECK(stalled->ms == 750);
	CHECK(judge.stalled());
	CHECK_FALSE(judge.look(beat, beat + 1'000 * ms)); // still stalled: said once
	CHECK_FALSE(judge.look(beat, beat + 5'000 * ms));
	const auto resumed = judge.look(beat + 5'100 * ms, beat + 5'250 * ms);
	REQUIRE(resumed);
	CHECK(resumed->kind == StallJudge::Verdict::Kind::resumed);
	CHECK(resumed->ms == 5'100); // the last beat before the stall to the first after it
	CHECK_FALSE(judge.stalled());
	CHECK_FALSE(judge.look(beat + 5'200 * ms, beat + 5'450 * ms));
	// A second stall is a second report.
	const auto again = judge.look(beat + 5'200 * ms, beat + 5'800 * ms);
	REQUIRE(again);
	CHECK(again->kind == StallJudge::Verdict::Kind::stalled);
	CHECK(again->ms == 600);
}

TEST_CASE("StallWatch reports a heartbeat older than its bound once, then its resumption", "[stall_watch]") {
	Reports reports;
	StallWatch watch = reports.make({.bound = 100ms, .period = 10ms});
	watch.beat();
	const auto stalledFrom = std::chrono::steady_clock::now();
	REQUIRE(reports.waitFor(1));
	CHECK(std::chrono::steady_clock::now() - stalledFrom >= 100ms);
	std::this_thread::sleep_for(100ms); // still stalled: nothing more
	auto seen = reports.taken();
	REQUIRE(seen.size() == 1);
	CHECK(seen[0].stalled);
	CHECK(seen[0].ms > 100);
	CHECK(seen[0].where == "configure kitchen");
	CHECK(seen[0].thread != std::this_thread::get_id()); // where is asked on the watchdog's own thread
	watch.beat();
	REQUIRE(reports.waitFor(2));
	seen = reports.taken();
	CHECK_FALSE(seen[1].stalled);
	CHECK(seen[1].ms >= 200);
	CHECK(seen[1].thread == seen[0].thread);
	watch.stop();
	CHECK(reports.taken().size() == 2);
}

TEST_CASE("StallWatch says nothing while the heartbeat keeps moving", "[stall_watch]") {
	Reports reports;
	StallWatch watch = reports.make({.bound = 500ms, .period = 25ms});
	beatFor(watch, 1500ms);
	watch.stop();
	CHECK(reports.taken().empty());
}

TEST_CASE("StallWatch says nothing before the first beat, nor once stopped", "[stall_watch]") {
	Reports reports;
	StallWatch watch = reports.make({.bound = 50ms, .period = 10ms});
	std::this_thread::sleep_for(300ms); // boot: the main loop has not run yet
	CHECK(reports.taken().empty());
	watch.beat();
	watch.stop(); // ear's stop, past the main loop: no beat comes again
	std::this_thread::sleep_for(200ms);
	CHECK(reports.taken().empty());
	watch.stop(); // idempotent
}

TEST_CASE("StallWatch's stop joins its thread at once, whatever its period", "[stall_watch]") {
	Reports reports;
	const auto made = std::chrono::steady_clock::now();
	{
		StallWatch watch = reports.make({.bound = 500ms, .period = 60s});
		watch.beat();
	} // destroyed: stop()
	CHECK(std::chrono::steady_clock::now() - made < 5s);
	CHECK(reports.taken().empty());
}

TEST_CASE("StallWatch tells an unknown place as empty", "[stall_watch]") {
	Reports reports;
	StallWatch watch = reports.make({.bound = 50ms, .period = 10ms}, "");
	watch.beat();
	REQUIRE(reports.waitFor(1));
	watch.stop();
	CHECK(reports.taken()[0].where.empty());
}
