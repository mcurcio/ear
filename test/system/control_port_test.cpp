// The control port end to end (docs/design/batch2.md section 4.7 and slice J's criteria 1-5 and
// 7; ported to ear.conf by docs/design/knobly-config.md, S5 criterion T9): the real out/ear on a
// graph file folder, its control port on 127.0.0.1 at a free port, talked to over HTTP and
// JSON-RPC on WebSocket with ear_ws_client. Every edit is made at the source in ear.conf. Every wait
// is for an event (a line ear prints, a message, ear's exit); a deadline only fails a test.
#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

json route(const std::string& fromNode, const std::string& fromPort, const std::string& toNode,
           const std::string& toPort) {
	return {{"from", {{"node", fromNode}, {"port", fromPort}}}, {"to", {{"node", toNode}, {"port", toPort}}}};
}

std::filesystem::path writeWebRoot(const TempDirectory& directory) {
	const auto root = directory.path() / "web";
	std::filesystem::create_directory(root);
	std::ofstream(root / "index.html") << "<!doctype html><title>ear system test</title>\n";
	return root;
}

/// ear on noise -> rec:FL plus `more` (graph file lines), serving a web root of its own; ready once
/// `readyLine` is printed (by default, noise -> rec:FL linked).
struct Running {
	TempDirectory directory;
	Ear ear;
	explicit Running(const std::string& more = "", const std::string& readyLine = linkedLine("noise:MONO", "rec:FL"))
	    : ear(start(directory, more, readyLine)) {}

private:
	static Ear start(const TempDirectory& directory, const std::string& more, const std::string& readyLine) {
		writeGraphFile(directory, noiseToRec("FL", more));
		return Ear(directory, {readyLine}, {"--web-root", writeWebRoot(directory).string()});
	}
};

/// RMS of channel `channel` of a stereo WAV over its nonzero span; 0 if it is all zeros.
double channelRms(const std::filesystem::path& path, int channel) {
	SF_INFO info{};
	SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
	REQUIRE(file != nullptr);
	REQUIRE(info.channels == 2);
	std::vector<float> samples(static_cast<std::size_t>(info.frames * info.channels));
	CHECK(sf_readf_float(file, samples.data(), info.frames) == info.frames);
	sf_close(file);
	const auto frames = static_cast<std::size_t>(info.frames);
	std::size_t first = frames;
	std::size_t last = 0;
	for (std::size_t frame = 0; frame < frames; ++frame) {
		if (samples[frame * 2 + channel] != 0.0F) {
			if (first == frames) first = frame;
			last = frame;
		}
	}
	if (first == frames) return 0.0;
	double sumOfSquares = 0.0;
	for (std::size_t frame = first; frame <= last; ++frame) {
		sumOfSquares += static_cast<double>(samples[frame * 2 + channel]) * samples[frame * 2 + channel];
	}
	return std::sqrt(sumOfSquares / static_cast<double>(last - first + 1));
}

} // namespace

TEST_CASE("ear prints its scheduling and control port and serves --web-root", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	INFO("ear printed:\n" << ear.process().output());

	const auto scheduling = ear.process().lineStartingWith("engine: ");
	REQUIRE(scheduling);
	const bool realtime = scheduling->starts_with("engine: realtime ");
	const bool notRealtime = scheduling->starts_with("engine: not realtime (") && scheduling->ends_with(")");
	CHECK((realtime || notRealtime));

	const auto reply = httpRequest(ear.port(), http::verb::get, "/", "127.0.0.1:" + std::to_string(ear.port()));
	CHECK(reply.status == 200);
	CHECK(reply.body == "<!doctype html><title>ear system test</title>\n");
	CHECK(ear.stop() == 0);
}

