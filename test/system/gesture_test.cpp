// Gestures end to end (docs/control-port.md#gestures): the real out/ear with white noise through a gain node into one
// channel of a file node and straight into the other, so the WAV's left over its right is the gain node's factor.
// Two clients: one holds amp's gainDb, the other is refused parameter-held until the silent holder is released stale
// (ear started with a short silence by --gesture-stale-ms); a hand edit elsewhere leaves a gesture open, and one of the
// held value aborts it; a client reset mid-gesture, its last tick refused, leaves its last accepted value in ear.conf
// and the WAV.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

double factorOf(double db) { return std::pow(10.0, db / 20.0); }

/// Left over right at `frame` of interleaved stereo `samples`, or nothing where the noise is too quiet to read it.
std::optional<double> factorAt(const std::vector<float>& samples, std::size_t frame) {
	const float right = samples[2 * frame + 1];
	if (std::abs(right) < 0.01F) return std::nullopt;
	return static_cast<double>(samples[2 * frame]) / static_cast<double>(right);
}

/// The WAV followed until a second of it has passed its first frame at `db`'s factor: the level reached and held is
/// the event. Every frame read, interleaved. Each was written before the test asks ear to stop, so the stop's fades,
/// which a loaded host can let into the WAV's tail (docs/architecture.md#fades), are never judged.
std::vector<float> followUntilHeld(const std::filesystem::path& wav, double db) {
	const double target = factorOf(db);
	std::vector<float> samples;
	std::optional<std::size_t> reached; // the first frame at the target
	const bool held = waitForWav(
		wav,
		[&](std::span<const float> fresh, std::size_t channels) {
			REQUIRE(channels == 2);
			samples.insert(samples.end(), fresh.begin(), fresh.end());
			for (std::size_t frame = samples.size() / 2 - fresh.size() / 2; !reached && frame < samples.size() / 2;
		         ++frame)
				if (const auto factor = factorAt(samples, frame); factor && std::abs(*factor - target) < 1e-5)
					reached = frame;
			return reached && samples.size() / 2 >= *reached + 48000;
		},
		4 * deadline);
	INFO("frames read: " << samples.size() / 2);
	REQUIRE(held);
	return samples;
}

/// Past both nodes' 5 ms fade-ins (each starts at its own drive) the factor only falls, never below `db`'s, and from
/// its first frame there stays at it: the audio ramps to each new value once, never back up to the committed 0 dB. The
/// recording may begin at unity or already at a gesture's value.
void checkFallsOnceTo(const std::vector<float>& samples, double db) {
	const double target = factorOf(db);
	const std::size_t frames = samples.size() / 2;
	std::size_t first = 0;
	while (first < frames && samples[2 * first + 1] == 0.0F) ++first;
	std::size_t firstLeft = first;
	while (firstLeft < frames && samples[2 * firstLeft] == 0.0F) ++firstLeft;
	std::size_t frame = std::max(first, firstLeft) + 480;
	REQUIRE(frame < frames);
	double before = 1.0;
	bool reached = false;
	for (; frame < frames; ++frame) {
		const auto factor = factorAt(samples, frame);
		if (!factor) continue;
		INFO("frame " << frame << " of " << frames << ", factor " << *factor);
		reached = reached || std::abs(*factor - target) < 1e-5;
		if (reached) REQUIRE(*factor == Approx(target).epsilon(1e-4));
		REQUIRE(*factor <= before + 1e-5);
		REQUIRE(*factor >= target - 1e-5);
		before = *factor;
	}
	REQUIRE(reached);
}

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
	checkFallsOnceTo(followUntilHeld(wav, -20.0), -20.0);
	REQUIRE(ear.stop(holder) == 0);
	REQUIRE(headerFinished(wav));
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
	checkFallsOnceTo(followUntilHeld(wav, -12.0), -12.0);
	REQUIRE(ear.stop(other) == 0);
	REQUIRE(headerFinished(wav));
}
