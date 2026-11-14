// The nodes' loops on real threads (ear/runtime/loops.hpp; docs/writing-a-node-type.md#loops): post order, timers,
// watches and wakes, finish and close, outside work, and the strand pool's rules (onStrand, slow handlers, a stuck
// strand's replacement). Each wait is for the event it needs; a deadline only fails. What NodeHost does with a loop
// (staging, the state's life, the sources left armed, the readiness step) is core's (loop_test.cpp, node_host_test.cpp).
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/runtime/loops.hpp"
#include "sleeps.hpp"

using ear::LoopKind;
using ear::runtime::LoopOptions;
using ear::runtime::Loops;
using namespace std::chrono_literals;

namespace {

constexpr auto deadline = 10s;

/// A main loop the test turns: posts from any thread, run on the test's own.
class TestMainLoop final : public ear::Executor {
public:
	void post(std::move_only_function<void()> work) override {
		const std::scoped_lock lock(mutex_);
		queue_.push_back(std::move(work));
		posted_.notify_all();
	}
	/// Runs what is posted, as it comes, until `done()` holds (false past the deadline).
	bool runUntil(const std::function<bool()>& done) {
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (!done()) {
			std::deque<std::move_only_function<void()>> due;
			{
				std::unique_lock lock(mutex_);
				if (!posted_.wait_until(lock, until, [&] { return !queue_.empty(); })) return done();
				due.swap(queue_);
			}
			for (auto& work : due) work();
		}
		return true;
	}

private:
	std::mutex mutex_;
	std::condition_variable posted_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// What the adapter told about one loop, on the main loop.
struct Told final : ear::LoopEvents {
	void ready() override { readyCount++; }
	void failed(std::string detail) override { failures.push_back(std::move(detail)); }
	void slow(std::int64_t ms) override { slows.push_back(ms); }
	void threw(std::string what) override { throws.push_back(std::move(what)); }
	int readyCount = 0;
	std::vector<std::string> failures;
	std::vector<std::int64_t> slows;
	std::vector<std::string> throws;
};

/// A latch another thread waits on.
class Latch {
public:
	void open() {
		{
			const std::scoped_lock lock(mutex_);
			open_ = true;
		}
		changed_.notify_all();
	}
	bool wait() {
		std::unique_lock lock(mutex_);
		return changed_.wait_for(lock, deadline, [&] { return open_; });
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	bool open_ = false;
};

/// Core's Wake on an eventfd, as a source's clock wake is (ear_pipewire's): ring writes only when not pending; answer
/// drains, then clears.
class EventWake final : public ear::Wake {
public:
	EventWake() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
	~EventWake() { ::close(fd_); }
	void ring() noexcept override {
		if (pending_.exchange(true, std::memory_order_acq_rel)) return;
		const std::uint64_t one = 1;
		[[maybe_unused]] const auto ignored = ::write(fd_, &one, sizeof one);
	}
	int descriptor() const noexcept override { return fd_; }
	void answer() noexcept override {
		std::uint64_t count = 0;
		[[maybe_unused]] const auto ignored = ::read(fd_, &count, sizeof count);
		pending_.store(false, std::memory_order_release);
	}

private:
	int fd_;
	std::atomic<bool> pending_{false};
};

/// A loop made and told ready.
struct Made {
	Told told;
	std::unique_ptr<ear::NodeLoop> loop;
};
void make(Loops& loops, TestMainLoop& main, Made& made, LoopKind kind, std::string_view name = "work") {
	made.loop = loops.make("porch", name, kind, made.told);
	REQUIRE(main.runUntil([&] { return made.told.readyCount == 1; }));
}
/// Finishes and closes `made` (destroying `last` on its thread), waiting for both on the main loop.
void finishAndClose(TestMainLoop& main, Made& made, std::move_only_function<void()> last = [] {}) {
	bool finished = false;
	bool closed = false;
	made.loop->finish(std::move(last), [&] { finished = true; });
	REQUIRE(main.runUntil([&] { return finished; }));
	made.loop->close([&] { closed = true; });
	REQUIRE(main.runUntil([&] { return closed; }));
}

} // namespace

TEST_CASE("A dedicated loop runs posts in post order, one at a time, on its own thread named ear-<name> <node>",
          "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated, "sendspin");
	std::mutex seen;
	std::vector<int> order;
	std::array<char, 16> name{};
	std::thread::id ran;
	bool inLoop = false;
	for (int i = 0; i < 100; ++i)
		made.loop->post([&, i] {
			const std::scoped_lock lock(seen);
			order.push_back(i);
			if (i == 0) {
				::pthread_getname_np(::pthread_self(), name.data(), name.size());
				ran = std::this_thread::get_id();
				inLoop = made.loop->inLoop();
			}
		});
	bool done = false;
	made.loop->post([&] { main.post([&] { done = true; }); });
	REQUIRE(main.runUntil([&] { return done; }));
	const std::scoped_lock lock(seen);
	REQUIRE(order.size() == 100);
	for (int i = 0; i < 100; ++i) CHECK(order[static_cast<std::size_t>(i)] == i);
	CHECK(std::string(name.data()) == "ear-sendspin po"); // cut to 15 bytes
	CHECK(ran != std::this_thread::get_id());
	CHECK(inLoop);
	CHECK_FALSE(made.loop->inLoop());
	finishAndClose(main, made);
}

TEST_CASE("after runs once, a timer whose handle went never runs", "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	std::atomic<int> once{0};
	std::atomic<int> cancelled{0};
	std::unique_ptr<ear::Timer> kept = made.loop->after(5ms, [&] { once++; });
	std::unique_ptr<ear::Timer> gone = made.loop->after(5ms, [&] { cancelled++; });
	gone.reset();
	bool later = false;
	std::unique_ptr<ear::Timer> after = made.loop->after(50ms, [&] { main.post([&] { later = true; }); });
	REQUIRE(main.runUntil([&] { return later; }));
	CHECK(once == 1);
	CHECK(cancelled == 0);
	made.loop->post([&] {
		kept.reset();
		after.reset();
	});
	finishAndClose(main, made);
}

TEST_CASE("every keeps a fixed rate, and the ticks a slow handler missed run once", "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	const auto period = 10ms;
	const auto began = std::chrono::steady_clock::now();
	std::vector<std::chrono::steady_clock::time_point> fires; // the loop's only, read once done
	bool done = false;
	std::unique_ptr<ear::Timer> tick = made.loop->every(period, [&] {
		fires.push_back(std::chrono::steady_clock::now());
		if (fires.size() == 1) ear::sleeps::induce(200ms, "a handler that misses about 20 ticks");
		if (fires.size() == 6) main.post([&] { done = true; });
	});
	REQUIRE(main.runUntil([&] { return done; }));
	made.loop->post([&] { tick.reset(); });
	finishAndClose(main, made);
	// Uncoalesced, the 20 missed ticks would run back to back after the slow one: the sixth by about 210 ms. Coalesced,
	// they run once, and the next ticks keep the rate: the sixth no sooner than 200 ms and 3 periods.
	REQUIRE(fires.size() >= 6);
	CHECK(fires[5] - began >= 200ms + 3 * period);
}

TEST_CASE("onWake: a wake rung before it was watched runs its handler once, each ring after the answer runs it again",
          "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	EventWake wake;
	wake.ring();
	wake.ring(); // pending: one write
	std::atomic<int> rung{0};
	std::unique_ptr<ear::Timer> watch = made.loop->onWake(wake, [&] { rung++; });
	bool once = false;
	made.loop->post([&] {}); // a turn
	std::unique_ptr<ear::Timer> look = made.loop->after(30ms, [&] { main.post([&] { once = true; }); });
	REQUIRE(main.runUntil([&] { return once; }));
	CHECK(rung == 1);
	wake.ring();
	REQUIRE(main.runUntil([&] {
		ear::sleeps::poll(1ms, "the second ring's handler");
		return rung == 2;
	}));
	made.loop->post([&] {
		watch.reset();
		look.reset();
	});
	finishAndClose(main, made);
}

TEST_CASE("onWake under a million rings from another thread: the handler sees every ring's data and never sticks",
          "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	EventWake wake;
	constexpr std::uint64_t rings = 1'000'000;
	std::atomic<std::uint64_t> written{0};
	std::uint64_t seen = 0; // the loop's only
	bool all = false;
	std::unique_ptr<ear::Timer> watch = made.loop->onWake(wake, [&] {
		seen = written.load(std::memory_order_acquire);
		if (seen == rings) main.post([&] { all = true; });
	});
	std::thread ringer([&] {
		for (std::uint64_t i = 1; i <= rings; ++i) {
			written.store(i, std::memory_order_release);
			wake.ring();
		}
	});
	ringer.join();
	REQUIRE(main.runUntil([&] { return all; }));
	made.loop->post([&] { watch.reset(); });
	finishAndClose(main, made);
}

TEST_CASE("onReadable runs each time its descriptor is readable", "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	std::array<int, 2> pipe{};
	REQUIRE(::pipe(pipe.data()) == 0);
	std::atomic<int> reads{0};
	std::unique_ptr<ear::Timer> watch = made.loop->onReadable(pipe[0], [&] {
		char byte = 0;
		if (::read(pipe[0], &byte, 1) == 1) reads++;
	});
	for (int i = 1; i <= 3; ++i) {
		REQUIRE(::write(pipe[1], "x", 1) == 1);
		REQUIRE(main.runUntil([&] {
			ear::sleeps::poll(1ms, "the watch's handler reading the byte");
			return reads == i;
		}));
	}
	made.loop->post([&] { watch.reset(); });
	finishAndClose(main, made);
	::close(pipe[0]);
	::close(pipe[1]);
}

TEST_CASE("finish runs the queued work, then the last call on the loop's thread, and still takes work, close cancels "
          "the sources left armed, then refuses work",
          "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::dedicated);
	std::vector<std::string> order; // the loop's only, read once closed
	std::thread::id lastOn;
	std::atomic<int> leftRan{0};
	std::unique_ptr<ear::Timer> left = made.loop->every(1ms, [&] { leftRan++; }); // never destroyed by the state
	made.loop->post([&] { order.push_back("queued"); });
	bool finished = false;
	made.loop->finish(
		[&] {
			order.push_back("last");
			lastOn = std::this_thread::get_id();
		},
		[&] { finished = true; });
	made.loop->post([&] { order.push_back("after finish"); }); // another loop's goodbye: taken
	REQUIRE(main.runUntil([&] { return finished; }));
	bool closed = false;
	made.loop->close([&] { closed = true; });
	REQUIRE(main.runUntil([&] { return closed; }));
	const int ranAtClose = leftRan.load();
	bool refusedRan = false;
	made.loop->post([&] { refusedRan = true; }); // destroyed unrun
	ear::sleeps::negative(30ms, "a cancelled timer's or a refused post's handler");
	CHECK(order == std::vector<std::string>{"queued", "last", "after finish"});
	CHECK(lastOn != std::this_thread::get_id());
	CHECK(leftRan == ranAtClose);
	CHECK_FALSE(refusedRan);
	left.reset(); // after the close, on any thread
}

TEST_CASE("Outside work runs as the node's own until closeToOthers, which destroys what is queued unrun on the loop's "
          "thread and refuses the rest",
          "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::strand);
	Latch hold;
	std::atomic<bool> outsideRan{false};
	std::atomic<int> before{0};
	made.loop->postFromOutside([&] { before++; });
	REQUIRE(main.runUntil([&] {
		ear::sleeps::poll(1ms, "outside work running as the node's own");
		return before == 1;
	}));
	made.loop->post([&] { static_cast<void>(hold.wait()); });
	/// Says where it was destroyed.
	struct Guard {
		std::thread::id* where;
		explicit Guard(std::thread::id* w) : where(w) {}
		Guard(Guard&& other) noexcept : where(std::exchange(other.where, nullptr)) {}
		~Guard() {
			if (where != nullptr) *where = std::this_thread::get_id();
		}
	};
	std::thread::id destroyedOn;
	made.loop->postFromOutside([&, guard = Guard(&destroyedOn)] { outsideRan = true; });
	made.loop->closeToOthers();
	bool refusedRan = false;
	made.loop->postFromOutside([&] { refusedRan = true; }); // refused at once
	hold.open();
	finishAndClose(main, made);
	CHECK(before == 1);
	CHECK_FALSE(outsideRan);
	CHECK_FALSE(refusedRan);
	CHECK(destroyedOn != std::thread::id{});
	CHECK(destroyedOn != std::this_thread::get_id());
}

TEST_CASE(
	"A strand's handlers run one at a time with onStrand() true, a dedicated loop's and the main thread's with it "
	"false",
	"[loops]") {
	TestMainLoop main;
	Loops loops(main, LoopOptions{.strandThreads = 4});
	Made strand;
	Made dedicated;
	make(loops, main, strand, LoopKind::strand);
	make(loops, main, dedicated, LoopKind::dedicated);
	std::atomic<int> inside{0};
	std::atomic<int> most{0};
	std::atomic<int> strandSaid{0};
	std::atomic<int> dedicatedSaid{0};
	for (int i = 0; i < 200; ++i)
		strand.loop->post([&] {
			const int now = ++inside;
			most = std::max(most.load(), now);
			if (ear::onStrand()) strandSaid++;
			--inside;
		});
	dedicated.loop->post([&] {
		if (ear::onStrand()) dedicatedSaid++;
	});
	bool done = false;
	strand.loop->post([&] { main.post([&] { done = true; }); });
	REQUIRE(main.runUntil([&] { return done; }));
	CHECK(most == 1);
	CHECK(strandSaid == 200);
	CHECK(dedicatedSaid == 0);
	CHECK_FALSE(ear::onStrand());
	CHECK(strand.loop->kind() == LoopKind::strand);
	finishAndClose(main, strand);
	finishAndClose(main, dedicated);
}

TEST_CASE("A strand's handler past the slow bound is told once, a dedicated loop's never", "[loops]") {
	TestMainLoop main;
	Loops loops(main, LoopOptions{.strandThreads = 1, .slowBound = 20ms});
	Made strand;
	Made dedicated;
	make(loops, main, strand, LoopKind::strand);
	make(loops, main, dedicated, LoopKind::dedicated);
	for (int i = 0; i < 2; ++i) {
		strand.loop->post([] { ear::sleeps::induce(40ms, "a handler past the slow bound"); });
		dedicated.loop->post([] { ear::sleeps::induce(40ms, "a handler past the slow bound, on a dedicated loop"); });
	}
	finishAndClose(main, strand);
	finishAndClose(main, dedicated);
	REQUIRE(strand.told.slows.size() == 1);
	CHECK(strand.told.slows[0] >= 20);
	CHECK(dedicated.told.slows.empty());
}

TEST_CASE("A stuck strand gets a replacement thread, so another strand's work runs meanwhile", "[loops]") {
	TestMainLoop main;
	Loops loops(main, LoopOptions{.strandThreads = 1, .replaceAfter = 30ms, .maxThreads = 2});
	Made stuck;
	Made other;
	make(loops, main, stuck, LoopKind::strand, "stuck");
	make(loops, main, other, LoopKind::strand, "other");
	Latch hold;
	std::atomic<bool> holding{false};
	stuck.loop->post([&] {
		holding = true;
		static_cast<void>(hold.wait());
	});
	REQUIRE(main.runUntil([&] {
		ear::sleeps::poll(1ms, "the stuck handler began");
		return holding.load();
	}));
	bool ran = false;
	other.loop->post([&] { main.post([&] { ran = true; }); });
	REQUIRE(main.runUntil([&] { return ran; })); // only a replacement thread can run it
	CHECK(loops.poolThreads() == 2);
	hold.open();
	finishAndClose(main, stuck);
	finishAndClose(main, other);
	REQUIRE(main.runUntil([&] {
		ear::sleeps::poll(1ms, "the stuck thread, back, leaves the pool to its size");
		return loops.poolThreads() == 1;
	}));
}

TEST_CASE("A handler's throw is caught and told, and the loop runs on", "[loops]") {
	TestMainLoop main;
	Loops loops(main);
	Made made;
	make(loops, main, made, LoopKind::strand);
	made.loop->post([] { throw std::runtime_error("the library's bad day"); });
	bool after = false;
	made.loop->post([&] { main.post([&] { after = true; }); });
	REQUIRE(main.runUntil([&] { return after && made.told.throws.size() == 1; }));
	CHECK(made.told.throws[0] == "the library's bad day");
	finishAndClose(main, made);
}
