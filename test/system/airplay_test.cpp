// The owner's end-to-end tests of the airplay node (docs/node-types.md#airplay): the real out/ear runs airplay nodes
// routed into file nodes; the scripted AirPlay 1 sender (test/support/airplay_sender.py,
// docs/testing.md#the-airplay-sender) finds a speaker by a multicast mDNS browse, as a phone does, and plays a tone to
// it, which the WAV must hold channel for channel at its own level whatever volume the sender asks for. Removing the
// node must leave no process, no runtime file and no listing; two nodes of one speaker name are told apart by probing.
// With airplay2 the tests run nqptp as an operator would (test/system/nqptp.hpp) and judge the listings, the identity
// across a restart and both ways nqptp's absence fails the node; AirPlay 2's sound is checked by hand
// (docs/testing.md#what-ci-does-not-prove). Every wait is for an event (a line ear prints, a notification, a line the
// sender prints, a file nqptp makes, ear's exit); a deadline only fails the test.
#include <sndfile.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ear_system.hpp"
#include "nqptp.hpp"
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

/// Every file beneath `given`, the folder given as --runtime-dir, inside which ear makes its own runtime folder
/// (docs/architecture.md#boot-and-stop): a FIFO, feed or config a node left behind.
std::vector<std::string> runtimeFilesIn(const std::filesystem::path& given) {
	std::vector<std::string> found;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(given))
		if (!entry.is_directory()) found.push_back(entry.path().string());
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
/// Each tenth's fitted tone must be above this (the tone is sent at 0.5; the sender's volume, 20 of 100, is ignored, and
/// would leave it near 0.01 if shairport-sync applied it), and the other
/// channel's frequency below a tenth of it: AirPlay 1 is lossless (ALAC), but shairport-sync keeps its own sync by
/// inserting and dropping frames, and the pipe module resamples 44.1 kHz to the graph's 48 kHz, so a level floor and
/// a separation are judged, not an exact amplitude.
constexpr double levelFloor = 0.25;

/// The best fit of a tone within 1% of `hz` (steps of 0.025%): the pipe module steers its resampler by its ring's fill
/// (docs/engine.md#pipe-receivers), so a tenth's pitch wanders by up to a few tenths of a percent, which a fit at
/// exactly `hz` would read as a level lost (a 0.4% wander halves a 1.5 kHz tone's fit over a tenth). A level or a
/// leak is judged at the tone's own pitch, as module_converter_test.cpp's tone test judges it.
SineFit nearestFit(std::span<const float> window, double hz) {
	SineFit best{0.0, INFINITY};
	for (int step = -40; step <= 40; ++step) {
		const auto fit = fitSine(window, hz * (1.0 + step * 0.00025), graphFrequency);
		if (fit.amplitude > best.amplitude) best = fit;
	}
	return best;
}

void checkTone(const Wav& wav, int channel, std::size_t from, double wanted, double other) {
	INFO("channel " << channel + 1 << ": " << wanted << " Hz, not " << other << " Hz, from frame " << from);
	std::string profile;
	double weakest = 1.0;
	double strongestOther = 0.0;
	for (std::size_t i = 0; i < judgedTenths; ++i) {
		const std::vector<float> samples = channelOf(wav, channel, from + i * tenth, tenth);
		const std::span<const float> window(samples);
		const auto fit = nearestFit(window, wanted);
		const auto leak = nearestFit(window, other);
		profile += std::to_string(fit.amplitude) + "/" + std::to_string(leak.amplitude) + " ";
		weakest = std::min(weakest, fit.amplitude);
		strongestOther = std::max(strongestOther, leak.amplitude);
	}
	INFO("wanted/other amplitude per tenth: " << profile);
	CHECK(weakest > levelFloor);
	CHECK(strongestOther < weakest / 10);
}

/// The sender's volume in every test: low, so a level that clears levelFloor proves the node ignored it.
const std::string senderVolume = "20";

