// Port 7777: HTTP for the UI's files and WebSocket JSON-RPC at /rpc, on the control thread
// (docs/design/batch2.md sections 4.6 and 6, control_port.hpp). Slice s0 stub: slice R2 builds it.
#include "ear/server/control_port.hpp"

#include <stdexcept>
#include <utility>

namespace ear::server {

struct ControlPort::Impl {};

std::expected<std::unique_ptr<ControlPort>, ControlPortError> ControlPort::start(const ControlPortSettings&,
                                                                                ControlThread&,
                                                                                control::GraphGateway&,
                                                                                LevelReader&, LevelBell&) {
	return std::unexpected(ControlPortError{"the control port is not built yet (slice R2)"});
}

ControlPort::ControlPort(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ControlPort::~ControlPort() = default;

std::uint16_t ControlPort::port() const noexcept { return 0; }   // R2

void ControlPort::stop() { throw std::logic_error("ControlPort::stop is not built yet (slice R2)"); }

} // namespace ear::server
