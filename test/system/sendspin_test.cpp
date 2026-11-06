// The owner's end-to-end tests of the sendspin node (docs/design/sendspin.md section 14, S1 criteria 10 to 12): the real
// out/ear runs a graph file with a sendspin node routed into a file node, and dials the real server; the server plays a
// ramp in which every frame names its place, and the WAV must hold it. With no server the node runs and is silent; a peer
// that never answers, or a listener that drops the dial, holds up neither a stop nor a restart. Every wait is for an event
// (a line ear prints, an event the server prints, a message, ear's exit); a deadline only fails a test.
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ear_system.hpp"
#include "sendspin_server.hpp"

namespace {

using namespace ear::test;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t blockSize = 256;
/// Fails a play that never finishes: longer than the shared deadline, because a stream's start takes the server's lead.
constexpr auto playDeadline = 30s;
/// S1's bound on a stop and on a restart.
constexpr auto stopBound = 5s;

/// sendspin node "ma" dialling `url`, routed side for side into file node "rec".
std::string dialToRec(const std::string& url) {
	return graphFileText("node sendspin ma { server \"" + url + "\"; }\n"
	                     "node file rec { path \"rec.wav\"; }\n"
	                     "route ma:from_server_FL rec:FL;\n"
	                     "route ma:from_server_FR rec:FR;\n",
	                     graphFrequency, blockSize);
}

ChildProcess runEar(const TempDirectory& directory) { return ChildProcess(EAR_BINARY, earArguments(directory)); }

const std::vector<std::string> runningAndLinked{
    "node ma running",
    "node rec running",
    "route ma:from_server_FL -> rec:FL linked",
    "route ma:from_server_FR -> rec:FR linked",
};

bool waitForLines(ChildProcess& ear, const std::vector<std::string>& lines) {
	for (const auto& line : lines) {
		if (!ear.waitForLine(line, deadline)) return false;
	}
	return true;
}

/// The control port's number, once ear has said it.
std::uint16_t controlPort(ChildProcess& ear) {
	const auto listening = ear.waitForLineStartingWith(listeningPrefix, deadline);
	REQUIRE(listening);
	return static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));
}

/// A socket bound on 127.0.0.1 at a free port, listening.
class Listener {
public:
	explicit Listener(int backlog) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
		REQUIRE(fd_ >= 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
		REQUIRE(::listen(fd_, backlog) == 0);
		socklen_t length = sizeof address;
		REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
		port_ = ntohs(address.sin_port);
	}
	~Listener() {
		::close(fd_);
		for (const int filler : fillers_) ::close(filler);
	}
	std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/sendspin"; }

	/// Connects until the backlog is full: the kernel then drops every new SYN, so the last connect stays in progress.
	void fillBacklog() {
		for (int tries = 0; tries < 64; ++tries) {
			const int filler = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
			REQUIRE(filler >= 0);
			fillers_.push_back(filler);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(port_);
			::connect(filler, reinterpret_cast<sockaddr*>(&address), sizeof address);
			// A connect the backlog took has finished by now on loopback; one it dropped has not.
			pollfd writable{filler, POLLOUT, 0};
			if (::poll(&writable, 1, 200) == 0) return;
		}
		FAIL("the backlog never filled");
	}

private:
	int fd_;
	std::uint16_t port_ = 0;
	std::vector<int> fillers_;
};

/// A free TCP port on 127.0.0.1 with nothing behind it.
std::uint16_t freeTcpPort() {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
	socklen_t length = sizeof address;
	REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	::close(fd);
	return ntohs(address.sin_port);
}

/// The dialling node stops within the bound, and a new name restarts it within the bound, on a peer that holds the dial.
void checkHeldDial(const TempDirectory& directory, const std::string& url) {
	writeGraphFile(directory, dialToRec(url));
	{
		auto ear = runEar(directory);
		const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
		INFO("ear printed:\n" << ear.output());
		REQUIRE(ready);
		const std::uint16_t port = controlPort(ear);
		Rpc rpc(port);
		REQUIRE(rpc.call("graph.subscribe").contains("result"));

		const auto before = std::chrono::steady_clock::now();
		const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", url}, {"name", "Other"}}}});
		INFO(update.dump());
		REQUIRE(update.contains("result"));
		rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "running"}}); });
		CHECK(std::chrono::steady_clock::now() - before < stopBound);

		ear.signal(SIGTERM);
		const auto stopping = std::chrono::steady_clock::now();
		CHECK(rpc.readUntilClosed() == 1001);
		const auto exitStatus = ear.waitForExit(stopBound);
		CHECK(std::chrono::steady_clock::now() - stopping < stopBound);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 0);
	}
}

