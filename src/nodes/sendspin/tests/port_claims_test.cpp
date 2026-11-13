// Who holds a listening port (docs/node-types.md#failures), and the probe against a real socket.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "port_claims.hpp"

using ear::sendspin::PortClaims;
using ear::sendspin::probeListenPort;

namespace {

/// The first port of the kernel's range for sockets that pick their own (a connect, a bind to port 0).
std::uint16_t firstOutgoingPort() {
	std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
	int first = 0;
	REQUIRE((range >> first));
	return static_cast<std::uint16_t>(first);
}

/// A port the test holds for its whole life, so no other socket on the host can take it between the test's own sockets
/// closing and the probe: a socket bound to it on every address that never listens. The kernel never gives a bound port
/// to a socket that picks its own, and the port is taken from below the kernel's outgoing range, which a host's churn of
/// connects can exhaust. It is bound without SO_REUSEADDR, so the bind proves nothing else holds it on any address, not
/// even a neighbour's hold. Only then does the hold set SO_REUSEADDR: a socket with it (a listener, as the
/// library's is, a peer, the probe) may bind beside a holder that has it and does not listen, so only a live listener
/// refuses the probe, while a socket without it (a plain bind elsewhere) is refused.
class HeldPort {
public:
	HeldPort() : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		static std::mt19937 pick{std::random_device{}()};
		std::uniform_int_distribution<int> belowOutgoing(20000, firstOutgoingPort() - 1);
		for (int attempt = 0; attempt < 1000 && port_ == 0; ++attempt) {
			const auto port = static_cast<std::uint16_t>(belowOutgoing(pick));
			sockaddr_in address = loopback(port);
			address.sin_addr.s_addr = htonl(INADDR_ANY); // every address, as the probe binds
			if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0) port_ = port;
			else REQUIRE(errno == EADDRINUSE);
		}
		REQUIRE(port_ != 0);
		const int enable = 1;
		REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
	}
	HeldPort(const HeldPort&) = delete;
	HeldPort& operator=(const HeldPort&) = delete;
	~HeldPort() { ::close(fd_); }

	std::uint16_t port() const { return port_; }
	sockaddr_in address() const { return loopback(port_); }

private:
	static sockaddr_in loopback(std::uint16_t port) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		return address;
	}

	int fd_;
	std::uint16_t port_ = 0;
};

/// A socket bound beside the hold, with SO_REUSEADDR.
int boundBeside(const HeldPort& held) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	const int enable = 1;
	REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
	const auto address = held.address();
	REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
	return fd;
}

/// A listener on the held port, with SO_REUSEADDR as the library's has.
int listenOn(const HeldPort& held) {
	const int listener = boundBeside(held);
	REQUIRE(::listen(listener, 1) == 0);
	return listener;
}

/// Whether the kernel holds the connection 127.0.0.1:<local> to 127.0.0.1:<remote> in TIME_WAIT, by its own word: one
/// sock_diag lookup of that exact connection (a hash lookup, where reading /proc/net/tcp walks every socket on the host).
bool timeWaitOn(std::uint16_t local, std::uint16_t remote) {
	const int diag = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
	REQUIRE(diag >= 0);
	struct {
		nlmsghdr header;
		inet_diag_req_v2 request;
	} ask{};
	ask.header.nlmsg_len = sizeof ask;
	ask.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
	ask.header.nlmsg_flags = NLM_F_REQUEST;
	ask.request.sdiag_family = AF_INET;
	ask.request.sdiag_protocol = IPPROTO_TCP;
	ask.request.idiag_states = ~0U;
	ask.request.id.idiag_sport = htons(local);
	ask.request.id.idiag_dport = htons(remote);
	ask.request.id.idiag_src[0] = htonl(INADDR_LOOPBACK);
	ask.request.id.idiag_dst[0] = htonl(INADDR_LOOPBACK);
	ask.request.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
	ask.request.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
	sockaddr_nl kernel{};
	kernel.nl_family = AF_NETLINK;
	REQUIRE(::sendto(diag, &ask, sizeof ask, 0, reinterpret_cast<const sockaddr*>(&kernel), sizeof kernel) ==
	        static_cast<ssize_t>(sizeof ask));
	alignas(nlmsghdr) char answer[1024];
	const ssize_t length = ::recv(diag, answer, sizeof answer, 0);
	::close(diag);
	REQUIRE(length >= static_cast<ssize_t>(sizeof(nlmsghdr)));
	const auto* header = reinterpret_cast<const nlmsghdr*>(answer);
	if (header->nlmsg_type == NLMSG_ERROR) {
		const auto* error = static_cast<const nlmsgerr*>(NLMSG_DATA(header));
		INFO("sock_diag: " << std::system_category().message(-error->error));
		REQUIRE(error->error == -ENOENT); // no such connection: the kernel kept none
		return false;
	}
	REQUIRE(header->nlmsg_type == SOCK_DIAG_BY_FAMILY);
	return static_cast<const inet_diag_msg*>(NLMSG_DATA(header))->idiag_state == TCP_TIME_WAIT;
}

