// Ten sendspin nodes' status reaching a client that reads slowly (docs/testing.md#the-client-queue-under-status): the
// real ear, its nodes restarted together again and again so their status resets and is written anew in bursts, and a
// subscribed client with a small window reading one message every 20 ms. It is never closed, and what it folds is what
// graph.get says. (Shared helpers: sendspin_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "sendspin_system.hpp"
#include "sleeps.hpp"

namespace {

using namespace ear::test;

} // namespace

TEST_CASE("Ten sendspin nodes restarted together reach a slowly reading client whole, its fold graph.get's status",
          "[system][sendspin][status]") {
	constexpr int nodes = 10;
	constexpr int rounds = 5;
	std::vector<std::uint16_t> ports;
	std::string text;
	while (ports.size() < nodes) {
		const std::uint16_t port = freeTcpPort();
		if (std::ranges::find(ports, port) != ports.end()) continue;
		text += "node sendspin s" + std::to_string(ports.size()) + " { port " + std::to_string(port) + "; }\n";
		ports.push_back(port);
	}
	TempDirectory directory;
	writeGraphFile(directory, graphFileText(text, graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::uint16_t control = controlPort(ear);

	// The slow client, subscribed before any node has written: its snapshot holds every field at its initial value.
	WsClient slow(control, WsOptions().receiveBuffer(4096));
	slow.send(R"({"jsonrpc":"2.0","id":1,"method":"graph.subscribe"})");
	const json snapshot = json::parse(slow.read());
	REQUIRE(snapshot.contains("result"));
	json folded = json::object();
	for (const json& node : snapshot["result"]["nodes"]) folded[node["name"].get<std::string>()] = node["status"];

	// It reads on its own thread, one message every 20 ms, from the start until the marker below, a node added once every
	// node has written its last; then on, unpaced, until ear closes it, since ear's stop waits for each client's close.
	int statusMessages = 0;
	std::optional<std::uint16_t> closedWith;
	std::promise<void> marked;
	std::thread reader([&] {
		bool past = false;
		try {
			for (;;) {
				const json message = json::parse(slow.read());
				if (past) continue;
				const std::string method = message.value("method", "");
				if (method == "node.added" && message["params"]["name"] == "marker") {
					past = true;
					marked.set_value();
					continue;
				}
				if (method == "node.statusChanged") {
					++statusMessages;
					const json& params = message["params"];
					json& status = folded[params["name"].get<std::string>()];
					if (params.value("reset", false)) status = params["fields"];
					else status.update(params["fields"]);
				}
				ear::sleeps::pace(std::chrono::milliseconds(20), "a client reading slowly");
			}
		} catch (const Closed& closed) {
			closedWith = closed.code;
		}
		if (!past) marked.set_value();
	});

	Rpc rpc(control);
	for (int round = 0; round < rounds; ++round) {
		for (int node = 0; node < nodes; ++node) waitForInstance(ear, "s" + std::to_string(node), ports[node], round);
		if (round + 1 == rounds) break;
		for (int node = 0; node < nodes; ++node)
			REQUIRE(rpc.call("node.restart", {{"name", "s" + std::to_string(node)}}).contains("result"));
	}
	REQUIRE(rpc.call("node.add", {{"name", "marker"}, {"type", "white"}}).contains("result"));
	marked.get_future().wait();
	CHECK_FALSE(closedWith.has_value());
	INFO(statusMessages << " status messages read");
	const json graph = rpc.call("graph.get")["result"];
	for (const json& node : graph["nodes"]) {
		const std::string name = node["name"];
		if (name == "marker") continue;
		INFO(name);
		CHECK(folded[name] == node["status"]);
		CHECK(node["status"]["connection"] == "listening");
	}
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	reader.join();
	CHECK(closedWith == 1001);
	CHECK(waitForStop(ear) == 0);
}
