// The owner's end-to-end tests of the rtp node (docs/node-types.md#rtp): the real out/ear runs a graph file
// with an rtp node routed into a file node; the tests' own sender plays
// a tone to it over 127.0.0.1 and the WAV file must hold that tone, channel for channel. With no
// sender the node still runs and its meters show silence (gate answer Q1). Every wait is for an
// event (a line ear prints, the sender's last packet, a levels message, ear's exit); a deadline
// only fails a test.
#include <sndfile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ear_system.hpp"
#include "rtp_tone.hpp"

namespace {

using namespace ear::test;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t blockSize = 256;
constexpr double amplitude = 0.5;
/// The rtp node's receive buffer: the graph file names no latencyMs, so the node's default (docs/node-types.md#rtp). A
/// packet late by less is still in time for it.
constexpr std::chrono::milliseconds receiverHeadroom{100};
/// ear counts its own lost cycles (docs/engine.md#counters) but not this test sender's late sends, so a tenth is timed by
/// the WAV: how much later than its frames were due to be sent a tenth is first seen in the WAV (heldBy below), so heldBy less this is no later than the tenth
/// played. It is the receiver's buffer (100 ms), the file node's write behind the graph and the 50 ms between looks.
/// Measured every look (mostLag, checked against this): 147 to 158 ms over 20 runs at load 2 to 3, at most 188 ms
/// over 10 beside six busy threads; 300 ms leaves a loaded host room, and a look past it fails the test naming it.
constexpr std::chrono::milliseconds heldByLag{300};
/// How long after its first sound the rtp node's output is left unjudged, once per start: its resampler's rate control
/// steering to the packets' arrival, which this test has always given 0.2 s. What follows is judged unless a late send
/// reaches it (docs/testing.md#the-senders-own-counter).
constexpr std::chrono::milliseconds receiverStartSettle{200};

/// rtp node "fpp" (any address, `port`, L24, two channels, no rate: the graph frequency) routed
/// channel for channel into file node "rec" (rec.wav in the recordings folder).
void writeGraph(const TempDirectory& directory, std::uint16_t port) {
	writeGraphFile(directory, graphFileText("node rtp fpp { address \"0.0.0.0\"; port " + std::to_string(port) +
	                                            "; format \"L24\"; channels 2; }\n"
	                                            "node file rec { path \"rec.wav\"; }\n"
	                                            "route fpp:stream.1 rec:record.left;\n"
	                                            "route fpp:stream.2 rec:record.right;\n",
	                                        graphFrequency, blockSize));
}

/// The real out/ear on `directory`, its control port on 127.0.0.1 at a free port.
ChildProcess runEar(const TempDirectory& directory) { return ChildProcess(EAR_BINARY, earArguments(directory)); }

/// Waits for every line in `lines`, in any order.
bool waitForLines(ChildProcess& ear, const std::vector<std::string>& lines) {
	for (const auto& line : lines) {
		if (!ear.waitForLine(line, deadline)) return false;
	}
	return true;
}

const std::vector<std::string> runningAndLinked{
	"node fpp running",
	"route fpp:stream.1 -> rec:record.left linked",
	"route fpp:stream.2 -> rec:record.right linked",
};

/// Channel `channel` of a stereo WAV, frames [from, from + frames).
std::vector<float> channelOf(const Wav& wav, int channel, std::size_t from, std::size_t frames) {
	std::vector<float> samples;
	samples.reserve(frames);
	for (std::size_t frame = from; frame < from + frames; ++frame) samples.push_back(wav.samples[frame * 2 + channel]);
	return samples;
}

constexpr std::size_t tenth = graphFrequency / 10;
/// The tenths the tone test judges: a second of what the receiver played that the sender's host left alone.
constexpr std::size_t plannedTenths = 10;

/// Channel `channel` carries `wanted` Hz at `amplitude` and not `other` Hz in each tenth of a second starting at the
/// frames `tenths` names: the receiver steers its resampler by the packets' arrival, so its rate wobbles by parts in
/// ten thousand, which over a whole second moves a sine's phase too far for one fit.
void checkTone(const Wav& wav, int channel, const std::vector<std::size_t>& tenths, double wanted, double other) {
	INFO(wanted << " Hz, not " << other << " Hz");
	std::string profile;
	double worstAmplitude = amplitude;
	double worstResidual = 0.0;
	double worstOther = 0.0;
	REQUIRE(tenths.size() >= plannedTenths);
	for (const std::size_t at : tenths) {
		const std::vector<float> samples = channelOf(wav, channel, at, tenth);
		const std::span<const float> window(samples);
		const auto fit = fitSine(window, wanted, graphFrequency);
		profile += std::to_string(fit.amplitude) + "/" + std::to_string(fit.residual) + " ";
		if (std::abs(fit.amplitude - amplitude) > std::abs(worstAmplitude - amplitude)) worstAmplitude = fit.amplitude;
		worstResidual = std::max(worstResidual, fit.residual);
		worstOther = std::max(worstOther, fitSine(window, other, graphFrequency).amplitude);
	}
	INFO("amplitude/residual per tenth: " << profile);
	CHECK(worstAmplitude == Approx(amplitude).margin(0.01));
	CHECK(worstResidual < 0.1);
	CHECK(worstOther < 0.01);
}

} // namespace

