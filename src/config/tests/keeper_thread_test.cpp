// The keeper's thread (keeper_thread.hpp; S4 criterion 16) on a real thread and a real folder:
// posted work in post order on its own thread, a rename of ear.conf as a disk event (and another
// file's as none), free after each wake, and stop. Every wait is for an event; the deadline only
// fails the test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "catch2/catch.hpp"
#include "keeper_support.hpp"
#include "keeper_thread.hpp"

using namespace ear::config;
using namespace ear::config::test;
using namespace std::chrono_literals;

namespace {

/// What the thread did, in order, and a way to wait for it.
class Seen {
public:
	void add(std::string what) {
		{
			const std::scoped_lock lock(mutex_);
			lines_.push_back(std::move(what));
		}
		changed_.notify_all();
	}
	/// Waits until `line` was seen `times` times; false at the deadline.
	bool waitFor(const std::string& line, std::size_t times = 1) {
		std::unique_lock lock(mutex_);
		return changed_.wait_for(lock, 10s, [&] { return count(line) >= times; });
	}
	std::size_t countOf(const std::string& line) {
		const std::scoped_lock lock(mutex_);
		return count(line);
	}
	std::vector<std::string> lines() {
		const std::scoped_lock lock(mutex_);
		return lines_;
	}

private:
	std::size_t count(const std::string& line) const { return static_cast<std::size_t>(std::ranges::count(lines_, line)); }
	std::mutex mutex_;
	std::condition_variable changed_;
	std::vector<std::string> lines_;
};

std::unique_ptr<KeeperThread> startThread(const TempDir& dir, Seen& seen) {
	auto thread = KeeperThread::start(dir.path().string(), "ear.conf",
	                                  KeeperThread::Handlers{[&seen] { seen.add("disk"); }, [&seen] { seen.add("free"); }});
	REQUIRE(thread.has_value());
	return std::move(*thread);
}

/// Returns once every wake begun before it has ended (its disk event and free included): a marker's
/// own wake can only start after the earlier wakes finished, and a second marker after that one's.
void settle(KeeperThread& thread, Seen& seen, int& markers) {
	for (int i = 0; i < 2; ++i) {
		const auto name = "marker " + std::to_string(++markers);
		thread.post([&seen, name] { seen.add(name); });
		REQUIRE(seen.waitFor(name));
	}
}

} // namespace

TEST_CASE("Posted work runs in post order on the keeper's thread, and free follows each wake", "[keeper_thread]") {
	TempDir dir("thread_order");
	Seen seen;
	auto thread = startThread(dir, seen);
	std::atomic<bool> elsewhere = true;
	const auto test = std::this_thread::get_id();
	for (int i = 1; i <= 3; ++i)
		thread->post([&, i] {
			if (std::this_thread::get_id() == test) elsewhere = false;
			seen.add("work " + std::to_string(i));
		});
	REQUIRE(seen.waitFor("free"));
	REQUIRE(seen.waitFor("work 3"));
	CHECK(elsewhere);
	const auto lines = seen.lines();
	std::vector<std::string> work;
	for (const auto& line : lines)
		if (line.starts_with("work")) work.push_back(line);
	CHECK(work == std::vector<std::string>{"work 1", "work 2", "work 3"});
	// free comes after the work of its wake, never before the first item
	CHECK(lines.front() == "work 1");
}

TEST_CASE("A rename of ear.conf into the folder is a disk event, and another file's is not", "[keeper_thread]") {
	TempDir dir("thread_watch");
	Seen seen;
	auto thread = startThread(dir, seen);
	int markers = 0;
	settle(*thread, seen, markers);

	handEdit(dir.path() / "other.conf", "other");
	settle(*thread, seen, markers);
	CHECK(seen.countOf("disk") == 0);

	handEdit(dir.path() / "ear.conf", "graph");
	REQUIRE(seen.waitFor("disk"));
	settle(*thread, seen, markers);
	const auto lines = seen.lines();
	const auto disk = std::ranges::find(lines, "disk");
	CHECK(std::find(disk, lines.end(), "free") != lines.end());   // free after the wake's disk event
}

TEST_CASE("stop() runs the work already posted, then joins, and work posted after it is destroyed unrun",
          "[keeper_thread]") {
	TempDir dir("thread_stop");
	Seen seen;
	auto thread = startThread(dir, seen);
	for (int i = 1; i <= 3; ++i) thread->post([&seen, i] { seen.add("work " + std::to_string(i)); });
	thread->stop();
	CHECK(seen.countOf("work 1") == 1);
	CHECK(seen.countOf("work 2") == 1);
	CHECK(seen.countOf("work 3") == 1);

	auto sentinel = std::make_shared<int>(0);
	thread->post([&seen, sentinel] { seen.add("late"); });
	CHECK(sentinel.use_count() == 1);   // destroyed
	thread->stop();                      // idempotent
	CHECK(seen.countOf("late") == 0);
}
