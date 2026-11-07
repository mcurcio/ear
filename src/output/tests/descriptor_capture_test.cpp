// Descriptors 1 and 2 captured (docs/design/node-framework.md section 8.8), in this test program itself: what C's
// streams, iostreams and plain write() put on either becomes lines, in order per descriptor; a line past the bound is
// cut and marked; a last partial line is kept; a watched descriptor's handler runs on the capture thread; stop puts
// the real descriptors back. Each check runs after stop, because Catch reports on the real standard output.
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/eventfd.h>
#include <sys/stat.h>
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
