// Sockets that prove nothing (docs/node-types.md#on-an-untrusted-network): what they can and cannot keep a
// server out of. (Shared helpers: sendspin_system.hpp.)

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "sendspin_system.hpp"

namespace {

using namespace ear::test;

/// How long a silent peer's read waits for an answer it ignores: it only paces the peer, so a round never waits long on
/// one socket. Never an honest peer's patience.
constexpr std::chrono::seconds silentPatience{1};

/// A socket to `port` on 127.0.0.1 from `source`, or -1. Every 127/8 address is this host's, so one test can be peers on
/// several addresses. It closes with a reset, so a thousand of them leave no TIME_WAIT behind. Its reads wait at most
/// `patience`: for an honest peer the shared deadline, which only fails a test. A loaded host was seen to leave every one
/// of ear's threads runnable and not run for most of a second while an upgrade waited, so a second's patience judged the
/// host, not the transport, whose refusal closes the socket at once.
int connectFrom(const std::string& source, std::uint16_t port, std::chrono::seconds patience = deadline) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	const linger reset{1, 0};
	::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof reset);
	const timeval wait{.tv_sec = static_cast<time_t>(patience.count()), .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
	sockaddr_in from{};
	from.sin_family = AF_INET;
	::inet_pton(AF_INET, source.c_str(), &from.sin_addr);
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons(port);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&from), sizeof from) != 0 ||
	    ::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

std::string upgradeRequest(std::uint16_t port) {
	return "GET /sendspin HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
	       "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
	       "dGhlIHNhbXBsZSBub25jZQ==\r\n"
	       "Sec-WebSocket-Version: 13\r\n\r\n";
}

/// Reads to the end of the upgrade's answer, and says whether it was a 101.
bool answeredUpgrade(int fd) {
	std::string answer;
	char byte = 0;
	while (!answer.ends_with("\r\n\r\n") && ::recv(fd, &byte, 1, 0) == 1) answer += byte;
	return answer.starts_with("HTTP/1.1 101");
}

/// Peers that never say a word of Sendspin, on their own thread until destroyed: each round opens `perSource` sockets from
/// each source and holds them for `hold`. One `renewing` closes the last round's before it opens this one's, as a holder
/// keeping its place does, and waits for the transport to close each of them too: a socket's thread sees its peer's
/// close in its own time, and on a loaded host a holder renewing on a period alone outran it and filled the address's
/// share with sockets still listed (the transport's close of each is the event, never a period). Otherwise it closes
/// them after, so they press on the room the whole time, as a flood does.
class SilentPeers {
public:
	struct Shape {
		std::vector<std::string> sources;
		std::size_t perSource;
		bool upgrades; // false: a bare connect, no byte sent
		std::chrono::milliseconds hold;
		bool renewing;
	};

	SilentPeers(std::uint16_t port, Shape shape) : port_(port), shape_(std::move(shape)), thread_([this] { run(); }) {}

	~SilentPeers() {
		{
			const std::scoped_lock lock(mutex_);
			stop_ = true;
		}
		changed_.notify_all();
		thread_.join();
	}

	SilentPeers(const SilentPeers&) = delete;
	SilentPeers& operator=(const SilentPeers&) = delete;

	/// Until `rounds` more rounds have opened, each within the deadline of the one before; false if one did not. Each round
	/// is the event: a loaded host only makes many of them take longer (100 rounds of 32 upgrades were seen to take over
	/// 10 s, every one answered or refused at once), and only a round that never comes fails.
	bool waitForRounds(std::size_t rounds) {
		std::unique_lock lock(mutex_);
		const std::size_t until = rounds_ + rounds;
		while (rounds_ < until) {
			const std::size_t next = rounds_ + 1;
			if (!changed_.wait_for(lock, deadline, [&] { return rounds_ >= next; })) return false;
		}
		return true;
	}

	/// Until `rounds` more rounds have opened or `within` has passed, whichever is first: how many did.
	std::size_t roundsWithin(std::size_t rounds, std::chrono::milliseconds within) {
		std::unique_lock lock(mutex_);
		const std::size_t from = rounds_;
		changed_.wait_for(lock, within, [&] { return rounds_ >= from + rounds; });
		return rounds_ - from;
	}

