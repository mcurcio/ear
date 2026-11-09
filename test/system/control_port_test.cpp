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
#include <fstream>
#include <iterator>
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

/// ear on noise -> rec:record.left plus `more` (graph file lines), serving a web root of its own; ready once
/// `readyLine` is printed (by default, noise -> rec:record.left linked).
struct Running {
	TempDirectory directory;
	Ear ear;
	explicit Running(const std::string& more = "",
	                 const std::string& readyLine = linkedLine("noise:noise.mono", "rec:record.left"))
		: ear(start(directory, more, readyLine)) {}

private:
	static Ear start(const TempDirectory& directory, const std::string& more, const std::string& readyLine) {
		writeGraphFile(directory, noiseToRec("record.left", more));
		return Ear(directory, {readyLine}, {"--web-root", writeWebRoot(directory).string()});
	}
};

std::string readFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// What channelRms measured: the RMS and how many frames it is over.
struct Rms {
	double value = 0.0;
	std::size_t frames = 0;
};

/// RMS of channel `channel` of a stereo WAV over its nonzero span, past its first 5 ms; 0 over no frames if it is all
/// zeros. A node's drive fades its outputs in over 5 ms (240 frames at 48 kHz; docs/design/node-lifecycle.md 5.3), and a
/// route linked just after the node runs hears that ramp, which would weigh on the level.
Rms channelRms(const std::filesystem::path& path, int channel) {
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
	if (first == frames) return {};
	constexpr std::size_t fadeFrames = 48000 * 5 / 1000;
	if (last - first > 2 * fadeFrames) first += fadeFrames;
	double sumOfSquares = 0.0;
	for (std::size_t frame = first; frame <= last; ++frame) {
		sumOfSquares += static_cast<double>(samples[frame * 2 + channel]) * samples[frame * 2 + channel];
	}
	const std::size_t counted = last - first + 1;
	return {std::sqrt(sumOfSquares / static_cast<double>(counted)), counted};
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
	CHECK(graph["nodes"][0]["outputs"] == json{"noise.mono"});
	CHECK(graph["nodes"][0]["state"] == "running");
	CHECK(graph["nodes"][1]["name"] == "rec");
	CHECK(graph["nodes"][1]["type"] == "file");
	CHECK(graph["nodes"][1]["settings"]["path"] == "rec.wav");
	CHECK(graph["nodes"][1]["inputs"] == json{"record.left", "record.right"});
	CHECK(graph["nodes"][1]["state"] == "running");
	REQUIRE(graph["routes"].size() == 1);
	CHECK(graph["routes"][0]["from"] == route("noise", "noise.mono", "rec", "record.left")["from"]);
	CHECK(graph["routes"][0]["to"] == route("noise", "noise.mono", "rec", "record.left")["to"]);
	CHECK(graph["routes"][0]["status"] == "linked");

	CHECK(rpc.call("node.add", {{"name", "n2"}, {"type", "white"}})["result"] == json::object());
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "n2"}, {"state", "running"}}); });
	const json added = route("n2", "noise.mono", "rec", "record.right");
	CHECK(rpc.call("route.add", added)["result"] == json::object());
	rpc.waitFor([&](const json& m) {
		return Rpc::matches(m, "route.status", {{"from", added["from"]}, {"to", added["to"]}, {"status", "linked"}});
	});

	INFO("notifications: " << json(rpc.notifications).dump());
	const std::size_t nodeAdded = rpc.indexOf("node.added", {{"name", "n2"}});
	const std::size_t starting = rpc.indexOf("node.state", {{"name", "n2"}, {"state", "starting"}});
	const std::size_t running = rpc.indexOf("node.state", {{"name", "n2"}, {"state", "running"}});
	const std::size_t routeAdded = rpc.indexOf("route.added", added);
	const std::size_t linked =
		rpc.indexOf("route.status", {{"from", added["from"]}, {"to", added["to"]}, {"status", "linked"}});
	CHECK(nodeAdded < starting);
	CHECK(starting < running);
	CHECK(running < routeAdded);
	CHECK(routeAdded < linked);
	CHECK(linked < rpc.notifications.size());

	// Noise has reached rec:record.right once a level window of it is measured there; then half a second of it is
	// recorded before ear stops, so the level below is measured over enough noise to be sharp. Levels are a latest-value
	// snapshot (a window can come in several messages, and windows can be passed over), so new windows are told apart by
	// their values, and 15 of them mean at least 15 windows went by.
	constexpr int recordedWindows = 15;
	constexpr std::size_t windowFrames = 48000 / 30; // the graph file's rate, 30 windows a second
	CHECK(rpc.call("levels.subscribe").contains("result"));
	std::optional<json> last; // record.right's level in the newest window read, once noise is heard there
	int newWindows = 0;
	rpc.waitFor([&](const json& m) {
		if (m.value("method", "") != "levels" || !m["params"]["nodes"].contains("rec")) return false;
		const json& now = m["params"]["nodes"]["rec"]["record.right"];
		if (!last) {
			if (now[1].get<double>() > 0.5) last = now;
			return false;
		}
		if (now == *last) return false; // the same window again
		last = now;
		return ++newWindows == recordedWindows;
	});

	CHECK(ear.stop(rpc) == 0);
	// Uniform noise in [-1, 1) has a mean square of 1/3 and var(x^2) = 4/45, so the RMS of N samples of it is 0.577 with a
	// standard deviation of sqrt(4/45) / (2 sqrt(1/3)) / sqrt(N) = 0.258 / sqrt(N). Over the 14 whole windows recorded at
	// least (22400 frames) that is 0.0017, and the margin is 5 of them for the frames actually measured: a gain off by 2%,
	// silence or the wrong channel still fails.
	const Rms right = channelRms(directory.recordings() / "rec.wav", 1);
	INFO("measured over " << right.frames << " frames");
	REQUIRE(right.frames >= (recordedWindows - 1) * windowFrames);
	const double sigma =
		std::sqrt(4.0 / 45.0) / (2.0 * std::sqrt(1.0 / 3.0)) / std::sqrt(static_cast<double>(right.frames));
	CHECK(right.value == Approx(std::sqrt(1.0 / 3.0)).margin(5.0 * sigma));
}

