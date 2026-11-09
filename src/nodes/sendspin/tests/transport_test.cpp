// The transport's share per address (cmake/ixwebsocket-limits.patch; docs/design/sendspin.md erratum 43): the sockets an
// address holds are those its peer has not closed, whether or not their own threads have seen the close yet.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <ixwebsocket/IXWebSocketServer.h>

#include "catch2/catch.hpp"

namespace {

using namespace std::chrono_literals;

/// A port nobody listens on now, for a server that cannot be asked which one it took.
std::uint16_t freePort() {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	::close(fd);
	return ntohs(address.sin_port);
}

/// A socket from 127.0.0.1 that has sent the upgrade, or -1. Its reads give up after `patience`, which only fails a test.
int upgrade(std::uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	const timeval patience{.tv_sec = 10, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons(port);
	const std::string request = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
	                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
	                            "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
	if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0 ||
	    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
		::close(fd);
		return -1;
	}
	return fd;
}

/// Reads to the end of the upgrade's answer: whether it was a 101.
bool answered(int fd) {
	std::string answer;
	char byte = 0;
	while (!answer.ends_with("\r\n\r\n") && ::recv(fd, &byte, 1, 0) == 1) answer += byte;
	return answer.starts_with("HTTP/1.1 101");
}

/// Sends one masked text frame, "x", as a client must.
bool sendFrame(int fd) {
	const unsigned char frame[] = {0x81, 0x81, 0x01, 0x02, 0x03, 0x04, 'x' ^ 0x01};
	return ::send(fd, frame, sizeof frame, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof frame);
}

/// Closes with a reset, as a peer that is gone does.
void reset(int fd) {
	const linger now{1, 0};
	::setsockopt(fd, SOL_SOCKET, SO_LINGER, &now, sizeof now);
	::close(fd);
}

} // namespace

TEST_CASE("An address's sockets its peer has closed leave its share at once, though their threads have not seen it",
          "[sendspin][transport]") {
	constexpr std::size_t share = 8; // ixwebsocket-limits.patch, kSocketsPerAddress
	const std::uint16_t port = freePort();
	ix::WebSocketServer server(port, "127.0.0.1", 64, 32);

	// Each socket's thread, once upgraded and given a message, waits in the message's callback until the test lets it go:
	// none of them sees its peer's close. Not at Open: IXWebSocket calls that before the transport counts the socket as
	// upgraded, and one still upgrading gives way to a newcomer anyway.
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t opened = 0;
	bool released = false;
	server.setOnClientMessageCallback(
		[&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr& message) {
			if (message->type != ix::WebSocketMessageType::Message) return;
			std::unique_lock lock(mutex);
			++opened;
			changed.notify_all();
			changed.wait(lock, [&] { return released; });
		});
	REQUIRE(server.listen().first);
	server.start();
	const auto release = [&] {
		{
			const std::scoped_lock lock(mutex);
			released = true;
		}
		changed.notify_all();
	};

	std::vector<int> held;
	for (std::size_t i = 0; i < share; ++i) {
		const int fd = upgrade(port);
		REQUIRE(fd >= 0);
		INFO("socket " << i);
		CHECK(answered(fd));
		CHECK(sendFrame(fd));
		held.push_back(fd);
	}
	{
		std::unique_lock lock(mutex);
		const bool allIn = changed.wait_for(lock, 10s, [&] { return opened == share; });
		if (!allIn) {
			lock.unlock();
			release();
			server.stop();
			FAIL("only " << opened << " of " << share << " messages arrived");
		}
	}
	for (const int fd : held) reset(fd);

	// The address's eight are all upgraded, so none can give way, and all still listed: only their being closed lets a
	// ninth in. Before the patch counted only open sockets, it was refused at once.
	const int ninth = upgrade(port);
	const bool ninthAnswered = ninth >= 0 && answered(ninth);
	if (ninth >= 0) reset(ninth);
	release();
	server.stop();
	CHECK(ninthAnswered);
}