	/// Sockets a renewing round closed that the transport never closed in turn.
	std::size_t unreleased() {
		const std::scoped_lock lock(mutex_);
		return unreleased_;
	}

private:
	void run() {
		std::vector<int> held;
		const auto closeAll = [](std::vector<int>& sockets) {
			for (const int fd : sockets) ::close(fd);
			sockets.clear();
		};
		const std::string request = upgradeRequest(port_);
		for (;;) {
			if (shape_.renewing) {
				const std::size_t left = release(held);
				const std::scoped_lock lock(mutex_);
				unreleased_ += left;
			}
			std::vector<int> round;
			for (const std::string& source : shape_.sources) {
				for (std::size_t i = 0; i < shape_.perSource; ++i) {
					const int fd = connectFrom(source, port_, silentPatience);
					if (fd < 0) continue;
					round.push_back(fd);
					if (shape_.upgrades && ::send(fd, request.data(), request.size(), MSG_NOSIGNAL) > 0)
						answeredUpgrade(fd);
				}
			}
			closeAll(held);
			held = std::move(round);
			std::unique_lock lock(mutex_);
			++rounds_;
			changed_.notify_all();
			if (changed_.wait_for(lock, shape_.hold, [this] { return stop_; })) break;
		}
		closeAll(held);
	}

	/// Half-closes each socket, then reads each until the transport closes its end (its thread saw the close and
	/// forgot it), then closes them: how many it never closed by the deadline.
	static std::size_t release(std::vector<int>& sockets) {
		for (const int fd : sockets) ::shutdown(fd, SHUT_WR);
		const auto until = std::chrono::steady_clock::now() + deadline;
		std::size_t left = 0;
		for (const int fd : sockets) {
			char buffer[4096];
			for (;;) {
				// Each read waits at most silentPatience (SO_RCVTIMEO).
				const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
				if (got > 0 || (got < 0 && (errno == EAGAIN || errno == EINTR))) {
					if (std::chrono::steady_clock::now() < until) continue;
					++left;
				}
				break; // 0: the transport closed its end; any other error: it reset it
			}
			::close(fd);
		}
		sockets.clear();
		return left;
	}

	std::uint16_t port_;
	Shape shape_;
	std::mutex mutex_;
	std::condition_variable changed_;
	std::size_t rounds_ = 0;
	std::size_t unreleased_ = 0;
	bool stop_ = false;
	std::thread thread_;
};

/// Two upgraded sockets from `source`, renewed each `every`: the holder that sockets which prove nothing give way to, with the renewal shortened
/// from 28 s so a test sees several.
SilentPeers::Shape holder(const std::string& source, std::chrono::milliseconds every = 1s) {
	return {{source}, 2, true, every, true};
}
/// Upgraded sockets from these sources, as fast as a test should ask of a shared host.
SilentPeers::Shape flood(std::vector<std::string> sources) { return {std::move(sources), 2, true, 20ms, false}; }
/// How many threads past the sockets' bound a test allows, for those ending as the next begins: peaks of up to 88 were seen
/// over 13 idle, against 102 and up with no bound.
constexpr std::size_t socketThreadsEnding = 16;
/// A running listening node for a test's peers.
struct ListeningEar {
	std::uint16_t port = freeTcpPort();
	TempDirectory directory;
	ChildProcess ear;
	ListeningEar()
		: ear([this] {
			  writeGraphFile(directory, listenToRec(port));
			  return runEar(directory);
		  }()) {
		waitForInstance(ear, "ma", port);
	}
	std::string url() const { return "ws://127.0.0.1:" + std::to_string(port) + "/sendspin"; }
};

