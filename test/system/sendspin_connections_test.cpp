// The sendspin node's connections: dialling, listening, held and shared ports, a restart or stop with no peer or a stuck one.
// (Shared helpers: sendspin_system.hpp.)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

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
	std::uint16_t port() const noexcept { return port_; }

	/// Connects until the backlog is full, then once more: the kernel then drops every new SYN, so that connect, and any
	/// after it, stays in progress. Full is the listening socket's own word (TCP_INFO on a listener: tcpi_unacked is its
	/// accept queue, tcpi_sacked the queue's bound, and it is full past the bound), never a guess from how long a connect
	/// took; each connect the queue can take is waited for until it has finished.
	void fillBacklog() {
		const auto queue = [this] {
			tcp_info info{};
			socklen_t length = sizeof info;
			REQUIRE(::getsockopt(fd_, IPPROTO_TCP, TCP_INFO, &info, &length) == 0);
			return info;
		};
		for (int tries = 0; tries < 64; ++tries) {
			const tcp_info before = queue();
			const bool full = before.tcpi_unacked > before.tcpi_sacked;
			const int filler = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
			REQUIRE(filler >= 0);
			fillers_.push_back(filler);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(port_);
			::connect(filler, reinterpret_cast<sockaddr*>(&address), sizeof address);
			if (full) return; // this one's SYN was dropped
			pollfd writable{filler, POLLOUT, 0};
			REQUIRE(::poll(&writable, 1, static_cast<int>(std::chrono::milliseconds(deadline).count())) == 1);
		}
		FAIL("the backlog never filled");
	}

private:
	int fd_;
	std::uint16_t port_ = 0;
	std::vector<int> fillers_;
};

/// The dialling node stops within the bound, and a new name restarts it within the bound, on a peer that holds the dial.
void checkHeldDial(const TempDirectory& directory, const std::string& url) {
	writeGraphFile(directory, dialToRec(url));
	{
		auto ear = runEar(directory);
		const bool ready =
			waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
		INFO("ear printed:\n" << ear.output());
		REQUIRE(ready);
		const std::uint16_t port = controlPort(ear);
		Rpc rpc(port);
		REQUIRE(rpc.call("graph.subscribe").contains("result"));

		const auto before = std::chrono::steady_clock::now();
		const json update =
			rpc.call("node.update", {{"name", "ma"}, {"settings", {{"server", url}, {"name", "Other"}}}});
		INFO(update.dump());
		REQUIRE(update.contains("result"));
		rpc.waitFor(
			[](const json& m) { return Rpc::matches(m, "node.state", {{"name", "ma"}, {"state", "running"}}); });
		CHECK(std::chrono::steady_clock::now() - before < stopBound);

		ear.signal(SIGTERM);
		CHECK(rpc.readUntilClosed() == 1001);
		const auto exitStatus = waitForStop(ear);
		INFO("ear printed:\n" << ear.output());
		CHECK(exitStatus == 0);
		checkStopWithinBound(ear, "ma");
	}
}

} // namespace

TEST_CASE("With no server at all the sendspin node runs, its meters silent, and SIGTERM exits 0",
          "[system][sendspin]") {
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
		for (const char* port : {"from.left", "from.right"}) {
			const json& meter = levels["params"]["nodes"]["ma"][port];
			CHECK(meter[0].get<double>() == 0.0); // peak
			CHECK(meter[1].get<double>() == 0.0); // RMS
		}
	}
	ear.signal(SIGTERM);
	const auto exitStatus = waitForStop(ear);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
	checkStopWithinBound(ear);
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
	const bool ready =
		waitForLines(ear, runningAndLinked) && ear.waitForLine(std::string(connectedUnpaired), playDeadline);
	INFO("ear printed:\n" << ear.output());
	INFO("the server printed:\n" << server.events());
	REQUIRE(ready);
	CHECK(server.count("upgrade") == 1);
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A listening node says how it is known, and a server that dials it pairs and plays", "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	waitForInstance(ear, "ma", port);
	CHECK_FALSE(ear.lineStartingWith("node ma says dialling"));

	SendspinTestServer server({"--dial", "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	server.send(playLong);
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "rec.wav"));
	REQUIRE_SEEN(ear, endStream(ear, server));

	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, waitForStop(ear) == 0);
	CHECK(ear.hasLine("node rec removed dropped=0"));
	const RampCheck ramp = checkRamp(readWav(directory.recordings() / "rec.wav"), lostSyncUs(ear));
	INFO(ramp);
	CHECK(ramp.rampFrames >= rampFramesAtLeast);
	CHECK(ramp.badFrames == 0);
	CHECK(ramp.badSteps == 0);
	// Every skip is a cycle ear says it lost: the graph cut it, or a node missed it (docs/engine.md#counters).
	const LostCycles lost = lostCycles(ear);
	INFO(lost);
	CHECK(ramp.skippedBlocks <= lost.budget());
}

