// The owner's end-to-end tests of the sendspin node (docs/design/sendspin.md section 14, S1 criteria 10 to 12, S2 criteria
// 3 to 8, S3 criteria 2 to 8): the real out/ear runs a graph file with a sendspin node routed into a file node, and dials
// the real server, or listens for it and is found over mDNS. A listening node's port is refused when it is held, and an
// upgrade with Origin is closed.
// Paired by a code, the server plays a ramp in which every frame names its place, and the WAV must hold it; routed noise
// reaches the server on the routed channel alone. Unpaired, nothing moves. A round limit holds, a bad key file fails only
// the node. With no server the node runs and is silent; a peer that never answers, or a listener that drops the dial,
// holds up neither a stop nor a restart. Every wait is for an event (a line ear prints, an event the server prints, a
// message, ear's exit); a deadline only fails a test.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>

#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ear_system.hpp"
#include "sendspin_server.hpp"

/// REQUIRE on a wait for what ear prints, naming all it printed by the time the wait ended.
#define REQUIRE_SEEN(ear, wait)                     \
	do {                                            \
		const bool seen = static_cast<bool>(wait);  \
		INFO("ear printed:\n" << (ear).output());   \
		REQUIRE(seen);                              \
	} while (false)

namespace {

using namespace ear::test;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t blockSize = 256;
/// Fails a play that never finishes: longer than the shared deadline, because a stream's start takes the server's lead.
constexpr auto playDeadline = 30s;
/// S1's bound on a stop and on a restart.
constexpr auto stopBound = 5s;
/// How long a source's first chunk may take from capture to the server: a chunk, the library's buffer, the handoff's wake.
constexpr std::int64_t captureToArrivalBoundUs = 500000;

/// A sendspin node called "ma" with these settings, routed side for side into file node "rec".
std::string maToRec(const std::string& settings) {
	return graphFileText("node sendspin ma { " + settings + " }\n"
	                     "node file rec { path \"rec.wav\"; }\n"
	                     "route ma:from_server_FL rec:FL;\n"
	                     "route ma:from_server_FR rec:FR;\n",
	                     graphFrequency, blockSize);
}

std::string dialToRec(const std::string& url) { return maToRec("server \"" + url + "\";"); }

std::string listenToRec(std::uint16_t port) { return maToRec("port " + std::to_string(port) + ";"); }

ChildProcess runEar(const TempDirectory& directory) { return ChildProcess(EAR_BINARY, earArguments(directory)); }

constexpr std::string_view connectedUnpaired = "node ma says connected \"ear-test-server\" unpaired";
constexpr std::string_view connectedPaired = "node ma says connected \"ear-test-server\" paired";
const std::string pairingCodePrefix = "node ma says pairing-code ";

const std::vector<std::string> runningAndLinked{
    "node ma running",
    "node rec running",
    "route ma:from_server_FL -> rec:FL linked",
    "route ma:from_server_FR -> rec:FR linked",
};

bool waitForLines(ChildProcess& ear, const std::vector<std::string>& lines) {
	for (const auto& line : lines) {
		if (!ear.waitForLine(line, deadline)) return false;
	}
	return true;
}

/// The `index`th line (from 0) starting with `prefix`, once ear has printed it. ChildProcess gives only the first, and
/// offers no wake on "something new", so this reads what arrives in short bounded reads; the line arriving ends the wait.
std::optional<std::string> waitForNthLine(ChildProcess& ear, const std::string& prefix, std::size_t index, std::chrono::milliseconds patience) {
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		std::istringstream lines(ear.output());
		std::size_t seen = 0;
		for (std::string line; std::getline(lines, line);) {
			if (line.starts_with(prefix) && seen++ == index) return line;
		}
		if (std::chrono::steady_clock::now() >= until) return std::nullopt;
		ear.waitForLine("\x01", 20ms);
	}
}

std::size_t countLines(const ChildProcess& ear, const std::string& prefix) {
	std::istringstream lines(ear.output());
	std::size_t seen = 0;
	for (std::string line; std::getline(lines, line);) seen += line.starts_with(prefix) ? 1U : 0U;
	return seen;
}

/// The digits of a `says pairing-code 123-456` line.
std::string digitsOf(const std::string& line) {
	std::string digits = line.substr(pairingCodePrefix.size());
	std::erase(digits, '-');
	return digits;
}

/// Six digits that are not `digits`.
std::string wrongCode(std::string digits) {
	digits[0] = digits[0] == '0' ? '1' : '0';
	return digits;
}

/// Connected and unpaired: `pair`, then the code ear shows, until ear says it is paired on the new key.
void pairOnce(ChildProcess& ear, SendspinTestServer& server) {
	server.send("pair");
	const auto code = ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(code);
	server.send("code " + digitsOf(*code));
	REQUIRE(ear.waitForLine(connectedPaired, deadline));
}

std::filesystem::perms modeOf(const std::filesystem::path& path) {
	return std::filesystem::symlink_status(path).permissions() & std::filesystem::perms::mask;
}