/// The recording at `path` holds `left` Hz on channel 1 and `right` Hz on channel 2 at the graph's rate, judged over
/// judgedTenths from half a second after its first sound.
void checkRecording(const std::filesystem::path& path, double left, double right) {
	INFO("recording " << path);
	const Wav wav = readWav(path);
	REQUIRE(wav.info.channels == 2);
	CHECK(wav.info.samplerate == static_cast<int>(graphFrequency));
	const auto frames = static_cast<std::size_t>(wav.info.frames);
	std::size_t first = frames;
	for (std::size_t frame = 0; frame < frames && first == frames; ++frame)
		if (std::abs(wav.samples[frame * 2]) > 0.01F || std::abs(wav.samples[frame * 2 + 1]) > 0.01F) first = frame;
	REQUIRE(first < frames);
	const std::size_t from = first + graphFrequency / 2;
	REQUIRE(from + judgedTenths * tenth <= frames);
	checkTone(wav, 0, from, left, right);
	checkTone(wav, 1, from, right, left);
}

/// What a `listed` note names: the instance and its port.
struct Listed {
	std::string instance;
	int port = 0;
};

/// Node `node`'s first `listed` note, once ear printed it.
Listed listedBy(ChildProcess& process, const std::string& node) {
	const std::string prefix = "node " + node + " says listed ";
	const auto listed = process.waitForLineStartingWith(prefix, deadline);
	INFO("ear printed:\n" << process.output());
	REQUIRE(listed);
	const std::string words = listed->substr(prefix.size());
	const auto on = words.find(" on port ");
	REQUIRE(on != std::string::npos);
	return {words.substr(0, on), std::stoi(words.substr(on + std::string_view(" on port ").size()))};
}

/// The sender playing `wav` to the speaker `discovered` found, named `speaker`, at senderVolume.
ChildProcess playTo(const std::filesystem::path& wav, const json& discovered, int port, const std::string& speaker) {
	return sender({"--play", wav.string(), "--address", discovered["address"].get<std::string>(), "--port",
	               std::to_string(port), "--txt", discovered["txt"].dump(), "--speaker", speaker, "--volume",
	               senderVolume});
}

/// Node `node`'s first `count` distinct `listed` notes, once ear printed them all.
std::vector<Listed> listingsBy(ChildProcess& process, const std::string& node, std::size_t count) {
	const std::string prefix = "node " + node + " says listed ";
	std::set<std::string> seen;
	std::vector<Listed> all;
	while (all.size() < count) {
		const auto record = process.waitForRecord(
			[&](const json& r) {
				const std::string msg = r.value("msg", "");
				return msg.starts_with(prefix) && !seen.contains(msg);
			},
			deadline);
		INFO("ear printed:\n" << process.output());
		REQUIRE(record);
		const std::string msg = (*record)["msg"].get<std::string>();
		seen.insert(msg);
		const std::string words = msg.substr(prefix.size());
		const auto on = words.find(" on port ");
		REQUIRE(on != std::string::npos);
		all.push_back({words.substr(0, on), std::stoi(words.substr(on + std::string_view(" on port ").size()))});
	}
	return all;
}

/// AirPlay 2's RAOP instance's device id, from the two listings: "<12 uppercase hex>@<speaker>", the other the speaker.
std::string raopPrefixOf(const std::vector<Listed>& listings, const std::string& speaker) {
	std::vector<std::string> instances;
	for (const Listed& one : listings) instances.push_back(one.instance);
	REQUIRE(instances.size() == 2);
	const auto raop = std::ranges::find_if(instances, [&](const std::string& i) { return i.ends_with("@" + speaker); });
	REQUIRE(raop != instances.end());
	CHECK(std::ranges::count(instances, speaker) == 1);
	const std::string prefix = raop->substr(0, raop->size() - speaker.size() - 1);
	CHECK(prefix.size() == 12);
	CHECK(std::ranges::all_of(prefix, [](char c) { return std::isdigit(c) || (c >= 'A' && c <= 'F'); }));
	return prefix;
}

/// The airplay node "kitchen" (speaker name "Kitchen") with airplay2, alone.
void writeAirplay2Graph(const TempDirectory& directory) {
	writeGraphFile(directory,
	               graphFileText("node airplay kitchen { name \"Kitchen\"; airplay2 true; }\n", graphFrequency));
}

