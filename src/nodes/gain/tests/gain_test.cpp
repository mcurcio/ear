// The "gain" node type through its type harness, by the handles its spec declares (gain_spec.hpp;
// docs/writing-a-node-type.md#testing-a-type: its class, run over a real NodeHost). Every value is read from the
// audio its code writes. What holds for every type together is test/nodes/'s.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/gain.hpp"
#include "ear/testing/type_harness.hpp"
#include "gain_spec.hpp"

namespace {
namespace gain = ear::nodes::gain;
using Harness = ear::testing::TypeHarness<gain::spec, gain::Gain>;
using ear::NodeSettings;

constexpr ear::AudioFormat format{.rate = ear::SampleRate::fixed<48000>(), .maxBlock = 256};

/// Every input channel holds `level` on every frame.
void feed(Harness& h, float level) {
	for (std::size_t c = 0; c < h->ports().bank("in").ports.size(); ++c) std::ranges::fill(h.input(gain::in, c), level);
}

/// A running node with `settings`, every input at `level`, past the start's 5 ms fade-in and every ramp a seeded value
/// could start (none: the slots are seeded, so nothing ramps at the first drive).
void running(Harness& h, const NodeSettings& settings, float level = 0.5F) {
	REQUIRE_FALSE(h.parse(settings));
	h.add();
	h.blockReady();
	if (h.phase() != "running") h->completeBarrier(); // autoComplete off: the start's drive
	REQUIRE(h.phase() == "running");
	feed(h, level);
	h.process();
	h.process();
	REQUIRE(h.lastFadeGain(gain::out) == 1.0F);
}

/// Every frame of out.<channel + 1> over the last cycle is `value`, exactly.
bool plays(const Harness& h, std::size_t channel, float value) {
	return std::ranges::all_of(h.output(gain::out, channel), [value](float sample) { return sample == value; });
}

/// Runs `total` frames in blocks of `block`, collecting out.<channel + 1> frame by frame.
std::vector<float> outputOver(Harness& h, std::uint32_t total, std::uint32_t block = 256, std::size_t channel = 0) {
	std::vector<float> frames;
	for (std::uint32_t done = 0; done < total; done += block) {
		const std::uint32_t size = std::min(block, total - done);
		h.process(size);
		const std::span<const float> part = h.output(gain::out, channel);
		frames.insert(frames.end(), part.begin(), part.begin() + size);
	}
	return frames;
}

bool told(Harness& h, ear::NodePhase phase) {
	return std::ranges::any_of(h->healths(), [&](const ear::NodeHealth& one) { return one.phase == phase; });
}

} // namespace

TEST_CASE("Gain declares in.N and out.N by channels, two by default, and every row but channels is live", "[gain]") {
	CHECK(ear::nodes::makeGainNodeType()->name() == "gain");
	Harness h;
	REQUIRE_FALSE(h.parse(NodeSettings::object()));
	CHECK(h->ports().inputs() == std::vector<ear::PortName>{"in.1", "in.2"});
	CHECK(h->ports().outputs() == std::vector<ear::PortName>{"out.1", "out.2"});
	REQUIRE_FALSE(h.parse({{"channels", 64}}));
	CHECK(h->ports().inputs().size() == 64);
	CHECK(h->ports().outputs().back() == "out.64");

	const auto type = ear::nodes::makeGainNodeType();
	CHECK(type->changeOf("channels") == ear::setting::Change::inPlace);
	for (const std::string_view key : {"gainDb", "mute", "trimDb.1", "trimDb.64", "invert.1", "invert.64"}) {
		INFO(key);
		CHECK(type->changeOf(key) == ear::setting::Change::live);
	}
	CHECK_FALSE(type->liveIndexOf("trimDb.65"));
}

TEST_CASE("Gain passes unity at 0 dB, halves near -6 dB and mutes to zero, once steady", "[gain]") {
	Harness h({.format = format});
	running(h, NodeSettings::object());
	CHECK(plays(h, 0, 0.5F));
	CHECK(plays(h, 1, 0.5F));

	Harness six({.format = format});
	running(six, {{"gainDb", -6.0}});
	CHECK(six.output(gain::out, 0)[0] == Approx(0.5 * 0.5012).epsilon(1e-4));
	CHECK(plays(six, 0, 0.5F * gain::dbToFactor(-6.0F)));
	CHECK(plays(six, 1, 0.5F * gain::dbToFactor(-6.0F)));

	Harness muted({.format = format});
	running(muted, {{"mute", true}});
	CHECK(plays(muted, 0, 0.0F));
	CHECK(plays(muted, 1, 0.0F));
}

