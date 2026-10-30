// The "white" node type through its NodeType contract, and the built-in catalogue list
// (docs/design/m1-white-to-file.md section 3, slice S5 criteria 1 and 4).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/builtin.hpp"

namespace {

/// White asks for nothing: any handoff request fails the test.
class NoHandoffGrant final : public ear::NodeGrant {
public:
	explicit NoHandoffGrant(ear::AudioFormat format) : format_(format) {}
	ear::AudioFormat format() const override { return format_; }
	ear::RtWriter& handoff(std::uint32_t, std::uint32_t, ear::HandoffConsumer&) override {
		throw std::logic_error("white requested a handoff");
	}
	void soundCard(const ear::SoundCardRequest&) override {
		throw std::logic_error("white asked for a sound card");
	}

private:
	ear::AudioFormat format_;
};

}  // namespace

TEST_CASE("White declares one output, MONO, and no inputs", "[white]") {
	const auto type = ear::nodes::makeWhiteNodeType();
	CHECK(type->name() == "white");

	for (const ear::NodeSettings& settings : {ear::NodeSettings{}, ear::NodeSettings::object()}) {
		const auto config = type->parse("noise", settings);
		REQUIRE(config.has_value());
		CHECK((*config)->ports().inputs.empty());
		CHECK((*config)->ports().outputs == std::vector<ear::PortName>{"MONO"});
	}
}

TEST_CASE("White rejects any settings key", "[white]") {
	const auto type = ear::nodes::makeWhiteNodeType();
	for (const ear::NodeSettings& settings :
	     {ear::NodeSettings{{"gain", 0.5}}, ear::NodeSettings{{"path", "/tmp/a.wav"}}, ear::NodeSettings(3)}) {
		const auto config = type->parse("noise", settings);
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "noise");
		CHECK_FALSE(config.error().detail.empty());
	}
}

TEST_CASE("White fills its output with uniform noise from -1 up to but not including 1", "[white]") {
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 480};
	const auto config = ear::nodes::makeWhiteNodeType()->parse("noise", {});
	REQUIRE(config.has_value());
	NoHandoffGrant grant{format};
	auto insides = (*config)->start(grant);
	REQUIRE(insides.has_value());
	ear::AudioCode& code = (*insides)->audioCode();
	code.prepare(format);

	// One second at the format's rate, block by block, each block primed with a value the
	// noise cannot produce so an unwritten sample shows up.
	std::vector<float> samples;
	std::vector<float> block(format.maxBlock);
	for (std::uint32_t done = 0; done < format.rate; done += format.maxBlock) {
		std::ranges::fill(block, 2.0F);
		const std::span<float> output{block};
		code.process({}, std::span<const std::span<float>>{&output, 1}, format.maxBlock);
		samples.insert(samples.end(), block.begin(), block.end());
	}
	code.release();

	REQUIRE(samples.size() == 48000);
	CHECK(std::ranges::all_of(samples, [](float s) { return s >= -1.0F && s < 1.0F; }));
	double sumOfSquares = 0.0;
	for (const float s : samples) {
		sumOfSquares += static_cast<double>(s) * s;
	}
	// Uniform on [-1, 1): RMS = 1 / sqrt(3).
	CHECK(std::sqrt(sumOfSquares / static_cast<double>(samples.size())) == Approx(0.577).margin(0.01));
}

TEST_CASE("The built-in node types are white and file", "[builtin]") {
	const auto types = ear::nodes::builtinNodeTypes();
	std::vector<std::string> names;
	for (const auto& type : types) {
		names.emplace_back(type->name());
	}
	std::ranges::sort(names);
	CHECK(names == std::vector<std::string>{"file", "white"});
}