TEST_CASE("A tone sent over RTP to an rtp node lands in the file node's WAV, channel for channel", "[system][rtp]") {
	TempDirectory directory;
	const std::uint16_t port = freeUdpPort();
	writeGraph(directory, port);
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);

	// The sender sends on until the WAV holds the planned tenths, each judged only when no late send reaches it (the
	// sender's own counter of what the host did to it, its reach the receiver's buffer: lateSendReaches,
	// docs/testing.md#the-senders-own-counter). A tenth's time is when the WAV was first seen to hold it, looked at every
	// 50 ms: coarse, and later than its frames were due by up to heldByLag. Long enough that a loaded host still leaves a
	// second alone.
	const auto reach = receiverReach(ear::FrameClock(*ear::SampleRate::of(graphFrequency)), receiverHeadroom);
	RtpSender sender(port, RtpPcm::l24, graphFrequency, RtpTone{{750.0, 1500.0}, amplitude});
	sender.send(graphFrequency * 25);
	const auto path = directory.recordings() / "rec.wav";
	std::vector<std::pair<std::size_t, std::chrono::steady_clock::time_point>> looks; // frames held, and when
	std::vector<std::size_t> tenths;                                                  // judged, by first frame
	std::size_t excused = 0;
	std::optional<std::size_t> next;       // the next tenth to judge, once the first sound is known
	std::optional<std::size_t> firstSound; // the first sound's frame
	std::size_t framesHeld = 0;            // so far
	std::chrono::nanoseconds mostLag{0};   // the most a look came after the first new frame it saw was due to be sent
	const auto heldBy = [&](std::size_t frame) {
		for (const auto& [frames, at] : looks)
			if (frames > frame) return at;
		return looks.back().second;
	};
	const bool held = waitForWav(
		path,
		[&](std::span<const float> fresh, std::size_t channels) {
			// The first tenth judged starts once the receiver's start has settled, after its first sound.
			for (std::size_t at = 0; at + 1 < fresh.size() && !next; at += channels)
				if (fresh[at] != 0.0F || fresh[at + 1] != 0.0F) {
					firstSound = framesHeld + at / channels;
					next = *firstSound +
				           static_cast<std::size_t>(
							   ear::FrameClock(*ear::SampleRate::of(graphFrequency)).frames(receiverStartSettle));
				}
			const auto now = std::chrono::steady_clock::now();
			// heldByLag's evidence, every look: the WAV's frame n after the first sound is the sender's frame n (the
			// receiver plays them in order at the graph's rate), so the first frame new to this look was due to be sent
			// at dueAt(n), no later than it played. How long after that the look saw it bounds how late heldBy is.
			if (firstSound && framesHeld >= *firstSound && fresh.size() >= channels)
				mostLag = std::max(mostLag, std::chrono::duration_cast<std::chrono::nanoseconds>(
												now - sender.dueAt(framesHeld - *firstSound)));
			framesHeld += fresh.size() / channels;
			looks.emplace_back(framesHeld, now);
			if (!next) return false;
			while (tenths.size() < plannedTenths && *next + tenth <= framesHeld) {
				const auto from = heldBy(*next) - heldByLag;
				const auto to = heldBy(*next + tenth);
				// A send still held could reach back to this tenth: judged at a later look, once the sender has sent
				// what was due by then.
				if (sender.sentDue() < to) break;
				const auto late = sender.lateSends();
				const bool senderLate = std::ranges::any_of(
					late, [&](const RtpSender::LateSend& send) { return lateSendReaches(send, reach, from, to); });
				if (senderLate) ++excused;
				else tenths.push_back(*next);
				*next += tenth;
			}
			return tenths.size() >= plannedTenths;
		},
		std::chrono::seconds(60));
	INFO("judged " << tenths.size() << " tenths, excused " << excused << "; a look came at most "
	               << std::chrono::duration_cast<std::chrono::milliseconds>(mostLag).count()
	               << " ms after the first new frame it saw was due");
	REQUIRE(held);
	// heldBy's lag was within what the late sends' reach allowed for it, or a tenth was judged by a wrong time.
	CHECK(mostLag <= heldByLag);
	ear.signal(SIGTERM);
	const auto exitStatus = waitForStop(ear);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec removed dropped=0"));

	const Wav wav = readWav(directory.recordings() / "rec.wav");
	REQUIRE(wav.info.channels == 2);
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	const auto frames = static_cast<std::size_t>(wav.info.frames);
	std::size_t first = frames;
	std::size_t last = 0;
	for (std::size_t frame = 0; frame < frames; ++frame) {
		if (wav.samples[frame * 2] != 0.0F || wav.samples[frame * 2 + 1] != 0.0F) {
			if (first == frames) first = frame;
			last = frame;
		}
	}
	REQUIRE(first < frames);
	INFO("received span: frames " << first << " to " << last);
	REQUIRE(last + 1 >= tenths.back() + tenth);
	checkTone(wav, 0, tenths, 750.0, 1500.0);
	checkTone(wav, 1, tenths, 1500.0, 750.0);
}

