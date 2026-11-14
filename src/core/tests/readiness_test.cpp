// The readiness list (readiness.hpp; docs/engine.md#readiness-deadlines): how it asks its items in steps, bounds each
// from its asking, hears one ready, judges a bound that passed, and reads what is still waited for. NodeHost's own
// readiness rows (node_host_test.cpp, [deadline]) prove the two kinds built on it.
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "readiness.hpp"

using namespace ear;
using namespace ear::detail;
using namespace std::chrono_literals;

namespace {

/// Timers on a clock the test moves; what came due runs in arming order.
struct StepTimers final : Timers {
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
	std::size_t live() const {
		std::size_t n = 0;
		for (const auto& timer : armed) n += !timer->cancelled && timer->due ? 1 : 0;
		return n;
	}
	void advance(std::chrono::milliseconds by) {
		clock += by;
		auto due = armed;
		for (const auto& timer : due) {
			if (timer->cancelled || !timer->due || timer->at > clock) continue;
			auto call = std::move(timer->due);
			timer->due = nullptr;
			call();
		}
	}
};

/// A list on the test's clock, recording each asking ("asked <name> at <ms>") and each failure ("failed <detail>").
struct Fixture {
	StepTimers timers;
	ReadinessList list;
	std::vector<std::string> log;
	std::vector<std::string> names;

	std::size_t add(std::string name, ReadinessStep step, std::optional<std::chrono::milliseconds> bound,
	                WaitingFor::Kind kind = WaitingFor::Kind::companion, Fact fact = Fact::engine) {
		names.push_back(name);
		return list.add(
			ReadinessItem{.kind = kind, .name = name, .step = step, .bound = bound, .fact = fact, .overdue = [name] {
							  return name + " overdue";
						  }});
	}
	void begin() {
		list.begin([this](std::size_t index, std::chrono::milliseconds bound) {
			log.push_back("asked " + names.at(index) + " at " + std::to_string(timers.clock.count()));
			return timers.after(bound, [this, index] {
				if (auto overdue = list.passed(index)) log.push_back("failed " + overdue->detail);
			});
		});
	}
};

using Log = std::vector<std::string>;

} // namespace

