// Descriptors 1 and 2 captured (docs/design/node-framework.md section 8.8), in this test program itself: what C's
// streams, iostreams and plain write() put on either becomes lines, in order per descriptor; a line past the bound is
// cut and marked; a last partial line is kept; a watched descriptor's handler runs on the capture thread; stop puts
// the real descriptors back, even while a duplicate of descriptor 2 keeps a pipe open. Each check runs after stop,
// because Catch reports on the real standard output. A crash's last words reach the real descriptors: those cases run
// in a forked child whose descriptors 1 and 2 are a pipe this test reads.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/output/descriptor_capture.hpp"

using ear::output::DescriptorCapture;

namespace {

struct Seen {
	std::string source;
	std::string text;
	bool cut;
	bool operator==(const Seen&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Seen& seen) {
	return out << seen.source << ":" << seen.text.substr(0, 40) << (seen.cut ? " (cut)" : "");
}

/// The device and inode a descriptor names.
std::pair<dev_t, ino_t> identity(int descriptor) {
	struct stat info{};
	::fstat(descriptor, &info);
	return {info.st_dev, info.st_ino};
}

struct Crashed {
	std::string written; // what reached the child's real descriptors 1 and 2, together
	int signal = 0;      // what ended it; 0: it exited
};

/// Runs `body` in a forked child whose real descriptors 1 and 2 are one pipe, after a capture is installed (never
/// started, so nothing reads the capture's pipes but the crash path); `body` must not return.
template <typename Body> Crashed crashInChild(Body&& body) {
	int ends[2];
	REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
	std::fflush(nullptr);
	const pid_t child = ::fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		const rlimit noCore{0, 0}; // a crash on purpose leaves no core file behind
		::setrlimit(RLIMIT_CORE, &noCore);
		::dup2(ends[1], STDOUT_FILENO);
		::dup2(ends[1], STDERR_FILENO);
		auto installed = DescriptorCapture::install();
		if (!installed) ::_exit(90);
		body(**installed);
		::_exit(91);
	}
	::close(ends[1]);
	Crashed crashed;
	char buffer[4096];
	ssize_t got;
	while ((got = ::read(ends[0], buffer, sizeof buffer)) > 0)
		crashed.written.append(buffer, static_cast<std::size_t>(got));
	::close(ends[0]);
	int status = 0;
	REQUIRE(::waitpid(child, &status, 0) == child);
	if (WIFSIGNALED(status)) crashed.signal = WTERMSIG(status);
	INFO("exit status " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1));
	return crashed;
}

} // namespace

TEST_CASE("What anything writes to descriptors 1 and 2 becomes lines, and stop puts them back", "[capture]") {
	const auto realOut = identity(STDOUT_FILENO);
	const auto realErr = identity(STDERR_FILENO);
	std::mutex mutex;
	std::vector<Seen> seen;
	std::thread::id captureThread;
	int watchedCalls = 0;

	auto installed = DescriptorCapture::install();
	REQUIRE(installed);
	DescriptorCapture& capture = **installed;
	const auto keptOut = identity(capture.realOutput());
	const auto pipeOut = identity(STDOUT_FILENO);
	const int bell = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	capture.watch(bell, [&] {
		std::uint64_t count = 0;
		const bool rang = ::read(bell, &count, sizeof count) == sizeof count;
		const std::scoped_lock lock(mutex);
		if (rang) captureThread = std::this_thread::get_id();
		++watchedCalls;
	});
	capture.start([&](DescriptorCapture::Line line) {
		const std::scoped_lock lock(mutex);
		seen.push_back({std::string(line.source), std::move(line.text), line.cut});
	});

	std::printf("printf %d\n", 1);
	std::cout << "iostream" << std::endl;
	std::fprintf(stderr, "E lib: something\n");
	const std::string longLine(DescriptorCapture::lineBound + 10, 'y');
	(void)::write(STDERR_FILENO, (longLine + "\nafter\n").data(), longLine.size() + 7);
	(void)::write(STDOUT_FILENO, "no newline", 10);
	const std::uint64_t one = 1;
	(void)::write(bell, &one, sizeof one);
	capture.stop();
	const auto restoredOut = identity(STDOUT_FILENO);
	const auto restoredErr = identity(STDERR_FILENO);
	::close(bell);

	CHECK(keptOut == realOut);
	CHECK(pipeOut != realOut);
	CHECK(restoredOut == realOut);
	CHECK(restoredErr == realErr);
	std::vector<Seen> out;
	std::vector<Seen> err;
	for (const Seen& s : seen) (s.source == "stdout" ? out : err).push_back(s);
	CHECK(out == std::vector<Seen>{
					 {"stdout", "printf 1", false}, {"stdout", "iostream", false}, {"stdout", "no newline", false}});
	CHECK(err == std::vector<Seen>{{"stderr", "E lib: something", false},
	                               {"stderr", std::string(DescriptorCapture::lineBound, 'y'), true},
	                               {"stderr", "after", false}});
	CHECK(captureThread != std::thread::id());
	CHECK(captureThread != std::this_thread::get_id());
	CHECK(watchedCalls >= 2); // when it rang, and once more at stop
}

