// The control thread: one io_context, one thread (docs/design/batch2.md sections 2 and 4.6,
// control_thread.hpp). Slice s0 stub: slice R2 builds it.
#include "ear/server/control_thread.hpp"

#include <stdexcept>

namespace ear::server {

struct ControlThread::Impl {};

ControlThread::ControlThread() { throw std::logic_error("ControlThread is not built yet (slice R2)"); }

ControlThread::~ControlThread() = default;

void ControlThread::post(std::move_only_function<void()>) {
	throw std::logic_error("ControlThread::post is not built yet (slice R2)");
}

void ControlThread::stop() { throw std::logic_error("ControlThread::stop is not built yet (slice R2)"); }

} // namespace ear::server
