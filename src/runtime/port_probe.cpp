#include "ear/runtime/port_probe.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::runtime {
namespace {

/// Binds a fresh socket of `type` to `host` (network order; INADDR_ANY: every IPv4 address) at `port`, with
/// SO_REUSEADDR when `reuse`, and closes it.
std::expected<void, std::string> bindOnce(int type, std::uint16_t port, bool reuse, in_addr_t host) {
	const int probe = ::socket(AF_INET, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (probe < 0) return std::unexpected(std::system_category().message(errno));
	const int enable = 1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = host;
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
	return bindOnce(SOCK_STREAM, port, true, htonl(INADDR_ANY));
}

std::expected<void, std::string> KernelPortProbe::udp(std::uint16_t port, std::string_view group) {
	if (group.empty()) return bindOnce(SOCK_DGRAM, port, false, htonl(INADDR_ANY));
	in_addr parsed{};
	// The group is an RtpAddress's text, a dotted quad already checked, so a failed parse is a caller's bug.
	if (::inet_pton(AF_INET, std::string(group).c_str(), &parsed) != 1)
		throw std::logic_error("a UDP probe given a group that is not a dotted quad: " + std::string(group));
	return bindOnce(SOCK_DGRAM, port, true, parsed.s_addr);
}

} // namespace ear::runtime