/// The control port's number, once ear has said it.
std::uint16_t controlPort(ChildProcess& ear) {
	const auto listening = ear.waitForLineStartingWith(listeningPrefix, deadline);
	REQUIRE(listening);
	return static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));
}

/// A socket bound on 127.0.0.1 at a free port, listening.
class Listener {
public:
	explicit Listener(int backlog) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
		REQUIRE(::listen(fd_, backlog) == 0);
		socklen_t length = sizeof address;
		REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
		port_ = ntohs(address.sin_port);
	}
	~Listener() {
		::close(fd_);
		for (const int filler : fillers_) ::close(filler);
	}
	std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/sendspin"; }
	std::uint16_t port() const noexcept { return port_; }

	/// Connects until the backlog is full: the kernel then drops every new SYN, so the last connect stays in progress.
	void fillBacklog() {
		for (int tries = 0; tries < 64; ++tries) {
			const int filler = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
			REQUIRE(filler >= 0);
			fillers_.push_back(filler);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(port_);
			::connect(filler, reinterpret_cast<sockaddr*>(&address), sizeof address);
			// A connect the backlog took has finished by now on loopback; one it dropped has not.
			pollfd writable{filler, POLLOUT, 0};
			if (::poll(&writable, 1, 200) == 0) return;
		}
		FAIL("the backlog never filled");
	}

private:
	int fd_;
	std::uint16_t port_ = 0;
	std::vector<int> fillers_;
};

/// A free TCP port on 127.0.0.1 with nothing behind it.
std::uint16_t freeTcpPort() {
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

/// Whether a connect to `port` on 127.0.0.1 is refused: nothing listens there.
bool connectRefused(std::uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	const bool refused = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 && errno == ECONNREFUSED;
	::close(fd);
	return refused;
}

/// The instance a listening node says it goes by, once it has said so: `ear-<node>-<first 8 of its id>`, its id's own.
std::string waitForInstance(ChildProcess& ear, const std::string& node, std::uint16_t port) {
	const std::string saying = "node " + node + " says listening " + std::to_string(port) + " as ";
	const auto listening = ear.waitForLineStartingWith(saying, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(listening);
	const auto id = ear.lineStartingWith("node " + node + " says id ");
	REQUIRE(id);
	CHECK(ear.output().find(*id) < ear.output().find(*listening));
	const std::string instance = listening->substr(saying.size());
	CHECK(instance == "ear-" + node.substr(0, 50) + "-" + id->substr(("node " + node + " says id ").size(), 8));
	return instance;
}

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
		                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
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
	template <typename Next>
	std::optional<std::size_t> sendUntilClosed(std::size_t most, Next next) {
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
		for (int shift = 56; shift >= 0; shift -= 8) header += static_cast<char>((length >> static_cast<unsigned>(shift)) & 0xFFU);
	}
	return header + std::string(4, '\0');
}

/// The mDNS group's port, for an mDNS query as anyone could send it.
constexpr std::uint16_t mdnsPort = 5353;
constexpr std::string_view mdnsGroup = "224.0.0.251";
constexpr std::size_t mdnsHeaderBytes = 12;

/// A query with `questions` questions about `_sendspin._tcp.local`: the first in full, the rest pointing at it, so one datagram
/// of at most 2048 bytes holds over 300 of them. `flags` is the header's: 0x8000 says it is a response.
std::string mdnsQuery(std::size_t questions, std::uint16_t flags = 0) {
	std::string packet(mdnsHeaderBytes, '\0');
	packet[2] = static_cast<char>(flags >> 8U);
	packet[3] = static_cast<char>(flags & 0xFFU);
	packet[4] = static_cast<char>(questions >> 8U);
	packet[5] = static_cast<char>(questions & 0xFFU);
	const std::string typeAndClass("\0\x0c\0\x01", 4);   // PTR, IN
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
		REQUIRE(::sendto(fd_, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == static_cast<ssize_t>(packet.size()));
	}

	/// The next response (a datagram marked QR=1) that holds `text`, within `patience`; nothing otherwise. Queries, and other
	/// nodes' answers on a shared network, pass by.
	std::optional<std::string> nextResponse(std::string_view text, std::chrono::milliseconds patience) {
		const auto until = std::chrono::steady_clock::now() + patience;
		std::array<char, 9000> buffer{};
		for (;;) {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
			if (left <= 0) return std::nullopt;
			pollfd ready{fd_, POLLIN, 0};
			if (::poll(&ready, 1, static_cast<int>(left)) <= 0) continue;
			const auto got = ::recv(fd_, buffer.data(), buffer.size(), 0);
			if (got < static_cast<ssize_t>(mdnsHeaderBytes)) continue;
			const std::string packet(buffer.data(), static_cast<std::size_t>(got));
			if ((static_cast<unsigned char>(packet[2]) & 0x80U) != 0 && packet.find(text) != std::string::npos) return packet;
		}
	}

	/// How many such responses arrive within `patience`.
	std::size_t countResponses(std::string_view text, std::chrono::milliseconds patience) {
		const auto until = std::chrono::steady_clock::now() + patience;
		std::size_t found = 0;
		for (;;) {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
			if (left.count() <= 0 || !nextResponse(text, left)) return found;
			++found;
		}
	}

private:
	int fd_;
};

/// The dialling node stops within the bound, and a new name restarts it within the bound, on a peer that holds the dial.
void checkHeldDial(const TempDirectory& directory, const std::string& url) {
	writeGraphFile(directory, dialToRec(url));
	{
		auto ear = runEar(directory);
		const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
		INFO("ear printed:\n" << ear.output());
		REQUIRE(ready);
		const std::uint16_t port = controlPort(ear);
		Rpc rpc(port);
		REQUIRE(rpc.call("graph.subscribe").contains("result"));

		const auto before = std::chrono::steady_clock::now();
		const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", url}, {"name", "Other"}}}});
		INFO(update.dump());
		REQUIRE(update.contains("result"));
		rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "running"}}); });
		CHECK(std::chrono::steady_clock::now() - before < stopBound);

		ear.signal(SIGTERM);
		const auto stopping = std::chrono::steady_clock::now();
		CHECK(rpc.readUntilClosed() == 1001);
		const auto exitStatus = ear.waitForExit(stopBound);
		CHECK(std::chrono::steady_clock::now() - stopping < stopBound);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 0);
	}
}