/// The whole of what the silent sockets are meant to prevent: the server gets in, pairs and plays.
void checkPairsAndPlays(ChildProcess& ear, const TempDirectory& directory, SendspinTestServer& server) {
	REQUIRE_SEEN(ear, ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(ear, server);
	server.send(playLong);
	REQUIRE_SEEN(ear, waitForRampFrames(directory.recordings() / "rec.wav"));
	REQUIRE_SEEN(ear, endStream(ear, server));
	CHECK(server.count("disconnected ") == 0);
	ear.signal(SIGTERM);
	REQUIRE_SEEN(ear, waitForStop(ear) == 0);
	checkStopWithinBound(ear);
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

/// How a slow upgrade ended.
enum class SlowUpgrade {
	answered,   // 101
	closed,     // closed with no answer: it gave way, or was refused
	windowPast, // the transport's own 400: its 3 s for an upgrade ran out before the rest of the request came
};

/// Reads to the end of a slow upgrade's answer. The transport answers its own handshake's timeout with a 400 before it
/// closes, where a socket that gave way is shut down first and so says nothing.
SlowUpgrade slowUpgradeAnswer(int fd) {
	std::string answer;
	char byte = 0;
	while (!answer.ends_with("\r\n\r\n") && ::recv(fd, &byte, 1, 0) == 1) answer += byte;
	if (answer.starts_with("HTTP/1.1 101")) return SlowUpgrade::answered;
	return answer.starts_with("HTTP/1.1 400") ? SlowUpgrade::windowPast : SlowUpgrade::closed;
}

/// The attempts answersSlowUpgrade makes at an upgrade that waits across a round inside the transport's window.
constexpr int slowUpgradeAttempts = 5;

/// Whether a socket from `source`, half its upgrade sent, is answered once the rest comes after a round of these peers
/// has opened. The round is the event and is required, under the shared deadline, so the test judges that rounds keep
/// opening, never how fast. Up to `rounds` in all are waited for while the upgrade has had under 2 s, inside the 3 s
/// the transport gives one; those past the first are only more evidence. An attempt whose first round came after the
/// transport's window had passed (its 400) judged nothing, and the next attempt waits for a round of its own.
bool answersSlowUpgrade(SilentPeers& peers, const std::string& source, std::uint16_t port, std::size_t rounds) {
	const std::string request = upgradeRequest(port);
	const std::size_t half = request.size() / 2;
	for (int attempt = 0; attempt < slowUpgradeAttempts; ++attempt) {
		const int fd = connectFrom(source, port);
		REQUIRE(fd >= 0);
		REQUIRE(::send(fd, request.data(), half, MSG_NOSIGNAL) == static_cast<ssize_t>(half));
		const auto halfSent = std::chrono::steady_clock::now();
		INFO("attempt " << attempt << ": a round of the peers opening while the upgrade waits");
		REQUIRE(peers.waitForRounds(1));
		const auto waited =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - halfSent);
		if (rounds > 1 && waited < 2s) peers.roundsWithin(rounds - 1, 2s - waited);
		// A send the transport already closed on fails; its answer, if it gave one, is still there to read.
		::send(fd, request.data() + half, request.size() - half, MSG_NOSIGNAL);
		const SlowUpgrade answer = slowUpgradeAnswer(fd);
		::close(fd);
		if (answer != SlowUpgrade::windowPast) return answer == SlowUpgrade::answered;
	}
	FAIL("each of " << slowUpgradeAttempts << " slow upgrades outlasted the transport's 3 s waiting for one round");
	return false;
}

} // namespace

// What an unproved peer can keep a server out of. Peers on 127.0.0.1 share the server's address, as a
// peer behind the same NAT or proxy does; a peer on another 127/8 address does not.
TEST_CASE("A server pairs and plays beside two silent sockets renewed from its own address", "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, holder("127.0.0.1"));
	REQUIRE(peers.waitForRounds(1));
	SendspinTestServer server({"--dial", node.url()});
	checkPairsAndPlays(node.ear, node.directory, server);
}

TEST_CASE("Bare sockets filling an address's share and renewed before their upgrade is due keep "
          "nobody out",
          "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.1"}, 10, false, 2s, true});
	REQUIRE(peers.waitForRounds(1));
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
}

