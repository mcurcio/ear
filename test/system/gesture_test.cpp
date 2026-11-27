// Gestures end to end (docs/control-port.md#gestures): the real out/ear with white noise through a gain node into one
// channel of a file node and straight into the other, so the WAV's left over its right is the gain node's factor.
// Two clients: one holds amp's gainDb, the other is refused parameter-held until the silent holder is released stale
// (ear started with a short silence by --gesture-stale-ms); a hand edit elsewhere leaves a gesture open, and one of the
// held value aborts it; a client reset mid-gesture, its last tick refused, leaves its last accepted value in ear.conf
// and the WAV.
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

const std::string gainGraph = "node white noise;\n"
							  "node gain amp;\n"
							  "node file rec { path \"rec.wav\"; }\n"
							  "route noise:noise.mono amp:in.1;\n"
							  "route amp:out.1 rec:record.left;\n"
							  "route noise:noise.mono rec:record.right;\n";

std::string graphFileOf(const TempDirectory& directory) {
	std::ifstream in(directory.config() / "ear.conf", std::ios::binary);
	return {std::istreambuf_iterator<char>(in), {}};
}

bool gestureChanged(const json& message, const std::string& phase) {
	return Rpc::matches(message, "gesture.changed", {{"phase", phase}});
}

/// The factor left/right holds over the WAV's last second, read where the noise is loud enough.
double lastFactor(const std::filesystem::path& wav) {
	const Wav finished = readWav(wav);
	const std::span<const float> samples(finished.samples);
	const std::size_t frames = samples.size() / 2;
	double factor = std::nan("");
	for (std::size_t frame = frames > 48000 ? frames - 48000 : 0; frame < frames; ++frame)
		if (std::abs(samples[2 * frame + 1]) > 0.05F)
			factor = static_cast<double>(samples[2 * frame]) / static_cast<double>(samples[2 * frame + 1]);
	return factor;
}

double factorOf(double db) { return std::pow(10.0, db / 20.0); }

} // namespace

TEST_CASE("A held gainDb refuses another client's node.set parameter-held until its silent holder is released stale",
          "[system][gestures]") {
	TempDirectory directory;
	writeGraphFile(directory, graphFileText(gainGraph));
	Ear ear(directory, {linkedLine("amp:out.1", "rec:record.left")}, {"--gesture-stale-ms", "1500"});
	ChildProcess& process = ear.process();
	REQUIRE(process.waitForLine("node amp running", deadline));
	std::optional<Rpc> holder(std::in_place, ear.port());
	Rpc other(ear.port());
	REQUIRE(other.call("graph.subscribe").contains("result"));
	const json begun = holder->call("gesture.begin", {{"name", "amp"}, {"key", "gainDb"}});
	INFO(begun.dump());
	REQUIRE(begun.contains("result"));
	const std::string id = begun["result"]["gesture"];
	CHECK(begun["result"]["reach"] == json::array({{{"name", "amp"}, {"key", "gainDb"}}}));
	holder->notify("gesture.change", {{"gesture", id}, {"seq", 1}, {"value", -12}});
	CHECK(other.waitFor([](const json& m) { return gestureChanged(m, "change"); })["params"]["value"] == -12);
	const json held = other.call("node.set", {{"name", "amp"}, {"settings", {{"gainDb", -3}}}});
	INFO(held.dump());
	REQUIRE(held.contains("error"));
	CHECK(held["error"]["data"]["code"] == "parameter-held");
	CHECK(held["error"]["data"]["gesture"] == id);
	CHECK(held["error"]["data"]["silentMs"].is_number_unsigned());
	// The holder falls silent: released stale with its last accepted value.
	const json released = other.waitFor([](const json& m) { return gestureChanged(m, "end"); });
	CHECK(released["params"]["reason"] == "stale");
	CHECK(released["params"]["value"] == -12);
	REQUIRE(process.waitForLine("node amp gainDb 0 -> -12", deadline));
	const json set = other.call("node.set", {{"name", "amp"}, {"settings", {{"gainDb", -3}}}});
	INFO(set.dump());
	CHECK(set["result"]["applied"] == "retune");
	holder.reset(); // a client that stops reading holds ear's stop (docs/control-port.md#traps)
	REQUIRE(ear.stop(other) == 0);
	const auto& records = process.records();
	const auto param = std::ranges::find_if(records, [](const json& r) { return r.value("kind", "") == "param"; });
	REQUIRE(param != records.end());
	CHECK((*param)["cause"] == "gesture");
	CHECK((*param)["gesture"] == id);
	CHECK((*param)["reason"] == "stale");
}

