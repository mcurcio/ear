// The control port end to end (docs/design/batch2.md section 4.7 and slice J's criteria 1-5 and
// 7): the real out/ear on a graph file, its control port on 127.0.0.1 at a free port, talked to
// over HTTP and JSON-RPC on WebSocket with ear_ws_client. Every wait is for an event (a line ear
// prints, a message, ear's exit); a deadline only fails a test.
#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch2/catch.hpp"
#include "child_process.hpp"
#include "ws_client.hpp"

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t blockSize = 256;
/// Fails a test whose awaited event never comes; never paces one.
constexpr auto deadline = 10s;
constexpr std::string_view listeningPrefix = "control listening on 127.0.0.1:";

/// A fresh directory for one test's files, removed afterwards.
class TempDirectory {
public:
	TempDirectory() {
		std::string pattern = (std::filesystem::temp_directory_path() / "ear-control-XXXXXX").string();
		if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
		path_ = pattern;
	}
	~TempDirectory() { std::filesystem::remove_all(path_); }
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

json route(const std::string& fromNode, const std::string& fromPort, const std::string& toNode,
           const std::string& toPort) {
	return {{"from", {{"node", fromNode}, {"port", fromPort}}}, {"to", {{"node", toNode}, {"port", toPort}}}};
}

/// white "noise" routed to file "rec":FL, plus `extraNodes` and `extraRoutes`.
std::filesystem::path writeGraph(const TempDirectory& directory, json extraNodes = json::array(),
                                 json extraRoutes = json::array()) {
	json nodes = {{{"name", "noise"}, {"type", "white"}},
	              {{"name", "rec"}, {"type", "file"}, {"settings", {{"path", (directory.path() / "rec.wav").string()}}}}};
	json routes = {route("noise", "MONO", "rec", "FL")};
	for (auto& node : extraNodes) nodes.push_back(node);
	for (auto& r : extraRoutes) routes.push_back(r);
	const json graph = {{"graphFrequency", graphFrequency}, {"blockSize", blockSize}, {"nodes", nodes}, {"routes", routes}};
	const auto path = directory.path() / "ear.json";
	std::ofstream(path) << graph.dump(2);
	return path;
}

/// The real out/ear on `graph`, listening on 127.0.0.1 at a free port, serving `webRoot`. Ready
/// once the last route in the file is linked (the port is open before the file is replayed).
class Ear {
public:
	Ear(const std::filesystem::path& graph, const std::filesystem::path& webRoot, const std::string& readyLine)
	    : process_(EAR_BINARY, {"--config", graph.string(), "--control-address", "127.0.0.1", "--control-port", "0",
	                            "--web-root", webRoot.string()}) {
		const bool ready = process_.waitForLine(readyLine, deadline);
		INFO("ear printed:\n" << process_.output());
		REQUIRE(ready);
		const auto line = lineStartingWith(listeningPrefix);
		REQUIRE(line);
		port_ = static_cast<std::uint16_t>(std::stoul(line->substr(listeningPrefix.size())));
	}

	std::optional<std::string> lineStartingWith(std::string_view prefix) const {
		const std::string& output = process_.output();
		std::size_t start = 0;
		for (std::size_t end; (end = output.find('\n', start)) != std::string::npos; start = end + 1) {
			const std::string line = output.substr(start, end - start);
			if (line.starts_with(prefix)) return line;
		}
		return std::nullopt;
	}

	ear::test::ChildProcess& process() noexcept { return process_; }
	std::uint16_t port() const noexcept { return port_; }

	/// SIGTERM, then ear's exit status.
	std::optional<int> stop() {
		process_.signal(SIGTERM);
		return process_.waitForExit(deadline);
	}

	/// SIGTERM with `client` connected: ear finishes the close handshake with each client before
	/// it exits, so the client reads until ear closes it (with 1001), then ear's exit status.
	template <typename Client>
	std::optional<int> stop(Client& client) {
		process_.signal(SIGTERM);
		CHECK(client.readUntilClosed() == 1001);
		return process_.waitForExit(deadline);
	}

private:
	ear::test::ChildProcess process_;
	std::uint16_t port_ = 0;
};

/// A JSON-RPC client on one WebSocket: calls wait for their own response and keep every
/// notification that arrived meanwhile, in order.
class Rpc {
public:
	explicit Rpc(std::uint16_t port) : ws_(port) {}