/// nqptp's start, its control port's first answer and its stop: only fail the test.
constexpr auto nqptpDeadline = std::chrono::seconds(10);

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

	// Its listing, once probed: the instance shairport-sync named, `<device id>@Kitchen`, on the port ear picked for it.
	const Listed listed = listedBy(process, "kitchen");
	CHECK(listed.instance.ends_with("@Kitchen"));
	const int port = listed.port;

	// Found as a phone finds it: by multicast, on the port the note named.
	auto browser = sender({"--browse", "Kitchen"});
	const auto discovered = waitForEvent(browser, "discovered", browseDeadline);
	INFO("browser printed:\n" << browser.output());
	REQUIRE(discovered);
	CHECK((*discovered)["port"] == port);

	{
		auto player = playTo(directory.path() / "tone.wav", *discovered, port, "Kitchen");
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
	CHECK(runtimeFilesIn(runtime) == std::vector<std::string>{});

	const auto status = ear.stop(rpc);
	INFO("ear printed:\n" << process.output());
	REQUIRE(status == 0);

	checkRecording(directory.recordings() / "rec.wav", 1000.0, 1500.0);
}

TEST_CASE("Two airplay nodes of one speaker name are told apart by probing: one renamed, both found, both play at once "
          "into their own file nodes",
          "[system][airplay]") {
	TempDirectory directory;
	const std::filesystem::path runtime = directory.path() / "runtime";
	std::filesystem::create_directories(runtime);
	writeTone(directory.path() / "kitchen.wav", 1000.0, 1500.0, 5);
	writeTone(directory.path() / "porch.wav", 700.0, 1100.0, 5);
	// AirPlay 1's instance is MD5(speaker name, MAC)@<speaker name>: the two nodes ask for the same one.
	writeGraphFile(directory, graphFileText("node airplay kitchen { name \"Kitchen\"; }\n"
	                                        "node airplay porch { name \"Kitchen\"; }\n"
	                                        "node file rec1 { path \"rec1.wav\"; }\n"
	                                        "node file rec2 { path \"rec2.wav\"; }\n"
	                                        "route kitchen:out.1 rec1:record.left;\n"
	                                        "route kitchen:out.2 rec1:record.right;\n"
	                                        "route porch:out.1 rec2:record.left;\n"
	                                        "route porch:out.2 rec2:record.right;\n",
	                                        graphFrequency));
	Ear ear(directory, {"node kitchen running"}, {"--runtime-dir", runtime.string()});
	ChildProcess& process = ear.process();
	REQUIRE(process.waitForLine("node porch running", deadline));

	const Listed kitchen = listedBy(process, "kitchen");
	const Listed porch = listedBy(process, "porch");
	INFO("ear printed:\n" << process.output());
	CHECK(kitchen.port != porch.port);
	// A rename is said before its listed, so both are in by now: exactly one node's, from the other's instance.
	const auto kitchenRenamed = process.lineStartingWith("node kitchen says renamed ");
	const auto porchRenamed = process.lineStartingWith("node porch says renamed ");
	REQUIRE(kitchenRenamed.has_value() != porchRenamed.has_value());
	const Listed& kept = kitchenRenamed ? porch : kitchen;
	const Listed& renamed = kitchenRenamed ? kitchen : porch;
	REQUIRE(kept.instance.ends_with("@Kitchen"));
	CHECK(renamed.instance == kept.instance + " (2)");
	const std::string renamer = kitchenRenamed ? "kitchen" : "porch";
	CHECK(process.lineStartingWith("node " + renamer + " says renamed ") ==
	      std::optional<std::string>("node " + renamer + " says renamed " + kept.instance + " to " + renamed.instance));

	// Each found by its own name, on its own port.
	auto keptBrowser = sender({"--browse", "Kitchen"});
	auto renamedBrowser = sender({"--browse", "Kitchen (2)"});
	const auto keptFound = waitForEvent(keptBrowser, "discovered", browseDeadline);
	const auto renamedFound = waitForEvent(renamedBrowser, "discovered", browseDeadline);
	INFO("browsers printed:\n" << keptBrowser.output() << renamedBrowser.output());
	REQUIRE(keptFound);
	REQUIRE(renamedFound);
	CHECK((*keptFound)["instance"] == kept.instance + "._raop._tcp.local.");
	CHECK((*keptFound)["port"] == kept.port);
	CHECK((*renamedFound)["instance"] == renamed.instance + "._raop._tcp.local.");
	CHECK((*renamedFound)["port"] == renamed.port);

	// Both at once, a tone each: kitchen's to the kitchen node, porch's to the porch node.
	const json& kitchenFound = kitchenRenamed ? *renamedFound : *keptFound;
	const json& porchFound = kitchenRenamed ? *keptFound : *renamedFound;
	{
		auto toKitchen = playTo(directory.path() / "kitchen.wav", kitchenFound, kitchen.port, "Kitchen");
		auto toPorch = playTo(directory.path() / "porch.wav", porchFound, porch.port, "Kitchen");
		const bool kitchenPlaying = process.waitForLine("node kitchen says playing", playDeadline);
		const bool porchPlaying = process.waitForLine("node porch says playing", playDeadline);
		const auto kitchenFinished = waitForEvent(toKitchen, "finished", playDeadline);
		const auto porchFinished = waitForEvent(toPorch, "finished", playDeadline);
		const bool kitchenIdle = process.waitForLine("node kitchen says idle", playDeadline);
		const bool porchIdle = process.waitForLine("node porch says idle", playDeadline);
		INFO("players printed:\n" << toKitchen.output() << toPorch.output());
		INFO("ear printed:\n" << process.output());
		CHECK(kitchenPlaying);
		CHECK(porchPlaying);
		REQUIRE(kitchenFinished);
		REQUIRE(porchFinished);
		CHECK(kitchenIdle);
		CHECK(porchIdle);
		CHECK(toKitchen.waitForExit(deadline) == 0);
		CHECK(toPorch.waitForExit(deadline) == 0);
	}
	keptBrowser.signal(SIGTERM);
	renamedBrowser.signal(SIGTERM);
	CHECK(keptBrowser.waitForExit(deadline) == 0);
	CHECK(renamedBrowser.waitForExit(deadline) == 0);

	const auto status = ear.stop();
	INFO("ear printed:\n" << process.output());
	REQUIRE(status == 0);
	checkRecording(directory.recordings() / "rec1.wav", 1000.0, 1500.0);
	checkRecording(directory.recordings() / "rec2.wav", 700.0, 1100.0);
}

