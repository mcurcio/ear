// The "white" node type through the harness (docs/design/m1-white-to-file.md section 3, slice S5 criterion 1;
// docs/design/node-framework.md section 7.6). What holds for every type together is test/nodes/'s.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/white.hpp"
#include "ear/testing/node_harness.hpp"

TEST_CASE("White declares one output, noise.mono, and no inputs", "[white]") {
	const auto type = ear::nodes::makeWhiteNodeType();
	CHECK(type->name() == "white");

	for (const ear::NodeSettings& settings : {ear::NodeSettings{}, ear::NodeSettings::object()}) {
		ear::testing::NodeHarness harness(*type);
		REQUIRE(harness.parse(settings));
		CHECK(harness.ports().inputs().empty());
		CHECK(harness.ports().outputs() == std::vector<ear::PortName>{"noise.mono"});
	}
}

TEST_CASE("White rejects any settings key", "[white]") {
	const auto type = ear::nodes::makeWhiteNodeType();
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{{"gain", 0.5}}, ear::NodeSettings{{"path", "a.wav"}}, ear::NodeSettings(3)}) {
		ear::testing::NodeHarness harness(*type);
		const auto parsed = harness.parse(settings);
		REQUIRE_FALSE(parsed);
		CHECK_FALSE(parsed.error().empty());
	}
}

TEST_CASE("White fills its output with uniform noise from -1 up to but not including 1", "[white]") {
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 480};
	const auto type = ear::nodes::makeWhiteNodeType();
	ear::testing::NodeHarness harness(*type, format);
	REQUIRE(harness.parse({}));
	REQUIRE(harness.start());
	CHECK(harness.requests().empty()); // it acquires nothing
	CHECK(harness.state() == ear::testing::NodeHarness::State::running);
	CHECK(harness.runObject() == nullptr); // no run object: white has no threads

	// One second at the format's rate, block by block; the harness primes every output with NaN, so an unwritten
	// sample shows up.
	std::vector<float> samples;
	for (std::uint32_t done = 0; done < format.rate; done += format.maxBlock) {
		harness.process(ear::GraphCycle{0, done}, format.maxBlock);
		REQUIRE(harness.unwritten() == 0);
		const std::span<const float> block = harness.output("noise.mono");
		samples.insert(samples.end(), block.begin(), block.end());
	}
	harness.close();

	REQUIRE(samples.size() == 48000);
	CHECK(std::ranges::all_of(samples, [](float s) { return s >= -1.0F && s < 1.0F; }));
	double sumOfSquares = 0.0;
	for (const float s : samples) {
		sumOfSquares += static_cast<double>(s) * s;
	}
	// Uniform on [-1, 1): RMS = 1 / sqrt(3).
	CHECK(std::sqrt(sumOfSquares / static_cast<double>(samples.size())) == Approx(0.577).margin(0.01));
}
