// What a library in the test's own process says on standard error, read as events (header-only; the target
// ear_standard_error_lines puts it on the include path): for a library with no log hook, whose own word is a line it
// prints (docs/testing.md#the-librarys-own-word). It lives here, not beside the node's tests, because a node's own
// folder never names standard error (docs/output-records.md's guard).
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ear::test {

/// Standard error goes through a pipe for this object's life. Each whole line that holds one of `markers` is kept,
/// stamped with the value `stamp` holds when it was read (a count the test keeps, such as cycles recorded), and every
/// line that does not start with one of `swallowed` is passed on to the real standard error: what the test asked the
/// library to say is the test's alone, everything else is still shown. Make it before the library's threads start and
/// let it go after they stop.
class StandardErrorLines {
public:
	struct Line {
		std::size_t stamp; // `stamp` when the line was read
		std::string text;  // with its newline
	};

	StandardErrorLines(std::vector<std::string> markers, std::vector<std::string> swallowed)
		: markers_(std::move(markers)), swallowed_(std::move(swallowed)) {
		int ends[2];
		if (::pipe2(ends, O_CLOEXEC) != 0) throw std::runtime_error("StandardErrorLines: pipe2 failed");
		std::fflush(stderr);
		saved_ = ::dup(STDERR_FILENO);
		if (saved_ < 0 || ::dup2(ends[1], STDERR_FILENO) != STDERR_FILENO) {
			::close(ends[0]);
			::close(ends[1]);
			throw std::runtime_error("StandardErrorLines: could not move standard error");
		}
		::close(ends[1]);
		read_ = ends[0];
		reader_ = std::thread([this] { run(); });
	}
	~StandardErrorLines() {
		std::fflush(stderr);
		::dup2(saved_, STDERR_FILENO); // the pipe's last write end goes: the reader sees its end
		reader_.join();
		::close(read_);
		::close(saved_);
	}
	StandardErrorLines(const StandardErrorLines&) = delete;
	StandardErrorLines& operator=(const StandardErrorLines&) = delete;

	/// What each kept line is stamped with from now on; nothing (0) until it is set.
	void stampFrom(const std::atomic<std::size_t>& count) { stamp_.store(&count, std::memory_order_release); }
	std::vector<Line> lines() const {
		const std::scoped_lock lock(mutex_);
		return lines_;
	}

private:
	void run() {
		std::string partial;
		char buffer[4096];
		for (;;) {
			const ssize_t got = ::read(read_, buffer, sizeof buffer);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) return;
			partial.append(buffer, static_cast<std::size_t>(got));
			for (std::size_t end; (end = partial.find('\n')) != std::string::npos; partial.erase(0, end + 1))
				take(partial.substr(0, end + 1));
		}
	}
	/// To the real standard error; a line it cannot take is lost, as it would be there.
	void passOn(const std::string& line) const {
		[[maybe_unused]] const ssize_t passed = ::write(saved_, line.data(), line.size());
	}
	void take(const std::string& line) {
		bool swallow = false;
		for (const std::string& prefix : swallowed_) swallow = swallow || line.starts_with(prefix);
		if (!swallow) passOn(line);
		bool kept = false;
		for (const std::string& marker : markers_) kept = kept || line.find(marker) != std::string::npos;
		if (!kept) return;
		const std::atomic<std::size_t>* stamp = stamp_.load(std::memory_order_acquire);
		const std::size_t at = stamp != nullptr ? stamp->load(std::memory_order_acquire) : 0;
		const std::scoped_lock lock(mutex_);
		lines_.push_back({at, line});
	}

	std::vector<std::string> markers_;
	std::vector<std::string> swallowed_;
	int saved_ = -1;
	int read_ = -1;
	std::atomic<const std::atomic<std::size_t>*> stamp_{nullptr};
	mutable std::mutex mutex_;
	std::vector<Line> lines_;
	std::thread reader_;
};

} // namespace ear::test
