#include "ear/runtime/port_probe.hpp"

#include <cerrno>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::runtime {
namespace {

/// Binds a fresh socket of `type` to every IPv4 address at `port`, with SO_REUSEADDR when `reuse`, and closes it.
std::expected<void, std::string> bindOnce(int type, std::uint16_t port, bool reuse) {
	const int probe = ::socket(AF_INET, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (probe < 0) return std::unexpected(std::system_category().message(errno));
	const int enable = 1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	const bool bound = (!reuse || ::setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0) &&
	                   ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
	const int why = errno;
	::close(probe);
	if (!bound) return std::unexpected(std::system_category().message(why));
	return {};
}

} // namespace

std::expected<void, std::string> KernelPortProbe::tcpListen(std::uint16_t port) {
	return bindOnce(SOCK_STREAM, port, true);
}

std::expected<void, std::string> KernelPortProbe::udp(std::uint16_t port) { return bindOnce(SOCK_DGRAM, port, false); }

} // namespace ear::runtime