/// The WAV's first non-zero frame onward: how many frames are exact ramp frames, and how many steps are neither +1 nor a
/// block's multiple. Left is the low 16 bits of the frame index, right the high 16, each as a signed sample.
struct RampCheck {
	std::size_t rampFrames = 0;
	std::size_t badFrames = 0;
	std::size_t badSteps = 0;
	std::size_t zeroFramesInside = 0;
	std::uint32_t firstIndex = 0;
	std::uint32_t lastIndex = 0;
	std::size_t lastRunFrames = 0;   // frames since the last step that was neither +1 nor a block's multiple
};

RampCheck checkRamp(const Wav& wav) {
	RampCheck result;
	const auto frames = static_cast<std::size_t>(wav.info.frames);
	std::size_t first = frames;
	std::size_t last = 0;
	for (std::size_t frame = 0; frame < frames; ++frame) {
		if (wav.samples[frame * 2] != 0.0F || wav.samples[frame * 2 + 1] != 0.0F) {
			if (first == frames) first = frame;
			last = frame;
		}
	}
	if (first == frames) return result;
	std::optional<std::uint32_t> previous;
	for (std::size_t frame = first; frame <= last; ++frame) {
		const float left = wav.samples[frame * 2];
		const float right = wav.samples[frame * 2 + 1];
		if (left == 0.0F && right == 0.0F) {
			++result.zeroFramesInside;
			continue;
		}
		const float lo = left * 32768.0F;
		const float hi = right * 32768.0F;
		if (lo != std::trunc(lo) || hi != std::trunc(hi)) {
			++result.badFrames;
			continue;
		}
		const auto word = [](float value) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xFFFFU; };
		const std::uint32_t index = word(lo) | (word(hi) << 16U);
		if (previous) {
			const std::uint32_t step = index - *previous;
			if (step != 1 && (step - 1) % blockSize != 0) {
				++result.badSteps;
				result.lastRunFrames = 0;
			}
		} else {
			result.firstIndex = index;
		}
		previous = index;
		result.lastIndex = index;
		++result.rampFrames;
		++result.lastRunFrames;
	}
	return result;
}

} // namespace

