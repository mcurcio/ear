// The kernel's word on a port (docs/writing-a-node-type.md#claims): core's PortProbe, which NodeHost asks once the claim
// registry granted a port and before anything of the configuration is opened. Main thread; a non-blocking socket
// bound and closed at once, so it never waits.
#pragma once
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include "ear/core/claims.hpp"
namespace ear::runtime {
class KernelPortProbe final : public PortProbe {
public:
	/// IPv4, all addresses, SO_REUSEADDR set (as a listener binds): a live listener refuses, a TIME_WAIT passes.
	std::expected<void, std::string> tcpListen(std::uint16_t port) override;
	/// No group: IPv4, all addresses, neither SO_REUSEADDR nor SO_REUSEPORT, so any socket bound to the port refuses,
	/// PipeWire's RTP receiver (which sets SO_REUSEADDR) among them. A multicast group: `<group>:<port>` with
	/// SO_REUSEADDR, as that receiver binds it, so a socket sharing the port by SO_REUSEADDR passes.
	std::expected<void, std::string> udp(std::uint16_t port, std::string_view group) override;
};
} // namespace ear::runtime
