// The sendspin node's audio both ways, and what a stream does: a ramp to the file, noise to the server, a depth the server cannot change, the lead after a clear.
// (Shared helpers: sendspin_system.hpp.)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// How far a chunk's stamp may stand from the last one's plus that one's length and still be the frames' own time: the
/// stamps are dated by the graph's cycles (docs/node-types.md#clock), so between cycles the host left alone they step by
/// the frames exactly, but for the microseconds each is rounded to and the internal clock's loop moves.
constexpr std::int64_t stampSlackUs = 200;
/// The least a chunk takes from its capture to the server when nothing holds it up: its own length (a piece is at most
/// 100 ms), the library's send and the socket. Judged on the quickest chunk of the stream, which only a stamp on another
/// clock or carrying an offset can keep above it; a loaded host only slows the others.
constexpr std::int64_t quickestArrivalBoundUs = 250000;

/// The source's chunks as they arrive, judged against their stamps: each step from the last stamp is the last chunk's
/// length (a clean step), or more by the graph's lost time when a cycle was skipped (counted, never judged: the frames
/// skip nothing, the stamps move on), and never less.
struct SourceStamps {
	std::size_t chunks = 0;
	std::size_t cleanFrames = 0; // frames whose chunk followed the last with a clean step
	std::size_t skips = 0;       // steps longer by more than the slack: the host skipped cycles while capturing
	std::size_t backward = 0;    // steps shorter by more than the slack: a stamp that is not the frames' time
	std::int64_t worstCleanUs = 0;
	std::int64_t quickestLagUs = INT64_MAX;
	std::int64_t leastLagUs = INT64_MAX; // arrival - stamp: below 0 the stamp is in the server's future
	std::optional<std::int64_t> lastStampUs;
	std::size_t lastFrames = 0;

	void take(std::size_t frames, std::int64_t stampUs, std::int64_t lagUs) {
		++chunks;
		leastLagUs = std::min(leastLagUs, lagUs);
		quickestLagUs = std::min(quickestLagUs, lagUs);
		if (lastStampUs) {
			const std::int64_t lengthUs = static_cast<std::int64_t>(lastFrames) * 1000000 / graphFrequency;
			const std::int64_t excessUs = stampUs - *lastStampUs - lengthUs;
			if (excessUs > stampSlackUs) {
				++skips;
			} else if (excessUs < -stampSlackUs) {
				++backward;
			} else {
				cleanFrames += frames;
				worstCleanUs = std::max(worstCleanUs, std::abs(excessUs));
			}
		}
		lastStampUs = stampUs;
		lastFrames = frames;
	}
};

inline std::ostream& operator<<(std::ostream& out, const SourceStamps& stamps) {
	return out << stamps.chunks << " chunks, clean frames " << stamps.cleanFrames << ", skips " << stamps.skips
	           << ", backward " << stamps.backward << ", worst clean step " << stamps.worstCleanUs
	           << " us off its length, quickest arrival " << stamps.quickestLagUs << " us";
}

} // namespace

TEST_CASE("Paired, routed noise reaches the server on the routed channel, with exact zeros on the other",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node white noise;\n"
	                                        "node sendspin ma { server \"" +
	                                            server.url() +
	                                            "\"; }\n"
	                                            "route noise:noise.mono ma:to.left;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool ready = ear.waitForLine("route noise:noise.mono -> ma:to.left linked", deadline) &&
	                   ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	pairOnce(ear, server);

	server.send("source-start");
	REQUIRE(ear.waitForLine("node ma says sending 48000 2 24", playDeadline));
	const auto started = server.waitForEvent("source-started ", deadline);
	REQUIRE(started);
	CHECK(*started == "source-started 48000 2 24");
	// Chunk by chunk until a second of frames has come on clean steps and one chunk came as quickly as nothing holding
	// it up allows: a host that skips cycles or holds chunks up only makes the source stream longer
	// (docs/testing.md#making-the-rare-event). The deadline only fails the test.
	SourceStamps stamps;
	const auto until = std::chrono::steady_clock::now() + rampDeadline;
	while (stamps.cleanFrames < graphFrequency || stamps.quickestLagUs > quickestArrivalBoundUs) {
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
		const auto chunk = left.count() > 0 ? server.waitForEvent("source-chunk ", left) : std::nullopt;
		INFO(stamps);
		REQUIRE(chunk);
		std::istringstream fields(chunk->substr(std::string("source-chunk ").size()));
		std::size_t frames = 0;
		std::int64_t stampUs = 0;
		std::int64_t lagUs = 0;
		fields >> frames >> stampUs >> lagUs;
		stamps.take(frames, stampUs, lagUs);
	}
	server.send("source-stop");
	REQUIRE(ear.waitForLine("node ma says sending-stopped", deadline));
	const auto ended = server.waitForEvent("source-ended ", deadline);
	REQUIRE(ended);

	std::istringstream fields(ended->substr(std::string("source-ended ").size()));
	std::size_t frames = 0;
	double rms = 0.0;
	std::size_t rightNonZero = 0;
	fields >> frames >> rms >> rightNonZero;
	CHECK(frames >= graphFrequency);
	// Uniform noise in [-1, 1), within 6 standard deviations for the frames measured
	// (docs/testing.md#statistics-from-the-count).
	INFO("left RMS " << rms << " over " << frames << " frames, sigma " << noiseRmsSigma(frames));
	CHECK(noiseRmsWithin(rms, frames, 6.0));
	CHECK(rightNonZero == 0);
	INFO(stamps);
	// No stamp steps back against its frames, and on the server's own clock no chunk arrived before its capture: a stamp
	// on another clock, or carrying an offset, shows in one of these or keeps every chunk slower than the bound above.
	CHECK(stamps.backward == 0);
	CHECK(stamps.leastLagUs >= 0);

	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(ear.hasLine("node ma removed dropped=0"));
}