TEST_CASE("A wrong code then the right one pairs in band, and a ramp the server plays lands in the WAV, frame for frame",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	CHECK(ear.hasLine("node ma says dialling " + server.url()));
	const auto id = ear.lineStartingWith("node ma says id ");
	REQUIRE(id);
	CHECK(ear.output().find(*id) < ear.output().find("node ma says dialling"));

	server.send("pair");
	const auto code = ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	REQUIRE(code);
	CHECK(code->size() == pairingCodePrefix.size() + 7);
	server.send("code " + wrongCode(digitsOf(*code)));
	server.send("code " + digitsOf(*code));
	REQUIRE(ear.waitForLineStartingWith("node ma says paired ", deadline));
	REQUIRE(ear.waitForLine(connectedPaired, deadline));
	CHECK(ear.hasLine("node ma says pairing-code-cleared"));
	CHECK(countLines(ear, pairingCodePrefix) == 1);   // the wrong round kept the code
	CHECK_FALSE(ear.lineStartingWith("node ma says pairing-failed"));
	CHECK_FALSE(ear.hasLine("node ma says disconnected"));

	server.send("play 1.5");
	const bool ended = ear.waitForLine("node ma says stream-ended", playDeadline);
	REQUIRE(ended);

	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));
	CHECK(ear.hasLine("node ma says stream flac 48000 2 24"));

	const Wav wav = readWav(directory.recordings() / "rec.wav");
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	REQUIRE(wav.info.channels == 2);
	const RampCheck ramp = checkRamp(wav);
	INFO("ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps
	                    << ", zeros inside " << ramp.zeroFramesInside << ", indices " << ramp.firstIndex << " to " << ramp.lastIndex);
	CHECK(ramp.rampFrames >= graphFrequency * 6 / 5);   // at least 1.2 s
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("A restarted ear keeps its identity and its pairing, at 0600, and is asked for no code", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	std::string id;
	{
		auto ear = runEar(directory);
		REQUIRE(ear.waitForLine(connectedUnpaired, deadline));
		id = ear.lineStartingWith("node ma says id ").value_or("");
		pairOnce(ear, server);
		ear.signal(SIGTERM);
		REQUIRE(ear.waitForExit(deadline) == 0);
	}
	const auto keyFile = directory.config() / "sendspin-ma.key";
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600));
	CHECK(readText(keyFile).find("\"createdBy\"") != std::string::npos);

	auto ear = runEar(directory);
	const bool resumed = ear.waitForLine(connectedPaired, playDeadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(resumed);
	REQUIRE_FALSE(id.empty());
	CHECK(ear.hasLine(id));
	CHECK_FALSE(ear.lineStartingWith(pairingCodePrefix));
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600));
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A server that trusts without pairing gets no stream and no source role", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	REQUIRE(ear.waitForLine(connectedUnpaired, deadline));

	server.send("trust");
	REQUIRE(server.waitForEvent("trusted", deadline));
	server.send("roles");
	const auto roles = server.waitForEvent("roles ", deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(roles);
	CHECK(*roles == "roles none");
	CHECK_FALSE(ear.lineStartingWith("node ma says stream"));
	CHECK_FALSE(ear.lineStartingWith("node ma says sending"));
	CHECK_FALSE(ear.lineStartingWith("node ma says paired"));
	CHECK_FALSE(ear.hasLine(std::string(connectedPaired)));
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("Paired, routed noise reaches the server on the routed channel, with exact zeros on the other", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node white noise;\n"
	                                        "node sendspin ma { server \"" + server.url() + "\"; }\n"
	                                        "route noise:MONO ma:to_server_FL;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool ready = ear.waitForLine("route noise:MONO -> ma:to_server_FL linked", deadline) && ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	pairOnce(ear, server);

	server.send("source-start");
	REQUIRE(ear.waitForLine("node ma says sending 48000 2 24", playDeadline));
	const auto started = server.waitForEvent("source-started ", deadline);
	REQUIRE(started);
	CHECK(*started == "source-started 48000 2 24");
	REQUIRE(server.waitForEvent("source-heard", deadline));
	server.send("source-stop");
	REQUIRE(ear.waitForLine("node ma says sending-stopped", deadline));
	const auto ended = server.waitForEvent("source-ended ", deadline);
	REQUIRE(ended);

	std::istringstream fields(ended->substr(std::string("source-ended ").size()));
	std::size_t frames = 0;
	double rms = 0.0;
	std::size_t rightNonZero = 0;
	std::int64_t stampsUs = 0;
	std::size_t framesBeforeLast = 0;
	std::int64_t arrivalLagUs = 0;
	fields >> frames >> rms >> rightNonZero >> stampsUs >> framesBeforeLast >> arrivalLagUs;
	CHECK(frames >= graphFrequency);
	CHECK(rms == Approx(0.577).margin(0.03));   // uniform noise in [-1, 1)
	CHECK(rightNonZero == 0);
	const auto durationUs = static_cast<std::int64_t>(framesBeforeLast) * 1000000 / graphFrequency;
	CHECK(std::abs(stampsUs - durationUs) <= 50000);
	// On the server's own clock the first chunk was captured a moment before it arrived: a stamp on another clock, or carrying
	// an offset, shows here and not in the difference above.
	INFO("first chunk's arrival - its stamp: " << arrivalLagUs << " us");
	CHECK(arrivalLagUs >= 0);
	CHECK(arrivalLagUs <= captureToArrivalBoundUs);

	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(ear.hasLine("node ma stopped dropped=0"));
}

TEST_CASE("20 wrong rounds hold pairing back: an update with the same settings leaves it held, a new name releases it", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	Rpc rpc(controlPort(ear));
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	server.send("pair");
	const auto code = ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	REQUIRE(code);
	for (int round = 0; round < 20; ++round) server.send("code " + wrongCode(digitsOf(*code)));
	REQUIRE(ear.waitForLine("node ma says pairing-failed pairing-code-mismatch", playDeadline));
	REQUIRE(ear.waitForLine("node ma says pairing-held", deadline));
	REQUIRE(server.waitForEvent("pair-failed ", deadline));

	// The same settings change nothing, so the hold stands: the next attempt is held back, and shows no code.
	const json same = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}}}});
	INFO(same.dump());
	REQUIRE(same.contains("result"));
	server.send("pair");
	REQUIRE(server.waitForEvent("pair-pending", deadline));
	CHECK(countLines(ear, pairingCodePrefix) == 1);
	// The attempt held back says why no code appears.
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says pairing-held", 1, deadline));

	// A new name is a new client, on the same key file, and the round limit lives in memory.
	const std::size_t idsBefore = countLines(ear, "node ma says id ");
	const std::size_t connectsBefore = countLines(ear, "node ma says connected ");
	const json renamed = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}, {"name", "Other"}}}});
	INFO(renamed.dump());
	REQUIRE(renamed.contains("result"));
	REQUIRE(waitForNthLine(ear, "node ma says id ", idsBefore, deadline));
	REQUIRE(waitForNthLine(ear, "node ma says connected ", connectsBefore, playDeadline));
	CHECK(*ear.lineStartingWith("node ma says id ") == *waitForNthLine(ear, "node ma says id ", idsBefore, deadline));

	server.send("pair");
	const auto fresh = waitForNthLine(ear, pairingCodePrefix, 1, deadline);
	REQUIRE(fresh);
	server.send("code " + digitsOf(*fresh));
	REQUIRE(ear.waitForLine(connectedPaired, deadline));

	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A corrupt key file fails the node, unchanged, and the rest of the graph runs", "[system][sendspin]") {
	TempDirectory directory;
	const std::string url = "ws://127.0.0.1:" + std::to_string(freeTcpPort()) + "/sendspin";
	writeGraphFile(directory, graphFileText("node sendspin ma { server \"" + url + "\"; }\n"
	                                        "node white noise;\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route noise:MONO rec:FL;\n",
	                                        graphFrequency, blockSize));
	const auto keyFile = directory.config() / "sendspin-ma.key";
	const std::string corrupt = "{ this is no key file";
	std::ofstream(keyFile, std::ios::binary) << corrupt;
	std::filesystem::permissions(keyFile, std::filesystem::perms(0640));

	auto ear = runEar(directory);
	const auto failed = ear.waitForLineStartingWith("node ma failed start-failed: ", deadline);
	const bool others = waitForLines(ear, {"node rec running", "route noise:MONO -> rec:FL linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	CHECK(*failed == "node ma failed start-failed: sendspin-ma.key: malformed: not JSON");
	REQUIRE(others);
	CHECK(readText(keyFile) == corrupt);
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600));   // a refused file may still hold a private key
	CHECK_FALSE(ear.lineStartingWith("node ma says id "));

	{
		WsClient ws(controlPort(ear));
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		for (;;) {
			const json levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("rec") &&
			    levels["params"]["nodes"]["rec"]["FL"][0].get<double>() > 0.0) {
				break;
			}
		}
	}
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(readText(keyFile) == corrupt);
}