TEST_CASE("A port another program holds fails the node with the kernel's reason, and a free port "
          "recovers it",
          "[system][sendspin]") {
	Listener holder(16);
	const std::uint16_t held = holder.port();
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(held) +
	                                            "; }\n"
	                                            "node white noise;\n"
	                                            "node file rec { path \"rec.wav\"; }\n"
	                                            "route noise:noise.mono rec:record.left;\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool failed = ear.waitForLine("node ma failed start-failed: cannot listen on port " + std::to_string(held) +
	                                        ": Address already in use",
	                                    deadline) &&
	                    waitForLines(ear, {"node rec running", "route noise:noise.mono -> rec:record.left linked"});
	INFO("ear printed:\n" << ear.output());
	REQUIRE(failed);
	CHECK_FALSE(ear.lineStartingWith("node ma says listening"));

	Rpc rpc(controlPort(ear));
	const std::uint16_t freePort = freeTcpPort();
	const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"port", freePort}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	waitForInstance(ear, "ma", freePort);
	CHECK_FALSE(connectRefused(freePort));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("Two sendspin nodes on one port: the second fails naming the first, and a free port "
          "recovers it",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string saying = " says listening " + std::to_string(port) + " as ";
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin a { port " + std::to_string(port) +
	                                            "; }\n"
	                                            "node sendspin b { port " +
	                                            std::to_string(port) + "; }\n",
	                                        graphFrequency, blockSize));
	auto ear = runEar(directory);
	const std::string refusal =
		"failed start-failed: cannot listen on port " + std::to_string(port) + ": held by node ";
	// Which of the two starts first is not this test's business.
	const auto until = std::chrono::steady_clock::now() + deadline;
	std::optional<std::string> refused;
	while (!refused && std::chrono::steady_clock::now() < until) {
		refused = ear.lineStartingWith("node a " + refusal);
		if (!refused) refused = ear.lineStartingWith("node b " + refusal);
		if (!refused) ear.waitForLine("\x01", 20ms);
	}
	REQUIRE_SEEN(ear, refused);
	const std::string loser = refused->substr(5, 1);
	const std::string winner = loser == "a" ? "b" : "a";
	CHECK(*refused == "node " + loser + " " + refusal + winner);
	REQUIRE_SEEN(ear, ear.waitForLineStartingWith("node " + winner + saying, deadline));
	REQUIRE_SEEN(ear, ear.waitForLine("node " + winner + " running", deadline));

	Rpc rpc(controlPort(ear));
	const std::uint16_t freePort = freeTcpPort();
	const json update = rpc.call("node.update", {{"name", loser}, {"settings", {{"port", freePort}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	waitForInstance(ear, loser, freePort);
	REQUIRE_SEEN(ear, ear.waitForLine("node " + loser + " running", deadline));
	CHECK_FALSE(ear.lineStartingWith("node " + winner + " failed"));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A new name while a server is connected keeps the port: the node still listens and a "
          "server reaches it",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	writeGraphFile(directory, listenToRec(port));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);
	SendspinTestServer server({"--dial", "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));

	Rpc rpc(controlPort(ear));
	const std::string saying = "node ma says listening " + std::to_string(port) + " as ";
	const json update = rpc.call("node.update", {{"name", "ma"}, {"settings", {{"port", port}, {"name", "Other"}}}});
	INFO(update.dump());
	REQUIRE(update.contains("result"));
	const auto again = waitForNthLine(ear, saying, 1, playDeadline);
	REQUIRE_SEEN(ear, again);
	CHECK(*again == *ear.lineStartingWith(saying)); // the same key file, so the same instance
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	const auto gone = server.waitForEvent("disconnected ", deadline);
	INFO("the server printed:\n" << server.events());
	REQUIRE(gone);
	// The goodbye is `shutdown`, and this server does not redial by itself after it, so the test asks. One that browses
	// hears the new announcement.
	server.send("dial ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says connected ", 1, playDeadline));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A dialling node opens no listening socket", "[system][sendspin]") {
	TempDirectory directory;
	const std::string url = "ws://127.0.0.1:" + std::to_string(freeTcpPort()) + "/sendspin";
	writeGraphFile(directory, dialToRec(url));
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked) && ear.waitForLine("node ma says dialling " + url, deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	CHECK(connectRefused(8928)); // the library's default, and no test here listens on it
	CHECK_FALSE(ear.lineStartingWith("node ma says listening"));
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("A node removed and added back while a server is connected takes its port back, once the "
          "old one has let go",
          "[system][sendspin]") {
	const std::uint16_t port = freeTcpPort();
	const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/sendspin";
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node sendspin ma { port " + std::to_string(port) + "; }\n", graphFrequency,
	                                        blockSize));
	auto ear = runEar(directory);
	waitForInstance(ear, "ma", port);
	SendspinTestServer server({"--dial", url});
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));

	Rpc rpc(controlPort(ear));
	REQUIRE(rpc.call("node.remove", {{"name", "ma"}}).contains("result"));
	const json added = rpc.call("node.add", {{"name", "ma"}, {"type", "sendspin"}, {"settings", {{"port", port}}}});
	INFO(added.dump());
	REQUIRE(added.contains("result"));

	const std::string saying = "node ma says listening " + std::to_string(port) + " as ";
	REQUIRE_SEEN(ear, waitForNthLine(ear, saying, 1, playDeadline));
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma running", 1, deadline));
	CHECK_FALSE(ear.lineStartingWith("node ma failed"));
	const std::string printed = ear.output();
	CHECK(printed.find("node ma removed") < printed.rfind(saying)); // the old one was gone first
	server.send("dial " + url);
	REQUIRE_SEEN(ear, waitForNthLine(ear, "node ma says connected ", 1, playDeadline));
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}
