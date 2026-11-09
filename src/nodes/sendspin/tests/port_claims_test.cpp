// Who holds a listening port (docs/node-types.md#failures), and the probe against a real socket.
#include <cerrno>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "port_claims.hpp"

using ear::sendspin::PortClaims;
using ear::sendspin::probeListenPort;

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
	const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(listener >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	REQUIRE(::listen(listener, 1) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	const auto port = ntohs(address.sin_port);

	const auto refused = probeListenPort(port);
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error() == "Address already in use");

	::close(listener);
	CHECK(probeListenPort(port).has_value());
}

// What the probe's SO_REUSEADDR is for: the port of a node that just restarted is in TIME_WAIT, and the library's own
// listener could bind it.
TEST_CASE("The probe passes on a port in TIME_WAIT, the server side having closed first", "[sendspin][port_claims]") {
	const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(listener >= 0);
	const int enable = 1;
	REQUIRE(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	REQUIRE(::listen(listener, 1) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	const auto port = ntohs(address.sin_port);

	const int peer = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(peer >= 0);
	REQUIRE(::connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
	const int accepted = ::accept(listener, nullptr, nullptr);
	REQUIRE(accepted >= 0);
	::close(listener);
	::close(accepted); // the server side first: it is this side that waits
	char nothing = 0;
	REQUIRE(::read(peer, &nothing, 1) == 0); // the peer has seen the close
	::close(peer);

	// A plain bind, as a program without the option would, finds the port still taken: that is what proves the TIME_WAIT.
	const int plain = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(plain >= 0);
	sockaddr_in any{};
	any.sin_family = AF_INET;
	any.sin_addr.s_addr = htonl(INADDR_ANY);
	any.sin_port = htons(port);
	const int bound = ::bind(plain, reinterpret_cast<const sockaddr*>(&any), sizeof any);
	const int why = errno;
	::close(plain);
	REQUIRE(bound != 0);
	REQUIRE(why == EADDRINUSE);

	const auto probed = probeListenPort(port);
	INFO((probed ? std::string("passed") : probed.error()));
	CHECK(probed.has_value());
}
