// The owner's end-to-end tests of the sendspin node (docs/design/sendspin.md section 14, S1 criteria 10 to 12, S2 criteria
// 3 to 8): the real out/ear runs a graph file with a sendspin node routed into a file node, and dials the real server.
// Paired by a code, the server plays a ramp in which every frame names its place, and the WAV must hold it; routed noise
// reaches the server on the routed channel alone. Unpaired, nothing moves. A round limit holds, a bad key file fails only
// the node. With no server the node runs and is silent; a peer that never answers, or a listener that drops the dial,
// holds up neither a stop nor a restart. Every wait is for an event (a line ear prints, an event the server prints, a
// message, ear's exit); a deadline only fails a test.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
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

constexpr std::string_view connectedUnpaired = "node ma says connected ear-test-server unpaired";
constexpr std::string_view connectedPaired = "node ma says connected ear-test-server paired";
const std::string pairingCodePrefix = "node ma says pairing-code ";

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

/// The `index`th line (from 0) starting with `prefix`, once ear has printed it. ChildProcess gives only the first, and
/// offers no wake on "something new", so this reads what arrives in short bounded reads; the line arriving ends the wait.
std::optional<std::string> waitForNthLine(ChildProcess& ear, const std::string& prefix, std::size_t index, std::chrono::milliseconds patience) {
	const auto until = std::chrono::steady_clock::now() + patience;
	for (;;) {
		std::istringstream lines(ear.output());
		std::size_t seen = 0;
		for (std::string line; std::getline(lines, line);) {
			if (line.starts_with(prefix) && seen++ == index) return line;
		}
		if (std::chrono::steady_clock::now() >= until) return std::nullopt;
		ear.waitForLine("\x01", 20ms);
	}
}

std::size_t countLines(const ChildProcess& ear, const std::string& prefix) {
	std::istringstream lines(ear.output());
	std::size_t seen = 0;
	for (std::string line; std::getline(lines, line);) seen += line.starts_with(prefix) ? 1U : 0U;
	return seen;
}

/// The digits of a `says pairing-code 123-456` line.
std::string digitsOf(const std::string& line) {
	std::string digits = line.substr(pairingCodePrefix.size());
	std::erase(digits, '-');
	return digits;
}

/// Six digits that are not `digits`.
std::string wrongCode(std::string digits) {
	digits[0] = digits[0] == '0' ? '1' : '0';
	return digits;
}

/// Connected and unpaired: `pair`, then the code ear shows, until ear says it is paired on the new key.
void pairOnce(ChildProcess& ear, SendspinTestServer& server) {
	server.send("pair");
	const auto code = ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(code);
	server.send("code " + digitsOf(*code));
	REQUIRE(ear.waitForLine(connectedPaired, deadline));
}

std::filesystem::perms modeOf(const std::filesystem::path& path) {
	return std::filesystem::symlink_status(path).permissions() & std::filesystem::perms::mask;
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

TEST_CASE("A wrong code then the right one pairs in band, and a ramp the server plays lands in the WAV, frame for frame",
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
	CHECK(countLines(ear, pairingCodePrefix) == 1);   // the wrong round kept the code
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
	INFO("ramp frames " << ramp.rampFrames << ", not ramp " << ramp.badFrames << ", bad steps " << ramp.badSteps
	                    << ", zeros inside " << ramp.zeroFramesInside << ", indices " << ramp.firstIndex << " to " << ramp.lastIndex);
	CHECK(ramp.rampFrames >= graphFrequency * 6 / 5);   // at least 1.2 s
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
}

TEST_CASE("A restarted ear keeps its identity and its pairing, at 0600, and is asked for no code", "[system][sendspin]") {
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

TEST_CASE("Paired, routed noise reaches the server on the routed channel, with exact zeros on the other", "[system][sendspin]") {
	SendspinTestServer server;
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node white noise;\n"
	                                        "node sendspin ma { server \"" + server.url() + "\"; }\n"
	                                        "route noise:MONO ma:to_server_FL;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool ready = ear.waitForLine("route noise:MONO -> ma:to_server_FL linked", deadline) && ear.waitForLine(connectedUnpaired, deadline);
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
	fields >> frames >> rms >> rightNonZero >> stampsUs >> framesBeforeLast;
	CHECK(frames >= graphFrequency);
	CHECK(rms == Approx(0.577).margin(0.03));   // uniform noise in [-1, 1)
	CHECK(rightNonZero == 0);
	const auto durationUs = static_cast<std::int64_t>(framesBeforeLast) * 1000000 / graphFrequency;
	CHECK(std::abs(stampsUs - durationUs) <= 50000);

	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(ear.hasLine("node ma stopped dropped=0"));
}

TEST_CASE("20 wrong rounds hold pairing back: an update with the same settings leaves it held, a new name releases it", "[system][sendspin]") {
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

	// A new name is a new client, on the same key file, and the round limit lives in memory.
	const std::size_t idsBefore = countLines(ear, "node ma says id ");
	const std::size_t connectsBefore = countLines(ear, "node ma says connected ");
	const json renamed = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", server.url()}, {"name", "Other"}}}});
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
	writeGraphFile(directory, graphFileText("node sendspin ma { server \"" + url + "\"; }\n"
	                                        "node white noise;\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route noise:MONO rec:FL;\n",
	                                        graphFrequency, blockSize));
	const auto keyFile = directory.config() / "sendspin-ma.key";
	const std::string corrupt = "{ this is no key file";
	std::ofstream(keyFile, std::ios::binary) << corrupt;
	std::filesystem::permissions(keyFile, std::filesystem::perms(0640));

	auto ear = runEar(directory);
	const auto failed = ear.waitForLineStartingWith("node ma failed start-failed: ", deadline);
	const bool others = waitForLines(ear, {"node rec running", "route noise:MONO -> rec:FL linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	CHECK(*failed == "node ma failed start-failed: sendspin-ma.key: malformed: not JSON");
	REQUIRE(others);
	CHECK(readText(keyFile) == corrupt);
	CHECK(modeOf(keyFile) == std::filesystem::perms(0640));
	CHECK_FALSE(ear.lineStartingWith("node ma says id "));

	{
		WsClient ws(controlPort(ear));
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		for (;;) {
			const json levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("rec") &&
			    levels["params"]["nodes"]["rec"]["FL"][0].get<double>() > 0.0) {
				break;
			}
		}
	}
	ear.signal(SIGTERM);
	CHECK(ear.waitForExit(deadline) == 0);
	CHECK(readText(keyFile) == corrupt);
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
