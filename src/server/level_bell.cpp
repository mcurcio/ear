// The level bell: the eventfd the realtime thread rings when levels are published
// (docs/design/batch2.md sections 2 and 4.6, control_port.hpp). The control port waits for it
// to be readable on the control thread, reads it (which resets it) and then reads the levels.
//
// "Rung at most once until answered" is the eventfd's own counter: every ring until the control
// thread reads it adds to one counter, which wakes the reader once. A ring is one write(2) on a
// non-blocking descriptor: no lock, no allocation, never a wait (EAGAIN only at a counter of
// 2^64 - 2, and then the bell is already rung).
#include "ear/server/control_port.hpp"

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <sys/eventfd.h>
#include <unistd.h>

namespace ear::server {

struct LevelBell::Impl {
	int fd;
};

LevelBell::LevelBell() {
	const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0) throw std::system_error(errno, std::system_category(), "LevelBell: eventfd");
	impl_ = std::make_unique<Impl>(Impl{fd});
}

LevelBell::~LevelBell() { ::close(impl_->fd); }

void LevelBell::levelsPublished() noexcept {
	const std::uint64_t one = 1;
	[[maybe_unused]] const auto written = ::write(impl_->fd, &one, sizeof one);
}

int LevelBell::fd() const noexcept { return impl_->fd; }

} // namespace ear::server