TEST_CASE("With airplay2 and nqptp running, an airplay node lists _raop._tcp and _airplay._tcp under one device id, "
          "kept across a restart, and leaves no process or runtime file once removed",
          "[system][airplay][nqptp]") {
	Nqptp nqptp(nqptpDeadline);
	TempDirectory directory;
	const std::filesystem::path runtime = directory.path() / "runtime";
	std::filesystem::create_directories(runtime);
	writeAirplay2Graph(directory);
	std::string prefix;
	{
		Ear ear(directory, {"node kitchen running"}, {"--runtime-dir", runtime.string()});
		ChildProcess& process = ear.process();
		const std::vector<Listed> listings = listingsBy(process, "kitchen", 2);
		prefix = raopPrefixOf(listings, "Kitchen");
		CHECK(sharedMemoryExists("/ear-kitchen"));

		// Found as AirPlay 2 senders find it: _airplay._tcp, its TXT deviceid the RAOP instance's prefix.
		auto browser = sender({"--browse", "Kitchen", "--service", "airplay"});
		const auto discovered = waitForEvent(browser, "discovered", browseDeadline);
		INFO("browser printed:\n" << browser.output());
		REQUIRE(discovered);
		CHECK((*discovered)["instance"] == "Kitchen._airplay._tcp.local.");
		std::string deviceid = (*discovered)["txt"].value("deviceid", "");
		std::erase(deviceid, ':');
		std::ranges::transform(deviceid, deviceid.begin(), [](unsigned char c) { return std::toupper(c); });
		CHECK(deviceid == prefix);
		browser.signal(SIGTERM);
		CHECK(browser.waitForExit(deadline) == 0);

		const auto status = ear.stop();
		INFO("ear printed:\n" << process.output());
		REQUIRE(status == 0);
	}

	// The same host and node: the same device id, so a sender knows the speaker again.
	Ear ear(directory, {"node kitchen running"}, {"--runtime-dir", runtime.string()});
	ChildProcess& process = ear.process();
	CHECK(raopPrefixOf(listingsBy(process, "kitchen", 2), "Kitchen") == prefix);

	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	REQUIRE(rpc.call("node.remove", {{"name", "kitchen"}}).contains("result"));
	rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.gone", {{"name", "kitchen"}}); });
	CHECK(processesOf(process.pid(), "shairport-sync") == std::vector<std::string>{});
	CHECK(runtimeFilesIn(runtime) == std::vector<std::string>{});

	const auto status = ear.stop(rpc);
	INFO("ear printed:\n" << process.output());
	REQUIRE(status == 0);
}