TEST_CASE("A hand edit elsewhere leaves a gesture open, and one of the held value aborts it, the audio going straight "
          "to the new level",
          "[system][gestures]") {
	TempDirectory directory;
	writeGraphFile(directory, graphFileText(gainGraph));
	Ear ear(directory, {linkedLine("amp:out.1", "rec:record.left")});
	ChildProcess& process = ear.process();
	REQUIRE(process.waitForLine("node amp running", deadline));
	Rpc holder(ear.port());
	REQUIRE(holder.call("graph.subscribe").contains("result"));
	const json begun = holder.call("gesture.begin", {{"name", "amp"}, {"key", "gainDb"}});
	REQUIRE(begun.contains("result"));
	const std::string id = begun["result"]["gesture"];
	holder.notify("gesture.change", {{"gesture", id}, {"seq", 1}, {"value", -12}});
	// A comment added: not related, the gesture stays open.
	writeGraphFile(directory, graphFileText(gainGraph + "# a note\n"));
	REQUIRE(process.waitForLineStartingWith("graph-file disk", deadline));
	const json graph = holder.call("graph.get");
	const auto amp = std::ranges::find_if(graph["result"]["nodes"], [](const json& n) { return n["name"] == "amp"; });
	REQUIRE(amp != graph["result"]["nodes"].end());
	CHECK((*amp)["gestures"] == json::array({{{"gesture", id}, {"key", "gainDb"}, {"value", -12}}}));
	// The held value edited by hand: aborted, and the audio goes to -20 once.
	std::string edited = gainGraph;
	edited.replace(edited.find("node gain amp;"), std::string("node gain amp;").size(),
	               "node gain amp { gainDb -20; }");
	writeGraphFile(directory, graphFileText(edited));
	const json aborted = holder.waitFor([](const json& m) { return gestureChanged(m, "aborted"); });
	CHECK(aborted["params"]["reason"] == "hand-edit");
	REQUIRE(process.waitForLine("node amp gainDb 0 -> -20", deadline));
	const auto wav = directory.recordings() / "rec.wav";
	REQUIRE(waitForWav(
		wav,
		[n = std::size_t{0}](std::span<const float> fresh, std::size_t channels) mutable {
			n += fresh.size() / channels;
			return n >= 96000;
		},
		4 * deadline));
	REQUIRE(ear.stop(holder) == 0);
	REQUIRE(headerFinished(wav));
	CHECK(lastFactor(wav) == Approx(factorOf(-20.0)).epsilon(1e-4));
}

TEST_CASE("A client reset mid-gesture, its last tick refused, leaves its last accepted value in ear.conf and the WAV",
          "[system][gestures]") {
	TempDirectory directory;
	writeGraphFile(directory, graphFileText(gainGraph));
	Ear ear(directory, {linkedLine("amp:out.1", "rec:record.left")});
	ChildProcess& process = ear.process();
	REQUIRE(process.waitForLine("node amp running", deadline));
	Rpc other(ear.port());
	REQUIRE(other.call("graph.subscribe").contains("result"));
	std::string id;
	{
		Rpc holder(ear.port());
		const json begun = holder.call("gesture.begin", {{"name", "amp"}, {"key", "gainDb"}});
		REQUIRE(begun.contains("result"));
		id = begun["result"]["gesture"].get<std::string>();
		holder.notify("gesture.change", {{"gesture", id}, {"seq", 1}, {"value", -12}});
		other.waitFor([](const json& m) { return gestureChanged(m, "change"); });
		holder.notify("gesture.change", {{"gesture", id}, {"seq", 2}, {"value", 30}}); // above 24 dB: refused
		holder.waitFor([](const json& m) { return gestureChanged(m, "tick-refused"); });
		holder.reset();
	}
	const json released = other.waitFor([](const json& m) { return gestureChanged(m, "end"); });
	CHECK(released["params"]["reason"] == "session-closed");
	CHECK(released["params"]["value"] == -12);
	REQUIRE(process.waitForLineStartingWith("graph-file written", deadline));
	CHECK(graphFileOf(directory).find("node gain amp { gainDb -12; }") != std::string::npos);
	const json graph = other.call("graph.get");
	for (const json& node : graph["result"]["nodes"]) CHECK(node["gestures"].empty());
	const auto wav = directory.recordings() / "rec.wav";
	REQUIRE(waitForWav(
		wav,
		[n = std::size_t{0}](std::span<const float> fresh, std::size_t channels) mutable {
			n += fresh.size() / channels;
			return n >= 96000;
		},
		4 * deadline));
	REQUIRE(ear.stop(other) == 0);
	REQUIRE(headerFinished(wav));
	CHECK(lastFactor(wav) == Approx(factorOf(-12.0)).epsilon(1e-4));
}
