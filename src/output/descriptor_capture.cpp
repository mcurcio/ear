#include "ear/output/descriptor_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "masked_thread.hpp"

namespace ear::output {

namespace {

std::string failed(std::string_view call) { return std::string(call) + ": " + std::strerror(errno); }

/// One pipe's bytes, cut into lines.
class LineSplitter {
public:
	explicit LineSplitter(std::string_view source) : source_(source) {}

	template <typename Emit> void add(const char* data, std::size_t size, Emit&& emit) {
		for (std::size_t i = 0; i < size; ++i) {
			const char c = data[i];
			if (c == '\n') {
				if (!discarding_) emit(DescriptorCapture::Line{source_, std::move(pending_), false});
				pending_.clear();
				discarding_ = false;
				continue;
			}
			if (discarding_) continue;
			pending_ += c;
			if (pending_.size() == DescriptorCapture::lineBound) {
				emit(DescriptorCapture::Line{source_, std::move(pending_), true});
				pending_.clear();
				discarding_ = true;
			}
		}
	}

	template <typename Emit> void end(Emit&& emit) {
		if (!pending_.empty()) emit(DescriptorCapture::Line{source_, std::move(pending_), false});
		pending_.clear();
	}

private:
	std::string_view source_;
	std::string pending_;
	bool discarding_ = false;
};

/// Reads what `descriptor` holds now, without waiting, and hands each piece to `take`. Async-signal-safe.
template <typename Take> void readWhatIsThere(int descriptor, Take&& take) {
	const int flags = ::fcntl(descriptor, F_GETFL);
	if (flags >= 0 && (flags & O_NONBLOCK) == 0) ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	char buffer[4096];
	while (true) {
		const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) return; // empty (EAGAIN), or every write end closed
		take(buffer, static_cast<std::size_t>(got));
	}
}

/// Writes all of it, or gives up, never waiting more than 200 ms for the reader to take more (a dying process must
/// die). Async-signal-safe. Chunks of at most PIPE_BUF: once a pipe polls writable, that much is written at once.
void writeRaw(int descriptor, const char* data, std::size_t size) {
	while (size > 0) {
		pollfd ready{descriptor, POLLOUT, 0};
		const int polled = ::poll(&ready, 1, 200);
		if (polled < 0 && errno == EINTR) continue;
		if (polled <= 0 || (ready.revents & POLLOUT) == 0) return;
		const ssize_t wrote = ::write(descriptor, data, std::min<std::size_t>(PIPE_BUF, size));
		if (wrote < 0 && errno == EINTR) continue;
		if (wrote <= 0) return;
		data += wrote;
		size -= static_cast<std::size_t>(wrote);
	}
}

// The crash path's state: what a signal handler may read, set while a capture is installed. One capture at a time.
std::atomic<int> crashRealOutput{-1};
std::atomic<int> crashRealError{-1};
std::atomic<int> crashOutputPipe{-1};
std::atomic<int> crashErrorPipe{-1};
std::atomic<bool> spilled{false};
std::atomic<DescriptorCapture*> crashCapture{nullptr};
std::terminate_handler previousTerminate = nullptr;
constexpr int crashSignals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};
struct sigaction previousActions[std::size(crashSignals)];

/// The crash path's one step, async-signal-safe: once, writes what the pipes still hold, as raw bytes, to the real
/// descriptors, and puts the real descriptors back on 1 and 2, so what the dying process writes next (abort's,
/// terminate's or assert's last words) reaches them directly.
void spill() noexcept {
	if (spilled.exchange(true)) return;
	const int realOutput = crashRealOutput.load();
	const int realError = crashRealError.load();
	if (realOutput < 0 || realError < 0) return;
	const int savedErrno = errno;
	readWhatIsThere(crashOutputPipe.load(),
	                [&](const char* data, std::size_t size) { writeRaw(realOutput, data, size); });
	readWhatIsThere(crashErrorPipe.load(),
	                [&](const char* data, std::size_t size) { writeRaw(realError, data, size); });
	::dup2(realOutput, STDOUT_FILENO);
	::dup2(realError, STDERR_FILENO);
	errno = savedErrno;
}

