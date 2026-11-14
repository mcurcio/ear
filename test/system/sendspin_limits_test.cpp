// What a peer may not make the sendspin node do: an upgrade with Origin, a frame too large to hold, bare connects, a name that breaks a status line.
// (Shared helpers: sendspin_system.hpp.)

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// A peer's upgrade to `port`, with `options`: what the listening node sends it first; empty if it closes the connection first.
std::string firstMessage(std::uint16_t port, const WsOptions& options) {
	WsClient peer(port, options);
	try {
		return peer.read();
	} catch (const Closed&) {
		return {};
	}
}

std::size_t residentBytes(pid_t pid) {
	std::ifstream status("/proc/" + std::to_string(pid) + "/status");
	for (std::string line; std::getline(status, line);) {
		if (line.starts_with("VmRSS:")) return std::stoull(line.substr(6)) * 1024;
	}
	FAIL("no VmRSS for " << pid);
	return 0;
}

/// A peer that has done nothing but upgrade: no Noise and no Sendspin message. Its socket is non-blocking.
class RawPeer {
public:
	explicit RawPeer(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
		const std::string upgrade = "GET /sendspin HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
		                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
		                            "dGhlIHNhbXBsZSBub25jZQ==\r\n"
		                            "Sec-WebSocket-Version: 13\r\n\r\n";
		REQUIRE(::send(fd_, upgrade.data(), upgrade.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(upgrade.size()));
		std::string answer;
		char byte = 0;
		while (!answer.ends_with("\r\n\r\n") && ::recv(fd_, &byte, 1, 0) == 1) answer += byte;
		INFO(answer);
		REQUIRE(answer.starts_with("HTTP/1.1 101"));
		REQUIRE(::fcntl(fd_, F_SETFL, O_NONBLOCK) == 0);
	}
	~RawPeer() { ::close(fd_); }
	RawPeer(const RawPeer&) = delete;
	RawPeer& operator=(const RawPeer&) = delete;

	/// Sends `next(offset)`'s bytes for as long as ear keeps the connection, up to `most`. How much went out before ear closed
	/// it, or nothing if it never did within that much or the deadline.
	template <typename Next> std::optional<std::size_t> sendUntilClosed(std::size_t most, Next next) {
		std::size_t sent = 0;
		std::string pending;
		std::array<char, 4096> inbound{};
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (std::chrono::steady_clock::now() < until) {
			pollfd ready{fd_, static_cast<short>(POLLIN | (sent < most ? POLLOUT : 0)), 0};
			if (::poll(&ready, 1, 100) <= 0) continue;
			if ((ready.revents & POLLIN) != 0) {
				const auto got = ::recv(fd_, inbound.data(), inbound.size(), 0);
				if (got == 0 || (got < 0 && errno != EAGAIN)) return sent;
			}
			if ((ready.revents & POLLOUT) != 0) {
				if (pending.empty()) pending = next();
				const auto wrote = ::send(fd_, pending.data(), pending.size(), MSG_NOSIGNAL);
				if (wrote < 0 && errno != EAGAIN) return sent;
				if (wrote > 0) {
					sent += static_cast<std::size_t>(wrote);
					pending.erase(0, static_cast<std::size_t>(wrote));
				}
			}
		}
		return std::nullopt;
	}

private:
	int fd_;
};

/// One masked WebSocket frame header (the mask is zeros, so the payload goes as it is).
std::string frameHeader(std::uint8_t first, std::uint64_t length) {
	std::string header(1, static_cast<char>(first));
	if (length < 126) {
		header += static_cast<char>(0x80U | length);
	} else {
		header += static_cast<char>(0x80U | 127U);
		for (int shift = 56; shift >= 0; shift -= 8)
			header += static_cast<char>((length >> static_cast<unsigned>(shift)) & 0xFFU);
	}
	return header + std::string(4, '\0');
}

} // namespace

TEST_CASE("An upgrade carrying Origin is closed before any client/init, and the same without it "
          "gets client/init",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);

	CHECK(firstMessage(port, WsOptions().origin("https://example.com")).empty());
	const json init = json::parse(firstMessage(port, WsOptions()));
	INFO(init.dump());
	CHECK(init.value("type", "") == "client/init");

	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A frame that claims more than a message may hold is closed before it costs any memory, "
          "and the node runs on",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);

	constexpr std::size_t flood = std::size_t{64} * 1024 * 1024;
	const std::size_t before = residentBytes(ear.pid());
	SECTION("one frame of 2^40 bytes") {
		RawPeer peer(port);
		bool first = true;
		const auto sent = peer.sendUntilClosed(flood, [&] {
			std::string next = first ? frameHeader(0x82, std::uint64_t{1} << 40U) : std::string();
			first = false;
			next.resize(next.size() + 65536);
			return next;
		});
		REQUIRE(sent.has_value());
		CHECK(*sent < flood);
	}
	SECTION("fragments that add up past it") {
		RawPeer peer(port);
		bool first = true;
		const auto sent = peer.sendUntilClosed(flood, [&] {
			std::string next = frameHeader(first ? 0x02 : 0x00, 65536); // never the last of a message
			first = false;
			next.resize(next.size() + 65536);
			return next;
		});
		REQUIRE(sent.has_value());
		CHECK(*sent < flood);
	}
	SECTION("empty fragments, which add up to nothing") {
		RawPeer peer(port);
		bool first = true;
		const auto sent = peer.sendUntilClosed(flood, [&] {
			std::string next;
			for (int i = 0; i < 1024; ++i) {
				next += frameHeader(first ? 0x02 : 0x00, 0);
				first = false;
			}
			return next;
		});
		REQUIRE(sent.has_value());
		CHECK(*sent < flood);
	}
	const std::size_t after = residentBytes(ear.pid());
	INFO("resident before " << before << ", after " << after);
	CHECK(after < before + 16 * 1024 * 1024);

	// The node still answers: an upgrade with nothing wrong with it gets client/init.
	const json init = json::parse(firstMessage(port, WsOptions()));
	CHECK(init.value("type", "") == "client/init");
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("Bare connects cannot fill the log", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);

	const auto start = std::chrono::steady_clock::now();
	constexpr int connects = 60;
	for (int i = 0; i < connects; ++i) CHECK_FALSE(connectRefused(port));
	// What the transport says, it has said by the time a real upgrade after them is answered.
	CHECK(json::parse(firstMessage(port, WsOptions())).value("type", "") == "client/init");
	ear.waitForLine("\x01", 200ms);
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::istringstream lines(ear.output());
	std::size_t transportLines = 0;
	for (std::string line; std::getline(lines, line);)
		transportLines += line.find("handleConnection") != std::string::npos ? 1U : 0U;
	INFO(connects << " connects in " << seconds << " s, " << transportLines << " lines");
	CHECK(static_cast<double>(transportLines) <= seconds / 10.0 + 2.0); // one every 10 s, whatever the connects
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A server's name with control characters and a line break stays on one status line, quoted",
          "[system][sendspin]") {
	SendspinTestServer server({"--name", "line\nnode ma says paired x\xc2\x9b\xe2\x80\xa8 \"q\""});
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear,
	             ear.waitForLine("node ma says connected \"line?node ma says paired x?? ?q?\" unpaired", deadline));
	CHECK_FALSE(ear.lineStartingWith("node ma says paired"));
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}