	/// Sends one request and returns its response.
	json call(const std::string& method, const json& params = nullptr) {
		const int id = nextId_++;
		json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
		if (!params.is_null()) request["params"] = params;
		ws_.send(request.dump());
		while (true) {
			json message = json::parse(ws_.read());
			if (message.contains("id") && message["id"] == id) return message;
			notifications.push_back(std::move(message));
		}
	}

	/// Reads until a notification matching `wanted` arrives (or one already did) and returns it.
	json waitFor(const std::function<bool(const json&)>& wanted) {
		for (std::size_t i = 0; i < notifications.size(); ++i) {
			if (wanted(notifications[i])) return notifications[i];
		}
		try {
			while (true) {
				json message = json::parse(ws_.read());
				notifications.push_back(message);
				if (wanted(message)) return message;
			}
		} catch (const ear::test::TimedOut&) {
			FAIL("the awaited notification never came; notifications: " << json(notifications).dump());
			throw;
		}
	}

	/// Reads until the server closes the WebSocket (answering its close, as a browser does) and
	/// returns the close code.
	std::uint16_t readUntilClosed() {
		try {
			while (true) notifications.push_back(json::parse(ws_.read()));
		} catch (const ear::test::Closed& closed) {
			return closed.code;
		}
	}

	/// Index in `notifications` of the first one after `from` with `method` and, in params,
	/// every key of `has`; notifications.size() if none.
	std::size_t indexOf(const std::string& method, const json& has, std::size_t from = 0) const {
		for (std::size_t i = from; i < notifications.size(); ++i) {
			if (matches(notifications[i], method, has)) return i;
		}
		return notifications.size();
	}

	static bool matches(const json& message, const std::string& method, const json& has) {
		if (message.value("method", "") != method) return false;
		const json& params = message["params"];
		for (const auto& [key, value] : has.items()) {
			if (!params.contains(key) || params[key] != value) return false;
		}
		return true;
	}

	std::vector<json> notifications;

private:
	ear::test::WsClient ws_;
	int nextId_ = 1;
};

std::filesystem::path writeWebRoot(const TempDirectory& directory) {
	const auto root = directory.path() / "web";
	std::filesystem::create_directory(root);
	std::ofstream(root / "index.html") << "<!doctype html><title>ear system test</title>\n";
	return root;
}

std::string linkedLine(const json& r) {
	return "route " + r["from"]["node"].get<std::string>() + ":" + r["from"]["port"].get<std::string>() + " -> " +
	       r["to"]["node"].get<std::string>() + ":" + r["to"]["port"].get<std::string>() + " linked";
}

/// The RIFF header's size field matches the file: the writer closed the file properly.
bool headerFinished(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	if (bytes.size() < 8) return false;
	const std::uint32_t riffSize =
	    bytes[4] | (bytes[5] << 8U) | (bytes[6] << 16U) | (static_cast<std::uint32_t>(bytes[7]) << 24U);
	return riffSize == bytes.size() - 8;
}

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
	TempDirectory directory;
	const auto webRoot = writeWebRoot(directory);
	Ear ear(writeGraph(directory), webRoot, linkedLine(route("noise", "MONO", "rec", "FL")));
	INFO("ear printed:\n" << ear.process().output());

	const auto scheduling = ear.lineStartingWith("engine: ");
	REQUIRE(scheduling);
	const bool realtime = scheduling->starts_with("engine: realtime ");
	const bool notRealtime = scheduling->starts_with("engine: not realtime (") && scheduling->ends_with(")");
	CHECK((realtime || notRealtime));

