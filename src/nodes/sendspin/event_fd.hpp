// An eventfd's three moves, in one place: made non-blocking and close-on-exec, rung (realtime-safe: one write of an
// eventfd never blocks here), drained. Waiters poll fd().
#pragma once
#include <cerrno>
#include <cstdint>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>
namespace ear::sendspin {
class EventFd {
public:
	/// Throws std::system_error when it cannot make one.
	EventFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
		if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
	}
	~EventFd() { ::close(fd_); }
	EventFd(const EventFd&) = delete;
	EventFd& operator=(const EventFd&) = delete;
	int fd() const noexcept { return fd_; }
	void ring() const noexcept {
		const std::uint64_t one = 1;
		[[maybe_unused]] const auto ignored = ::write(fd_, &one, sizeof one);
	}
	/// Whether it had rung since the last drain.
	bool drain() const noexcept {
		std::uint64_t rings = 0;
		return ::read(fd_, &rings, sizeof rings) == sizeof rings;
	}

private:
	int fd_;
};
} // namespace ear::sendspin