TEST_CASE("A client sees the file's graph, adds a node and a route, and they are heard", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	const TempDirectory& directory = started.directory;
	Rpc rpc(ear.port());

	const json snapshot = rpc.call("graph.subscribe");
	INFO(snapshot.dump(2));
	REQUIRE(snapshot.contains("result"));
	const json& graph = snapshot["result"];
	CHECK(graph["graphFrequency"] == 48000);
	CHECK(graph["blockSize"] == 256);
	REQUIRE(graph["nodes"].size() == 2);
	CHECK(graph["nodes"][0]["name"] == "noise");
	CHECK(graph["nodes"][0]["type"] == "white");
	CHECK(graph["nodes"][0]["outputs"] == json{"MONO"});
	CHECK(graph["nodes"][0]["state"] == "running");
	CHECK(graph["nodes"][1]["name"] == "rec");
	CHECK(graph["nodes"][1]["type"] == "file");
	CHECK(graph["nodes"][1]["settings"]["path"] == "rec.wav");
	CHECK(graph["nodes"][1]["inputs"] == json{"FL", "FR"});
	CHECK(graph["nodes"][1]["state"] == "running");
	REQUIRE(graph["routes"].size() == 1);
	CHECK(graph["routes"][0]["from"] == route("noise", "MONO", "rec", "FL")["from"]);
	CHECK(graph["routes"][0]["to"] == route("noise", "MONO", "rec", "FL")["to"]);
	CHECK(graph["routes"][0]["status"] == "linked");

	CHECK(rpc.call("node.add", {{"name", "n2"}, {"type", "white"}})["result"] == json::object());
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "n2"}, {"state", "running"}}); });
	const json added = route("n2", "MONO", "rec", "FR");
	CHECK(rpc.call("route.add", added)["result"] == json::object());
	rpc.waitFor([&](const json& m) { return Rpc::matches(m, "route.status", {{"from", added["from"]}, {"to", added["to"]}, {"status", "linked"}}); });

	INFO("notifications: " << json(rpc.notifications).dump());
	const std::size_t nodeAdded = rpc.indexOf("node.added", {{"name", "n2"}});
	const std::size_t starting = rpc.indexOf("node.state", {{"name", "n2"}, {"state", "starting"}});
	const std::size_t running = rpc.indexOf("node.state", {{"name", "n2"}, {"state", "running"}});
	const std::size_t routeAdded = rpc.indexOf("route.added", added);
	const std::size_t linked = rpc.indexOf("route.status", {{"from", added["from"]}, {"to", added["to"]}, {"status", "linked"}});
	CHECK(nodeAdded < starting);
	CHECK(starting < running);
	CHECK(running < routeAdded);
	CHECK(routeAdded < linked);
	CHECK(linked < rpc.notifications.size());

	// Noise has reached rec:FR once a level window of it is measured there.
	CHECK(rpc.call("levels.subscribe").contains("result"));
	rpc.waitFor([](const json& m) {
		return m.value("method", "") == "levels" && m["params"]["nodes"].contains("rec") &&
		       m["params"]["nodes"]["rec"]["FR"][1].get<double>() > 0.5;
	});

	CHECK(ear.stop(rpc) == 0);
	CHECK(channelRms(directory.recordings() / "rec.wav", 1) == Approx(0.577).margin(0.02));
}