TEST_CASE("With no server at all the sendspin node runs, its meters silent, and SIGTERM exits 0", "[system][sendspin]") {
	TempDirectory directory;
	const std::string url = "ws://127.0.0.1:" + std::to_string(freeTcpPort()) + "/sendspin";
	writeGraphFile(directory, dialToRec(url));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	{
		WsClient ws(controlPort(ear));
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		json levels;
		while (true) {
			levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("ma")) break;
		}
		INFO(levels.dump());
		for (const char* port : {"from_server_FL", "from_server_FR"}) {
			const json& meter = levels["params"]["nodes"]["ma"][port];
			CHECK(meter[0].get<double>() == 0.0);   // peak
			CHECK(meter[1].get<double>() == 0.0);   // RMS
		}
	}
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(stopBound);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
}

TEST_CASE("A peer that accepts and never answers holds up neither a restart nor a stop", "[system][sendspin]") {
	Listener peer(16);
	TempDirectory directory;
	checkHeldDial(directory, peer.url());
}

TEST_CASE("A listener with a full backlog holds up neither a restart nor a stop", "[system][sendspin]") {
	Listener peer(0);
	peer.fillBacklog();
	TempDirectory directory;
	checkHeldDial(directory, peer.url());
}

TEST_CASE("A slow upgrade sees one connection and no another_server, then connects", "[system][sendspin]") {
	SendspinTestServer server({"--slow-upgrade", "3"});
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) &&
	                   ear.waitForLine(std::string(connectedUnpaired), playDeadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	CHECK(server.count("upgrade") == 1);
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A listening node says how it is known, and a server that dials it pairs and plays", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	waitForInstance(ear, "ma", port);
	CHECK_FALSE(ear.lineStartingWith("node ma says dialling"));

	SendspinTestServer server({"--dial", "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	server.send("play 1.5");
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says stream-ended", playDeadline));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, ear.waitForExit(deadline) == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"));
	INFO("ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps);
	CHECK(ramp.rampFrames >= graphFrequency * 6 / 5);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

namespace {

/// A server browsing mDNS finds `node` with this TXT `name`, dials it and pairs; SIGTERM removes the service.
void checkFoundOverMdns(const std::string& node, const std::string& settings, const std::string& name) {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin " + node + " { port " + std::to_string(port) + "; " + settings + " }\n", graphFrequency, blockSize));
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
	CHECK(ear.waitForExit(deadline) == 0);
	const auto removed = server.waitForEvent("service-removed ", playDeadline);
	INFO("the server then printed:\n" << server.events());
	REQUIRE_SEEN(ear, removed);
	CHECK(*removed == "service-removed " + instance);
}

} // namespace

TEST_CASE("A server browsing mDNS finds a listening node, its TXT says its path and name, and it is gone after SIGTERM", "[system][sendspin]") {
	checkFoundOverMdns("found", "name \"Front Room\";", "Front Room");
}

TEST_CASE("A 64-character node name is found over mDNS too", "[system][sendspin]") {
	const std::string node(64, 'n');
	checkFoundOverMdns(node, "", node);
}

TEST_CASE("A server browsing mDNS comes back by itself after a new name restarts the node", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin back { port " + std::to_string(port) + "; }\n", graphFrequency, blockSize));
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
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A server lost mid-stream leaves the node running, and restarted with its state it resumes paired with no action", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/sendspin";
	TempDirectory directory;
	const auto state = directory.path() / "server-state";
	std::filesystem::create_directories(state);
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(port) + "; }\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "node white noise;\n"
	                                        "node file other { path \"other.wav\"; }\n"
	                                        "route ma:from_server_FL rec:FL;\n"
	                                        "route ma:from_server_FR rec:FR;\n"
	                                        "route noise:MONO other:FL;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);
	const std::uint16_t control = controlPort(ear);
	const auto heard = [&](const char* node, const char* meteredPort) {
		WsClient levels(control);
		levels.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		const auto until = std::chrono::steady_clock::now() + playDeadline;
		while (std::chrono::steady_clock::now() < until) {
			const json message = json::parse(levels.read());
			if (message.value("method", "") == "levels" && message["params"]["nodes"].contains(node) &&
			    message["params"]["nodes"][node][meteredPort][0].get<double>() > 0.0) {
				return true;
			}
		}
		return false;
	};
	{
		SendspinTestServer server({"--dial", url, "--state", state.string()});
		REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
		pairOnce(ear, server);
		server.send("play 30");
		REQUIRE_SEEN(ear, heard("ma", "from_server_FR"));   // the ramp's high half: mid-stream, past its first 1.4 s
	}
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says disconnected", playDeadline));
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	CHECK_FALSE(ear.hasLine("node ma stopping"));
	CHECK(heard("other", "FL"));

	const std::size_t connectsBefore = countLines(ear, "node ma says connected ");
	SendspinTestServer server({"--dial", url, "--state", state.string()});
	const auto back = waitForNthLine(ear, "node ma says connected ", connectsBefore, playDeadline);
	REQUIRE(back);
	CHECK(*back == connectedPaired);
	CHECK(countLines(ear, pairingCodePrefix) == 1);
	const std::size_t endsBefore = countLines(ear, "node ma says stream-ended");
	server.send("play 1.5");
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says stream-ended", endsBefore, playDeadline));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, ear.waitForExit(deadline) == 0);
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"));
	INFO("frames of the last run " << ramp.lastRunFrames << ", last index " << ramp.lastIndex << ", not ramp " << ramp.badFrames);
	CHECK(ramp.lastRunFrames >= graphFrequency * 6 / 5);
	CHECK(ramp.badFrames == 0);
}

TEST_CASE("A port another program holds fails the node with the kernel's reason, and a free port recovers it", "[system][sendspin]") {
	Listener holder(16);
	const std::uint16_t held = holder.port();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(held) + "; }\n"
	                                        "node white noise;\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route noise:MONO rec:FL;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool failed = ear.waitForLine("node ma failed start-failed: cannot listen on port " + std::to_string(held) + ": Address already in use", deadline) &&
	                    waitForLines(ear, {"node rec running", "route noise:MONO -> rec:FL linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	CHECK_FALSE(ear.lineStartingWith("node ma says listening"));

	Rpc rpc(controlPort(ear));
	const std::uint16_t freePort = freeTcpPort();
	const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"port", freePort}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	waitForInstance(ear, "ma", freePort);
	CHECK_FALSE(connectRefused(freePort));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("Two sendspin nodes on one port: the second fails naming the first, and a free port recovers it", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string saying = " says listening " + std::to_string(port) + " as ";
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin a { port " + std::to_string(port) + "; }\n"
	                                        "node sendspin b { port " + std::to_string(port) + "; }\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::string refusal = "failed start-failed: cannot listen on port " + std::to_string(port) + ": held by node ";
	// Which of the two starts first is not this test's business.
	const auto until = std::chrono::steady_clock::now() + deadline;
	std::optional<std::string> refused;
	while (!refused && std::chrono::steady_clock::now() < until) {
		refused = ear.lineStartingWith("node a " + refusal);
		if (!refused) refused = ear.lineStartingWith("node b " + refusal);
		if (!refused) ear.waitForLine("\x01", 20ms);
	}
	REQUIRE_SEEN(ear, refused);
	const std::string loser = refused->substr(5, 1);
	const std::string winner = loser == "a" ? "b" : "a";
	CHECK(*refused == "node " + loser + " " + refusal + winner);
	REQUIRE_SEEN(ear, ear.waitForLineStartingWith("node " + winner + saying, deadline));
	REQUIRE_SEEN(ear, ear.waitForLine("node " + winner + " running", deadline));

	Rpc rpc(controlPort(ear));
	const std::uint16_t freePort = freeTcpPort();
	const json update = rpc.call("node.update", {{"name", loser}, {"settings", {{"port", freePort}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	waitForInstance(ear, loser, freePort);
	REQUIRE_SEEN(ear, ear.waitForLine("node " + loser + " running", deadline));
	CHECK_FALSE(ear.lineStartingWith("node " + winner + " failed"));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A new name while a server is connected keeps the port: the node still listens and a server reaches it", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);
	SendspinTestServer server({"--dial", "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));

	Rpc rpc(controlPort(ear));
	const std::string saying = "node ma says listening " + std::to_string(port) + " as ";
	const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"port", port}, {"name", "Other"}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	const auto again = waitForNthLine(ear, saying, 1, playDeadline);
	REQUIRE_SEEN(ear, again);
	CHECK(*again == *ear.lineStartingWith(saying));   // the same key file, so the same instance
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	const auto gone = server.waitForEvent("disconnected ", deadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE(gone);
	// The goodbye is `shutdown`, and this server does not redial by itself after it, so the test asks. One that browses
	// hears the new announcement.
	server.send("dial ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says connected ", 1, playDeadline));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("An upgrade carrying Origin is closed before any client/init, and the same without it gets client/init", "[system][sendspin]") {
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
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("One dialling and one listening node have separate identities, on one server, and both play", "[system][sendspin]") {
	SendspinTestServer server;
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin out { server \"" + server.url() + "\"; }\n"
	                                        "node sendspin in { port " + std::to_string(port) + "; }\n"
	                                        "node file recOut { path \"out.wav\"; }\n"
	                                        "node file recIn { path \"in.wav\"; }\n"
	                                        "route out:from_server_FL recOut:FL;\n"
	                                        "route out:from_server_FR recOut:FR;\n"
	                                        "route in:from_server_FL recIn:FL;\n"
	                                        "route in:from_server_FR recIn:FR;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "in", port);

	const auto pair = [&](const std::string& node) {
		const std::string says = "node " + node + " says ";
		const auto connected = server.waitForEvent("connected ", deadline);
		REQUIRE(connected);
		REQUIRE_SEEN(ear, ear.waitForLine(says + "connected \"ear-test-server\" unpaired", deadline));
		server.send("pair");
		const auto code = ear.waitForLineStartingWith(says + "pairing-code ", deadline);
		REQUIRE(code);
		std::string digits = code->substr((says + "pairing-code ").size());
		std::erase(digits, '-');
		server.send("code " + digits);
		REQUIRE_SEEN(ear, ear.waitForLine(says + "connected \"ear-test-server\" paired", deadline));
		return connected->substr(std::string("connected ").size());
	};
	const std::string outId = pair("out");
	server.send("dial ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
	const std::string inId = pair("in");
	CHECK(outId != inId);
	CHECK(ear.hasLine("node out says id " + outId));
	CHECK(ear.hasLine("node in says id " + inId));
	CHECK(instance == "ear-in-" + inId.substr(0, 8));
	CHECK(std::filesystem::exists(directory.config() / "sendspin-out.key"));
	CHECK(std::filesystem::exists(directory.config() / "sendspin-in.key"));

	server.send("play 1.5");   // the node paired last
	server.send("use " + outId);
	server.send("play 1.5");
	REQUIRE_SEEN(ear, ear.waitForLine("node in says stream-ended", playDeadline));
	REQUIRE_SEEN(ear, ear.waitForLine("node out says stream-ended", playDeadline));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, ear.waitForExit(deadline) == 0);
	for (const char* file : {"out.wav", "in.wav"}) {
		const RampCheck ramp = checkRamp(readWav(directory.recordings() / file));
		INFO(file << ": ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps);
		CHECK(ramp.rampFrames >= graphFrequency * 6 / 5);
		CHECK(ramp.badFrames == 0);
		CHECK(ramp.badSteps == 0);
	}
}

TEST_CASE("A dialling node opens no listening socket", "[system][sendspin]") {
	TempDirectory directory;
	const std::string url = "ws://127.0.0.1:" + std::to_string(freeTcpPort()) + "/sendspin";
	writeGraphFile(directory, dialToRec(url));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	CHECK(connectRefused(8928));   // the library's default, and no test here listens on it
	CHECK_FALSE(ear.lineStartingWith("node ma says listening"));
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A frame that claims more than a message may hold is closed before it costs any memory, and the node runs on", "[system][sendspin]") {
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
			std::string next = frameHeader(first ? 0x02 : 0x00, 65536);   // never the last of a message
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
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("Bare connects cannot fill the log", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);

	const auto start = std::chrono::steady_clock::now();
	constexpr int connects = 300;
	for (int i = 0; i < connects; ++i) CHECK_FALSE(connectRefused(port));
	// What the transport says, it has said by the time a real upgrade after them is answered.
	CHECK(json::parse(firstMessage(port, WsOptions())).value("type", "") == "client/init");
	ear.waitForLine("\x01", 200ms);
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::istringstream lines(ear.output());
	std::size_t transportLines = 0;
	for (std::string line; std::getline(lines, line);) transportLines += line.find("handleConnection") != std::string::npos ? 1U : 0U;
	INFO(connects << " connects in " << seconds << " s, " << transportLines << " lines");
	CHECK(static_cast<double>(transportLines) <= seconds / 10.0 + 2.0);   // one every 10 s, whatever the connects
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A node removed and added back while a server is connected takes its port back, once the old one has let go", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/sendspin";
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(port) + "; }\n", graphFrequency, blockSize));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);
	SendspinTestServer server({"--dial", url});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));

	Rpc rpc(controlPort(ear));
	REQUIRE(rpc.call("node.remove", {{"name", "ma"}}).contains("result"));
	const json added = rpc.call("node.add", {{"name", "ma"}, {"type", "sendspin"}, {"settings", {{"port", port}}}});
	INFO(added.dump());
	REQUIRE(added.contains("result"));

	const std::string saying = "node ma says listening " + std::to_string(port) + " as ";
	REQUIRE_SEEN(ear, waitForNthLine(ear, saying, 1, playDeadline));
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma running", 1, deadline));
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	const std::string printed = ear.output();
	CHECK(printed.find("node ma stopped") < printed.rfind(saying));   // the old one was gone first
	server.send("dial " + url);
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says connected ", 1, playDeadline));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A server's name with control characters and a line break stays on one status line, quoted", "[system][sendspin]") {
	SendspinTestServer server({"--name", "line\nnode ma says paired x\xc2\x9b\xe2\x80\xa8 \"q\""});
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says connected \"line?node ma says paired x?? ?q?\" unpaired", deadline));
	CHECK_FALSE(ear.lineStartingWith("node ma says paired"));
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A server cannot change the depth of a running stream: asked for 16 bits it refuses, and the WAV holds the ramp", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	pairOnce(ear, server);

	server.send("play 3");
	REQUIRE(server.waitForEvent("play-start ", playDeadline));
	server.send("prefer-16");
	const auto answer = server.waitForEvent("prefer-16 ", deadline);
	REQUIRE(answer);
	CHECK(*answer == "prefer-16 false");
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says stream-ended", playDeadline));
	ear.signal(SIGTERM);
	REQUIRE(ear.waitForExit(deadline) == 0);
	CHECK(countLines(ear, "node ma says stream ") == 1);
	CHECK(ear.hasLine("node ma says stream flac 48000 2 24"));
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"));
	INFO("ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps);
	CHECK(ramp.rampFrames >= graphFrequency * 5 / 2);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("A large block raises the lead the player reports to what still plays after a clear", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin ma { server \"" + server.url() + "\"; }\n", graphFrequency, 4096));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	server.send("lead");
	const auto lead = server.waitForEvent("lead ", playDeadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE(lead);
	CHECK(*lead == "lead 256");   // the feed's 8192 frames and a block of 4096, at 48 kHz
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("A listening node announces itself unasked, with its PTR, SRV, TXT and A", "[system][sendspin]") {
	MdnsSocket browser(true);   // before ear: only an announcement can reach it
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
	const auto count = [&](std::size_t at) { return (static_cast<unsigned char>((*announcement)[at]) << 8U) | static_cast<unsigned char>((*announcement)[at + 1]); };
	CHECK(count(4) == 0);                            // no questions: nobody asked
	CHECK(count(6) == 1);                            // the PTR
	CHECK(count(10) >= 3);                           // the SRV, the A and the TXT
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}

TEST_CASE("One datagram of many questions gets one answer, and a response or a runt gets none", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	const std::string instance = waitForInstance(ear, "ma", port);

	// A one-shot querier: from any port but 5353 the answer is unicast, so nobody else's reaches this socket except what asked.
	MdnsSocket querier(false);
	constexpr auto window = 300ms;
	const std::string service = "_sendspin";
	const auto asked = [&](const std::string& packet) {
		querier.sendTo("127.0.0.1", packet);
		return querier.countResponses(service, window);
	};
	const std::string many = mdnsQuery(330);
	REQUIRE(many.size() <= 2040);
	CHECK(asked(mdnsQuery(1)) == 1);
	CHECK(asked(many) == 1);
	CHECK(asked(mdnsQuery(330, 0x8000)) == 0);       // QR=1
	CHECK(asked(many.substr(0, 6)) == 0);            // shorter than a header
	CHECK(asked(mdnsQuery(1)) == 1);                 // and it still answers
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
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
	CHECK(static_cast<double>(answers) <= seconds + 2.0);   // one at each whole second, the announcement among them
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}