TEST_CASE("levels.subscribe sends the noise's level", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	Rpc rpc(ear.port());

	const json reply = rpc.call("levels.subscribe");
	CHECK(reply["result"]["windowsPerSecond"] == 30);
	// A whole window of the noise. Levels are a latest-value snapshot, sent when any block publishes, so one window can
	// come in several messages (the noise's first again once the recorder's block has published too). And the noise's
	// first windows are not all noise: its block processes silence until the asynchronous drive lands, as late as the
	// main loop gets to it, then fades the noise in over 5 ms (docs/design/node-lifecycle.md 5.3). So the first window
	// with any noise holds the drive, the next new one may hold the fade's tail (240 frames of a 1600-frame window, when
	// the drive lands near a window's end), and the new one after that is noise throughout.
	std::optional<json> last; // the noise's level in the newest window read, once it is heard
	int newWindows = 0;       // windows after that one
	const json levels = rpc.waitFor([&](const json& m) {
		if (m.value("method", "") != "levels" || !m["params"]["nodes"].contains("noise")) return false;
		const json& now = m["params"]["nodes"]["noise"]["noise.mono"];
		if (!last) {
			if (now[0].get<double>() > 0.0) last = now;
			return false;
		}
		if (now == *last) return false; // the same window again
		last = now;
		return ++newWindows == 2;
	});
	INFO(levels.dump());
	const json& level = levels["params"]["nodes"]["noise"]["noise.mono"];
	// One window is 1600 frames, so its RMS of uniform noise is 0.577 with a standard deviation of 0.258 / sqrt(1600) =
	// 0.0065 (see the test above): the margin is 7.7 of them, and a window that is partly silent or faded, which this test
	// read before it waited for a whole one (0.47 to 0.52), still fails.
	CHECK(level[1].get<double>() == Approx(0.577).margin(0.05));
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("Removing a node tells its routes first, and a wrong port is an unknown-port error", "[system][control]") {
	Running started;
	Ear& ear = started.ear;
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	const json wrong = rpc.call("route.add", route("noise", "noise.mono", "rec", "FX"));
	INFO(wrong.dump());
	CHECK(wrong["error"]["code"] == -32000);
	CHECK(wrong["error"]["data"]["code"] == "unknown-port");

	CHECK(rpc.call("node.remove", {{"name", "noise"}})["result"] == json::object());
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.gone", {{"name", "noise"}}); });
	INFO("notifications: " << json(rpc.notifications).dump());
	const json removed = route("noise", "noise.mono", "rec", "record.left");
	const std::size_t routeRemoved = rpc.indexOf("route.removed", removed);
	const std::size_t nodeRemoved = rpc.indexOf("node.removed", {{"name", "noise"}});
	const std::size_t stopping = rpc.indexOf("node.state", {{"name", "noise"}, {"state", "stopping"}});
	const std::size_t stopped = rpc.indexOf("node.stopped", {{"name", "noise"}});
	const std::size_t gone = rpc.indexOf("node.gone", {{"name", "noise"}, {"droppedFrames", 0}});
	CHECK(routeRemoved < nodeRemoved);
	CHECK(nodeRemoved < stopping);
	CHECK(stopping < stopped);
	CHECK(stopped < gone); // node.stopped (deprecated) first, then node.gone (control-plane.md 4.8)
	CHECK(gone < rpc.notifications.size());
	CHECK(ear.stop(rpc) == 0);
}

TEST_CASE("node.stop and node.start through the control port edit nothing, and write an action record before the "
          "node records they cause",
          "[system][control][lifecycle]") {
	Running started;
	Ear& ear = started.ear;
	const TempDirectory& directory = started.directory;
	const std::string before = readFile(directory.graphFile());
	Rpc rpc(ear.port());
	const json snapshot = rpc.call("graph.subscribe");
	REQUIRE(snapshot.contains("result"));
	CHECK(snapshot["result"]["protocol"] == 1);
	CHECK(snapshot["result"]["features"] == json::array({"lifecycle"}));
	CHECK(snapshot["result"]["nodes"][0]["phase"] == "running");
	CHECK(snapshot["result"]["nodes"][0]["wanted"] == "running");

	const json stop = rpc.call("node.stop", {{"name", "noise"}});
	INFO(stop.dump());
	CHECK(stop["result"] == json{{"applied", "stop"}});
	const json stopped = rpc.waitFor(
		[](const json& m) { return Rpc::matches(m, "node.state", {{"name", "noise"}, {"phase", "stopped"}}); });
	CHECK(stopped["params"]["wanted"] == "stopped");
	CHECK(stopped["params"].contains("since"));
	CHECK(rpc.call("node.start", {{"name", "noise"}})["result"] == json{{"applied", "start"}});
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "noise"}, {"state", "running"}}); });
	const json ghost = rpc.call("node.restart", {{"name", "ghost"}});
	CHECK(ghost["error"]["data"]["code"] == "unknown-node");

	REQUIRE(ear.process().waitForLine("node noise running", deadline));
	CHECK(ear.stop(rpc) == 0);
	CHECK(readFile(directory.graphFile()) == before); // a stop and a start are never written
	const std::string& out = ear.process().output();
	const std::size_t invoked = out.find("node noise stop invoked\n");
	REQUIRE(invoked != std::string::npos);
	CHECK(invoked < out.find("node noise stopping: stop\n"));
	CHECK(out.find("node noise start invoked\n") != std::string::npos);
	CHECK(out.find("node ghost restart refused: unknown-node\n") != std::string::npos);
}

TEST_CASE("A file path outside the recordings folder is refused as invalid-settings and nothing is "
          "written",
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
	writeGraphFile(directory,
	               noiseToRec("record.left",
	                          "node card out { card \"Test\"; playback 2; }\nroute noise:noise.mono out:play.1;\n"));
	const json toCard = route("noise", "noise.mono", "out", "play.1");
	Ear ear(directory, {linkedLine("noise:noise.mono", "out:play.1")});
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
