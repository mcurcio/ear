// MainLoopActivity (docs/design/node-lifecycle.md section 5.12): what the main loop is in, read from another thread
// without a lock, never torn.
#include <atomic>
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
