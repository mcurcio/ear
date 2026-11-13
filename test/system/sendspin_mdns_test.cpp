// The sendspin node found over mDNS: its announcement, its answers to queries and the rate it answers at.
// (Shared helpers: sendspin_system.hpp.)

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// The mDNS group's port, for an mDNS query as anyone could send it.
constexpr std::uint16_t mdnsPort = 5353;
constexpr std::string_view mdnsGroup = "224.0.0.251";
constexpr std::size_t mdnsHeaderBytes = 12;

/// A query with `questions` questions about `_sendspin._tcp.local`: the first in full, the rest pointing at it, so one datagram
/// of at most 2048 bytes holds over 300 of them. `flags` is the header's: 0x8000 says it is a response.
std::string mdnsQuery(std::size_t questions, std::uint16_t flags = 0, std::uint16_t id = 0) {
	std::string packet(mdnsHeaderBytes, '\0');
	packet[0] = static_cast<char>(id >> 8U);
	packet[1] = static_cast<char>(id & 0xFFU);
	packet[2] = static_cast<char>(flags >> 8U);
	packet[3] = static_cast<char>(flags & 0xFFU);
	packet[4] = static_cast<char>(questions >> 8U);
	packet[5] = static_cast<char>(questions & 0xFFU);
	const std::string typeAndClass("\0\x0c\0\x01", 4); // PTR, IN
	packet += std::string("\x09_sendspin\x04_tcp\x05local\0", 22) + typeAndClass;
	for (std::size_t i = 1; i < questions; ++i) packet += std::string("\xc0\x0c", 2) + typeAndClass;
	return packet;
}

/// A UDP socket for talking to an advertiser. With `joined` it is on the mDNS port and in the group, as a browser is; otherwise
/// on a free port, as a one-shot querier is.
class MdnsSocket {
public:
	explicit MdnsSocket(bool joined) : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (joined) {
			const int enable = 1;
			REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) == 0);
			REQUIRE(::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof enable) == 0);
			address.sin_port = htons(mdnsPort);
		}
		REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
		if (joined) {
			ip_mreq group{};
			REQUIRE(::inet_pton(AF_INET, std::string(mdnsGroup).c_str(), &group.imr_multiaddr) == 1);
			group.imr_interface.s_addr = htonl(INADDR_ANY);
			REQUIRE(::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) == 0);
		}
	}
	~MdnsSocket() { ::close(fd_); }
	MdnsSocket(const MdnsSocket&) = delete;
	MdnsSocket& operator=(const MdnsSocket&) = delete;

	void sendTo(std::string_view address, const std::string& packet) {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(mdnsPort);
		REQUIRE(::inet_pton(AF_INET, std::string(address).c_str(), &to.sin_addr) == 1);
		REQUIRE(::sendto(fd_, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) ==
		        static_cast<ssize_t>(packet.size()));
	}

	/// The next response (a datagram marked QR=1) that holds `text`, within `patience`; nothing otherwise. Queries, and other
	/// nodes' answers on a shared network, pass by.
	std::optional<std::string> nextResponse(std::string_view text, std::chrono::milliseconds patience) {
		const auto until = std::chrono::steady_clock::now() + patience;
		std::array<char, 9000> buffer{};
		for (;;) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
			if (left <= 0) return std::nullopt;
			pollfd ready{fd_, POLLIN, 0};
			if (::poll(&ready, 1, static_cast<int>(left)) <= 0) continue;
			const auto got = ::recv(fd_, buffer.data(), buffer.size(), 0);
			if (got < static_cast<ssize_t>(mdnsHeaderBytes)) continue;
			const std::string packet(buffer.data(), static_cast<std::size_t>(got));
			if ((static_cast<unsigned char>(packet[2]) & 0x80U) != 0 && packet.find(text) != std::string::npos)
				return packet;
		}
	}

	/// How many answers `packet` drew from a socket that answers in turn: a query marked by its id goes after it, and answers
	/// are counted until the marked one comes back, however long that takes.
	std::optional<std::size_t> answersTo(std::string_view address, const std::string& packet, std::string_view text,
	                                     std::chrono::milliseconds patience) {
		constexpr std::uint16_t marker = 0x7E57;
		sendTo(address, packet);
		sendTo(address, mdnsQuery(1, 0, marker));
		const auto until = std::chrono::steady_clock::now() + patience;
		std::size_t found = 0;
		for (;;) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
			const auto answer = nextResponse(text, left);
			if (!answer) return std::nullopt;
			const auto id = static_cast<std::uint16_t>((static_cast<unsigned char>((*answer)[0]) << 8U) |
			                                           static_cast<unsigned char>((*answer)[1]));
			if (id == marker) return found;
			++found;
		}
	}

	/// How many such responses arrive within `patience`.
	std::size_t countResponses(std::string_view text, std::chrono::milliseconds patience) {
		const auto until = std::chrono::steady_clock::now() + patience;
		std::size_t found = 0;
		for (;;) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
			if (left.count() <= 0 || !nextResponse(text, left)) return found;
			++found;
		}
	}

private:
	int fd_;
};

