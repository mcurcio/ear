#include "ear/output/descriptor_capture.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <poll.h>
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

} // namespace

std::expected<std::unique_ptr<DescriptorCapture>, std::string> DescriptorCapture::install() {
	std::unique_ptr<DescriptorCapture> capture(new DescriptorCapture());
	int output[2] = {-1, -1};
	int error[2] = {-1, -1};
	const auto undo = [&](std::string why) {
		for (const int fd : {capture->realOutput_, capture->realError_, output[0], output[1], error[0], error[1]})
			if (fd >= 0) ::close(fd);
		capture->realOutput_ = capture->realError_ = -1;
		return std::unexpected(std::move(why));
	};
	std::fflush(stdout);
	std::fflush(stderr);
	if ((capture->realOutput_ = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3)) < 0) return undo(failed("dup stdout"));
	if ((capture->realError_ = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)) < 0) return undo(failed("dup stderr"));
	if (::pipe2(output, O_CLOEXEC) != 0 || ::pipe2(error, O_CLOEXEC) != 0) return undo(failed("pipe2"));
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
	return capture;
}

DescriptorCapture::~DescriptorCapture() {
	stop();
	for (const int fd : {realOutput_, realError_, outputPipe_, errorPipe_})
		if (fd >= 0) ::close(fd);
}

void DescriptorCapture::watch(int descriptor, std::function<void()> onReady) {
	watched_.push_back({descriptor, std::move(onReady)});
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
	if (realOutput_ >= 0) ::dup2(realOutput_, STDOUT_FILENO);
	if (realError_ >= 0) ::dup2(realError_, STDERR_FILENO);
	if (thread_.joinable()) thread_.join();
}

void DescriptorCapture::run() {
	LineSplitter splitters[2] = {LineSplitter("stdout"), LineSplitter("stderr")};
	std::vector<pollfd> polled{{outputPipe_, POLLIN, 0}, {errorPipe_, POLLIN, 0}};
	for (const Watched& w : watched_) polled.push_back({w.descriptor, POLLIN, 0});
	const auto emit = [this](Line line) { onLine_(std::move(line)); };
	char buffer[65536];
	while (polled[0].fd >= 0 || polled[1].fd >= 0) {
		if (::poll(polled.data(), polled.size(), -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		for (std::size_t i = 0; i < 2; ++i) {
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
		for (std::size_t i = 2; i < polled.size(); ++i)
			if (polled[i].revents != 0) watched_[i - 2].onReady();
	}
	for (const Watched& w : watched_) w.onReady();
}

} // namespace ear::output
