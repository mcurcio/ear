// The transport (cmake/ixwebsocket-limits.patch; docs/node-types.md#library-facts-and-carried-patches): the sockets an
// address, or the room, holds are those their peers have not closed, whether or not their own threads have seen the
// close yet; every listed socket, closed or not, counts against twice the room, so a flood starts no thread past it;
// and an open connection whose socket is closed under it ends.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <ixwebsocket/IXSelectInterrupt.h>
#include <ixwebsocket/IXSocket.h>
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

/// This process's threads.
std::size_t threadsNow() {
	const std::filesystem::directory_iterator tasks("/proc/self/task");
	return static_cast<std::size_t>(std::distance(begin(tasks), end(tasks)));
}

/// What answeredAfterClosing saw.
struct AfterClosing {
	std::size_t answered = 0;     // of the upgrades tried after the rounds
	std::size_t threadsAdded = 0; // threads this process gained from the server's start to the last of those
};

/// On a server of `maxConnections`, `rounds` times: holds `count` upgraded sockets from 127.0.0.1 and closes them all with
/// a reset while their threads cannot see it. Then tries `more` upgrades, each closed before the next: how many were
/// answered, and how many threads the server holds then, every socket's still blocked where the test holds it, so none
/// can end and the count is exact.
AfterClosing tryAfterClosing(std::size_t maxConnections, std::size_t count, std::size_t rounds, std::size_t more) {
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
	const std::size_t threadsBefore = threadsNow();
	const auto release = [&] {
		{
			const std::scoped_lock lock(mutex);
			released = true;
		}
		changed.notify_all();
	};

	for (std::size_t round = 0; round < rounds; ++round) {
		std::vector<int> held;
		for (std::size_t i = 0; i < count; ++i) {
			const int fd = upgrade(port);
			REQUIRE(fd >= 0);
			INFO("round " << round << ", socket " << i);
			CHECK(answered(fd));
			CHECK(sendFrame(fd));
			held.push_back(fd);
		}
		{
			const std::size_t expected = (round + 1) * count;
			std::unique_lock lock(mutex);
			const bool allIn = changed.wait_for(lock, 10s, [&] { return opened == expected; });
			if (!allIn) {
				lock.unlock();
				release();
				server.stop();
				FAIL("only " << opened << " of " << expected << " messages arrived");
			}
		}
		for (const int fd : held) reset(fd);
	}

	// All upgraded, so none can give way, and all still listed: only their being closed lets one more in.
	AfterClosing seen;
	for (std::size_t i = 0; i < more; ++i) {
		const int next = upgrade(port);
		if (next >= 0 && answered(next)) ++seen.answered;
		if (next >= 0) reset(next);
	}
	seen.threadsAdded = threadsNow() - threadsBefore;
	release();
	server.stop();
	return seen;
}

/// One upgrade after the rounds: whether it was answered.
bool answeredAfterClosing(std::size_t maxConnections, std::size_t count, std::size_t rounds = 1) {
	return tryAfterClosing(maxConnections, count, rounds, 1).answered == 1;
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

TEST_CASE("Sockets their peers have closed still count against twice the room in threads", "[sendspin][transport]") {
	// A room of 2: two rounds of two, each closed before the next, leave four listed and none open, so the room has space
	// but twice the room is reached, and the fifth is refused. The bound is checked before the room's open count.
	CHECK_FALSE(answeredAfterClosing(2, 2, 2));
}

TEST_CASE("A flood past twice the room starts no thread beyond it", "[sendspin][transport]") {
	// The bound the system test's flood samples from outside (sendspin_floods_test.cpp), proven where it is enforced: two
	// rounds of two fill twice a room of 2 with listed sockets, each thread held where it cannot end, and a flood of
	// upgrades after them is refused before any thread of its own starts, so the server holds four threads, exactly.
	constexpr std::size_t room = 2;
	const AfterClosing seen = tryAfterClosing(room, room, 2, 50);
	CHECK(seen.answered == 0);
	CHECK(seen.threadsAdded <= 2 * room);
}

namespace {

/// A server-side connection, upgraded and open on its own transport, with nothing running it: the test polls it.
struct OpenConnection {
	ix::WebSocketTransport transport;
	ix::Socket* socket = nullptr; // the transport's own, kept before it moved in
	int peer = -1;
	int listener = -1;

	OpenConnection() {
		listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		REQUIRE(listener >= 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
		REQUIRE(::listen(listener, 1) == 0);
		socklen_t length = sizeof address;
		REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
		peer = upgrade(ntohs(address.sin_port)); // the upgrade waits in the socket for the handshake to read
		REQUIRE(peer >= 0);
		const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
		REQUIRE(accepted >= 0);

		std::string error;
		auto created = ix::createSocket(false, accepted, error, ix::SocketTLSOptions());
		INFO(error);
		REQUIRE(created);
		socket = created.get();
		REQUIRE(transport.connectToSocket(std::move(created), 5, false).success);
		CHECK(answered(peer));
		REQUIRE(transport.getReadyState() == ix::WebSocketTransport::ReadyState::OPEN);
	}
	~OpenConnection() {
		if (peer >= 0) ::close(peer);
		if (listener >= 0) ::close(listener);
	}
	OpenConnection(const OpenConnection&) = delete;
	OpenConnection& operator=(const OpenConnection&) = delete;

	void pollOnce() {
		transport.dispatch(transport.poll(),
		                   [](const std::string&, std::size_t, bool, ix::WebSocketTransport::MessageKind) {});
	}
};

} // namespace

// A server's stop() closes every listed connection, and one is listed before its handshake ends. A close that lands
// after the 101 went out and before the state became OPEN finds it CLOSED and leaves only a close request in the pipe;
// the first poll in OPEN closed the socket on that request and changed nothing else, so every later poll failed at once,
// without a syscall, and the thread spun and stop() never joined it.

TEST_CASE("A close request left for an open connection ends it on the poll that reads it", "[sendspin][transport]") {
	// The race's own path: the request in the pipe, read in OPEN. One poll, so a request handled apart from a failed
	// poll, closing the socket and nothing else, fails here.
	OpenConnection connection;
	int closes = 0;
	connection.transport.setOnCloseCallback([&](std::uint16_t, const std::string&, std::size_t, bool) { ++closes; });
	REQUIRE(connection.socket->wakeUpFromPoll(ix::SelectInterrupt::kCloseRequest));
	connection.pollOnce();
	CHECK(connection.transport.getReadyState() == ix::WebSocketTransport::ReadyState::CLOSED);
	connection.pollOnce(); // nothing left to say
	CHECK(closes == 1);
}

TEST_CASE("An open connection whose socket is closed under it ends, as one a stop raced to its 101 did not",
          "[sendspin][transport]") {
	// The state the race left, made directly: an open connection whose socket is then closed.
	OpenConnection connection;
	connection.transport.closeSocket();
	// Each round returns at once either way: the socket is gone, so nothing is waited for.
	for (int round = 0; round < 4 && connection.transport.getReadyState() != ix::WebSocketTransport::ReadyState::CLOSED;
	     ++round)
		connection.pollOnce();
	CHECK(connection.transport.getReadyState() == ix::WebSocketTransport::ReadyState::CLOSED);
}