/// Closes a connection on the held port, the server side first as a restarted node's last run leaves it, and says
/// whether the kernel's own table shows the server side's TIME_WAIT (the hold keeps any plain bind out, so a refused
/// bind would prove nothing). A socket that has sent its close and then hears the peer's is in TIME_WAIT from that
/// moment, so the server's read ending is the event, on any host; the kernel keeps none while its TIME_WAIT table is
/// full (tcp_max_tw_buckets). The peer's end is a held port too, as a connect's own pick fails once a host's churn has
/// exhausted the outgoing range.
bool closeIntoTimeWait(const HeldPort& held, const HeldPort& peerEnd) {
	const int listener = listenOn(held);
	const int peer = boundBeside(peerEnd);
	const auto address = held.address();
	REQUIRE(::connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
	const int accepted = ::accept(listener, nullptr, nullptr);
	REQUIRE(accepted >= 0);
	::close(listener);
	REQUIRE(::shutdown(accepted, SHUT_WR) == 0);
	char nothing = 0;
	REQUIRE(::read(peer, &nothing, 1) == 0); // the peer has seen the server's close
	::close(peer);
	REQUIRE(::read(accepted, &nothing, 1) == 0); // the server side has seen the peer's
	::close(accepted);
	return timeWaitOn(held.port(), peerEnd.port());
}

/// The kernel's count of TIME_WAITs it did not keep for a full table (TCPTimeWaitOverflow in /proc/net/netstat).
long long timeWaitsDropped() {
	std::ifstream table("/proc/net/netstat");
	REQUIRE(table.is_open());
	std::string names;
	std::string values;
	while (std::getline(table, names) && std::getline(table, values)) {
		std::istringstream name(names);
		std::istringstream value(values);
		std::string field;
		std::string number;
		while (name >> field && value >> number)
			if (field == "TCPTimeWaitOverflow") return std::stoll(number);
	}
	FAIL("/proc/net/netstat has no TCPTimeWaitOverflow");
	return 0;
}

} // namespace

TEST_CASE("A port is claimed by one node until it is released", "[sendspin][port_claims]") {
	PortClaims claims;
	CHECK(claims.claim(9000, "a").has_value());
	CHECK(claims.claim(9001, "b").has_value());
	const auto refused = claims.claim(9000, "b");
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error() == "held by node a");
	claims.release(9000);
	CHECK(claims.claim(9000, "b").has_value());
}

TEST_CASE("The probe refuses a live listener with the kernel's reason, and passes once it is gone",
          "[sendspin][port_claims]") {
	const HeldPort held;
	const int listener = listenOn(held);

	const auto refused = probeListenPort(held.port());
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error() == "Address already in use");

	::close(listener);
	const auto probed = probeListenPort(held.port());
	INFO((probed ? std::string("passed") : probed.error()));
	CHECK(probed.has_value());
}

// What the probe's SO_REUSEADDR is for: the port of a node that just restarted is in TIME_WAIT, and the library's own
// listener could bind it. A host whose TIME_WAIT table is full (other programs' churn) keeps none for a round, so the
// test makes rounds until one shows the TIME_WAIT it judges; the deadline only fails, naming the kernel's count of
// TIME_WAITs it dropped meanwhile.
TEST_CASE("The probe passes on a port in TIME_WAIT, the server side having closed first", "[sendspin][port_claims]") {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	const auto droppedBefore = timeWaitsDropped();
	const HeldPort held;
	const HeldPort peerEnd;
	for (int round = 1;; ++round) {
		if (closeIntoTimeWait(held, peerEnd)) {
			const auto probed = probeListenPort(held.port());
			INFO("round " << round << ": " << (probed ? std::string("passed") : probed.error()));
			CHECK(probed.has_value());
			return;
		}
		if (std::chrono::steady_clock::now() > deadline)
			FAIL("the kernel kept no TIME_WAIT in " << round << " rounds, and dropped "
			                                        << timeWaitsDropped() - droppedBefore
			                                        << " for a full table meanwhile (TCPTimeWaitOverflow)");
	}
}
