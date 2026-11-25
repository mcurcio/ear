// The sendspin node's changes in place (docs/node-types.md#changes-in-place): a port, a name and a server changed while
// it runs, listening to dialling and back, each `reconfiguring` and never `stopping`, on the same client id and pairing,
// the key file not read again, the connection dropped and back; and a change to a port another node holds restarting
// and failing as a start on it would. (Shared helpers: sendspin_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// node.update of "ma" to `settings`: answered applied in place, `reconfiguring` with `keys`, then `running`, and no
/// `stopping` among the notifications since.
void changeInPlace(Rpc& rpc, const json& settings, const json& keys) {
	const std::size_t from = rpc.notifications.size();
	const json updated = rpc.call("node.update", {{"name", "ma"}, {"settings", settings}});
	INFO(updated.dump());
	REQUIRE(updated.contains("result"));
	const json changing = rpc.waitFor(
		[](const json& m) { return Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "reconfiguring"}}); },
		from);
	CHECK(changing["params"]["keys"] == keys);
	CHECK(rpc.indexOf("node.updated", {{"name", "ma"}, {"applied", "reconfigure"}}, from) < rpc.notifications.size());
	const json settled = rpc.waitFor(
		[](const json& m) {
			return Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "running"}}) ||
		           Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "failed"}});
		},
		from);
	INFO("notifications: " << json(rpc.notifications).dump());
	REQUIRE(settled["params"]["state"] == "running");
	CHECK(rpc.indexOf("node.state", {{"name", "ma"}, {"state", "stopping"}}, from) == rpc.notifications.size());
}

std::string url(std::uint16_t port) { return "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"; }

} // namespace

TEST_CASE("A sendspin node's port, name and server change in place, listening to dialling and back, on the same "
          "client id and pairing, the key file not read again",
          "[system][sendspin][reconfigure]") {
	const std::uint16_t first = freeTcpPort();
	std::uint16_t second = freeTcpPort();
	while (second == first) second = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(first));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, waitForLines(ear, runningAndLinked));
	waitForInstance(ear, "ma", first);
	const std::string id = *ear.lineStartingWith("node ma says id ");
	SendspinTestServer server({"--dial", url(first)});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	Rpc rpc(controlPort(ear));
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	// The key file is read when a configuration is made, and never by a change: one that is no key record now would fail
	// any restart (docs/node-types.md#the-identity-file), so the node running on through every change below, paired,
	// says no change read it.
	const auto keyFile = directory.config() / "sendspin-ma.key";
	std::ofstream(keyFile, std::ios::trunc) << "{ no key record";

	// A port and a name: the old port let go, the new client listening on the new one, found again and paired with no
	// code.
	changeInPlace(rpc, {{"port", second}, {"name", "Kitchen"}}, json::array({"name", "port"}));
	CHECK(ear.waitForLine("node ma reconfiguring: name, port", deadline));
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says disconnected", deadline));
	CHECK(waitForInstance(ear, "ma", second).starts_with("ear-ma-"));
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says id ", 1, deadline));
	CHECK(*waitForNthLine(ear, "node ma says id ", 1, deadline) == id);
	REQUIRE(server.waitForEvent("disconnected ", deadline));
	CHECK(connectRefused(first));
	server.send("dial " + url(second));
	REQUIRE_SEEN(ear, waitForNthLine(ear, std::string(connectedPaired), 1, playDeadline));

	// Listening to dialling: the listening port let go, the same client id dialling the server, paired.
	changeInPlace(rpc, {{"server", server.url()}, {"name", "Kitchen"}}, json::array({"port", "server"}));
	REQUIRE_SEEN(ear, ear.waitForLine("node ma says dialling " + server.url(), deadline));
	REQUIRE_SEEN(ear, waitForNthLine(ear, std::string(connectedPaired), 2, playDeadline));
	CHECK(*waitForNthLine(ear, "node ma says id ", 2, deadline) == id);
	CHECK(connectRefused(second));

	// Dialling to listening, back on the first port.
	changeInPlace(rpc, {{"port", first}}, json::array({"name", "port", "server"}));
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says listening " + std::to_string(first) + " as ", 1, deadline));
	CHECK(*waitForNthLine(ear, "node ma says id ", 3, deadline) == id);
	server.send("dial " + url(first));
	REQUIRE_SEEN(ear, waitForNthLine(ear, std::string(connectedPaired), 3, playDeadline));

	CHECK_FALSE(ear.lineStartingWith("node ma stopping"));
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	CHECK(countLines(ear, pairingCodePrefix) == 1); // paired once, at the start
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A sendspin change to a port another node holds restarts the node, which fails naming the holder",
          "[system][sendspin][reconfigure]") {
	const std::uint16_t held = freeTcpPort();
	std::uint16_t own = freeTcpPort();
	while (own == held) own = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin a { port " + std::to_string(held) +
	                                            "; }\n"
	                                            "node sendspin b { port " +
	                                            std::to_string(own) + "; }\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	REQUIRE_SEEN(ear, waitForLines(ear, {"node a running", "node b running"}));
	waitForInstance(ear, "b", own);
	Rpc rpc(controlPort(ear));
	const json update = rpc.call("node.update", {{"name", "b"}, {"settings", {{"port", held}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	REQUIRE_SEEN(ear, ear.waitForLine("node b failed start-failed: cannot listen on port " + std::to_string(held) +
	                                      ": held by node a",
	                                  deadline));
	CHECK(ear.lineStartingWith("node b stopping: restarting"));
	CHECK_FALSE(ear.lineStartingWith("node a failed"));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}
