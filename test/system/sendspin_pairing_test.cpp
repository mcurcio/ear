// The sendspin node and its codes, identity and key files: pairing by a code, a restart that keeps it, the round limit, a corrupt key file.
// (Shared helpers: sendspin_system.hpp.)

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// Six digits that are not `digits`.
std::string wrongCode(std::string digits) {
	digits[0] = digits[0] == '0' ? '1' : '0';
	return digits;
}

std::filesystem::perms modeOf(const std::filesystem::path& path) {
	return std::filesystem::symlink_status(path).permissions() & std::filesystem::perms::mask;
}

} // namespace

TEST_CASE("A wrong code then the right one pairs in band, and a ramp the server plays lands in the "
          "WAV, frame for frame",
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
	CHECK(countLines(ear, pairingCodePrefix) == 1); // the wrong round kept the code
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
	INFO(ramp);
	CHECK(ramp.rampFrames >= rampFramesAtLeast);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("A restarted ear keeps its identity and its pairing, at 0600, and is asked for no code",
          "[system][sendspin]") {
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

TEST_CASE("20 wrong rounds hold pairing back: an update with the same settings leaves it held, a "
          "new name releases it",
          "[system][sendspin]") {
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
	const json renamed =
		rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}, {"name", "Other"}}}});
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
	writeGraphFile(directory, graphFileText("node sendspin ma { server \"" + url +
	                                            "\"; }\n"
	                                            "node white noise;\n"
	                                            "node file rec { path \"rec.wav\"; }\n"
	                                            "route noise:noise.mono rec:record.left;\n",
	                                        graphFrequency, blockSize));
	const auto keyFile = directory.config() / "sendspin-ma.key";
	const std::string corrupt = "{ this is no key file";
	std::ofstream(keyFile, std::ios::binary) << corrupt;
	std::filesystem::permissions(keyFile, std::filesystem::perms(0640));

	auto ear = runEar(directory);
	const auto failed = ear.waitForLineStartingWith("node ma failed start-failed: ", deadline);
	const bool others = waitForLines(ear, {"node rec running", "route noise:noise.mono -> rec:record.left linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	CHECK(*failed == "node ma failed start-failed: sendspin-ma.key: malformed: not JSON");
	REQUIRE(others);
	CHECK(readText(keyFile) == corrupt);
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600)); // a refused file may still hold a private key
	CHECK_FALSE(ear.lineStartingWith("node ma says id "));

	{
		WsClient ws(controlPort(ear));
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		for (;;) {
			const json levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("rec") &&
			    levels["params"]["nodes"]["rec"]["record.left"][0].get<double>() > 0.0) {
				break;
			}
		}
	}
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(readText(keyFile) == corrupt);
}

TEST_CASE("One dialling and one listening node have separate identities, on one server, and both play",
          "[system][sendspin]") {
	SendspinTestServer server;
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin out { server \"" + server.url() +
	                                            "\"; }\n"
	                                            "node sendspin in { port " +
	                                            std::to_string(port) +
	                                            "; }\n"
	                                            "node file recOut { path \"out.wav\"; }\n"
	                                            "node file recIn { path \"in.wav\"; }\n"
	                                            "route out:from.left recOut:record.left;\n"
	                                            "route out:from.right recOut:record.right;\n"
	                                            "route in:from.left recIn:record.left;\n"
	                                            "route in:from.right recIn:record.right;\n",
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

	server.send("play 1.5"); // the node paired last
	server.send("use " + outId);
	server.send("play 1.5");
	REQUIRE_SEEN(ear, ear.waitForLine("node in says stream-ended", playDeadline));
	REQUIRE_SEEN(ear, ear.waitForLine("node out says stream-ended", playDeadline));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, ear.waitForExit(deadline) == 0);
	for (const char* file : {"out.wav", "in.wav"}) {
		const RampCheck ramp = checkRamp(readWav(directory.recordings() / file));
		INFO(file << ": " << ramp);
		CHECK(ramp.rampFrames >= rampFramesAtLeast);
		CHECK(ramp.badFrames == 0);
		CHECK(ramp.badSteps == 0);
	}
}