// The library's own ConnectionLifecycle.JunkProbeDoesNotBlockRealServer, against ear's patched tree and a real server:
// a port scan's two connects held open and never renewed. Its own copy hangs with the listener patch only because its
// fake server's IXWebSocket client sends Origin (docs/node-types.md#library-facts-and-carried-patches).
TEST_CASE("A server gets in beside two bare sockets held open from its own address", "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.1"}, 2, false, 10min, false});
	REQUIRE(peers.waitForRounds(1));
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	CHECK(server.count("disconnected ") == 0);
}

TEST_CASE("Bare sockets from many addresses filling every socket and renewed keep nobody out", "[system][sendspin]") {
	ListeningEar node;
	std::vector<std::string> sources;
	for (int i = 2; i < 12; ++i) sources.push_back("127.0.0." + std::to_string(i));
	SilentPeers peers(node.port, {sources, 8, false, 2s, true});
	REQUIRE(peers.waitForRounds(1));
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
}

TEST_CASE("A server gets in beside a flood of silent upgrades from one other address", "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.2"}, 8, true, 1ms, false});
	REQUIRE(peers.waitForRounds(10));
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	CHECK(server.count("disconnected ") == 0);
}

TEST_CASE("A peer still proving itself is not displaced by a flood from one other address", "[system][sendspin]") {
	ListeningEar node;
	const int fd = connectFrom("127.0.0.3", node.port);
	REQUIRE(fd >= 0);
	const std::string request = upgradeRequest(node.port);
	REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
	REQUIRE(answeredUpgrade(fd));
	std::array<char, 4096> init{};
	REQUIRE(::recv(fd, init.data(), init.size(), 0) > 0); // client/init: it is in the room
	{
		SilentPeers peers(node.port, {{"127.0.0.2"}, 8, true, 1ms, false});
		REQUIRE(peers.waitForRounds(50));
		REQUIRE(::fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
		// The handshake a far or a busy server is still on: it has as long as it had with nobody there.
		const auto stillOpen = ::recv(fd, init.data(), init.size(), 0);
		CHECK(stillOpen < 0);
		CHECK(errno == EAGAIN);
	}
	::close(fd);
}

TEST_CASE("A server waiting for its code is not displaced by silent sockets and neither is the "
          "stream it then plays",
          "[system][sendspin]") {
	ListeningEar node;
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	server.send("pair");
	const auto code = node.ear.waitForLineStartingWith(pairingCodePrefix, deadline);
	REQUIRE_SEEN(node.ear, code);

	// The human takes their time: the code goes in only once the room has turned over many times.
	SilentPeers peers(node.port, flood({"127.0.0.1", "127.0.0.2"}));
	REQUIRE(peers.waitForRounds(20));
	server.send("code " + digitsOf(*code));
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedPaired, deadline));
	server.send("play 1.5");
	REQUIRE_SEEN(node.ear, node.ear.waitForLine("node ma says stream-ended", playDeadline));
	INFO("the server printed:\n" << server.events());
	CHECK(server.count("disconnected ") == 0);
	CHECK(server.count("pair-failed") == 0);
	CHECK_FALSE(node.ear.hasLine("node ma says disconnected"));
}

TEST_CASE("A paired server that has proved itself and not yet activated is never displaced by a "
          "silent socket",
          "[system][sendspin]") {
	ListeningEar node;
	TempDirectory state;
	{
		SendspinTestServer first({"--dial", node.url(), "--state", state.path().string()});
		REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
		pairOnce(node.ear, first);
	}
	REQUIRE_SEEN(node.ear, node.ear.waitForLine("node ma says disconnected", deadline));

	SendspinTestServer server({"--dial", node.url(), "--state", state.path().string(), "--hold-activate"});
	REQUIRE(server.waitForEvent("activate-held", deadline));
	// As far as a peer without a pairing can go, younger than the paired one and from its own address, so only the proof
	// keeps the paired one from going first.
	SendspinTestServer unproved({"--dial", node.url(), "--hold-activate"});
	REQUIRE(unproved.waitForEvent("activate-held", deadline));
	{
		SilentPeers peers(node.port, flood({"127.0.0.1", "127.0.0.2"}));
		REQUIRE(peers.waitForRounds(20));
		server.send("activate-release");
		REQUIRE_SEEN(node.ear, waitForNthLine(node.ear, std::string(connectedPaired), 1,
		                                      deadline)); // the first was the pairing
	}
	INFO("the server printed:\n" << server.events());
	CHECK(server.count("activate-held") == 1);
}