/// A server browsing mDNS finds `node` with this TXT `name`, dials it and pairs; SIGTERM removes the service.
void checkFoundOverMdns(const std::string& node, const std::string& settings, const std::string& name) {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory,
	               graphFileText("node sendspin " + node + " { port " + std::to_string(port) + "; " + settings + " }\n",
	                             graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, node, port);
	CHECK(instance.size() <= 63);
	CHECK_FALSE(ear.lineStartingWith("node " + node + " says advertise-failed"));

	// Tests side by side advertise on one network, so this server looks only for its own.
	SendspinTestServer server({"--discover", instance});
	const auto found = server.waitForEvent("discovered ", playDeadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE_SEEN(ear, found);
	CHECK(*found == "discovered " + instance + " port=" + std::to_string(port) + " path=/sendspin name=" + name);

	const std::string prefix = "node " + node + " says ";
	REQUIRE_SEEN(ear, ear.waitForLine(prefix + "connected \"ear-test-server\" unpaired", playDeadline));
	server.send("pair");
	const auto code = ear.waitForLineStartingWith(prefix + "pairing-code ", deadline);
	REQUIRE(code);
	std::string digits = code->substr((prefix + "pairing-code ").size());
	std::erase(digits, '-');
	server.send("code " + digits);
	REQUIRE_SEEN(ear, ear.waitForLine(prefix + "connected \"ear-test-server\" paired", deadline));

	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
	const auto removed = server.waitForEvent("service-removed ", playDeadline);
	INFO("the server then printed:\n" << server.events());
	REQUIRE_SEEN(ear, removed);
	CHECK(*removed == "service-removed " + instance);
}

} // namespace

TEST_CASE("A server browsing mDNS finds a listening node, its TXT says its path and name, and it "
          "is gone after SIGTERM",
          "[system][sendspin]") {
	checkFoundOverMdns("found", "name \"Front Room\";", "Front Room");
}

TEST_CASE("A 64-character node name is found over mDNS too", "[system][sendspin]") {
	const std::string node(64, 'n');
	checkFoundOverMdns(node, "", node);
}

TEST_CASE("A server browsing mDNS comes back by itself after a new name restarts the node", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin back { port " + std::to_string(port) + "; }\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "back", port);
	SendspinTestServer server({"--discover", instance});
	const std::string connected = "node back says connected ";
	REQUIRE_SEEN(ear, ear.waitForLineStartingWith(connected, playDeadline));

	Rpc rpc(controlPort(ear));
	const json update = rpc.call("node.update", {{"name", "back"}, {"settings", {{"port", port}, {"name", "Other"}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	const auto again = waitForNthLine(ear, connected, 1, playDeadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE_SEEN(ear, again);
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A listening node announces itself unasked, with its PTR, SRV, TXT and A", "[system][sendspin]") {
	MdnsSocket browser(true); // before ear: only an announcement can reach it
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "ma", port);
	CHECK_FALSE(ear.lineStartingWith("node ma says advertise-failed"));
	const auto announcement = browser.nextResponse(instance, deadline);
	REQUIRE_SEEN(ear, announcement);
	CHECK(announcement->find("path=/sendspin") != std::string::npos);
	CHECK(announcement->find("name=ma") != std::string::npos);
	const auto count = [&](std::size_t at) {
		return (static_cast<unsigned char>((*announcement)[at]) << 8U) |
		       static_cast<unsigned char>((*announcement)[at + 1]);
	};
	CHECK(count(4) == 0);  // no questions: nobody asked
	CHECK(count(6) == 1);  // the PTR
	CHECK(count(10) >= 3); // the SRV, the A and the TXT
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("One datagram of many questions gets one answer, and a response or a runt gets none", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	std::optional<MdnsSocket> browser(std::in_place, true);
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "ma", port);
	// Its socket is open once it has announced. Then this one goes: a unicast query must reach the node's, the only one left.
	REQUIRE_SEEN(ear, browser->nextResponse(instance, deadline));
	browser.reset();

	// A one-shot querier: from any port but 5353 the answer is unicast, so only what this socket asked for reaches it. Tests side
	// by side share port 5353, and the kernel hands a unicast datagram to one socket of it, chosen by the source's port, which
	// may be a browser's that answers nothing: so a marker never answered means another source port, not a wrong answer.
	const auto answers = [&](const std::string& packet) -> std::optional<std::size_t> {
		const auto until = std::chrono::steady_clock::now() + deadline;
		while (std::chrono::steady_clock::now() < until) {
			MdnsSocket querier(false);
			if (const auto found = querier.answersTo("127.0.0.1", packet, "_sendspin", 500ms)) return found;
		}
		return std::nullopt;
	};
	const std::string many = mdnsQuery(330);
	REQUIRE(many.size() <= 2040);
	CHECK(answers(mdnsQuery(1)) == std::optional<std::size_t>(1));
	CHECK(answers(many) == std::optional<std::size_t>(1));
	CHECK(answers(mdnsQuery(330, 0x8000)) == std::optional<std::size_t>(0)); // QR=1
	CHECK(answers(many.substr(0, 6)) == std::optional<std::size_t>(0));      // shorter than a header
	CHECK(answers(mdnsQuery(1)) == std::optional<std::size_t>(1));           // and it still answers
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A burst of multicast queries gets at most an answer a second", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "ma", port);

	MdnsSocket browser(true);
	const auto start = std::chrono::steady_clock::now();
	std::size_t answers = 0;
	for (int i = 0; i < 20; ++i) {
		browser.sendTo(mdnsGroup, mdnsQuery(330));
		answers += browser.countResponses(instance, 75ms);
	}
	answers += browser.countResponses(instance, 100ms);
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	INFO(answers << " answers in " << seconds << " s");
	CHECK(answers >= 1);
	CHECK(static_cast<double>(answers) <= seconds + 2.0); // one at each whole second, the announcement among them
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}
