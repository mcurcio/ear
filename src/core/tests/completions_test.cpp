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