TEST_CASE("An unpaired server that has finished its Noise handshake outlasts sockets that have not",
          "[system][sendspin]") {
	ListeningEar node;
	SendspinTestServer server({"--dial", node.url(), "--hold-activate"});
	REQUIRE(server.waitForEvent("activate-held", deadline));
	SilentPeers peers(node.port, flood({"127.0.0.2", "127.0.0.3"}));
	REQUIRE(peers.waitForRounds(20));
	server.send("activate-release");
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	INFO("the server printed:\n" << server.events());
	CHECK(server.count("activate-held") == 1);
}

TEST_CASE("Two peers that do speak and never activate keep a third server out of nothing", "[system][sendspin]") {
	ListeningEar node;
	SendspinTestServer heldOne({"--dial", node.url(), "--hold-activate"});
	REQUIRE(heldOne.waitForEvent("activate-held", deadline));
	SendspinTestServer heldTwo({"--dial", node.url(), "--hold-activate"});
	REQUIRE(heldTwo.waitForEvent("activate-held", deadline));
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	// The oldest gave way with no goodbye: another_server would tell an honest server not to come back.
	const auto ended = heldOne.waitForEvent("activate-ended", deadline);
	INFO("the server that gave way printed:\n" << heldOne.events());
	REQUIRE(ended);
	CHECK(*ended == "activate-ended close");
}

TEST_CASE("A paired server playing keeps its socket while bare sockets fill its own address's share",
          "[system][sendspin]") {
	ListeningEar node;
	SendspinTestServer server({"--dial", node.url()});
	REQUIRE_SEEN(node.ear, node.ear.waitForLine(connectedUnpaired, deadline));
	pairOnce(node.ear, server);
	// More than the 8 an address may hold, so the oldest of its still upgrading gives way each time, and the server, older
	// than all of them, goes first unless having upgraded takes it out of the running.
	SilentPeers peers(node.port, {{"127.0.0.1"}, 10, false, 20ms, false});
	REQUIRE(peers.waitForRounds(10));
	server.send("play 1.5");
	REQUIRE_SEEN(node.ear, node.ear.waitForLine("node ma says stream-ended", playDeadline));
	INFO("the server printed:\n" << server.events());
	CHECK(server.count("disconnected ") == 0);
	CHECK_FALSE(node.ear.hasLine("node ma says disconnected"));
}

TEST_CASE("One address holding all it may of upgraded sockets leaves other addresses their upgrades",
          "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.2"}, 70, true, 400ms, false});
	REQUIRE(peers.waitForRounds(1));
	const std::string request = upgradeRequest(node.port);
	for (int i = 0; i < 20; ++i) {
		const int fd = connectFrom("127.0.0.3", node.port);
		REQUIRE(fd >= 0);
		const bool answered = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL) > 0 && answeredUpgrade(fd);
		::close(fd);
		INFO("upgrade " << i);
		REQUIRE(answered);
	}
}

TEST_CASE("One address past its share gives way alone and a slow upgrade from another is answered",
          "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.2"}, 70, true, 400ms, false});
	REQUIRE(peers.waitForRounds(1));
	for (int i = 0; i < 5; ++i) {
		const bool answered = answersSlowUpgrade(peers, "127.0.0.3", node.port, 1);
		INFO("upgrade " << i);
		CHECK(answered);
	}
}

