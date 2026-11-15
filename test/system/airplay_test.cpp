// The owner's end-to-end test of the airplay node (docs/node-types.md#airplay): the real out/ear runs an airplay node
// routed into a file node; the scripted AirPlay 1 sender (test/support/airplay_sender.py,
// docs/testing.md#the-airplay-sender) finds the speaker by a multicast mDNS browse, as a phone does, and plays a tone to
// it, which the WAV must hold channel for channel. Removing the node must leave no process, no runtime file and no
// listing. Every wait is for an event (a line ear prints, a notification, a line the sender prints, ear's exit); a
// deadline only fails the test.
#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ear_system.hpp"
#include "rtp_tone.hpp"

namespace {

using namespace ear::test;

constexpr std::uint32_t graphFrequency = 48000;
constexpr std::uint32_t senderFrequency = 44100; // AirPlay 1's
constexpr double amplitude = 0.5;
/// The sender's connect, a 5 s tone sent at its pace, and shairport-sync's session around it: only fails the test.
constexpr auto playDeadline = std::chrono::seconds(60);
/// A browse's first answer, and a goodbye's arrival: only fail the test.
constexpr auto browseDeadline = std::chrono::seconds(30);

/// airplay node "kitchen" (speaker name "Kitchen") routed channel for channel into file node "rec" (rec.wav).
void writeGraph(const TempDirectory& directory) {
	writeGraphFile(directory, graphFileText("node airplay kitchen { name \"Kitchen\"; }\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route kitchen:out.1 rec:record.left;\n"
	                                        "route kitchen:out.2 rec:record.right;\n",
	                                        graphFrequency));
}

/// A 16-bit stereo WAV at AirPlay 1's rate: `left` Hz on channel 1, `right` Hz on channel 2, `seconds` long.
void writeTone(const std::filesystem::path& path, double left, double right, int seconds) {
	SF_INFO info{};
	info.samplerate = static_cast<int>(senderFrequency);
	info.channels = 2;
	info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
	REQUIRE(file != nullptr);
	std::vector<float> frames;
	const std::size_t count = static_cast<std::size_t>(seconds) * senderFrequency;
	frames.reserve(count * 2);
	for (std::size_t i = 0; i < count; ++i) {
		const double t = static_cast<double>(i) / senderFrequency;
		frames.push_back(static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * left * t)));
		frames.push_back(static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * right * t)));
	}
	REQUIRE(sf_writef_float(file, frames.data(), static_cast<sf_count_t>(count)) == static_cast<sf_count_t>(count));
	REQUIRE(sf_close(file) == 0);
}

/// The scripted sender, run by the image's venv (EAR_AIRPLAY_SENDER_PYTHON) in isolated mode.
ChildProcess sender(const std::vector<std::string>& arguments) {
	std::vector<std::string> all{"-I", EAR_AIRPLAY_SENDER_SCRIPT};
	all.insert(all.end(), arguments.begin(), arguments.end());
	return ChildProcess(EAR_AIRPLAY_SENDER_PYTHON, all);
}

/// The sender's next event `event` (one JSON object a line), once it printed it.
std::optional<json> waitForEvent(ChildProcess& process, std::string_view event, std::chrono::milliseconds patience) {
	const auto line = process.waitForLineStartingWith(R"({"event": ")" + std::string(event) + "\"", patience);
	if (!line) return std::nullopt;
	return json::parse(*line);
}

/// Every process whose parent is `parent`, and every one named `name`, by /proc/<pid>/stat: "<pid> (<comm>) <state>
/// <ppid> ...", the comm read up to its last ')'.
std::vector<std::string> processesOf(pid_t parent, std::string_view name) {
	std::vector<std::string> found;
	for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
		const std::string pid = entry.path().filename().string();
		if (pid.find_first_not_of("0123456789") != std::string::npos) continue;
		std::ifstream stat(entry.path() / "stat");
		std::string text;
		if (!std::getline(stat, text)) continue; // it ended meanwhile
		const auto open = text.find('(');
		const auto close = text.rfind(')');
		if (open == std::string::npos || close == std::string::npos) continue;
		const std::string comm = text.substr(open + 1, close - open - 1);
		std::istringstream rest(text.substr(close + 2));
		std::string state;
		pid_t ppid = 0;
		rest >> state >> ppid;
		if (ppid == parent || comm == name) found.push_back(pid + " (" + comm + ") parent " + std::to_string(ppid));
	}
	return found;
}

/// Channel `channel` of a stereo WAV, frames [from, from + frames).
std::vector<float> channelOf(const Wav& wav, int channel, std::size_t from, std::size_t frames) {
	std::vector<float> samples;
	samples.reserve(frames);
	for (std::size_t frame = from; frame < from + frames; ++frame) samples.push_back(wav.samples[frame * 2 + channel]);
	return samples;
}

constexpr std::size_t tenth = graphFrequency / 10;
/// The tenths judged: three seconds of the five-second tone, from half a second after it is first heard, so neither
/// the session's start nor its end is inside.
constexpr std::size_t judgedTenths = 30;
/// Each tenth's fitted tone must be above this (the tone is sent at 0.5, at the sender's full volume), and the other
/// channel's frequency below a tenth of it: AirPlay 1 is lossless (ALAC), but shairport-sync keeps its own sync by
/// inserting and dropping frames, and the pipe module resamples 44.1 kHz to the graph's 48 kHz, so a level floor and
/// a separation are judged, not an exact amplitude.
constexpr double levelFloor = 0.25;