TEST_CASE("With airplay2 and no nqptp, an airplay node fails start-failed in shairport-sync's own words",
          "[system][airplay][nqptp]") {
	TempDirectory directory;
	writeAirplay2Graph(directory);
	Ear ear(directory, {"node kitchen failed ", true});
	const auto failed = ear.process().lineStartingWith("node kitchen failed ");
	INFO("ear printed:\n" << ear.process().output());
	REQUIRE(failed);
	const std::string start = "node kitchen failed start-failed: shairport-sync exited with status ";
	CHECK(failed->starts_with(start));
	const auto colon = failed->find(": ", start.size());
	REQUIRE(colon != std::string::npos);
	CHECK(failed->find("The NQPTP service can not be found", colon) != std::string::npos);
	CHECK(processesOf(ear.process().pid(), "shairport-sync") == std::vector<std::string>{});
	REQUIRE(ear.stop() == 0);
}

TEST_CASE("With airplay2, nqptp stopping removes the node's channel, and ear ends shairport-sync and fails the node "
          "lost in the removal's words, and once nqptp runs again node.restart brings it back running and listed",
          "[system][airplay][nqptp]") {
	Nqptp nqptp(nqptpDeadline);
	TempDirectory directory;
	writeAirplay2Graph(directory);
	Ear ear(directory, {"node kitchen running"});
	ChildProcess& process = ear.process();
	REQUIRE(sharedMemoryExists("/ear-kitchen"));

	CHECK(nqptp.stop(nqptpDeadline) == 0);
	const bool lost = process.waitForLine(
		"node kitchen failed lost: shairport-sync was ended: shared memory /ear-kitchen, which it needs, was removed",
		deadline);
	INFO("ear printed:\n" << process.output());
	CHECK(lost);
	CHECK(processesOf(process.pid(), "shairport-sync") == std::vector<std::string>{});

	// The operator's recovery (docs/node-types.md#airplay-2-and-nqptp): nqptp started again, then node.restart.
	Nqptp again(nqptpDeadline);
	const std::size_t from = process.records().size();
	Rpc rpc(ear.port());
	REQUIRE(rpc.call("node.restart", {{"name", "kitchen"}}).contains("result"));
	const auto running = process.waitForRecord(
		[](const json& r) { return r.value("msg", "") == "node kitchen running"; }, deadline, from);
	INFO("ear printed:\n" << process.output());
	REQUIRE(running);
	std::set<std::string> listed;
	while (listed.size() < 2) {
		const auto record = process.waitForRecord(
			[&](const json& r) {
				const std::string msg = r.value("msg", "");
				return msg.starts_with("node kitchen says listed ") && !listed.contains(msg);
			},
			deadline, from);
		REQUIRE(record);
		listed.insert((*record)["msg"].get<std::string>());
	}
	CHECK(sharedMemoryExists("/ear-kitchen"));
	REQUIRE(ear.stop(rpc) == 0);
}
