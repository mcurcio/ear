// A configuration's way back to the main loop (completions.hpp): what each configuration's channel posts through, its
// handle's notes and losses today and its I/O jobs' completions from slice 3c's stores and recordings. Through a real
// NodeHost, an old configuration's late note dropped, is node_handle_test.cpp's.
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "completions.hpp"

using ear::detail::Completions;

namespace {

/// The main loop, turned by the test.
class QueueLoop final : public ear::Executor {
public:
	void post(std::move_only_function<void()> work) override { queue_.push_back(std::move(work)); }
	std::size_t queued() const noexcept { return queue_.size(); }
	void run() {
		while (!queue_.empty()) {
			auto work = std::move(queue_.front());
			queue_.pop_front();
			work();
		}
	}

private:
	std::deque<std::move_only_function<void()>> queue_;
};

/// What a job fills, held by shared_ptr: a descriptor it opened, closed by its destructor.
struct Opened {
	int fd = -1;
	Opened() {
		int ends[2];
		REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
		::close(ends[1]);
		fd = ends[0];
	}
	Opened(const Opened&) = delete;
	Opened& operator=(const Opened&) = delete;
	~Opened() { ::close(fd); }
};

bool isOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

} // namespace

TEST_CASE("A completion lands on the main loop in post order, never inside the post", "[completions]") {
	QueueLoop mainLoop;
	const auto completions = std::make_shared<Completions>(mainLoop);
	std::vector<std::string> landed;
	const std::thread::id main = std::this_thread::get_id();
	std::thread job([&] {
		completions->post([&] {
			CHECK(std::this_thread::get_id() == main);
			landed.push_back("opened");
		});
		completions->post([&] { landed.push_back("written"); });
	});
	job.join();
	CHECK(landed.empty());
	mainLoop.run();
	CHECK(landed == std::vector<std::string>{"opened", "written"});
}

TEST_CASE("A completion of a configuration that ended is dropped, and the descriptor its job opened is closed",
          "[completions]") {
	QueueLoop mainLoop;
	auto completions = std::make_shared<Completions>(mainLoop);
	bool ran = false;
	int fd = -1;
	SECTION("posted before the configuration ended, landing after") {
		auto opened = std::make_shared<Opened>();
		fd = opened->fd;
		completions->post([&ran, opened = std::move(opened)] { ran = true; });
		completions->detach();
	}
	SECTION("posted after it ended") {
		completions->detach();
		auto opened = std::make_shared<Opened>();
		fd = opened->fd;
		CHECK_FALSE(completions->attached());
		completions->post([&ran, opened = std::move(opened)] { ran = true; });
	}
	REQUIRE(isOpen(fd));
	completions.reset(); // what is queued holds the way back, not its owner
	mainLoop.run();
	CHECK_FALSE(ran);
	CHECK_FALSE(isOpen(fd));
	CHECK(mainLoop.queued() == 0);
}

namespace {

/// The I/O executor, turned by the test.
class QueueIo final : public ear::IoExecutor {
public:
	using ear::IoExecutor::post;
	void post(std::string job, std::move_only_function<void()> work) override {
		jobs.push_back(std::move(job));
		queue_.push_back(std::move(work));
	}
	void post(std::move_only_function<void()> work) override { post("a job", std::move(work)); }
	std::size_t queued() const noexcept { return queue_.size(); }
	/// Runs what is queued on a thread of its own, as the I/O thread would.
	void runOnItsThread() {
		std::thread thread([this] {
			while (!queue_.empty()) {
				auto work = std::move(queue_.front());
				queue_.pop_front();
				work();
			}
		});
		thread.join();
	}
	std::vector<std::string> jobs;

private:
	std::deque<std::move_only_function<void()>> queue_;
};

/// A job's handle that says where it was released.
struct Handle {
	Opened opened;
	std::thread::id* releasedOn;
	explicit Handle(std::thread::id* on) : releasedOn(on) {}
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	~Handle() { *releasedOn = std::this_thread::get_id(); }
};

} // namespace

TEST_CASE("An I/O job's completion hands its handle to the main loop while attached, and once ended releases it on "
          "the I/O thread, never on the main loop",
          "[completions]") {
	QueueLoop mainLoop;
	QueueIo io;
	const auto completions = std::make_shared<Completions>(mainLoop);
	std::thread::id releasedOn;
	const std::thread::id main = std::this_thread::get_id();

	SECTION("attached: landed with the handle") {
		auto handle = std::make_shared<Handle>(&releasedOn);
		const int fd = handle->opened.fd;
		int seen = -1;
		completions->postHolding<Handle>(std::move(handle), io, "close store",
		                                 [&](Handle& landed) { seen = landed.opened.fd; });
		mainLoop.run();
		CHECK(seen == fd);
		CHECK(io.queued() == 0);
	}
	SECTION("ended: released on the I/O thread") {
		auto handle = std::make_shared<Handle>(&releasedOn);
		const int fd = handle->opened.fd;
		bool ran = false;
		completions->postHolding<Handle>(std::move(handle), io, "close store", [&](Handle&) { ran = true; });
		completions->detach();
		mainLoop.run();
		CHECK_FALSE(ran);
		CHECK(isOpen(fd)); // not closed on the main loop
		CHECK(releasedOn == std::thread::id{});
		REQUIRE(io.jobs == std::vector<std::string>{"close store"});
		io.runOnItsThread();
		CHECK_FALSE(isOpen(fd));
		CHECK(releasedOn != std::thread::id{});
		CHECK(releasedOn != main);
	}
}