void checkTone(const Wav& wav, int channel, std::size_t from, double wanted, double other) {
	INFO("channel " << channel + 1 << ": " << wanted << " Hz, not " << other << " Hz, from frame " << from);
	std::string profile;
	double weakest = 1.0;
	double strongestOther = 0.0;
	for (std::size_t i = 0; i < judgedTenths; ++i) {
		const std::vector<float> samples = channelOf(wav, channel, from + i * tenth, tenth);
		const std::span<const float> window(samples);
		const auto fit = fitSine(window, wanted, graphFrequency);
		const auto leak = fitSine(window, other, graphFrequency);
		profile += std::to_string(fit.amplitude) + "/" + std::to_string(leak.amplitude) + " ";
		weakest = std::min(weakest, fit.amplitude);
		strongestOther = std::max(strongestOther, leak.amplitude);
	}
	INFO("wanted/other amplitude per tenth: " << profile);
	CHECK(weakest > levelFloor);
	CHECK(strongestOther < weakest / 10);
}

} // namespace

TEST_CASE("An airplay node is found by an mDNS browse, plays an AirPlay 1 tone into a file node channel for channel, "
          "and leaves no process, runtime file or listing once removed",
          "[system][airplay]") {
	TempDirectory directory;
	const std::filesystem::path runtime = directory.path() / "runtime";
	std::filesystem::create_directories(runtime);
	writeTone(directory.path() / "tone.wav", 1000.0, 1500.0, 5);
	writeGraph(directory);
	Ear ear(directory, {"node kitchen running"}, {"--runtime-dir", runtime.string()});
	ChildProcess& process = ear.process();

	// Its listing: the instance shairport-sync named, `<device id>@Kitchen`, on the port ear picked for it.
	const auto listed = process.waitForLineStartingWith("node kitchen says listed ", deadline);
	INFO("ear printed:\n" << process.output());
	REQUIRE(listed);
	const std::string words = listed->substr(std::string_view("node kitchen says listed ").size());
	const auto on = words.find(" on port ");
	REQUIRE(on != std::string::npos);
	CHECK(words.substr(0, on).ends_with("@Kitchen"));
	const auto port = std::stoi(words.substr(on + std::string_view(" on port ").size()));

	// Found as a phone finds it: by multicast, on the port the note named.
	auto browser = sender({"--browse", "Kitchen"});
	const auto discovered = waitForEvent(browser, "discovered", browseDeadline);
	INFO("browser printed:\n" << browser.output());
	REQUIRE(discovered);
	CHECK((*discovered)["port"] == port);

	{
		auto player = sender({"--play", (directory.path() / "tone.wav").string(), "--address",
		                      (*discovered)["address"].get<std::string>(), "--port", std::to_string(port), "--txt",
		                      (*discovered)["txt"].dump(), "--speaker", "Kitchen"});
		const bool playing = process.waitForLine("node kitchen says playing", playDeadline);
		const auto finished = waitForEvent(player, "finished", playDeadline);
		const bool idle = process.waitForLine("node kitchen says idle", playDeadline);
		INFO("player printed:\n" << player.output());
		INFO("ear printed:\n" << process.output());
		CHECK(playing);
		REQUIRE(finished);
		CHECK(idle);
		CHECK(player.waitForExit(deadline) == 0);
	}

	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	REQUIRE(rpc.call("node.remove", {{"name", "kitchen"}}).contains("result"));
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.gone", {{"name", "kitchen"}}); });

	// Gone from the network (the advertiser's goodbye at the teardown), from the process table (shairport-sync reaped)
	// and from the runtime folder (its FIFO, feed and config removed).
	const auto removed = waitForEvent(browser, "removed", browseDeadline);
	INFO("browser printed:\n" << browser.output());
	CHECK(removed);
	CHECK(processesOf(process.pid(), "shairport-sync") == std::vector<std::string>{});
	std::vector<std::string> left;
	for (const auto& entry : std::filesystem::directory_iterator(runtime)) left.push_back(entry.path().string());
	CHECK(left == std::vector<std::string>{});

	const auto status = ear.stop(rpc);
	INFO("ear printed:\n" << process.output());
	REQUIRE(status == 0);

	const Wav wav = readWav(directory.recordings() / "rec.wav");
	REQUIRE(wav.info.channels == 2);
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	const auto frames = static_cast<std::size_t>(wav.info.frames);
	std::size_t first = frames;
	for (std::size_t frame = 0; frame < frames && first == frames; ++frame)
		if (std::abs(wav.samples[frame * 2]) > 0.01F || std::abs(wav.samples[frame * 2 + 1]) > 0.01F) first = frame;
	REQUIRE(first < frames);
	const std::size_t from = first + graphFrequency / 2;
	REQUIRE(from + judgedTenths * tenth <= frames);
	checkTone(wav, 0, from, 1000.0, 1500.0);
	checkTone(wav, 1, from, 1500.0, 1000.0);
}
