// The "white" node type through its type harness, by the bank its spec declares (white_spec.hpp;
// docs/writing-a-node-type.md#testing-a-type: its class, run over a real NodeHost). What holds for every type
// together is test/nodes/'s.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/white.hpp"
#include "ear/testing/type_harness.hpp"
#include "white_spec.hpp"

namespace {
namespace white = ear::nodes::white;
using Harness = ear::testing::TypeHarness<white::spec, white::White>;
} // namespace

TEST_CASE("White declares one output, noise.mono, and no inputs", "[white]") {
	CHECK(ear::nodes::makeWhiteNodeType()->name() == "white");

	for (const ear::NodeSettings& settings : {ear::NodeSettings{}, ear::NodeSettings::object()}) {
		Harness harness;
		CHECK(harness.type().name() == "white");
		REQUIRE_FALSE(harness.parse(settings));
		CHECK(harness->ports().inputs().empty());
		CHECK(harness->ports().outputs() == std::vector<ear::PortName>{"noise.mono"});
	}
}

TEST_CASE("White rejects any settings key", "[white]") {
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{{"gain", 0.5}}, ear::NodeSettings{{"path", "a.wav"}}, ear::NodeSettings(3)}) {
		Harness harness;
		const auto refused = harness.parse(settings);
		REQUIRE(refused);
		CHECK_FALSE(refused->empty());
	}
}

TEST_CASE("White runs once its block is ready, with nothing to acquire", "[white]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({}));
	harness.add();
	CHECK(harness.phase() == "starting"); // configured at once (nothing to acquire), waiting for its block
	harness.blockReady();
	CHECK(harness.phase() == "running");
	CHECK(harness->companions().empty());
	CHECK(harness->runObject() == nullptr); // no run object: white has no threads
	CHECK(harness->notes().empty());
	harness.remove();
	CHECK(harness->gone());
	CHECK(harness->droppedFrames() == 0);
}

TEST_CASE("White fills its output with uniform noise from -1 up to but not including 1", "[white]") {
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 480};
	Harness harness({.format = format});
	REQUIRE_FALSE(harness.parse({}));
	harness.add();
	harness.blockReady();
	REQUIRE(harness.phase() == "running");
	harness.process(); // the drive's 5 ms fade-in
	CHECK(harness.lastFadeGain(white::noise) == 1.0F);

	// One second at the format's rate, block by block; the harness primes every output with NaN, so an unwritten
	// sample shows up.
	std::vector<float> samples;
	for (std::uint32_t done = 0; done < format.rate; done += format.maxBlock) {
		harness.process();
		REQUIRE(harness->unwritten() == 0);
		const std::span<const float> block = harness.output(white::noise, 0);
		samples.insert(samples.end(), block.begin(), block.end());
	}

	REQUIRE(samples.size() == 48000);
	CHECK(std::ranges::all_of(samples, [](float s) { return s >= -1.0F && s < 1.0F; }));
	double sumOfSquares = 0.0;
	for (const float s : samples) {
		sumOfSquares += static_cast<double>(s) * s;
	}
	// Uniform on [-1, 1): RMS = 1 / sqrt(3).
	CHECK(std::sqrt(sumOfSquares / static_cast<double>(samples.size())) == Approx(0.577).margin(0.01));
}

TEST_CASE("White stops silent and starts again, and a restart configures it afresh", "[white]") {
	Harness harness;
	REQUIRE_FALSE(harness.parse({}));
	harness.add();
	harness.blockReady();
	harness.process();
	REQUIRE(harness.stop());
	CHECK(harness.phase() == "stopped");
	harness.process();
	CHECK(std::ranges::all_of(harness.output(white::noise, 0), [](float s) { return s == 0.0F; }));
	REQUIRE(harness.start());
	CHECK(harness.phase() == "running");
	REQUIRE(harness.restart());
	CHECK(harness.phase() == "running");
	harness.process();
	harness.process();
	CHECK(harness->unwritten() == 0);
	CHECK_FALSE(std::ranges::all_of(harness.output(white::noise, 0), [](float s) { return s == 0.0F; }));
}
