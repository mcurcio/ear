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
	const bool ready = waitForLines(ear, runningAndLinked) && connectedUnpaired(ear);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	// Who it is and how it is reached, before it dials (docs/node-types.md#status-and-events).
	CHECK(lastStatus(ear, "url") == json(server.url()));
	CHECK_FALSE(anyStatus(ear, "listenPort")); // 0, its initial value: it dials
	CHECK_FALSE(anyStatus(ear, "instance"));
	const auto id = waitForClientId(ear);
	REQUIRE(id);
	CHECK(id->size() > 8);
	CHECK(indexOf(ear, statusTo("clientId", *id)) < indexOf(ear, statusTo("connection", "dialling")));
	CHECK(indexOf(ear, statusTo("connection", "dialling")) < indexOf(ear, statusTo("connection", "connected")));

	server.send("pair");
	const auto code = waitForPairingCode(ear);
	REQUIRE(code);
	CHECK(code->size() == 7);
	CHECK((*code)[3] == '-');
	const auto codeRecord = ear.waitForRecord(codeOffered(), deadline);
	CHECK(codeRecord->value("msg", "") == "node ma pairingCode \"\" -> " + *code);
	server.send("code " + wrongCode(digitsOf(*code)));
	server.send("code " + digitsOf(*code));
	const auto paired = waitForEvent(ear, "paired");
	REQUIRE(paired);
	CHECK(paired->value("severity", "") == "info");
	CHECK((*paired)["args"]["server"].get<std::string>().size() > 0);
	REQUIRE(waitForPaired(ear));
	CHECK(lastStatus(ear, "paired") == json(true));
	CHECK(countStatus(ear, "pairingCode", "") == 1); // the code spent once paired
	CHECK(countPairingCodes(ear) == 1);              // the wrong round kept the code
	CHECK(countEvents(ear, "pairing-failed") == 0);
	CHECK_FALSE(everDisconnected(ear));

	server.send(playLong);
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "rec.wav"));
	REQUIRE_SEEN(ear, endStream(ear, server));

	ear.signal(SIGTERM);
	const auto exitStatus = waitForStop(ear);
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec removed dropped=0"));
	CHECK(streamedFlac24(ear));
	CHECK(countStreams(ear) == 1);
	CHECK(countStreamEnds(ear) == 1);

	const Wav wav = readWav(directory.recordings() / "rec.wav");
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	REQUIRE(wav.info.channels == 2);
	const RampCheck ramp = checkRamp(wav, lostSyncUs(ear));
	INFO(ramp);
	CHECK(ramp.rampFrames >= rampFramesAtLeast);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
	// Every skip is paid by a record of ear's of the cycles lost when it was due (docs/testing.md#the-counters-own-word).
	const auto unpaid = unpaidRampSkips(ear, ramp);
	INFO(lostCycles(ear) << "; unpaid: " << json(unpaid).dump());
	CHECK(unpaid.empty());
}

