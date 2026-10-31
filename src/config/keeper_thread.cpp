// The graph file keeper's thread (keeper_thread.hpp): posted work in post order, folder changes as
// disk events, and free after each wake once nothing posted is left.
#include "keeper_thread.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace ear::config {
namespace {

constexpr std::uint32_t fileEvents = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE;
constexpr std::uint32_t folderEvents = IN_DELETE_SELF | IN_MOVE_SELF;

std::string failed(std::string_view what) { return std::format("graph file watch: {}: {}", what, std::strerror(errno)); }

} // namespace

std::expected<std::unique_ptr<KeeperThread>, std::string> KeeperThread::start(std::string directory, std::string fileName,
                                                                               Handlers handlers) {
	const int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch < 0) return std::unexpected(failed("inotify_init1"));
	if (inotify_add_watch(watch, directory.c_str(), fileEvents | folderEvents | IN_ONLYDIR) < 0) {
		auto error = failed(std::format("cannot watch {}", directory));
		::close(watch);
		return std::unexpected(std::move(error));
	}
	const int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (wake < 0) {
		auto error = failed("eventfd");
		::close(watch);
		return std::unexpected(std::move(error));
	}
	std::unique_ptr<KeeperThread> thread(new KeeperThread(wake, watch, std::move(fileName), std::move(handlers)));
	thread->thread_ = std::thread([self = thread.get()] { self->run(); });
	return thread;
}

KeeperThread::KeeperThread(int wake, int watch, std::string fileName, Handlers handlers)
	: wake_(wake), watch_(watch), fileName_(std::move(fileName)), handlers_(std::move(handlers)) {}

KeeperThread::~KeeperThread() {
	stop();
	::close(wake_);
	::close(watch_);
}

void KeeperThread::post(std::move_only_function<void()> work) {
	{
		const std::scoped_lock lock(mutex_);
		if (stopping_) return;   // destroyed unrun, outside the lock
		queue_.push_back(std::move(work));
	}
	const std::uint64_t one = 1;
	[[maybe_unused]] const auto written = ::write(wake_, &one, sizeof one);
}

void KeeperThread::stop() {
	{
		const std::scoped_lock lock(mutex_);
		stopping_ = true;
	}
	const std::uint64_t one = 1;
	[[maybe_unused]] const auto written = ::write(wake_, &one, sizeof one);
	if (thread_.joinable()) thread_.join();
}

void KeeperThread::run() {
	std::array<pollfd, 2> fds{{{wake_, POLLIN, 0}, {watch_, POLLIN, 0}}};
	alignas(inotify_event) std::array<char, 4096> buffer{};
	for (;;) {
		if (::poll(fds.data(), fds.size(), -1) < 0) {
			if (errno == EINTR) continue;
			return;
		}
		bool disk = false;
		for (;;) {   // drain the watch: every event read is judged together
			const auto got = ::read(watch_, buffer.data(), buffer.size());
			if (got <= 0) break;
			disk = disk || namesFile(buffer.data(), got);
		}
		std::uint64_t count = 0;
		[[maybe_unused]] const auto read = ::read(wake_, &count, sizeof count);
		std::deque<std::move_only_function<void()>> batch;
		bool last = false;
		{
			const std::scoped_lock lock(mutex_);
			batch.swap(queue_);
			last = stopping_;
		}
		for (auto& work : batch) work();
		batch.clear();
		if (disk && !last) handlers_.diskEvent();   // after the work posted before it (the keeper's start first)
		bool idle = false;
		{
			const std::scoped_lock lock(mutex_);
			idle = queue_.empty();
		}
		if (idle) handlers_.free();
		if (last) return;
	}
}

bool KeeperThread::namesFile(const char* buffer, long size) const {
	for (long at = 0; at < size;) {
		const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
		if ((event->mask & (IN_Q_OVERFLOW | IN_IGNORED | folderEvents)) != 0) return true;
		if ((event->mask & fileEvents) != 0 && event->len > 0 && fileName_ == event->name) return true;
		at += static_cast<long>(sizeof(inotify_event) + event->len);
	}
	return false;
}

} // namespace ear::config
