// The I/O thread on its own (docs/architecture.md#threads): jobs off the calling thread in post order, completions back
// on the thread that runs the main loop, and its stop. Each wait is for the event it needs; a deadline only fails.
// That a configuration's completion is dropped once it ended is core's (completions_test.cpp). A job's throw that no
// adapter caught is said, then ends the process: in a forked child.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <cstdio>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/runtime/io_thread.hpp"
#include "sleeps.hpp"

using ear::runtime::IoThread;
using namespace std::chrono_literals;

namespace {

constexpr auto deadline = 10s;

/// For the tests whose jobs never throw: hearing a fault fails the test.
struct NoFaults final : ear::InternalErrorListener {
	void internalError(const ear::InternalError& error) override {
		FAIL("a fault: " << error.callback << " " << error.where << ": " << error.what);
	}
};

/// A main loop the test turns: posts from any thread, run on the test's own.
class TestMainLoop final : public ear::Executor {
public:
	void post(std::move_only_function<void()> work) override {
		const std::scoped_lock lock(mutex_);
		queue_.push_back(std::move(work));
		posted_.notify_all();
	}
	/// Waits until `count` are queued (false past the deadline), then runs what is queued.
	bool runOnce(std::size_t count) {
		std::deque<std::move_only_function<void()>> due;
		{
			std::unique_lock lock(mutex_);
			if (!posted_.wait_for(lock, deadline, [&] { return queue_.size() >= count; })) return false;
			due.swap(queue_);
		}
		for (auto& work : due) work();
		return true;
	}

private:
	std::mutex mutex_;
	std::condition_variable posted_;
	std::deque<std::move_only_function<void()>> queue_;
};

/// What a job holds by shared_ptr: gone once the last holder is.
struct Held {};

/// True once `held` is gone, looking every millisecond until the deadline.
bool goneSoon(const std::weak_ptr<Held>& held) {
	const auto until = std::chrono::steady_clock::now() + deadline;
	while (!held.expired()) {
		if (std::chrono::steady_clock::now() > until) return false;
		ear::sleeps::poll(1ms, "the stop destroys the queued jobs on its own thread");
	}
	return true;
}

} // namespace

TEST_CASE("Jobs run on the I/O thread in post order, and each completion is delivered on the main loop",
          "[io_thread]") {
	NoFaults faults;
	IoThread io(faults);
	TestMainLoop mainLoop;
	const std::thread::id test = std::this_thread::get_id();
	std::vector<std::string> ran;            // the main loop's only
	std::vector<std::thread::id> jobThreads; // written by the jobs, read once their completions landed
	std::mutex jobs;
	for (const std::string name : {"one", "two", "three"}) {
		io.post("open " + name, [&, name] {
			{
				const std::scoped_lock lock(jobs);
				jobThreads.push_back(std::this_thread::get_id());
			}
			mainLoop.post([&, name] {
				CHECK(std::this_thread::get_id() == test);
				ran.push_back(name);
			});
		});
	}
	REQUIRE(mainLoop.runOnce(3));
	CHECK(ran == std::vector<std::string>{"one", "two", "three"});
	const std::scoped_lock lock(jobs);
	REQUIRE(jobThreads.size() == 3);
	CHECK(jobThreads[0] != test);
	CHECK(jobThreads[1] == jobThreads[0]);
	CHECK(jobThreads[2] == jobThreads[0]);
	CHECK(io.stop() == std::nullopt);
}

TEST_CASE("A stop destroys the queued jobs unrun and gives the job in flight its grace to finish", "[io_thread]") {
	NoFaults faults;
	IoThread io(faults);
	std::promise<void> started;
	std::promise<void> release;
	io.post("open take1.wav", [&, wait = release.get_future()] {
		started.set_value();
		wait.wait();
	});
	auto queued = std::make_shared<Held>();
	const std::weak_ptr<Held> watch = queued;
	bool queuedRan = false;
	io.post("open take2.wav", [&queuedRan, held = std::move(queued)] { queuedRan = true; });
	REQUIRE(started.get_future().wait_for(deadline) == std::future_status::ready);

	// The job in flight is released only once the stop has destroyed the queued one, so the stop is what dropped it.
	std::thread releaser([&] {
		if (goneSoon(watch)) release.set_value();
	});
	CHECK(io.stop(deadline) == std::nullopt);
	releaser.join();
	CHECK(watch.expired());
	CHECK_FALSE(queuedRan);
}

TEST_CASE("A stop that finds a job still running names it, and a later stop ends once it finished", "[io_thread]") {
	NoFaults faults;
	IoThread io(faults);
	std::promise<void> started;
	std::promise<void> release;
	io.post("open take1.wav", [&, wait = release.get_future()] {
		started.set_value();
		wait.wait();
	});
	REQUIRE(started.get_future().wait_for(deadline) == std::future_status::ready);
	CHECK(io.stop(20ms) == std::optional<std::string>("open take1.wav"));
	release.set_value();
	CHECK(io.stop(deadline) == std::nullopt);
}

TEST_CASE("Work posted after a stop is destroyed unrun on the posting thread", "[io_thread]") {
	NoFaults faults;
	IoThread io(faults);
	REQUIRE(io.stop() == std::nullopt);
	auto state = std::make_shared<Held>();
	const std::weak_ptr<Held> watch = state;
	bool ran = false;
	io.post("open take1.wav", [&ran, held = std::move(state)] { ran = true; });
	CHECK(watch.expired());
	CHECK_FALSE(ran);
}

// docs/architecture.md#faults-by-owner: a job's throw that no adapter caught has no owner, so it ends the process, said
// first as a fatal ear.internal-error, never a bare std::terminate.
TEST_CASE("A job's throw no adapter caught is said as a fatal internal error, then the process ends", "[io_thread]") {
	int ends[2];
	REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		const rlimit noCore{0, 0}; // a crash on purpose leaves no core file behind
		::setrlimit(RLIMIT_CORE, &noCore);
		::close(ends[0]);
		const int quiet = ::open("/dev/null", O_WRONLY | O_CLOEXEC); // terminate's own message
		if (quiet >= 0) ::dup2(quiet, STDERR_FILENO);
		struct ToPipe final : ear::InternalErrorListener {
			int fd;
			explicit ToPipe(int d) : fd(d) {}
			void internalError(const ear::InternalError& error) override {
				const std::string line = std::string(error.callback) + "|" + std::string(error.where) + "|" +
				                         std::string(error.what) + "|" + (error.fatal ? "fatal" : "owned") + "|" +
				                         std::string(error.node) + "\n";
				[[maybe_unused]] const auto wrote = ::write(fd, line.data(), line.size());
			}
		} told(ends[1]);
		IoThread io(told);
		io.post("open take1.wav", [] { throw std::runtime_error("the disk is on fire"); });
		for (;;) ::pause(); // the I/O thread ends the process
	}
	::close(ends[1]);
	std::string written;
	char buffer[512];
	ssize_t got;
	while ((got = ::read(ends[0], buffer, sizeof buffer)) > 0) written.append(buffer, static_cast<std::size_t>(got));
	::close(ends[0]);
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	CHECK(written == "IoThread job|open take1.wav|the disk is on fire|fatal|\n");
	REQUIRE(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
}
