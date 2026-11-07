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
#include <ostream>
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

/// One second of the 1.5 s every test plays. On a loaded host the server can join its own ramp late, and it holds back the
/// tail: 50688 frames were seen, all exact (erratum 38).
inline constexpr std::uint32_t rampFramesAtLeast = graphFrequency;

/// The frames of one decoded chunk: what FFmpeg's FLAC encoder makes at 48000 Hz (aiosendspin's `_chunk_samples` is the
/// encoder's `frame_size`), whatever the test server's 1200 frames a `prepare_audio`. The library makes at most one blended
/// frame per chunk (section 7, erratum 42).
inline constexpr std::size_t chunkFrames = 4608;

/// The WAV's first non-zero frame onward. Left is the low 16 bits of the frame index and right the high 16, each as a signed
/// sample, so a frame names its place, and a whole frame must say exactly that.
/// The one other frame allowed is the library's correction of a clock error past 100 us (erratum 42), a blend: each channel
/// the average of two consecutive ramp frames, a frame's neighbours saying which two and how it was made.
///   insert: n-1, blend of n-1 and n, n
///   drop:   n-1, blend of n and n+1, n+2
/// Its neighbours are whole frames: the library's corrections are at least a chunk apart, less the two frames at its ends.
/// At the edge of a run of ramp (the WAV's first or last non-zero frame, or beside silence) one neighbour has to do. A drop
/// of a stream's first chunk leaves a blend with no frame before it, and an insert always has one, so that blend must be a
/// drop's. One with no frame after it, the tail cut by stream/end's clear, may be either's. No neighbour at all proves
/// nothing, and is refused.
/// A blend's index is worked out on the 32-bit index and each channel averaged as a signed sample, because a word's wrap is
/// no plain k + 0.5: 0x7FFF to 0x8000 gives left -0.5 (32767 and -32768), and 0xFFFF to 0x10000 gives left -0.5 and right
/// h + 0.5, the one blend with both channels fractional.
struct RampCheck {
	std::size_t rampFrames = 0; // exact ramp frames
	std::size_t blends = 0;     // blends accepted by their shape; those over the limit also count in badFrames
	std::size_t badFrames = 0;  // frames neither of those, and each blend past blendLimit()
	std::size_t badSteps = 0;   // steps between exact frames that are neither +1, a block's multiple nor a drop's 3
	std::size_t zeroFramesInside = 0;
	std::uint32_t firstIndex = 0;
	std::uint32_t lastIndex = 0;
	std::size_t lastRunFrames = 0; // exact frames since the last bad step
};

/// What a red needs to say about a ramp.
inline std::ostream& operator<<(std::ostream& out, const RampCheck& ramp) {
	return out << "ramp frames " << ramp.rampFrames << ", blends " << ramp.blends << ", not ramp " << ramp.badFrames
	           << ", bad steps " << ramp.badSteps << ", zeros inside " << ramp.zeroFramesInside << ", indices "
	           << ramp.firstIndex << " to " << ramp.lastIndex << ", frames of the last run " << ramp.lastRunFrames;
}

/// One blend per chunk the WAV's frames could have come from: each whole chunk, and one cut short at either end. Blends beyond
/// it are corrections the library's rule cannot make, so they fail as frames off the ramp, in every test that asks for none.
inline std::size_t blendLimit(std::size_t frames) { return frames / chunkFrames + 2; }

/// The ramp index a frame names, if it names one: both channels whole samples.
inline std::optional<std::uint32_t> rampIndexOf(float left, float right) {
	const float lo = left * 32768.0F;
	const float hi = right * 32768.0F;
	if (lo != std::trunc(lo) || hi != std::trunc(hi)) return std::nullopt;
	const auto word = [](float value) {
		return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xFFFFU;
	};
	return word(lo) | (word(hi) << 16U);
}

/// Whether a frame is exactly the average of ramp frames `n` and `n + 1`. A sum of two 16-bit samples over 2^16 fits a float.
inline bool isBlendOf(std::uint32_t n, float left, float right) {
	const std::uint32_t next = n + 1;
	const auto sample = [](std::uint32_t word) { return static_cast<int>(static_cast<std::int16_t>(word & 0xFFFFU)); };
	const auto average = [&](std::uint32_t shift) {
		return static_cast<float>(sample(n >> shift) + sample(next >> shift)) / 65536.0F;
	};
	return left == average(0U) && right == average(16U);
}

/// Whether the off-ramp frame at `frame` has a blend's value and neighbours (RampCheck's comment says which).
inline bool isBlend(const Wav& wav, std::size_t frame) {
	const auto frames = static_cast<std::size_t>(wav.info.frames);
	const auto left = [&](std::size_t at) { return wav.samples[at * 2]; };
	const auto right = [&](std::size_t at) { return wav.samples[at * 2 + 1]; };
	const auto isSilent = [&](std::size_t at) { return left(at) == 0.0F && right(at) == 0.0F; };
	const bool hasBefore = frame > 0 && !isSilent(frame - 1);
	const bool hasAfter = frame + 1 < frames && !isSilent(frame + 1);
	const auto before = hasBefore ? rampIndexOf(left(frame - 1), right(frame - 1)) : std::nullopt;
	const auto after = hasAfter ? rampIndexOf(left(frame + 1), right(frame + 1)) : std::nullopt;
	if (hasBefore != before.has_value() || hasAfter != after.has_value()) return false; // beside another off-ramp frame
	const auto blendOf = [&](std::uint32_t n) { return isBlendOf(n, left(frame), right(frame)); };
	if (before && after) {
		const std::uint32_t step = *after - *before;
		return (step == 1 && blendOf(*before)) || (step == 3 && blendOf(*before + 1));
	}
	if (after) return blendOf(*after - 2);
	if (before) return blendOf(*before) || blendOf(*before + 1);
	return false;
}

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
	bool afterBlend = false;
	for (std::size_t frame = first; frame <= last; ++frame) {
		const float left = wav.samples[frame * 2];
		const float right = wav.samples[frame * 2 + 1];
		if (left == 0.0F && right == 0.0F) {
			++result.zeroFramesInside;
			continue;
		}
		const std::optional<std::uint32_t> index = rampIndexOf(left, right);
		if (!index) {
			afterBlend = isBlend(wav, frame);
			++(afterBlend ? result.blends : result.badFrames);
			continue;
		}
		if (previous) {
			const std::uint32_t step = *index - *previous;
			if (step != 1 && (step - 1) % blockSize != 0 && !(afterBlend && step == 3)) {
				++result.badSteps;
				result.lastRunFrames = 0;
			}
		} else {
			result.firstIndex = *index;
		}
		afterBlend = false;
		previous = index;
		result.lastIndex = *index;
		++result.rampFrames;
		++result.lastRunFrames;
	}
	const std::size_t limit = blendLimit(result.rampFrames + result.blends);
	if (result.blends > limit) result.badFrames += result.blends - limit;
	return result;
}

} // namespace ear::test