TEST_CASE("Stop without a start, and a second stop, do nothing more", "[capture]") {
	const auto realOut = identity(STDOUT_FILENO);
	auto installed = DescriptorCapture::install();
	REQUIRE(installed);
	(*installed)->stop();
	(*installed)->stop();
	CHECK(identity(STDOUT_FILENO) == realOut);
}

TEST_CASE("Stop ends the capture even while a duplicate of descriptor 2 keeps its pipe open", "[capture]") {
	std::mutex mutex;
	std::vector<std::string> seen;
	auto installed = DescriptorCapture::install();
	REQUIRE(installed);
	DescriptorCapture& capture = **installed;
	capture.start([&](DescriptorCapture::Line line) {
		const std::scoped_lock lock(mutex);
		seen.push_back(std::move(line.text));
	});
	const int duplicate = ::dup(STDERR_FILENO); // as a forked child or a library would hold it
	(void)::write(duplicate, "through a duplicate\n", 20);
	auto stopped = std::async(std::launch::async, [&] { capture.stop(); });
	const bool ended = stopped.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
	::close(duplicate); // lets a hung stop end, so a failure is reported, not waited on
	stopped.wait();
	CHECK(ended);
	const std::scoped_lock lock(mutex);
	CHECK(seen == std::vector<std::string>{"through a duplicate"});
}

TEST_CASE("std::terminate's last words, and what the pipes and the queue still held, reach the real descriptors",
          "[capture][crash]") {
	const Crashed crashed = crashInChild([](DescriptorCapture& capture) {
		capture.onCrash([&] { (void)::write(capture.realOutput(), "a record still queued\n", 22); });
		std::fprintf(stderr, "last words before the throw\n");
		std::cout << "a line on stdout" << std::endl;
		// As an exception out of main ends: uncaught, so std::terminate (the call is opaque, so the compiler cannot warn).
		static void (*volatile thrower)() = [] { throw std::runtime_error("boom"); };
		[]() noexcept { thrower(); }();
	});
	INFO(crashed.written);
	CHECK(crashed.signal == SIGABRT);
	const auto at = [&](std::string_view text) { return crashed.written.find(text); };
	REQUIRE(at("a record still queued") != std::string::npos);
	REQUIRE(at("last words before the throw\n") != std::string::npos);
	REQUIRE(at("a line on stdout\n") != std::string::npos);
	REQUIRE(at("terminate called after throwing an instance of 'std::runtime_error'") != std::string::npos);
	CHECK(at("boom") != std::string::npos);
	CHECK(at("a record still queued") < at("last words before the throw"));
	CHECK(at("last words before the throw") < at("terminate called"));
}

TEST_CASE("What abort's caller wrote last (an assertion's message) reaches the real standard error",
          "[capture][crash]") {
	const Crashed crashed = crashInChild([](DescriptorCapture&) {
		(void)::write(STDERR_FILENO, "ear: engine.cpp:42: Assertion `ready' failed.\n", 46);
		std::abort();
	});
	INFO(crashed.written);
	CHECK(crashed.signal == SIGABRT);
	CHECK(crashed.written == "ear: engine.cpp:42: Assertion `ready' failed.\n");
}

TEST_CASE("A fault's earlier output reaches the real descriptors before the process ends", "[capture][crash]") {
	const Crashed crashed = crashInChild([](DescriptorCapture&) {
		(void)::write(STDOUT_FILENO, "before the fault\n", 17);
		::raise(SIGSEGV);
	});
	CHECK(crashed.signal == SIGSEGV);
	CHECK(crashed.written == "before the fault\n");
}