TEST_CASE("A later step is asked only once every item of every earlier step is ready", "[readiness]") {
	Fixture f;
	const auto store = f.add("store", ReadinessStep::opens, 1'000ms);
	const auto claim = f.add("claim", ReadinessStep::claims, 1'000ms);
	f.add("card", ReadinessStep::companions, 1'000ms);
	const auto loop = f.add("loop", ReadinessStep::loops, 1'000ms);
	f.begin();
	CHECK(f.log == Log{"asked claim at 0"});
	f.timers.advance(10ms);
	f.list.ready(claim);
	CHECK(f.log == Log{"asked claim at 0", "asked store at 10"});
	f.timers.advance(10ms);
	f.list.ready(store);
	f.timers.advance(10ms);
	f.list.ready(loop);
	CHECK(f.log == Log{"asked claim at 0", "asked store at 10", "asked loop at 20", "asked card at 30"});
	CHECK_FALSE(f.list.done());
}

TEST_CASE("A bound runs from its item's asking, not from the list's beginning", "[readiness]") {
	Fixture f;
	const auto claim = f.add("claim", ReadinessStep::claims, std::nullopt); // bounded elsewhere: no timer
	f.add("card", ReadinessStep::companions, 50ms);
	f.begin();
	CHECK(f.timers.live() == 0);
	f.timers.advance(100ms);
	CHECK(f.log.empty()); // the card is not asked yet, so its 50 ms have not begun
	f.list.ready(claim);
	f.timers.advance(49ms);
	CHECK(f.log == Log{"asked card at 100"});
	f.timers.advance(1ms);
	CHECK(f.log == Log{"asked card at 100", "failed card overdue"});
}

TEST_CASE("An item that becomes ready cancels its bound, a second ready is nothing, and the list is done once every "
          "item is",
          "[readiness]") {
	Fixture f;
	const auto card = f.add("card", ReadinessStep::companions, 50ms);
	const auto rtp = f.add("rtp", ReadinessStep::companions, 50ms);
	f.begin();
	CHECK(f.timers.live() == 2);
	f.list.ready(card);
	CHECK(f.list.isReady(card));
	CHECK_FALSE(f.list.isReady(rtp));
	CHECK(f.timers.live() == 1);
	CHECK_FALSE(f.list.done());
	f.list.ready(card);
	f.list.ready(rtp);
	CHECK(f.list.done());
	CHECK(f.timers.live() == 0);
	f.timers.advance(60ms);
	CHECK(f.log == Log{"asked card at 0", "asked rtp at 0"});
	CHECK_FALSE(f.list.passed(card).has_value()); // a bound judged after its ready finds nothing
}

TEST_CASE("A bound that passes unready fails with its item's fact and text, and ends the list, so no later step is "
          "asked and no other bound is judged",
          "[readiness]") {
	Fixture f;
	const auto claim =
		f.add("claim", ReadinessStep::claims, 50ms, WaitingFor::Kind::consumer, Fact::consumerNeverBegan);
	f.add("other", ReadinessStep::claims, 80ms);
	f.add("card", ReadinessStep::companions, 50ms);
	f.begin();
	f.timers.advance(50ms);
	CHECK(f.log == Log{"asked claim at 0", "asked other at 0", "failed claim overdue"});
	f.list.ready(1); // a later ready asks nothing once the list ended
	f.timers.advance(100ms);
	CHECK(f.log == Log{"asked claim at 0", "asked other at 0", "failed claim overdue"});
	CHECK_FALSE(f.list.passed(claim).has_value()); // only the first failure is told
	CHECK_FALSE(f.list.done());

	ReadinessList list;
	list.add(ReadinessItem{.kind = WaitingFor::Kind::consumer,
	                       .name = {},
	                       .step = ReadinessStep::companions,
	                       .bound = 50ms,
	                       .fact = Fact::consumerNeverBegan,
	                       .overdue = [] { return std::string("the sink pb's consumer did not begin within 5 s"); }});
	list.begin([](std::size_t, std::chrono::milliseconds) { return std::unique_ptr<Timer>(); });
	const std::optional<ReadinessOverdue> overdue = list.passed(0);
	REQUIRE(overdue.has_value());
	CHECK(overdue->fact == Fact::consumerNeverBegan);
	CHECK(overdue->detail == "the sink pb's consumer did not begin within 5 s");
}

TEST_CASE("Items are asked in the order they were added, and bounds due together are judged in that order, so the "
          "first unready one fails the list",
          "[readiness]") {
	Fixture f;
	f.add("b", ReadinessStep::companions, 50ms);
	f.add("a", ReadinessStep::companions, 50ms);
	const auto c = f.add("c", ReadinessStep::companions, 50ms);
	f.begin();
	CHECK(f.log == Log{"asked b at 0", "asked a at 0", "asked c at 0"});
	f.list.ready(c);
	f.timers.advance(50ms);
	CHECK(f.log == Log{"asked b at 0", "asked a at 0", "asked c at 0", "failed b overdue"});
}

TEST_CASE("What a list waits for is each item not ready, in item order, a repeated wait once, and nothing once every "
          "item is ready",
          "[readiness]") {
	Fixture f;
	const auto rtp = f.add("fpp.rtp", ReadinessStep::companions, 50ms);
	const auto card = f.add("desk.playback", ReadinessStep::companions, 50ms);
	const auto pb = f.add("", ReadinessStep::companions, 50ms, WaitingFor::Kind::consumer);
	const auto cap = f.add("", ReadinessStep::companions, 50ms, WaitingFor::Kind::consumer);
	CHECK(f.list.waiting() == std::vector<WaitingFor>{{WaitingFor::Kind::companion, "fpp.rtp"},
	                                                  {WaitingFor::Kind::companion, "desk.playback"},
	                                                  {WaitingFor::Kind::consumer}});
	f.begin();
	f.list.ready(rtp);
	f.list.ready(pb);
	CHECK(f.list.waiting() ==
	      std::vector<WaitingFor>{{WaitingFor::Kind::companion, "desk.playback"}, {WaitingFor::Kind::consumer}});
	f.list.ready(card);
	f.list.ready(cap);
	CHECK(f.list.waiting().empty());
}

TEST_CASE("A list with no items is done at once and asks nothing", "[readiness]") {
	Fixture f;
	f.begin();
	CHECK(f.list.done());
	CHECK(f.log.empty());
	CHECK(f.list.waiting().empty());
	CHECK_FALSE(f.list.isReady(noReadiness));
}