TEST_CASE("A flood from every address holds the transport to twice its budget of threads", "[system][sendspin]") {
	ListeningEar node;
	const auto threads = [&node] {
		const std::filesystem::directory_iterator tasks(std::filesystem::path("/proc") /
		                                                std::to_string(node.ear.pid()) / "task");
		return static_cast<std::size_t>(std::distance(begin(tasks), end(tasks)));
	};
	const std::size_t idle = threads();
	std::vector<std::unique_ptr<SilentPeers>> peers;
	for (int i = 2; i < 18; ++i) {
		peers.push_back(std::make_unique<SilentPeers>(
			node.port, SilentPeers::Shape{{"127.0.0." + std::to_string(i)}, 8, true, 0ms, false}));
	}
	// A negative look from outside: sampling can miss a breach, never invent one beyond the threads still ending. The
	// bound itself is proven where it is enforced, exactly ("A flood past twice the room starts no thread beyond it",
	// src/nodes/sendspin/tests/transport_test.cpp); this shows the node's server is the one bound.
	std::size_t peak = 0;
	for (int sample = 0; sample < 3000; ++sample) {
		peak = std::max(peak, threads());
		std::this_thread::sleep_for(1ms);
	}
	INFO("idle " << idle << " threads, peak " << peak);
	// 64 sockets with a thread each. Each one's thread ends just after it is forgotten, so a few more can be seen at once.
	CHECK(peak <= idle + 64 + socketThreadsEnding);
}

TEST_CASE("A slow honest upgrade is answered through a flood of bare sockets from another address",
          "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, {{"127.0.0.2"}, 8, false, 5ms, false});
	REQUIRE(peers.waitForRounds(10));
	const bool answered = answersSlowUpgrade(peers, "127.0.0.3", node.port, 50);
	CHECK(answered);
}

TEST_CASE("A slow honest upgrade is answered while bare sockets from many addresses fill every socket",
          "[system][sendspin]") {
	ListeningEar node;
	std::vector<std::string> sources;
	for (int i = 2; i < 12; ++i) sources.push_back("127.0.0." + std::to_string(i));
	SilentPeers peers(node.port, {sources, 4, false, 100ms, true});
	REQUIRE(peers.waitForRounds(1));
	const bool answered = answersSlowUpgrade(peers, "127.0.0.20", node.port, 3);
	CHECK(answered);
}

TEST_CASE("A slow honest upgrade is answered beside two silent sockets renewed from its own address",
          "[system][sendspin]") {
	ListeningEar node;
	// Renewed far faster than a holder must, each round once the transport has closed the last (SilentPeers).
	SilentPeers peers(node.port, holder("127.0.0.1", 500ms));
	REQUIRE(peers.waitForRounds(1));
	const bool answered = answersSlowUpgrade(peers, "127.0.0.1", node.port, 3);
	CHECK(answered);
	CHECK(peers.unreleased() == 0);
}

TEST_CASE("Peers resetting sockets as they are closed do not end the process", "[system][sendspin]") {
	ListeningEar node;
	{
		SilentPeers peers(node.port, {{"127.0.0.2", "127.0.0.3", "127.0.0.4", "127.0.0.5"}, 8, true, 1ms, false});
		REQUIRE(peers.waitForRounds(100));
	}
	node.ear.signal(SIGTERM);
	REQUIRE_SEEN(node.ear, waitForStop(node.ear) == 0);
	checkStopWithinBound(node.ear);
}

TEST_CASE("A flood of silent sockets holds up neither the graph nor a stop", "[system][sendspin]") {
	ListeningEar node;
	SilentPeers peers(node.port, flood({"127.0.0.1", "127.0.0.2"}));
	REQUIRE(peers.waitForRounds(10));

	const std::uint16_t control = controlPort(node.ear);
	Rpc rpc(control);
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	const auto before = std::chrono::steady_clock::now();
	REQUIRE(rpc.call("node.remove", {{"name", "rec"}}).contains("result"));
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.removed", {{"name", "rec"}}); });
	CHECK(std::chrono::steady_clock::now() - before < stopBound);

	node.ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	const auto exitStatus = waitForStop(node.ear);
	INFO("ear printed:\n" << node.ear.output());
	CHECK(exitStatus == 0);
	checkStopWithinBound(node.ear, "ma");
}
