// The gain node type end to end (docs/node-types.md#gain): the real out/ear routes white noise through a gain node into
// one channel of a file node and the same noise straight into the other, so the WAV's left over its right is the gain
// node's factor, frame by frame. A node.set of gainDb mid-recording is a retune (docs/architecture.md#retunes): the
// factor ramps from 1 to 0.1 over 20 ms, with no gap and no node record, and a param record says it.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "ear_system.hpp"

namespace {

using namespace ear::test;

/// Until the WAV at `path` holds `frames` more frames of noise on its right channel after the first one heard. Every
/// frame it read is counted in `followed`.
bool waitForMoreNoise(const std::filesystem::path& path, std::size_t frames, std::size_t& followed) {
	std::optional<std::size_t> sound;
	followed = 0;
	return waitForWav(
		path,
		[&](std::span<const float> fresh, std::size_t channels) {
			followed += fresh.size() / channels;
			for (std::size_t at = 1; at < fresh.size(); at += channels) {
				if (sound) ++*sound;
				else if (fresh[at] != 0.0F) sound = 1;
			}
			return sound.value_or(0) >= frames;
		},
		4 * deadline);
}

/// Until the WAV at `path` holds a second of noise at `target` times its right channel on its left, frame after frame:
/// the retune reached the audio and held is the event the checks need, however long a loaded host takes to get there.
/// Every frame it read is counted in `followed`.
bool waitForTargetHeld(const std::filesystem::path& path, float target, std::size_t& followed) {
	std::size_t held = 0; // frames at the target in a row, a silent right one neither counted nor breaking the run
	followed = 0;
	return waitForWav(
		path,
		[&](std::span<const float> fresh, std::size_t channels) {
			followed += fresh.size() / channels;
			for (std::size_t at = 0; at + 1 < fresh.size(); at += channels) {
				if (fresh[at + 1] == 0.0F) continue;
				held = fresh[at] == fresh[at + 1] * target ? held + 1 : 0;
			}
			return held >= 48000;
		},
		4 * deadline);
}

} // namespace