TEST_CASE("A restarted ear keeps its identity and its pairing, at 0600, and is asked for no code",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	std::string id;
	{
		auto ear = runEar(directory);
		REQUIRE(connectedUnpaired(ear));
		id = waitForClientId(ear).value_or("");
		pairOnce(ear, server);
		ear.signal(SIGTERM);
		REQUIRE(waitForStop(ear) == 0);
	}
	const auto keyFile = directory.config() / "sendspin-ma.key";
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600));
	CHECK(readText(keyFile).find("\"createdBy\"") != std::string::npos);

	auto ear = runEar(directory);
	const bool resumed = waitForConnected(ear, true, 0, playDeadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(resumed);
	REQUIRE_FALSE(id.empty());
	CHECK(waitForClientId(ear) == id);
	CHECK(countPairingCodes(ear) == 0);
	CHECK(countEvents(ear, "paired") == 0); // paired before, by its key file
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600));
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A server that trusts without pairing gets no stream and no source role", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	REQUIRE(connectedUnpaired(ear));

	server.send("trust");
	REQUIRE(server.waitForEvent("trusted", deadline));
	server.send("roles");
	const auto roles = server.waitForEvent("roles ", deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(roles);
	CHECK(*roles == "roles none");
	CHECK_FALSE(anyStatus(ear, "stream"));
	CHECK_FALSE(anyStatus(ear, "sending"));
	CHECK(countEvents(ear, "paired") == 0);
	CHECK_FALSE(anyStatus(ear, "paired"));
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("20 wrong rounds hold pairing back: an update with the same settings leaves it held, a "
          "new name releases it",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && connectedUnpaired(ear);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	Rpc rpc(controlPort(ear));
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	server.send("pair");
	const auto code = waitForPairingCode(ear);
	REQUIRE(code);
	for (int round = 0; round < 20; ++round) server.send("code " + wrongCode(digitsOf(*code)));
	const auto failed = waitForEvent(ear, "pairing-failed", 0, playDeadline);
	REQUIRE(failed);
	CHECK((*failed)["args"]["reason"] == "pairing-code-mismatch");
	CHECK(failed->value("severity", "") == "warn");
	CHECK(failed->value("msg", "") == "node ma pairing failed: pairing-code-mismatch");
	const auto held = waitForStatus(ear, "pairingHeld", true);
	REQUIRE(held);
	CHECK(held->value("msg", "") == "node ma pairingHeld false -> true");
	REQUIRE(server.waitForEvent("pair-failed ", deadline));

	// The same settings change nothing, so the hold stands: the next attempt is held back, and shows no code.
	const json same = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}}}});
	INFO(same.dump());
	REQUIRE(same.contains("result"));
	server.send("pair");
	REQUIRE(server.waitForEvent("pair-pending", deadline));
	// The attempt held back shows no code, and pairingHeld still says why.
	CHECK(countPairingCodes(ear) == 1);
	CHECK(lastStatus(ear, "pairingHeld") == json(true));

	// A new name is a new client, in place on the same key store, and the round limit lives in memory: the hold is
	// released, and the client id is kept, so its field writes nothing new.
	const std::size_t connectsBefore = countStatus(ear, "connection", "connected");
	const json renamed =
		rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}, {"name", "Other"}}}});
	INFO(renamed.dump());
	REQUIRE(renamed.contains("result"));
	REQUIRE(waitForStatus(ear, "pairingHeld", false));
	REQUIRE(waitForStatus(ear, "connection", "connected", connectsBefore, playDeadline));
	CHECK(countRecords(ear, [](const json& r) { return isStatus(r, "clientId"); }) == 1);
	CHECK(countRecords(ear, [](const json& r) { return r.value("reset", false); }) == 0);

	server.send("pair");
	const auto fresh = waitForPairingCode(ear, 1);
	REQUIRE(fresh);
	server.send("code " + digitsOf(*fresh));
	REQUIRE(waitForPaired(ear));

	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A key file changed by something else is left as it is: the pairing's write fails, told as an event, and "
          "the pairing holds for this run",
          "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, connectedUnpaired(ear));
	// The identity is written before the client connects; a hand edit since makes the store refuse the next write
	// (docs/node-types.md#the-identity-file).
	const auto keyFile = directory.config() / "sendspin-ma.key";
	const std::string edited = readText(keyFile) + "\n";
	std::ofstream(keyFile, std::ios::trunc | std::ios::binary) << edited;

	pairOnce(ear, server);
	const auto failed = waitForEvent(ear, "key-write-failed");
	REQUIRE_SEEN(ear, failed);
	CHECK(failed->value("severity", "") == "error");
	const std::string detail = (*failed)["args"]["detail"].get<std::string>();
	CHECK(detail == "sendspin-ma.key: changed by something else");
	CHECK(failed->value("msg", "") ==
	      "node ma its key file was not written, so the pairing holds for this run only: " + detail);
	CHECK(readText(keyFile) == edited); // theirs stands
	CHECK(lastStatus(ear, "paired") == json(true));
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
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
	const auto failed = ear.waitForLineStartingWith("node ma failed ", deadline);
	const bool others = waitForLines(ear, {"node rec running", "route noise:noise.mono -> rec:record.left linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	// The key file is the node's store, opened and judged before anything of the node runs, so the node fails before it
	// was ever running (docs/node-types.md#the-identity-file).
	CHECK(*failed == "node ma failed start-failed: sendspin-ma.key: malformed: not JSON");
	CHECK_FALSE(ear.hasLine("node ma running"));
	REQUIRE(others);
	CHECK(readText(keyFile) == corrupt);
	CHECK(modeOf(keyFile) == std::filesystem::perms(0600)); // a refused file may still hold a private key
	CHECK_FALSE(anyStatus(ear, "clientId"));

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
	CHECK(waitForStop(ear) == 0);
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
		const auto connected = server.waitForEvent("connected ", deadline);
		REQUIRE(connected);
		REQUIRE_SEEN(ear, connectedUnpaired(ear, deadline, node));
		server.send("pair");
		const auto code = waitForPairingCode(ear, 0, deadline, node);
		REQUIRE(code);
		server.send("code " + digitsOf(*code));
		REQUIRE_SEEN(ear, waitForPaired(ear, 0, deadline, node));
		return connected->substr(std::string("connected ").size());
	};
	const std::string outId = pair("out");
	server.send("dial ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
	const std::string inId = pair("in");
	CHECK(outId != inId);
	CHECK(waitForClientId(ear, 0, deadline, "out") == outId);
	CHECK(waitForClientId(ear, 0, deadline, "in") == inId);
	CHECK(instance == "ear-in-" + inId.substr(0, 8));
	CHECK(std::filesystem::exists(directory.config() / "sendspin-out.key"));
	CHECK(std::filesystem::exists(directory.config() / "sendspin-in.key"));

	server.send(playLong); // the node paired last
	server.send("use " + outId);
	server.send(playLong);
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "in.wav"));
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "out.wav"));
	REQUIRE_SEEN(ear, endStream(ear, server, "out"));
	server.send("use " + inId);
	REQUIRE_SEEN(ear, endStream(ear, server, "in"));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, waitForStop(ear) == 0);
	for (const char* file : {"out.wav", "in.wav"}) {
		const RampCheck ramp = checkRamp(readWav(directory.recordings() / file), lostSyncUs(ear));
		INFO(file << ": " << ramp);
		CHECK(ramp.rampFrames >= rampFramesAtLeast);
		CHECK(ramp.badFrames == 0);
		CHECK(ramp.badSteps == 0);
		// Every skip is paid by a record of ear's of the cycles lost when it was due
		// (docs/testing.md#the-counters-own-word).
		const auto unpaid = unpaidRampSkips(ear, ramp, std::string(file) == "out.wav" ? "recOut" : "recIn");
		INFO(lostCycles(ear) << "; unpaid: " << json(unpaid).dump());
		CHECK(unpaid.empty());
	}
}
