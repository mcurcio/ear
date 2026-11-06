// Who holds a listening port (docs/design/sendspin.md section 10), and the probe against a real socket.
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

TEST_CASE("The probe refuses a live listener with the kernel's reason, and passes once it is gone", "[sendspin][port_claims]") {
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
