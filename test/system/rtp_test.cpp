// The owner's end-to-end tests of the rtp node (docs/design/rtp.md R1c criteria 1 and 2; ported to
// ear.conf by docs/design/knobly-config.md, S5 criterion T9): the real out/ear runs a graph file
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
	                                        "route fpp:channel_1 rec:FL;\n"
	                                        "route fpp:channel_2 rec:FR;\n",
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
    "route fpp:channel_1 -> rec:FL linked",
    "route fpp:channel_2 -> rec:FR linked",
};

/// Channel `channel` of a stereo WAV, frames [from, from + frames).
std::vector<float> channelOf(const Wav& wav, int channel, std::size_t from, std::size_t frames) {
	std::vector<float> samples;
	samples.reserve(frames);
	for (std::size_t frame = from; frame < from + frames; ++frame) samples.push_back(wav.samples[frame * 2 + channel]);
	return samples;
}

/// `samples` carry `wanted` Hz at `amplitude` and not `other` Hz, checked in tenths of a second:
/// the receiver steers its resampler by the packets' arrival, so its rate wobbles by parts in ten
/// thousand, which over a whole second moves a sine's phase too far for one fit.
void checkTone(const std::vector<float>& samples, double wanted, double other) {
	INFO(wanted << " Hz, not " << other << " Hz");
	const std::span<const float> all(samples);
	constexpr std::size_t tenth = graphFrequency / 10;
	std::string profile;
	double worstAmplitude = amplitude;
	double worstResidual = 0.0;
	double worstOther = 0.0;
	for (std::size_t at = 0; at + tenth <= all.size(); at += tenth) {
		const auto window = all.subspan(at, tenth);
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

TEST_CASE("A tone sent over RTP to an rtp node lands in the file node's WAV, channel for channel",
          "[system][rtp]") {
	TempDirectory directory;
	const std::uint16_t port = freeUdpPort();
	writeGraph(directory, port);
	auto ear = runEar(directory);
	const bool ready = waitForLines(ear, runningAndLinked);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(ready);

	RtpSender sender(port, RtpPcm::l24, graphFrequency,
	                            RtpTone{{750.0, 1500.0}, amplitude});
	sender.send(graphFrequency * 3 / 2);
	REQUIRE(sender.waitSent(deadline));
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	REQUIRE(exitStatus == 0);
	CHECK(ear.hasLine("node rec stopped dropped=0"));

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
	REQUIRE(last - first + 1 >= graphFrequency * 6 / 5);   // at least 1.2 s
	// One second inside the span, after the receiver's first 0.2 s.
	const std::size_t from = first + graphFrequency / 5;
	checkTone(channelOf(wav, 0, from, graphFrequency), 750.0, 1500.0);
	checkTone(channelOf(wav, 1, from, graphFrequency), 1500.0, 750.0);
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
		const json& channel1 = levels["params"]["nodes"]["fpp"]["channel_1"];
		CHECK(channel1[0].get<double>() == 0.0);   // peak
		CHECK(channel1[1].get<double>() == 0.0);   // RMS
	}
	ear.signal(SIGTERM);
	const auto exitStatus = ear.waitForExit(deadline);
	INFO("ear printed:\n" << ear.output());
	CHECK(exitStatus == 0);
}
