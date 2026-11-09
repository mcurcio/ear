// MainLoopActivity (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog): what the main loop is in, read
// from another thread without a lock, never torn.
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#include "catch2/catch.hpp"
#include "ear/core/main_loop_activity.hpp"

using ear::MainLoopActivity;

TEST_CASE("MainLoopActivity names the call and its node while it runs, and nothing between calls",
          "[main_loop_activity]") {
	MainLoopActivity::clear();
	CHECK(MainLoopActivity::current().empty());
	{
		const MainLoopActivity::Scope scope("destroy block", "noise");
		CHECK(MainLoopActivity::current() == "destroy block noise");
	}
	CHECK(MainLoopActivity::current().empty());
	MainLoopActivity::set("configure", "");
	CHECK(MainLoopActivity::current() == "configure");
	MainLoopActivity::set("configure", std::string(200, 'n'));
	CHECK(MainLoopActivity::current() == "configure " + std::string(MainLoopActivity::nameBytes, 'n'));
	MainLoopActivity::clear();
	CHECK(MainLoopActivity::current().empty());
}

TEST_CASE("MainLoopActivity scopes nest: the inner one ends naming the outer call again", "[main_loop_activity]") {
	MainLoopActivity::clear();
	{
		const MainLoopActivity::Scope outer("make engine companion", "desk");
		{
			const MainLoopActivity::Scope inner("prepare", "porch");
			CHECK(MainLoopActivity::current() == "prepare porch");
		}
		CHECK(MainLoopActivity::current() == "make engine companion desk");
		{
			const MainLoopActivity::Scope inner("release", "");
			CHECK(MainLoopActivity::current() == "release");
		}
		CHECK(MainLoopActivity::current() == "make engine companion desk");
	}
	CHECK(MainLoopActivity::current().empty());
}

TEST_CASE("MainLoopActivity read from another thread is always one whole write", "[main_loop_activity]") {
	MainLoopActivity::clear();
	std::atomic<bool> done{false};
	std::atomic<int> torn{0};
	std::thread reader([&] {
		while (!done.load()) {
			const std::string now = MainLoopActivity::current();
			if (!now.empty() && now != "make block aaaa" && now != "destroy block bbbbbbbb") torn.fetch_add(1);
		}
	});
	for (int i = 0; i < 100'000; ++i) {
		MainLoopActivity::set("make block", "aaaa");
		MainLoopActivity::set("destroy block", "bbbbbbbb");
		MainLoopActivity::clear();
	}
	done.store(true);
	reader.join();
	CHECK(torn.load() == 0);
}

TEST_CASE("A throw that ends scopes keeps the innermost one's place until it is taken", "[main_loop_activity]") {
	MainLoopActivity::clear();
	MainLoopActivity::forgetThrown();
	CHECK_FALSE(MainLoopActivity::takeThrown());
	try {
		const MainLoopActivity::Scope outer("make engine companion", "desk");
		const MainLoopActivity::Scope inner("prepare", "porch");
		throw std::runtime_error("prepare threw");
	} catch (const std::runtime_error&) {
		CHECK(MainLoopActivity::current().empty());
		const auto at = MainLoopActivity::takeThrown();
		REQUIRE(at);
		CHECK(MainLoopActivity::describe(*at) == "prepare porch");
	}
	CHECK_FALSE(MainLoopActivity::takeThrown()); // taken
}

TEST_CASE("A scope that begins forgets the place a handled throw left, and one that returns leaves none",
          "[main_loop_activity]") {
	MainLoopActivity::clear();
	try {
		const MainLoopActivity::Scope scope("start", "attic");
		throw std::runtime_error("start threw");
	} catch (const std::runtime_error&) {
	}
	{
		const MainLoopActivity::Scope later("stop", "attic");
	}
	CHECK_FALSE(MainLoopActivity::takeThrown());
	MainLoopActivity::Saved put = MainLoopActivity::save();
	{
		const MainLoopActivity::Scope scope("recipe", "attic");
		put = MainLoopActivity::save();
	}
	MainLoopActivity::putThrown(put);
	const auto at = MainLoopActivity::takeThrown();
	REQUIRE(at);
	CHECK(MainLoopActivity::describe(*at) == "recipe attic");
	CHECK(MainLoopActivity::describe(MainLoopActivity::Saved{}).empty());
}