/// The WAV's first non-zero frame onward: how many frames are exact ramp frames, and how many steps are neither +1 nor a
/// block's multiple. Left is the low 16 bits of the frame index, right the high 16, each as a signed sample.
struct RampCheck {
	std::size_t rampFrames = 0;
	std::size_t badFrames = 0;
	std::size_t badSteps = 0;
	std::size_t zeroFramesInside = 0;
	std::uint32_t firstIndex = 0;
	std::uint32_t lastIndex = 0;
};

RampCheck checkRamp(const Wav& wav) {
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
	for (std::size_t frame = first; frame <= last; ++frame) {
		const float left = wav.samples[frame * 2];
		const float right = wav.samples[frame * 2 + 1];
		if (left == 0.0F && right == 0.0F) {
			++result.zeroFramesInside;
			continue;
		}
		const float lo = left * 32768.0F;
		const float hi = right * 32768.0F;
		if (lo != std::trunc(lo) || hi != std::trunc(hi)) {
			++result.badFrames;
			continue;
		}
		const auto word = [](float value) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xFFFFU; };
		const std::uint32_t index = word(lo) | (word(hi) << 16U);
		if (previous) {
			const std::uint32_t step = index - *previous;
			if (step != 1 && (step - 1) % blockSize != 0) ++result.badSteps;
		} else {
			result.firstIndex = index;
		}
		previous = index;
		result.lastIndex = index;
		++result.rampFrames;
	}
	return result;
}

} // namespace

TEST_CASE("A ramp the server plays to a sendspin node lands in the file node's WAV, frame for frame", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) &&
	                   ear.waitForLine("node ma says connected ear-test-server unpaired", deadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	CHECK(ear.hasLine("node ma says dialling " + server.url()));

	server.send("trust");
	server.send("play 1.5");
	const bool ended = ear.waitForLine("node ma says stream-ended", playDeadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ended);

	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));
	CHECK(ear.hasLine("node ma says stream flac 48000 2 24"));

	const Wav wav = readWav(directory.recordings() / "rec.wav");
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	REQUIRE(wav.info.channels == 2);
	const RampCheck ramp = checkRamp(wav);
	INFO("ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps
	                    << ", zeros inside " << ramp.zeroFramesInside << ", indices " << ramp.firstIndex << " to " << ramp.lastIndex);
	CHECK(ramp.rampFrames >= graphFrequency * 6 / 5);   // at least 1.2 s
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("With no server at all the sendspin node runs, its meters silent, and SIGTERM exits 0", "[system][sendspin]") {
	TempDirectory directory;
	const std::string url = "ws://127.0.0.1:" + std::to_string(freeTcpPort()) + "/sendspin";
	writeGraphFile(directory, dialToRec(url));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	{
		WsClient ws(controlPort(ear));
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		json levels;
		while (true) {
			levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("ma")) break;
		}
		INFO(levels.dump());
		for (const char* port : {"from_server_FL", "from_server_FR"}) {
			const json& meter = levels["params"]["nodes"]["ma"][port];
			CHECK(meter[0].get<double>() == 0.0);   // peak
			CHECK(meter[1].get<double>() == 0.0);   // RMS
		}
	}
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(stopBound);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
}

TEST_CASE("A peer that accepts and never answers holds up neither a restart nor a stop", "[system][sendspin]") {
	Listener peer(16);
	TempDirectory directory;
	checkHeldDial(directory, peer.url());
}

TEST_CASE("A listener with a full backlog holds up neither a restart nor a stop", "[system][sendspin]") {
	Listener peer(0);
	peer.fillBacklog();
	TempDirectory directory;
	checkHeldDial(directory, peer.url());
}

TEST_CASE("A slow upgrade sees one connection and no another_server, then connects", "[system][sendspin]") {
	SendspinTestServer server({"--slow-upgrade", "3"});
	TempDirectory directory;
	writeGraphFile(directory, dialToRec(server.url()));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) &&
	                   ear.waitForLine("node ma says connected ear-test-server unpaired", playDeadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	CHECK(server.count("upgrade") == 1);
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
}
