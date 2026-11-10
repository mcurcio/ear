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

	// The sender sends on until the WAV holds the planned tenths, each judged only when no packet went out late by more
	// than the sender's bound within its buffer's reach before it (the sender's own counter of what the host did to
	// it; docs/testing.md#making-the-rare-event). A tenth's time is when the WAV was first seen to hold it, looked at
	// every 10 ms. Long enough that a loaded host still leaves a second alone.
	RtpSender sender(port, RtpPcm::l24, graphFrequency, RtpTone{{750.0, 1500.0}, amplitude});
	sender.send(graphFrequency * 25);
	const auto path = directory.recordings() / "rec.wav";
	std::vector<std::pair<std::size_t, std::chrono::steady_clock::time_point>> looks; // frames held, and when
	std::vector<std::size_t> tenths;                                                  // judged, by first frame
	std::size_t excused = 0;
	std::optional<std::size_t> next; // the next tenth to judge, once the first sound is known
	const auto heldBy = [&](std::size_t frame) {
		for (const auto& [frames, at] : looks)
			if (frames > frame) return at;
		return looks.back().second;
	};
	const bool held = waitForWav(
		path,
		[&](const Wav& wav) {
			const auto frames = static_cast<std::size_t>(wav.info.frames);
			looks.emplace_back(frames, std::chrono::steady_clock::now());
			if (!next) {
				for (std::size_t frame = 0; frame < frames && !next; ++frame)
					if (wav.samples[frame * 2] != 0.0F || wav.samples[frame * 2 + 1] != 0.0F)
						next = frame + graphFrequency / 5; // after the receiver's first 0.2 s
				if (!next) return false;
			}
			// A frame being written may be torn: judge a tenth once a block past it is held too.
			while (tenths.size() < plannedTenths && *next + tenth + blockSize <= frames) {
				const auto from = heldBy(*next) - std::chrono::milliseconds(600); // its buffer, and the write's lag
				const auto to = heldBy(*next + tenth);
				const auto late = sender.lateSends();
				const bool senderLate = std::ranges::any_of(
					late, [&](const RtpSender::LateSend& send) { return send.at >= from && send.at <= to; });
				if (senderLate) ++excused;
				else tenths.push_back(*next);
				*next += tenth;
			}
			return tenths.size() >= plannedTenths;
		},
		std::chrono::seconds(60));
	INFO("judged " << tenths.size() << " tenths, excused " << excused);
	REQUIRE(held);
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
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
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
}