void onCrashSignal(int signal) {
	spill();
	// The disposition ear found (the default: end the process, with a core), then the signal again: it is blocked
	// while this handler runs, so it lands as the handler returns.
	for (std::size_t i = 0; i < std::size(crashSignals); ++i)
		if (crashSignals[i] == signal) ::sigaction(signal, &previousActions[i], nullptr);
	::raise(signal);
}

/// std::terminate's way out (an exception out of main or a thread, a noexcept broken): the records still queued, then
/// the pipes, then the handler ear found, which prints "terminate called after throwing ..." to the real stderr and
/// aborts.
[[noreturn]] void onTerminate() {
	if (DescriptorCapture* capture = crashCapture.load()) capture->beforeCrash();
	spill();
	if (previousTerminate != nullptr) previousTerminate();
	std::abort();
}

void armCrashPath(DescriptorCapture* capture, int realOutput, int realError, int outputPipe, int errorPipe) {
	crashRealOutput = realOutput;
	crashRealError = realError;
	crashOutputPipe = outputPipe;
	crashErrorPipe = errorPipe;
	spilled = false;
	crashCapture = capture;
	previousTerminate = std::set_terminate(&onTerminate);
	struct sigaction action{};
	action.sa_handler = &onCrashSignal;
	sigemptyset(&action.sa_mask);
	for (std::size_t i = 0; i < std::size(crashSignals); ++i)
		::sigaction(crashSignals[i], &action, &previousActions[i]);
}

void disarmCrashPath() {
	for (std::size_t i = 0; i < std::size(crashSignals); ++i)
		::sigaction(crashSignals[i], &previousActions[i], nullptr);
	std::set_terminate(previousTerminate);
	previousTerminate = nullptr;
	crashCapture = nullptr;
	crashOutputPipe = crashErrorPipe = crashRealOutput = crashRealError = -1;
}

} // namespace

std::expected<std::unique_ptr<DescriptorCapture>, std::string> DescriptorCapture::install() {
	std::unique_ptr<DescriptorCapture> capture(new DescriptorCapture());
	int output[2] = {-1, -1};
	int error[2] = {-1, -1};
	const auto undo = [&](std::string why) {
		for (const int fd :
		     {capture->realOutput_, capture->realError_, capture->wake_, output[0], output[1], error[0], error[1]})
			if (fd >= 0) ::close(fd);
		capture->realOutput_ = capture->realError_ = capture->wake_ = -1;
		return std::unexpected(std::move(why));
	};
	std::fflush(stdout);
	std::fflush(stderr);
	if ((capture->realOutput_ = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3)) < 0) return undo(failed("dup stdout"));
	if ((capture->realError_ = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)) < 0) return undo(failed("dup stderr"));
	if (::pipe2(output, O_CLOEXEC) != 0 || ::pipe2(error, O_CLOEXEC) != 0) return undo(failed("pipe2"));
	if ((capture->wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) < 0) return undo(failed("eventfd"));
	if (::dup2(output[1], STDOUT_FILENO) < 0) return undo(failed("dup2 stdout"));
	if (::dup2(error[1], STDERR_FILENO) < 0) {
		::dup2(capture->realOutput_, STDOUT_FILENO);
		return undo(failed("dup2 stderr"));
	}
	// Descriptors 1 and 2 are now the pipes' only write ends, so putting the real ones back ends the reads.
	::close(output[1]);
	::close(error[1]);
	capture->outputPipe_ = output[0];
	capture->errorPipe_ = error[0];
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	armCrashPath(capture.get(), capture->realOutput_, capture->realError_, output[0], error[0]);
	capture->armed_ = true;
	return capture;
}

