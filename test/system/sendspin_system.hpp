// The owner's end-to-end tests of the sendspin node (docs/design/sendspin.md section 14, S1 criteria 10 to 12, S2 criteria
// 3 to 8, S3 criteria 2 to 8): the real out/ear runs a graph file with a sendspin node routed into a file node, and dials
// the real server, or listens for it and is found over mDNS. A listening node's port is refused when it is held, and an
// upgrade with Origin is closed.
// Paired by a code, the server plays a ramp in which every frame names its place, and the WAV must hold it; routed noise
// reaches the server on the routed channel alone. Unpaired, nothing moves. A round limit holds, a bad key file fails only
// the node. With no server the node runs and is silent; a peer that never answers, or a listener that drops the dial,
// holds up neither a stop nor a restart. Sockets that prove nothing keep no server out (section 11, item 6). Every wait is for an event (a line ear prints, an event the server prints, a
// message, ear's exit); a deadline only fails a test.
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ear_system.hpp"
#include "sendspin_server.hpp"

/// REQUIRE on a wait for what ear prints, naming all it printed by the time the wait ended.
#define REQUIRE_SEEN(ear, wait)                                                                                        \
	do {                                                                                                               \
		const bool seen = static_cast<bool>(wait);                                                                     \
		INFO("ear printed:\n" << (ear).output());                                                                      \
		REQUIRE(seen);                                                                                                 \
	} while (false)

namespace ear::test {

inline constexpr std::uint32_t graphFrequency = 48000;
inline constexpr std::uint32_t blockSize = 256;
/// Fails a play that never finishes: longer than the shared deadline, because a stream's start takes the server's lead.
inline constexpr auto playDeadline = 30s;
/// S1's bound on a stop and on a restart.
inline constexpr auto stopBound = 5s;

/// A sendspin node called "ma" with these settings, routed side for side into file node "rec".
inline std::string maToRec(const std::string& settings) {
	return graphFileText("node sendspin ma { " + settings +
	                         " }\n"
	                         "node file rec { path \"rec.wav\"; }\n"
	                         "route ma:from.left rec:record.left;\n"
	                         "route ma:from.right rec:record.right;\n",
	                     graphFrequency, blockSize);
}

inline std::string dialToRec(const std::string& url) { return maToRec("server \"" + url + "\";"); }

inline std::string listenToRec(std::uint16_t port) { return maToRec("port " + std::to_string(port) + ";"); }

inline ChildProcess runEar(const TempDirectory& directory) { return ChildProcess(EAR_BINARY, earArguments(directory)); }

inline constexpr std::string_view connectedUnpaired = "node ma says connected \"ear-test-server\" unpaired";
inline constexpr std::string_view connectedPaired = "node ma says connected \"ear-test-server\" paired";
inline const std::string pairingCodePrefix = "node ma says pairing-code ";

inline const std::vector<std::string> runningAndLinked{
	"node ma running",
	"node rec running",
	"route ma:from.left -> rec:record.left linked",
	"route ma:from.right -> rec:record.right linked",
};

inline bool waitForLines(ChildProcess& ear, const std::vector<std::string>& lines) {
	for (const auto& line : lines) {
		if (!ear.waitForLine(line, deadline)) return false;
	}
	return true;
}

/// The `index`th line (from 0) starting with `prefix`, once ear has printed it. ChildProcess gives only the first, and
/// offers no wake on "something new", so this reads what arrives in short bounded reads; the line arriving ends the wait.
inline std::optional<std::string> waitForNthLine(ChildProcess& ear, const std::string& prefix, std::size_t index,
                                                 std::chrono::milliseconds patience) {
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

inline std::size_t countLines(const ChildProcess& ear, const std::string& prefix) {
	std::istringstream lines(ear.output());
	std::size_t seen = 0;
	for (std::string line; std::getline(lines, line);) seen += line.starts_with(prefix) ? 1U : 0U;
	return seen;
}

/// The digits of a `says pairing-code 123-456` line.
inline std::string digitsOf(const std::string& line) {
	std::string digits = line.substr(pairingCodePrefix.size());
	std::erase(digits, '-');
	return digits;
}

/// Connected and unpaired: `pair`, then the code ear shows, until ear says it is paired on the new key.
inline void pairOnce(ChildProcess& ear, SendspinTestServer& server) {
	server.send("pair");
	const auto code = ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(code);
	server.send("code " + digitsOf(*code));
	REQUIRE(ear.waitForLine(connectedPaired, deadline));
}

/// The control port's number, once ear has said it.
inline std::uint16_t controlPort(ChildProcess& ear) {
	const auto listening = ear.waitForLineStartingWith(listeningPrefix, deadline);
	REQUIRE(listening);
	return static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));
}

/// A free TCP port on 127.0.0.1 with nothing behind it, from below the kernel's range for outgoing sockets: a flood in a test
/// running beside this one churns through that range, and a port it holds as its own end fails a node's listen.
inline std::uint16_t freeTcpPort() {
	static std::mt19937 pick{std::random_device{}()};
	std::uniform_int_distribution<int> belowEphemeral(20000, 32767);
	for (int attempt = 0; attempt < 100; ++attempt) {
		const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		REQUIRE(fd >= 0);
		const auto port = static_cast<std::uint16_t>(belowEphemeral(pick));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		const bool free = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
		::close(fd);
		if (free) return port;
	}
	FAIL("no free port below the kernel's outgoing range");
	return 0;
}

inline bool connectRefused(std::uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	const bool refused =
		::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 && errno == ECONNREFUSED;
	::close(fd);
	return refused;
}

/// The instance a listening node says it goes by, once it has said so: `ear-<node>-<first 8 of its id>`, its id's own.
inline std::string waitForInstance(ChildProcess& ear, const std::string& node, std::uint16_t port) {
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

/// The WAV's first non-zero frame onward: how many frames are exact ramp frames, and how many steps are neither +1 nor a
/// block's multiple. Left is the low 16 bits of the frame index, right the high 16, each as a signed sample.
/// One second of the 1.5 s every test plays. On a loaded host the server can join its own ramp late, and it holds back the
/// tail: 50688 frames were seen, all exact (erratum 38).
inline constexpr std::uint32_t rampFramesAtLeast = graphFrequency;

struct RampCheck {
	std::size_t rampFrames = 0;
	std::size_t badFrames = 0;
	std::size_t badSteps = 0;
	std::size_t zeroFramesInside = 0;
	std::uint32_t firstIndex = 0;
	std::uint32_t lastIndex = 0;
	std::size_t lastRunFrames = 0; // frames since the last step that was neither +1 nor a block's multiple
};

inline RampCheck checkRamp(const Wav& wav) {
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
		const auto word = [](float value) {
			return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xFFFFU;
		};
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

} // namespace ear::test
