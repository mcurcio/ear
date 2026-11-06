#include "port_claims.hpp"

#include <cerrno>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::sendspin {

std::expected<void, std::string> PortClaims::claim(std::uint16_t port, const NodeName& node) {
	const auto [holder, claimed] = held_.try_emplace(port, node);
	if (!claimed) return std::unexpected("held by node " + holder->second);
	return {};
}

void PortClaims::release(std::uint16_t port) { held_.erase(port); }

std::expected<void, std::string> probeListenPort(std::uint16_t port) {
	const int probe = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (probe < 0) return std::unexpected(std::system_category().message(errno));
	const int enable = 1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	const bool bound = ::setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0 &&
	                   ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
	const int why = errno;
	::close(probe);
	if (!bound) return std::unexpected(std::system_category().message(why));
	return {};
}

} // namespace ear::sendspin
