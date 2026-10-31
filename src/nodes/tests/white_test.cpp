// The "white" node type through its NodeType contract, and the built-in catalogue list
// (docs/design/m1-white-to-file.md section 3, slice S5 criteria 1 and 4; docs/design/batch2.md
// E2 criterion 7), and each type's declared setting kinds (docs/design/knobly-config.md).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

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
	void rtpReceiver(const ear::RtpReceiverRequest&) override {
		throw std::logic_error("white asked for an RTP receiver");
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
	     {ear::NodeSettings{{"gain", 0.5}}, ear::NodeSettings{{"path", "a.wav"}}, ear::NodeSettings(3)}) {
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

TEST_CASE("The built-in node types are white, file, card and rtp", "[builtin]") {
	const auto path = std::filesystem::temp_directory_path() / ("ear_nodes_builtin_" + std::to_string(::getpid()));
	auto folder = ear::folder::ConfinedFolder::open(path);
	REQUIRE(folder.has_value());
	const auto types =
	    ear::nodes::builtinNodeTypes(std::make_shared<const ear::folder::ConfinedFolder>(std::move(*folder)));
	std::filesystem::remove(path);
	std::vector<std::string> names;
	for (const auto& type : types) {
		names.emplace_back(type->name());
	}
	std::ranges::sort(names);
	CHECK(names == std::vector<std::string>{"card", "file", "rtp", "white"});
}

TEST_CASE("Each built-in type declares every key once, and a value of each declared kind parses",
          "[builtin]") {
	const auto path = std::filesystem::temp_directory_path() / ("ear_nodes_kinds_" + std::to_string(::getpid()));
	auto folder = ear::folder::ConfinedFolder::open(path);
	REQUIRE(folder.has_value());
	const auto types =
	    ear::nodes::builtinNodeTypes(std::make_shared<const ear::folder::ConfinedFolder>(std::move(*folder)));
	std::filesystem::remove(path);

	std::map<std::string, std::vector<std::pair<std::string, ear::SettingKind>>> declared;
	for (const auto& type : types) {
		INFO(type->name());
		// "a" is a valid card id and a valid recordings path; 1 is a valid channel count, port and
		// latency. The rtp keys whose values those are not get a valid value of the same kind.
		const std::map<std::string, ear::NodeSettings> valid{
		    {"address", "0.0.0.0"}, {"format", "L16"}, {"rate", 48000}};
		ear::NodeSettings settings = ear::NodeSettings::object();
		auto& keys = declared[std::string(type->name())];
		for (const auto& [key, kind] : type->settingKinds()) {
			CHECK_FALSE(settings.contains(std::string(key)));
			const auto special = valid.find(std::string(key));
			settings[std::string(key)] = special != valid.end()          ? special->second
			                             : kind == ear::SettingKind::text ? ear::NodeSettings("a")
			                                                              : ear::NodeSettings(1);
			CHECK((kind == ear::SettingKind::text) == settings[std::string(key)].is_string());
			keys.emplace_back(key, kind);
		}
		const auto parsed = type->parse("node", settings);
		CHECK(parsed.has_value());
	}
	using enum ear::SettingKind;
	CHECK(declared["white"].empty());
	CHECK(declared["file"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"path", text}});
	CHECK(declared["card"] == std::vector<std::pair<std::string, ear::SettingKind>>{
	                              {"card", text}, {"playback", wholeNumber}, {"capture", wholeNumber}});
	CHECK(declared["rtp"] == std::vector<std::pair<std::string, ear::SettingKind>>{{"address", text},
	                                                                              {"port", wholeNumber},
	                                                                              {"format", text},
	                                                                              {"channels", wholeNumber},
	                                                                              {"rate", wholeNumber},
	                                                                              {"latencyMs", wholeNumber}});
}