TEST_CASE("Gain at its minimum is exact zeros, whatever the trim adds", "[gain]") {
	Harness h({.format = format});
	running(h, {{"gainDb", -90.0}, {"trimDb.1", 24.0}, {"trimDb.2", 24.0}, {"invert.2", true}}, 1.0F);
	for (int block = 0; block < 4; ++block) {
		h.process();
		CHECK(plays(h, 0, 0.0F));
		CHECK(plays(h, 1, 0.0F));
		CHECK_FALSE(std::signbit(h.output(gain::out, 0)[0])); // +0, not -0: the factor itself is 0
	}
	// Moved there live, the ramp ends on exact zeros too.
	Harness live({.format = format});
	running(live, {{"trimDb.1", 24.0}}, 1.0F);
	live.live<gain::gainRow>(-90.0);
	const std::vector<float> frames = outputOver(live, 960 + 256);
	CHECK(frames[958] > 0.0F);
	CHECK(std::all_of(frames.begin() + 959, frames.end(), [](float sample) { return sample == 0.0F; }));
}

TEST_CASE("Gain's mute and invert ramp over 10 ms, never stepping further in a frame than the ramp allows", "[gain]") {
	// 10 ms at 48 kHz is 480 frames: mute's weight moves 1/480 a frame, invert's factor 2/480.
	Harness h({.format = format});
	running(h, NodeSettings::object(), 1.0F);
	h.live<gain::muteRow>(true);
	std::vector<float> frames = outputOver(h, 768, 7);
	CHECK(frames[0] == Approx(1.0 - 1.0 / 480.0).margin(1e-6));
	CHECK(frames[478] > 0.0F);
	CHECK(frames[479] == 0.0F);
	float last = 1.0F;
	for (const float sample : frames) {
		REQUIRE(std::abs(sample - last) <= 1.0F / 480.0F + 1e-6F);
		last = sample;
	}

	Harness inverted({.format = format});
	running(inverted, NodeSettings::object(), 1.0F);
	inverted.live<gain::invertRow>(1, true);
	frames = outputOver(inverted, 768, 256, 1);
	CHECK(frames[239] == Approx(0.0).margin(1e-6)); // halfway through 0
	CHECK(frames[479] == -1.0F);
	last = 1.0F;
	for (const float sample : frames) {
		REQUIRE(std::abs(sample - last) <= 2.0F / 480.0F + 1e-6F);
		last = sample;
	}
	CHECK(plays(inverted, 0, 1.0F)); // channel 1 left alone
}

TEST_CASE("Gain's trims are per channel, on top of gainDb", "[gain]") {
	Harness h({.format = format});
	running(h, {{"channels", 3}, {"gainDb", -6.0}, {"trimDb.1", 6.0}, {"trimDb.3", -12.0}});
	CHECK(plays(h, 0, 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(6.0F))));
	CHECK(h.output(gain::out, 0)[0] == Approx(0.5).epsilon(1e-5));
	CHECK(plays(h, 1, 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(0.0F))));
	CHECK(plays(h, 2, 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(-12.0F))));

	// One trim moved live moves its channel alone, over gainDb's 20 ms.
	h.live<gain::trimRow>(1, 6.0);
	const std::vector<float> frames = outputOver(h, 1024, 256, 1);
	CHECK(frames[958] < frames[959]);
	CHECK(frames[959] == 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(6.0F)));
	CHECK(plays(h, 0, 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(6.0F))));
	CHECK(plays(h, 2, 0.5F * (gain::dbToFactor(-6.0F) * gain::dbToFactor(-12.0F))));
}

TEST_CASE("Gain latches clipping once with its event, and reset-clip clears it", "[gain]") {
	Harness h({.format = format});
	running(h, {{"gainDb", 6.0}}, 1.0F);
	CHECK(h.status<gain::clipping>() == ear::StatusValue(true));
	REQUIRE(h.events().size() == 1); // channel 2 clipped too, but the latch holds one event
	CHECK(h.events()[0].code == "clipped");
	REQUIRE(h.events()[0].args.size() == 2);
	CHECK(h.events()[0].args[0] == std::pair<std::string, ear::StatusValue>{"channel", std::int64_t{1}});
	CHECK(std::get<double>(h.events()[0].args[1].second) == Approx(6.0).margin(1e-4));
	for (int block = 0; block < 4; ++block) h.process();
	CHECK(h.events().size() == 1);

	// Brought down below full scale, it stays latched until reset.
	h.live<gain::gainRow>(-6.0);
	for (int block = 0; block < 5; ++block) h.process();
	CHECK(h.status<gain::clipping>() == ear::StatusValue(true));
	CHECK(h.trigger<gain::resetClip>());
	h.process();
	CHECK(h.status<gain::clipping>() == ear::StatusValue(false));
	CHECK(h.events().size() == 1);

	// Clipping again after the reset is a second event.
	h.live<gain::gainRow>(6.0);
	for (int block = 0; block < 5; ++block) h.process();
	CHECK(h.status<gain::clipping>() == ear::StatusValue(true));
	CHECK(h.events().size() == 2);
}

