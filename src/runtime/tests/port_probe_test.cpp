// The kernel's port probe on its own (docs/writing-a-node-type.md#claims): a free port passes, a port another socket
// holds refuses with the kernel's reason, the UDP probe for every address refuses a holder that set SO_REUSEADDR (as
// PipeWire's RTP receiver does) while the probe for a multicast group shares the port with one, as that receiver
// binds it, and the TCP probe refuses a live listener. Real sockets on
// 127.0.0.1's stack; each port is one the kernel picked free a moment before.
#include <cstdint>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "catch2/catch.hpp"
#include "ear/runtime/port_probe.hpp"

using ear::runtime::KernelPortProbe;

namespace {

/// A socket of `type` bound to every IPv4 address at a port the kernel picks (SO_REUSEADDR set when `reuse`), listening
/// when TCP; closed when it goes.
class Held {
public:
	Held(int type, bool reuse) : fd_(::socket(AF_INET, type | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		const int enable = 1;
		if (reuse) REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		REQUIRE(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
		if (type == SOCK_STREAM) REQUIRE(::listen(fd_, 1) == 0);
		socklen_t length = sizeof address;
		REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
		port_ = ntohs(address.sin_port);
	}
	~Held() {
		if (fd_ >= 0) ::close(fd_);
	}
	Held(const Held&) = delete;
	Held& operator=(const Held&) = delete;
	std::uint16_t port() const noexcept { return port_; }
	/// Closed now; its port stays the one it held.
	void close() {
		::close(fd_);
		fd_ = -1;
	}

private:
	int fd_;
	std::uint16_t port_ = 0;
};

} // namespace

TEST_CASE("The UDP probe passes a free port and refuses one held, even by a holder that set SO_REUSEADDR",
          "[runtime][claims]") {
	KernelPortProbe probe;
	SECTION("held without SO_REUSEADDR (another program)") {
		Held held(SOCK_DGRAM, false);
		const auto refused = probe.udp(held.port(), "");
		REQUIRE_FALSE(refused);
		CHECK(refused.error() == "Address already in use");
		held.close();
		CHECK(probe.udp(held.port(), ""));
	}
	SECTION("held with SO_REUSEADDR (PipeWire's RTP receiver)") {
		Held held(SOCK_DGRAM, true);
		const auto refused = probe.udp(held.port(), "");
		REQUIRE_FALSE(refused);
		CHECK(refused.error() == "Address already in use");
	}
}

TEST_CASE("The TCP probe refuses a live listener and passes once it closed", "[runtime][claims]") {
	KernelPortProbe probe;
	Held listener(SOCK_STREAM, true);
	const auto refused = probe.tcpListen(listener.port());
	REQUIRE_FALSE(refused);
	CHECK(refused.error() == "Address already in use");
	listener.close();
	CHECK(probe.tcpListen(listener.port()));
}

TEST_CASE(
	"The UDP probe for a multicast group binds it as an RTP receiver does, so another program sharing the port by "
	"SO_REUSEADDR passes, one holding it alone refuses, and the probe for every address refuses both",
	"[runtime][claims]") {
	KernelPortProbe probe;
	SECTION("shared by SO_REUSEADDR (a monitor listening to the group)") {
		Held shared(SOCK_DGRAM, true);
		CHECK(probe.udp(shared.port(), "239.255.0.1"));
		const auto strict = probe.udp(shared.port(), "");
		REQUIRE_FALSE(strict);
		CHECK(strict.error() == "Address already in use");
	}
	SECTION("held alone") {
		Held alone(SOCK_DGRAM, false);
		const auto refused = probe.udp(alone.port(), "239.255.0.1");
		REQUIRE_FALSE(refused);
		CHECK(refused.error() == "Address already in use");
	}
}
