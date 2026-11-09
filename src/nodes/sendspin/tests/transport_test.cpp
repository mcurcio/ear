// The transport (cmake/ixwebsocket-limits.patch; docs/node-types.md#library-facts-and-carried-patches): the sockets an
// address, or the room, holds are those their peers have not closed, whether or not their own threads have seen the
// close yet; and an open connection whose socket is closed under it ends.
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

#include <ixwebsocket/IXSocketFactory.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXWebSocketTransport.h>

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

/// Holds `count` upgraded sockets from 127.0.0.1 on a server of `maxConnections`, closes them all with a reset while
/// their threads cannot see it, then upgrades one more: whether it was answered.
bool answeredAfterClosing(std::size_t maxConnections, std::size_t count) {
	const std::uint16_t port = freePort();
	ix::WebSocketServer server(port, "127.0.0.1", 64, maxConnections);

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
	for (std::size_t i = 0; i < count; ++i) {
		const int fd = upgrade(port);
		REQUIRE(fd >= 0);
		INFO("socket " << i);
		CHECK(answered(fd));
		CHECK(sendFrame(fd));
		held.push_back(fd);
	}
	{
		std::unique_lock lock(mutex);
		const bool allIn = changed.wait_for(lock, 10s, [&] { return opened == count; });
		if (!allIn) {
			lock.unlock();
			release();
			server.stop();
			FAIL("only " << opened << " of " << count << " messages arrived");
		}
	}
	for (const int fd : held) reset(fd);

	// All upgraded, so none can give way, and all still listed: only their being closed lets one more in.
	const int next = upgrade(port);
	const bool nextAnswered = next >= 0 && answered(next);
	if (next >= 0) reset(next);
	release();
	server.stop();
	return nextAnswered;
}

} // namespace

TEST_CASE("An address's sockets its peer has closed leave its share at once, though their threads have not seen it",
          "[sendspin][transport]") {
	// Before the patch counted only open sockets in an address's share, the ninth was refused at once.
	constexpr std::size_t share = 8; // ixwebsocket-limits.patch, kSocketsPerAddress
	CHECK(answeredAfterClosing(32, share));
}

TEST_CASE("Sockets their peers have closed leave the room at once, though their threads have not seen it",
          "[sendspin][transport]") {
	// A room of 4, under an address's share of 8: before the room too counted only open sockets, the fifth was refused
	// at once, as the server after a burst of connects closed at once was.
	CHECK(answeredAfterClosing(4, 4));
}

TEST_CASE("An open connection whose socket is closed under it ends, as one a stop raced to its 101 did not",
          "[sendspin][transport]") {
	// A server's stop() closes every listed connection, and one is listed before its handshake ends. A close that lands
	// after the 101 went out and before the state became OPEN finds it CLOSED and leaves only a close request in the
	// pipe; the first poll in OPEN closed the socket on that request and changed nothing else, so every later poll failed
	// at once, without a syscall, and the thread spun and stop() never joined it. That state is made here directly: an
	// open connection whose socket is then closed.
	const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(listener >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	REQUIRE(::listen(listener, 1) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	const int peer = upgrade(ntohs(address.sin_port)); // the upgrade waits in the socket for the handshake to read
	REQUIRE(peer >= 0);
	const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
	REQUIRE(accepted >= 0);

	std::string error;
	auto socket = ix::createSocket(false, accepted, error, ix::SocketTLSOptions());
	INFO(error);
	REQUIRE(socket);
	ix::WebSocketTransport transport;
	REQUIRE(transport.connectToSocket(std::move(socket), 5, false).success);
	CHECK(answered(peer));
	REQUIRE(transport.getReadyState() == ix::WebSocketTransport::ReadyState::OPEN);

	transport.closeSocket();
	// Each round returns at once either way: the socket is gone, so nothing is waited for.
	for (int round = 0; round < 4 && transport.getReadyState() != ix::WebSocketTransport::ReadyState::CLOSED; ++round)
		transport.dispatch(transport.poll(),
		                   [](const std::string&, std::size_t, bool, ix::WebSocketTransport::MessageKind) {});
	CHECK(transport.getReadyState() == ix::WebSocketTransport::ReadyState::CLOSED);
	::close(peer);
	::close(listener);
}