TEST_CASE("With no sender the rtp node runs and its meters show silence", "[system][rtp]") {
	TempDirectory directory;
	writeGraph(directory, freeUdpPort());
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);

	const auto listening = ear.waitForLineStartingWith(listeningPrefix, deadline);
	REQUIRE(listening);
	const auto controlPort = static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));
	{
		WsClient ws(controlPort);
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		json levels;
		while (true) {
			levels = json::parse(ws.read());
			if (levels.value("method", "") == "levels" && levels["params"]["nodes"].contains("fpp")) break;
		}
		INFO(levels.dump());
		const json& channel1 = levels["params"]["nodes"]["fpp"]["stream.1"];
		CHECK(channel1[0].get<double>() == 0.0); // peak
		CHECK(channel1[1].get<double>() == 0.0); // RMS
	}
	ear.signal(SIGTERM);
	const auto exitStatus = waitForStop(ear);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
}

// Claims (docs/node-types.md#rtp; docs/writing-a-node-type.md#claims): an rtp node's UDP port has one holder, judged
// before its receiver is made (a port another program holds: lifecycle_test.cpp).

TEST_CASE("A second rtp node on one UDP port is refused held by node <first>, and the first hears what is sent to it",
          "[system][rtp][claims]") {
	TempDirectory directory;
	const std::uint16_t port = freeUdpPort();
	const std::string settings =
		" { address \"0.0.0.0\"; port " + std::to_string(port) + "; format \"L24\"; channels 2; }\n";
	writeGraphFile(directory,
	               graphFileText("node rtp a" + settings + "node rtp b" + settings, graphFrequency, blockSize));
	auto ear = runEar(directory);
	const bool running = ear.waitForLine("node a running", deadline);
	const std::string refusedLine =
		"node b failed start-failed: cannot bind UDP port " + std::to_string(port) + ": held by node a";
	const bool refused = ear.waitForLine(refusedLine, deadline);
	const auto listening = ear.lineStartingWith(listeningPrefix);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(listening);
	const auto controlPort = static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size())));

	// What each node plays of a tone sent to the port, by its meters: a unicast datagram reaches one socket only, so two
	// receivers on one port leave one of them silent with nothing said (docs/engine.md#rtp-receivers).
	RtpSender sender(port, RtpPcm::l24, graphFrequency, RtpTone{{750.0, 1500.0}, amplitude});
	sender.send(graphFrequency * 5);
	double peakA = 0.0;
	double peakB = 0.0;
	{
		WsClient ws(controlPort);
		ws.send(R"({"jsonrpc":"2.0","id":1,"method":"levels.subscribe"})");
		// Levels arrive about 30 times a second: a second and a half of them once a's meter has moved.
		int afterSound = 0;
		for (int message = 0; message < 300 && afterSound < 45; ++message) {
			const json levels = json::parse(ws.read());
			if (levels.value("method", "") != "levels") continue;
			const json& nodes = levels["params"]["nodes"];
			if (nodes.contains("a")) peakA = std::max(peakA, nodes["a"]["stream.1"][0].get<double>());
			if (nodes.contains("b")) peakB = std::max(peakB, nodes["b"]["stream.1"][0].get<double>());
			if (peakA > 0.0 || peakB > 0.0) ++afterSound;
		}
	}
	INFO("peaks: a " << peakA << ", b " << peakB << (ear.hasLine("node b running") ? "; node b running" : ""));
	CHECK(running);
	CHECK(refused);
	CHECK(peakA > amplitude / 2);
	CHECK(peakB == 0.0);
	ear.signal(SIGTERM);
	CHECK(waitForStop(ear) == 0);
}

TEST_CASE("node.restart of an rtp node binds its port again: its claim's probe runs once its old receiver let go",
          "[system][rtp][claims]") {
	TempDirectory directory;
	writeGraph(directory, freeUdpPort());
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	const auto listening = ear.lineStartingWith(listeningPrefix);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);
	REQUIRE(listening);
	Rpc rpc(static_cast<std::uint16_t>(std::stoul(listening->substr(listeningPrefix.size()))));
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	const json restart = rpc.call("node.restart", {{"name", "fpp"}});
	INFO(restart.dump());
	CHECK(restart["result"] == json{{"applied", "restart"}});
	// The old receiver goes in the turn's flush, and the new configuration's claim is probed behind it: a probe made
	// before would find the node's own old socket still bound and fail it "Address already in use".
	const json settled = rpc.waitFor([](const json& m) {
		return Rpc::matches(m, "node.state", {{"name", "fpp"}, {"state", "running"}}) ||
		       Rpc::matches(m, "node.state", {{"name", "fpp"}, {"state", "failed"}});
	});
	INFO("notifications: " << json(rpc.notifications).dump());
	CHECK(settled["params"]["state"] == "running");
	ear.signal(SIGTERM);
	CHECK(rpc.readUntilClosed() == 1001);
	CHECK(waitForStop(ear) == 0);
}
