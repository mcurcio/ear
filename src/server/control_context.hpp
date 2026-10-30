// Private to ear_server: how the control port reaches the control thread's io_context, which
// control_thread.hpp keeps out of its public header (no Asio type there).
#pragma once
#include <boost/asio/io_context.hpp>

namespace ear::server {

/// The io_context of the ControlThread running the calling code. Call it only from work posted
/// to a ControlThread; anywhere else it throws std::logic_error. Each control thread sets it for
/// its whole life (the per-thread lookup ControlHub uses for its sessions, session.cpp).
boost::asio::io_context& currentControlContext();

} // namespace ear::server