TEST_CASE("A server lost mid-stream leaves the node running, and restarted with its state it "
          "resumes paired with no action",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/sendspin";
	TempDirectory directory;
	const auto state = directory.path() / "server-state";
	std::filesystem::create_directories(state);
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(port) +
	                                            "; }\n"
	                                            "node file rec { path \"rec.wav\"; }\n"
	                                            "node white noise;\n"
	                                            "node file other { path \"other.wav\"; }\n"
	                                            "route ma:from.left rec:record.left;\n"
	                                            "route ma:from.right rec:record.right;\n"
	                                            "route noise:noise.mono other:record.left;\n",
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
		REQUIRE_SEEN(ear, heard("ma", "from.right")); // the ramp's high half: mid-stream, past its first 1.4 s
	}
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says disconnected", playDeadline));
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	CHECK_FALSE(ear.lineStartingWith("node ma stopping"));
	CHECK(heard("other", "record.left"));

	const std::size_t connectsBefore = countLines(ear, "node ma says connected ");
	SendspinTestServer server({"--dial", url, "--state", state.string()});
	const auto back = waitForNthLine(ear, "node ma says connected ", connectsBefore, playDeadline);
	REQUIRE(back);
	CHECK(*back == connectedPaired);
	CHECK(countLines(ear, pairingCodePrefix) == 1);
	// The new stream's ramp, in what is written from here on.
	const std::size_t mark = wavFramesSoFar(directory.recordings() / "rec.wav");
	server.send(playLong);
	REQUIRE_SEEN(ear, waitForRamp(
						  directory.recordings() / "rec.wav",
						  [](const RampCheck& ramp) { return ramp.lastRunFrames >= rampFramesAtLeast; }, mark));
	REQUIRE_SEEN(ear, endStream(ear, server));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, ear.waitForExit(deadline) == 0);
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"));
	INFO(ramp);
	CHECK(ramp.lastRunFrames >= rampFramesAtLeast);
	CHECK(ramp.badFrames == 0);
}

TEST_CASE("A server cannot change the depth of a running stream: asked for 16 bits it refuses, and "
          "the WAV holds the ramp",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine(connectedUnpaired, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	pairOnce(ear, server);

	server.send(playLong);
	REQUIRE(server.waitForEvent("play-start ", playDeadline));
	server.send("prefer-16");
	const auto answer = server.waitForEvent("prefer-16 ", deadline);
	REQUIRE(answer);
	CHECK(*answer == "prefer-16 false");
	// Two and a half seconds of the ramp, exact, around the ask: the depth stayed.
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "rec.wav", graphFrequency * 5 / 2));
	REQUIRE_SEEN(ear, endStream(ear, server));
	ear.signal(SIGTERM);
	REQUIRE(ear.waitForExit(deadline) == 0);
	CHECK(countLines(ear, "node ma says stream ") == 1);
	CHECK(ear.hasLine("node ma says stream flac 48000 2 24"));
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"));
	INFO(ramp);
	CHECK(ramp.rampFrames >= graphFrequency * 5 / 2);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("A large block raises the lead the player reports to what still plays after a clear", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory,
	               graphFileText("node sendspin ma { server \"" + server.url() + "\"; }\n", graphFrequency, 4096));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	server.send("lead");
	const auto lead = server.waitForEvent("lead ", playDeadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE(lead);
	CHECK(*lead == "lead 256"); // the feed's 8192 frames and a block of 4096, at 48 kHz
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}
