// The sendspin node's audio both ways, and what a stream does: a ramp to the file, noise to the server, a depth the server cannot change, the lead after a clear.
// (Shared helpers: sendspin_system.hpp.)

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// How long a source's first chunk may take from capture to the server: a chunk, the library's buffer, the handoff's wake.
constexpr std::int64_t captureToArrivalBoundUs = 500000;

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
	CHECK(rms == Approx(0.577).margin(0.03)); // uniform noise in [-1, 1)
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
	const std::size_t endsBefore = countLines(ear, "node ma says stream-ended");
	server.send("play 1.5");
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says stream-ended", endsBefore, playDeadline));

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
