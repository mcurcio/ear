// The output thread (docs/design/node-framework.md section 8.8): lines in order, a full queue refusing at once instead
// of waiting behind a reader that does not read, everything queued written at stop, and a closed reader survived.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/output/output_writer.hpp"

using ear::output::OutputWriter;

namespace {

struct Pipe {
	int read = -1;
	int write = -1;
	Pipe() {
		int ends[2];
		REQUIRE(::pipe2(ends, O_CLOEXEC) == 0);
		read = ends[0];
		write = ends[1];
	}
	~Pipe() {
		if (read >= 0) ::close(read);
		if (write >= 0) ::close(write);
	}
	/// Everything until every write end is closed.
	std::string readAll() const {
		std::string all;
		char buffer[4096];
		ssize_t got;
		while ((got = ::read(read, buffer, sizeof buffer)) > 0) all.append(buffer, static_cast<std::size_t>(got));
		return all;
	}
	/// Fills the pipe's buffer, so the next blocking write waits for a reader.
	std::size_t fill() const {
		const int flags = ::fcntl(write, F_GETFL);
		::fcntl(write, F_SETFL, flags | O_NONBLOCK);
		std::size_t filled = 0;
		const std::string block(4096, 'x');
		while (true) {
			const ssize_t wrote = ::write(write, block.data(), block.size());
			if (wrote <= 0) break;
			filled += static_cast<std::size_t>(wrote);
		}
		::fcntl(write, F_SETFL, flags);
		return filled;
	}
};

} // namespace

TEST_CASE("Lines are written in the order offered, each with its newline", "[output_writer]") {
	Pipe pipe;
	{
		OutputWriter writer(pipe.write);
		for (int i = 0; i < 100; ++i) REQUIRE(writer.offer(std::to_string(i)));
	} // stop: everything queued is written
	::close(pipe.write);
	pipe.write = -1;
	std::string expected;
	for (int i = 0; i < 100; ++i) expected += std::to_string(i) + "\n";
	CHECK(pipe.readAll() == expected);
}

TEST_CASE("A reader that does not read fills the queue, and offers are refused, not held", "[output_writer]") {
	Pipe pipe;
	const std::size_t filled = pipe.fill();
	constexpr std::size_t capacity = 8;
	OutputWriter writer(pipe.write, capacity);
	std::vector<std::string> accepted;
	bool refused = false;
	for (int i = 0; i < 1000 && !refused; ++i) {
		std::string line = "line " + std::to_string(i);
		if (writer.offer(line)) accepted.push_back(line);
		else refused = true;
	}
	CHECK(refused);
	// The writer holds at most one batch in its blocked write, besides the queue.
	CHECK(accepted.size() <= 2 * capacity);

	std::string got;
	std::thread reader([&] { got = pipe.readAll(); });
	writer.stop();
	::close(pipe.write);
	pipe.write = -1;
	reader.join();
	std::string expected;
	for (const std::string& line : accepted) expected += line + "\n";
	REQUIRE(got.size() == filled + expected.size());
	CHECK(got.substr(filled) == expected);
	CHECK_FALSE(writer.offer("after stop"));
}

TEST_CASE("A closed reader is survived: the write fails, the process goes on", "[output_writer]") {
	Pipe pipe;
	::close(pipe.read);
	pipe.read = -1;
	OutputWriter writer(pipe.write);
	CHECK(writer.offer("nobody reads this"));
	writer.stop(); // without the writer's blocked signals, SIGPIPE would end the test program here
	CHECK(true);
}

TEST_CASE("The queue is bounded in bytes too, and a line larger than the bound still goes alone", "[output_writer]") {
	Pipe pipe;
	const std::size_t filled = pipe.fill();
	OutputWriter writer(pipe.write, 100, 64);
	const std::string large(1000, 'L');
	CHECK(writer.offer(large)); // nothing waits: accepted whatever its size
	std::vector<std::string> accepted;
	for (int i = 0; i < 10; ++i) {
		std::string line(29, static_cast<char>('a' + i)); // 30 bytes with its newline
		if (!writer.offer(line)) break;
		accepted.push_back(std::move(line));
	}
	// The large line is in the blocked write (then two fit in 64 bytes) or still queued (then none does).
	CHECK(accepted.size() <= 2);

	std::string got;
	std::thread reader([&] { got = pipe.readAll(); });
	writer.stop();
	::close(pipe.write);
	pipe.write = -1;
	reader.join();
	std::string expected = large + "\n";
	for (const std::string& line : accepted) expected += line + "\n";
	CHECK(got.substr(filled) == expected);
}

TEST_CASE("The crash path's spill gives up in bounded time on a writer stuck in its write", "[output_writer]") {
	Pipe pipe;
	const std::size_t filled = pipe.fill();
	OutputWriter writer(pipe.write);
	REQUIRE(writer.offer("taken into the blocked write"));
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // lets the writer take it; either way spill is bounded
	REQUIRE(writer.offer("still queued"));
	const auto before = std::chrono::steady_clock::now();
	writer.spill();
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));

	std::string got;
	std::thread reader([&] { got = pipe.readAll(); });
	writer.stop();
	::close(pipe.write);
	pipe.write = -1;
	reader.join();
	// Stuck in its write, the writer keeps the queue's line; had it not taken the first yet, spill took both and gave up
	// on the full pipe, and only what the writer had is there.
	const std::string all = "taken into the blocked write\nstill queued\n";
	CHECK(got.substr(filled) == all.substr(0, got.size() - filled));
}