TEST_CASE("A node.set of gainDb mid-recording ramps the WAV's level over 20 ms with no gap and no node record",
          "[system][gain][live]") {
	TempDirectory directory;
	writeGraphFile(directory, graphFileText("node white noise;\n"
	                                        "node gain amp;\n"
	                                        "node file rec { path \"rec.wav\"; }\n"
	                                        "route noise:noise.mono amp:in.1;\n"
	                                        "route amp:out.1 rec:record.left;\n"
	                                        "route noise:noise.mono rec:record.right;\n"));
	Ear ear(directory, {linkedLine("amp:out.1", "rec:record.left")});
	ChildProcess& process = ear.process();
	REQUIRE(process.waitForLine(linkedLine("noise:noise.mono", "rec:record.right"), deadline));
	REQUIRE(process.waitForLine("node amp running", deadline));
	const auto wav = directory.recordings() / "rec.wav";
	std::size_t followed = 0;
	REQUIRE(waitForMoreNoise(wav, 24000, followed)); // half a second at unity before the change

	Rpc rpc(ear.port());
	REQUIRE(rpc.call("graph.subscribe").contains("result"));
	const json set = rpc.call("node.set", {{"name", "amp"}, {"settings", {{"gainDb", -20}}}});
	INFO(set.dump());
	REQUIRE(set.contains("result"));
	CHECK(set["result"]["applied"] == "retune");
	CHECK(set["result"]["retuned"] == json::array({"gainDb"}));
	const json updated = rpc.waitFor([](const json& m) { return Rpc::matches(m, "node.updated", {{"name", "amp"}}); });
	CHECK(updated["params"]["applied"] == "retune");
	REQUIRE(process.waitForLine("node amp gainDb 0 -> -20", deadline));
	// And a second of the WAV at the new level: this look follows the WAV from its start again.
	const float target = std::exp2(-20.0F * 0.16609640474F);
	REQUIRE(waitForTargetHeld(wav, target, followed));
	const std::size_t heard = rpc.notifications.size();

	REQUIRE(ear.stop(rpc) == 0);
	INFO("ear printed:\n" << process.output());
	// No node.state of amp before ear's stop removes it.
	CHECK(std::ranges::none_of(
		rpc.notifications.begin(), rpc.notifications.begin() + static_cast<std::ptrdiff_t>(heard),
		[](const json& m) { return m.value("method", "") == "node.state" && m["params"].value("name", "") == "amp"; }));
	// Standard output: the param record, and no node record for amp from its running until its removal at the stop.
	const auto& records = process.records();
	const auto running =
		std::ranges::find_if(records, [](const json& r) { return r.value("msg", "") == "node amp running"; });
	REQUIRE(running != records.end());
	const auto param = std::ranges::find_if(records, [](const json& r) { return r.value("kind", "") == "param"; });
	REQUIRE(param != records.end());
	CHECK((*param)["node"] == "amp");
	CHECK((*param)["key"] == "gainDb");
	CHECK((*param)["from"] == 0);
	CHECK((*param)["to"] == -20);
	CHECK((*param)["cause"] == "edit");
	for (auto at = running + 1; at != records.end(); ++at) {
		const std::string msg = at->value("msg", "");
		if (at->value("kind", "") == "node" && msg.starts_with("node amp "))
			CHECK(msg.starts_with("node amp stopping"));
		if (msg.starts_with("node amp stopping")) break; // ear's stop
	}

	REQUIRE(headerFinished(wav));
	const Wav finished = readWav(wav);
	REQUIRE(finished.info.channels == 2);
	const std::span<const float> samples(finished.samples);
	// Only the frames read before the stop was asked: the stop fades each node out by its own drive, and on a loaded host
	// the file node's silence can land a cycle after the gain node's fade began, so the WAV's tail may hold that fade
	// (docs/architecture.md#fades); every frame read before the stop was written before it.
	REQUIRE(samples.size() / 2 >= followed);
	const std::size_t frames = followed;
	const auto left = [&](std::size_t frame) { return samples[2 * frame]; };
	const auto right = [&](std::size_t frame) { return samples[2 * frame + 1]; };
	std::size_t first = 0;
	while (first < frames && right(first) == 0.0F) ++first;
	std::size_t firstLeft = first;
	while (firstLeft < frames && left(firstLeft) == 0.0F) ++firstLeft;
	// Past both nodes' 5 ms fade-ins (each starts at its own drive): from there to the stop, left is right at unity
	// until the ramp, then right × 0.1, with every frame between moving the factor one way only.
	const std::size_t settled = std::max(first, firstLeft) + 480;
	REQUIRE(settled + 48000 < frames);
	std::optional<std::size_t> lastUnity;
	std::optional<std::size_t> firstTarget;
	for (std::size_t frame = settled; frame < frames; ++frame) {
		if (right(frame) != 0.0F && left(frame) == right(frame) && !firstTarget) lastUnity = frame;
		if (!firstTarget && right(frame) != 0.0F && left(frame) == right(frame) * target) firstTarget = frame;
	}
	REQUIRE(lastUnity);
	REQUIRE(firstTarget);
	INFO("unity until " << *lastUnity << ", the target from " << *firstTarget << " of " << frames);
	// The ramp's 960 frames end on the target: no frame of it at unity, none after it off the target.
	CHECK(*firstTarget > *lastUnity);
	CHECK(*firstTarget - *lastUnity <= 960);
	// Never a step: a noise sample landing on unity or the target ends the span a frame early, and a cycle the driver cut
	// short after the gain node ran it but before the file node did takes a block of the ramp out of the WAV
	// (docs/engine.md#the-one-data-loop-and-invokes).
	CHECK(*firstTarget - *lastUnity >= 960 - 256 - 2);
	double before = 1.0;
	for (std::size_t frame = *lastUnity + 1; frame < frames; ++frame) {
		INFO("frame " << frame);
		if (std::abs(right(frame)) < 0.01F) continue; // the ratio is too coarse to read near zero
		const double factor = static_cast<double>(left(frame)) / static_cast<double>(right(frame));
		if (frame >= *firstTarget) {
			REQUIRE(left(frame) == right(frame) * target);
		} else {
			REQUIRE(factor <= before + 1e-5);
			REQUIRE(factor >= static_cast<double>(target) - 1e-5);
			before = factor;
		}
	}
	// No gap: the noise runs on both channels to the stop, never two frames of silence in a row.
	for (std::size_t frame = settled; frame < frames; ++frame) {
		INFO("frame " << frame);
		REQUIRE_FALSE((right(frame) == 0.0F && right(frame - 1) == 0.0F));
		REQUIRE_FALSE((left(frame) == 0.0F && left(frame - 1) == 0.0F));
	}
}