	const auto reply = ear::test::httpRequest(ear.port(), ear::test::http::verb::get, "/",
	                                          "127.0.0.1:" + std::to_string(ear.port()));
	CHECK(reply.status == 200);
	CHECK(reply.body == "<!doctype html><title>ear system test</title>\n");
	CHECK(ear.stop() == 0);
}

TEST_CASE("A client sees the file's graph, adds a node and a route, and they are heard", "[system][control]") {
	TempDirectory directory;
	const auto graphPath = writeGraph(directory);
	Ear ear(graphPath, writeWebRoot(directory), linkedLine(route("noise", "MONO", "rec", "FL")));
	Rpc rpc(ear.port());

	const json snapshot = rpc.call("graph.subscribe");
	INFO(snapshot.dump(2));
	REQUIRE(snapshot.contains("result"));
	const json& graph = snapshot["result"];
	CHECK(graph["graphFrequency"] == graphFrequency);
	CHECK(graph["blockSize"] == blockSize);
	REQUIRE(graph["nodes"].size() == 2);
	CHECK(graph["nodes"][0]["name"] == "noise");
	CHECK(graph["nodes"][0]["type"] == "white");
	CHECK(graph["nodes"][0]["outputs"] == json{"MONO"});
	CHECK(graph["nodes"][0]["state"] == "running");
	CHECK(graph["nodes"][1]["name"] == "rec");
	CHECK(graph["nodes"][1]["type"] == "file");
	CHECK(graph["nodes"][1]["settings"]["path"] == (directory.path() / "rec.wav").string());
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
	CHECK(channelRms(directory.path() / "rec.wav", 1) == Approx(0.577).margin(0.02));
}

TEST_CASE("levels.subscribe sends the noise's level", "[system][control]") {
	TempDirectory directory;
	Ear ear(writeGraph(directory), writeWebRoot(directory), linkedLine(route("noise", "MONO", "rec", "FL")));
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
	TempDirectory directory;
	Ear ear(writeGraph(directory), writeWebRoot(directory), linkedLine(route("noise", "MONO", "rec", "FL")));
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

TEST_CASE("Edits sent while ear stops are refused as stopping and ear exits cleanly", "[system][control]") {
	TempDirectory directory;
	Ear ear(writeGraph(directory), writeWebRoot(directory), linkedLine(route("noise", "MONO", "rec", "FL")));
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));

	ear.process().signal(SIGTERM);
	// The graph is closed before any node is told to stop, so edits from here on are refused.
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "rec"}, {"state", "stopping"}}); });
	const std::vector<std::pair<std::string, json>> edits = {
	    {"node.add", {{"name", "late"}, {"type", "white"}}},
	    {"node.update", {{"name", "rec"}, {"settings", {{"path", (directory.path() / "late.wav").string()}}}}},
	};
	std::optional<std::uint16_t> closeCode;
	for (const auto& [method, params] : edits) {
		try {
			const json reply = rpc.call(method, params);
			INFO(method << ": " << reply.dump());
			CHECK(reply["error"]["code"] == -32000);
			CHECK(reply["error"]["data"]["code"] == "stopping");
		} catch (const ear::test::Closed& closed) {
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
	CHECK(headerFinished(directory.path() / "rec.wav"));
}

TEST_CASE("A sound card node runs and its route is linked", "[system][control][card]") {
	TempDirectory directory;
	// The test ALSA configuration (slice E2): card "Test" is a file PCM writing into EAR_ALSA_OUT.
	const auto alsa = std::filesystem::path(__FILE__).parent_path() / "../../src/pipewire/tests/alsa/alsa.conf";
	REQUIRE(std::filesystem::exists(alsa));
	::setenv("ALSA_CONFIG_PATH", alsa.c_str(), 1);
	::setenv("EAR_ALSA_OUT", directory.path().c_str(), 1);
	const json card = {{"name", "out"}, {"type", "card"}, {"settings", {{"card", "Test"}, {"playback", 2}}}};
	const json toCard = route("noise", "MONO", "out", "playback_1");
	Ear ear(writeGraph(directory, json::array({card}), json::array({toCard})), writeWebRoot(directory),
	        linkedLine(toCard));
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