DescriptorCapture::~DescriptorCapture() {
	stop();
	for (const int fd : {realOutput_, realError_, outputPipe_, errorPipe_, wake_})
		if (fd >= 0) ::close(fd);
}

void DescriptorCapture::watch(int descriptor, std::function<void()> onReady) {
	watched_.push_back({descriptor, std::move(onReady)});
}

void DescriptorCapture::onCrash(std::function<void()> hook) { onCrash_ = std::move(hook); }

void DescriptorCapture::beforeCrash() noexcept {
	if (onCrash_) try {
			onCrash_();
		} catch (...) {
		}
}

void DescriptorCapture::lastWords(std::string_view line) noexcept {
	// The crash path's order, with the line last: what is queued, what the pipes hold, then the line, on the real
	// standard error. std::terminate's own path after this finds the pipes spilled and the queue written.
	beforeCrash();
	spill();
	const int realError = crashRealError.load() >= 0 ? crashRealError.load() : realError_;
	writeRaw(realError, line.data(), line.size());
}

void DescriptorCapture::start(std::function<void(Line)> onLine) {
	onLine_ = std::move(onLine);
	thread_ = maskedThread([this] { run(); });
}

void DescriptorCapture::stop() {
	if (stopped_) return;
	stopped_ = true;
	std::fflush(stdout);
	std::fflush(stderr);
	if (armed_) disarmCrashPath();
	armed_ = false;
	if (realOutput_ >= 0) ::dup2(realOutput_, STDOUT_FILENO);
	if (realError_ >= 0) ::dup2(realError_, STDERR_FILENO);
	// The thread would end at the pipes' end of file, but a duplicate of descriptor 1 or 2 that outlives this (a forked
	// child's, a library's dup) keeps a write end open: the bell tells it to read what is there and end.
	const std::uint64_t one = 1;
	[[maybe_unused]] const ssize_t rang = ::write(wake_, &one, sizeof one);
	if (thread_.joinable()) thread_.join();
}

void DescriptorCapture::run() {
	LineSplitter splitters[2] = {LineSplitter("stdout"), LineSplitter("stderr")};
	constexpr std::size_t pipes = 2;
	constexpr std::size_t firstWatched = pipes + 1; // after the stop bell
	std::vector<pollfd> polled{{outputPipe_, POLLIN, 0}, {errorPipe_, POLLIN, 0}, {wake_, POLLIN, 0}};
	for (const Watched& w : watched_) polled.push_back({w.descriptor, POLLIN, 0});
	const auto emit = [this](Line line) { onLine_(std::move(line)); };
	char buffer[65536];
	while (polled[0].fd >= 0 || polled[1].fd >= 0) {
		if (::poll(polled.data(), polled.size(), -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		for (std::size_t i = 0; i < pipes; ++i) {
			if (polled[i].fd < 0 || polled[i].revents == 0) continue;
			const ssize_t got = ::read(polled[i].fd, buffer, sizeof buffer);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) { // every write end closed: stop() put the real descriptor back
				splitters[i].end(emit);
				polled[i].fd = -1;
				continue;
			}
			splitters[i].add(buffer, static_cast<std::size_t>(got), emit);
		}
		for (std::size_t i = firstWatched; i < polled.size(); ++i)
			if (polled[i].revents != 0) watched_[i - firstWatched].onReady();
		if (polled[pipes].revents != 0) { // stop(), after this turn's reads and handlers: the pipes' last
			for (std::size_t i = 0; i < pipes; ++i) {
				if (polled[i].fd < 0) continue;
				readWhatIsThere(polled[i].fd,
				                [&](const char* data, std::size_t size) { splitters[i].add(data, size, emit); });
				splitters[i].end(emit);
				polled[i].fd = -1;
			}
			break;
		}
	}
	for (const Watched& w : watched_) w.onReady();
}

} // namespace ear::output