TEST_CASE("levels.subscribe sends the noise's level", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	Rpc rpc(ear.port());

	const json reply = rpc.call("levels.subscribe");
	CHECK(reply["result"]["windowsPerSecond"] == 30);
	const json levels = rpc.waitFor([](const json& m) {
		return m.value("method", "") == "levels" && m["params"]["nodes"].contains("noise");
	});
	INFO(levels.dump());
	const json& level = levels["params"]["nodes"]["noise"]["MONO"];
	CHECK(level[1].get<double>() == Approx(0.577).margin(0.05));
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("Removing a node tells its routes first, and a wrong port is an unknown-port error", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	const json wrong = rpc.call("route.add", route("noise", "MONO", "rec", "FX"));
	INFO(wrong.dump());
	CHECK(wrong["error"]["code"] == -32000);
	CHECK(wrong["error"]["data"]["code"] == "unknown-port");

	CHECK(rpc.call("node.remove", {{"name", "noise"}})["result"] == json::object());
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.stopped", {{"name", "noise"}}); });
	INFO("notifications: " << json(rpc.notifications).dump());
	const json removed = route("noise", "MONO", "rec", "FL");
	const std::size_t routeRemoved = rpc.indexOf("route.removed", removed);
	const std::size_t nodeRemoved = rpc.indexOf("node.removed", {{"name", "noise"}});
	const std::size_t stopping = rpc.indexOf("node.state", {{"name", "noise"}, {"state", "stopping"}});
	const std::size_t stopped = rpc.indexOf("node.stopped", {{"name", "noise"}});
	CHECK(routeRemoved < nodeRemoved);
	CHECK(nodeRemoved < stopping);
	CHECK(stopping < stopped);
	CHECK(stopped < rpc.notifications.size());
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("A file path outside the recordings folder is refused as invalid-settings and nothing is written",
          "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	const TempDirectory& directory = started.directory;
	Rpc rpc(ear.port());
	const auto outside = directory.path() / "outside.wav";

	for (const std::string& path : {outside.string(), std::string("../outside.wav")}) {
		INFO(path);
		json add = rpc.call("node.add", {{"name", "tape"}, {"type", "file"}, {"settings", {{"path", path}}}});
		INFO(add.dump());
		CHECK(add["error"]["code"] == -32000);
		CHECK(add["error"]["data"]["code"] == "invalid-settings");
		json update = rpc.call("node.update", {{"name", "rec"}, {"settings", {{"path", path}}}});
		INFO(update.dump());
		CHECK(update["error"]["code"] == -32000);
		CHECK(update["error"]["data"]["code"] == "invalid-settings");
	}
	CHECK(ear.stop(rpc) == 0);
	CHECK_FALSE(std::filesystem::exists(outside));
	CHECK(headerFinished(directory.recordings() / "rec.wav"));
}

TEST_CASE("Edits sent while ear stops are refused as stopping and ear exits cleanly", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	const TempDirectory& directory = started.directory;
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	ear.process().signal(SIGTERM);
	// The graph is closed before any node is told to stop, so edits from here on are refused.
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "rec"}, {"state", "stopping"}}); });
	const std::vector<std::pair<std::string, json>> edits = {
	    {"node.add", {{"name", "late"}, {"type", "white"}}},
	    {"node.update", {{"name", "rec"}, {"settings", {{"path", "late.wav"}}}}},
	};
	std::optional<std::uint16_t> closeCode;
	for (const auto& [method, params] : edits) {
		try {
			const json reply = rpc.call(method, params);
			INFO(method << ": " << reply.dump());
			CHECK(reply["error"]["code"] == -32000);
			CHECK(reply["error"]["data"]["code"] == "stopping");
		} catch (const Closed& closed) {
			closeCode = closed.code;
			break;
		}
	}
	// ear waits for each client's close reply before it exits, so keep reading until it closes.
	if (!closeCode) closeCode = rpc.readUntilClosed();
	CHECK(*closeCode == 1001);

	const auto exitStatus = ear.process().waitForExit(deadline);
	INFO("ear printed:\n" << ear.process().output());
	CHECK(exitStatus == 0);
	CHECK(ear.process().output().find("terminate") == std::string::npos);
	CHECK(headerFinished(directory.recordings() / "rec.wav"));
}

TEST_CASE("A sound card node runs and its route is linked", "[system][control][card]") {
	TempDirectory directory;
	// The test ALSA configuration (slice E2): card "Test" is a file PCM writing into EAR_ALSA_OUT.
	const auto alsa = std::filesystem::path(__FILE__).parent_path() / "../../src/pipewire/tests/alsa/alsa.conf";
	REQUIRE(std::filesystem::exists(alsa));
	::setenv("ALSA_CONFIG_PATH", alsa.c_str(), 1);
	::setenv("EAR_ALSA_OUT", directory.path().c_str(), 1);
	writeGraphFile(directory, noiseToRec("FL", "node card out { card \"Test\"; playback 2; }\nroute noise:MONO out:playback_1;\n"));
	const json toCard = route("noise", "MONO", "out", "playback_1");
	Ear ear(directory, {linkedLine("noise:MONO", "out:playback_1")});
	INFO("ear printed:\n" << ear.process().output());
	CHECK(ear.process().waitForLine("node out running", deadline));

	Rpc rpc(ear.port());
	const json graph = rpc.call("graph.get")["result"];
	INFO(graph.dump());
	bool cardRunning = false;
	for (const auto& node : graph["nodes"]) {
		if (node["name"] == "out") cardRunning = node["state"] == "running";
	}
	CHECK(cardRunning);
	bool cardLinked = false;
	for (const auto& r : graph["routes"]) {
		if (r["to"] == toCard["to"]) cardLinked = r["status"] == "linked";
	}
	CHECK(cardLinked);
	CHECK(ear.stop(rpc) == 0);
}