TEST_CASE("A change of gainDb is a retune: no hook, no restart, the level ramping over 20 ms", "[gain]") {
	Harness h({.format = format});
	running(h, NodeSettings::object(), 1.0F);
	const std::size_t healths = h->healths().size();
	CHECK(h.change({{"gainDb", -6.0}}) == ear::NodeOutcome::retune);
	const std::vector<float> frames = outputOver(h, 1024);
	CHECK(frames[0] < 1.0F);
	CHECK(frames[958] > gain::dbToFactor(-6.0F));
	CHECK(frames[959] == gain::dbToFactor(-6.0F));
	for (std::size_t i = 1; i < frames.size(); ++i) REQUIRE(frames[i] <= frames[i - 1]);
	CHECK(h->healths().size() == healths); // nothing told: no phase moved
	CHECK(h.swaps() == 0);
}

TEST_CASE("Gain's channels go from 2 to 4 and back in place, channels 1 and 2 never silent nor stopping",
          "[gain][reshape]") {
	Harness h({.format = format, .autoComplete = false});
	running(h, {{"gainDb", -6.0}});
	const float level = 0.5F * gain::dbToFactor(-6.0F);
	CHECK(h.change({{"channels", 4}, {"gainDb", -6.0}}) == ear::NodeOutcome::reshape);
	CHECK(h.phase() == "reconfiguring");
	feed(h, 0.5F);
	// Until the commit's swap the old code plays channels 1 and 2, and the new outputs carry zeros.
	h.process();
	CHECK(plays(h, 0, level));
	CHECK(plays(h, 1, level));
	h->completeBarrier(); // the commit's swap
	CHECK(h.phase() == "running");
	for (int block = 0; block < 4; ++block) {
		h.process();
		for (std::size_t c = 0; c < 4; ++c) CHECK(plays(h, c, level));
	}
	CHECK(h.change({{"channels", 2}, {"gainDb", -6.0}}) == ear::NodeOutcome::reshape);
	h.process();
	CHECK(plays(h, 0, level));
	CHECK(plays(h, 1, level));
	h->completeBarrier();
	h.process();
	CHECK(plays(h, 0, level));
	CHECK(plays(h, 1, level));
	CHECK(h->ports().outputs().size() == 2);
	CHECK(h.swaps() == 2);
	CHECK_FALSE(told(h, ear::NodePhase::stopping));
}

TEST_CASE("A trim written for an inactive channel plays from that channel's first sample once it is active",
          "[gain][reshape]") {
	Harness h({.format = format});
	running(h, {{"trimDb.3", -6.0}});
	CHECK(plays(h, 0, 0.5F));
	CHECK(h.change({{"channels", 4}, {"trimDb.3", -6.0}}) == ear::NodeOutcome::reshape);
	feed(h, 0.5F);
	h.process();
	CHECK(plays(h, 2, 0.5F * gain::dbToFactor(-6.0F))); // from its first sample: no ramp from 0 dB
	CHECK(plays(h, 3, 0.5F));
	CHECK(plays(h, 0, 0.5F));
}

TEST_CASE("A trim and an inversion changed with the channels that make them active play from their first sample",
          "[gain][reshape]") {
	Harness h({.format = format});
	running(h, NodeSettings::object());
	feed(h, 0.5F);
	h.process(); // the 2-channel code, with trimDb.3 and invert.4 inactive at their defaults
	CHECK(h.change({{"channels", 4}, {"trimDb.3", -6.0}, {"invert.4", true}}) == ear::NodeOutcome::reshape);
	feed(h, 0.5F);
	h.process();
	CHECK(plays(h, 2, 0.5F * gain::dbToFactor(-6.0F))); // no ramp from 0 dB
	CHECK(plays(h, 3, -0.5F));                          // no swing through zero
	CHECK(plays(h, 0, 0.5F));
}

TEST_CASE("A change of channels and gainDb together reshapes in place with gainDb retuned beside it",
          "[gain][reshape]") {
	Harness h({.format = format});
	running(h, NodeSettings::object(), 1.0F);
	CHECK(h.change({{"channels", 3}, {"gainDb", -6.0}}) == ear::NodeOutcome::reshape);
	feed(h, 1.0F);
	const std::vector<float> frames = outputOver(h, 1024, 256, 2);
	CHECK(frames[958] > gain::dbToFactor(-6.0F));
	CHECK(frames[959] == gain::dbToFactor(-6.0F));
	CHECK(h.output(gain::out, 0)[255] == frames[1023]); // channel 1 on the same ramp
	h.process();
	CHECK(plays(h, 0, gain::dbToFactor(-6.0F)));
	CHECK_FALSE(told(h, ear::NodePhase::stopping));
}
