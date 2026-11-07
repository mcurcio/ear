// The "card" node type through its NodeType contract, with a fake start context that records the sound cards it
// acquires (docs/design/batch2.md section 4.4, slice E2 criterion 6; docs/design/node-framework.md slice 1: named inside
// banks and the framework's pass-through). The engine side of a card (AlsaSoundCards) is tested in
// src/pipewire/tests/sound_card_test.cpp.
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/pass_through.hpp"
#include "ear/nodes/card.hpp"

namespace {

using Names = std::vector<ear::PortName>;

/// Records every sound card acquired; any other resource fails the test.
class RecordingContext final : public ear::StartContext {
public:
	ear::AudioFormat format() const override { return {.rate = 48000, .maxBlock = 256}; }
	const ear::NodeName& node() const override { return name_; }
	ear::InsidesEvents& events() override { throw std::logic_error("card asked for events"); }

	std::vector<ear::SoundCardRequest> requests;

protected:
	Acquired acquireResource(const ear::ResourceRequest& request) override {
		const auto* card = std::get_if<ear::SoundCardRequest>(&request);
		if (card == nullptr) throw std::logic_error("card acquired something other than a sound card");
		requests.push_back(*card);
		return std::monostate{};
	}

private:
	ear::NodeName name_ = "motu";
};

std::unique_ptr<const ear::NodeConfig> parseOk(const ear::NodeSettings& settings) {
	auto config = ear::nodes::makeCardNodeType({})->parse("motu", settings);
	REQUIRE(config.has_value());
	return std::move(*config);
}

} // namespace

TEST_CASE("Card accepts a card id with playback and capture channel counts", "[card]") {
	CHECK(ear::nodes::makeCardNodeType({})->name() == "card");
	const auto config = parseOk({{"card", "Pro24"}, {"playback", 3}, {"capture", 2}});
	const ear::Ports& ports = config->ports();
	CHECK(ports.inputs() == Names{"play.1", "play.2", "play.3"});
	CHECK(ports.insideOutputs() == Names{"to_card.1", "to_card.2", "to_card.3"});
	CHECK(ports.outputs() == Names{"capture.1", "capture.2"});
	CHECK(ports.insideInputs() == Names{"from_card.1", "from_card.2"});
	CHECK(ports.bank("to_card").reach == ear::PortBank::Reach::inside);
	CHECK(ports.bank("from_card").reach == ear::PortBank::Reach::inside);

	// Each direction defaults to 0 channels.
	const auto playbackOnly = parseOk({{"card", "Test"}, {"playback", 1}});
	CHECK(playbackOnly->ports().inputs() == Names{"play.1"});
	CHECK(playbackOnly->ports().outputs().empty());
	CHECK(playbackOnly->ports().insideInputs().empty());
	const auto captureOnly = parseOk({{"card", "Test"}, {"capture", 64}});
	CHECK(captureOnly->ports().inputs().empty());
	CHECK(captureOnly->ports().outputs().size() == 64);
	CHECK(captureOnly->ports().outputs().back() == "capture.64");
}

TEST_CASE("Card rejects settings outside section 4.4's rules", "[card]") {
	const auto type = ear::nodes::makeCardNodeType({});
	const std::vector<ear::NodeSettings> invalid{
		// A raw ALSA device name: the file plugin's "|command" form, and hw:0 under an unknown key.
		{{"card", "file:'|touch x'"}, {"playback", 2}},
		{{"device", "hw:0"}},
		{{"device", "hw:0"}, {"card", "Pro24"}, {"playback", 2}},
		{{"card", "hw:0"}, {"playback", 2}},
		{{"card", "0123456789abcdef"}, {"playback", 2}}, // 16 characters
		{{"card", ""}, {"playback", 2}},
		{{"card", 3}, {"playback", 2}},
		{{"playback", 2}},   // no card
		{{"card", "Pro24"}}, // no channels
		{{"card", "Pro24"}, {"playback", 0}, {"capture", 0}},
		{{"card", "Pro24"}, {"playback", 65}},
		{{"card", "Pro24"}, {"capture", -1}},
		{{"card", "Pro24"}, {"playback", 1.5}},
		{{"card", "Pro24"}, {"playback", "2"}},
		{{"card", "Pro24"}, {"rate", 48000}, {"playback", 2}},
		ear::NodeSettings{},
		ear::NodeSettings(3),
	};
	for (const auto& settings : invalid) {
		INFO(settings.dump());
		const auto config = type->parse("motu", settings);
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "motu");
		CHECK_FALSE(config.error().detail.empty());
	}
}

TEST_CASE("Card start acquires one sound card per direction, each joined to its inside bank", "[card]") {
	const auto config = parseOk({{"card", "Pro24"}, {"playback", 2}, {"capture", 3}});
	RecordingContext context;
	REQUIRE(config->start(context).has_value());
	REQUIRE(context.requests.size() == 2);
	const ear::CardId pro24 = *ear::CardId::parse("Pro24");
	CHECK(context.requests[0].card == pro24);
	CHECK(context.requests[0].direction == ear::CardDirection::playback);
	CHECK(context.requests[0].bank == "to_card");
	CHECK(context.requests[1].card == pro24);
	CHECK(context.requests[1].direction == ear::CardDirection::capture);
	CHECK(context.requests[1].bank == "from_card");

	RecordingContext playbackContext;
	REQUIRE(parseOk({{"card", "Test"}, {"playback", 1}})->start(playbackContext).has_value());
	REQUIRE(playbackContext.requests.size() == 1);
	CHECK(playbackContext.requests[0].direction == ear::CardDirection::playback);

	RecordingContext captureContext;
	REQUIRE(parseOk({{"card", "Test"}, {"capture", 1}})->start(captureContext).has_value());
	REQUIRE(captureContext.requests.size() == 1);
	CHECK(captureContext.requests[0].direction == ear::CardDirection::capture);
}

TEST_CASE("Card writes no audio code: the framework's pass-through carries play.i to to_card.i and from_card.j to "
          "capture.j sample for sample",
          "[card]") {
	constexpr std::uint32_t playback = 3;
	constexpr std::uint32_t capture = 2;
	constexpr std::uint32_t frames = 200;
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 256};
	const auto config = parseOk({{"card", "Pro24"}, {"playback", playback}, {"capture", capture}});
	RecordingContext context;
	auto started = config->start(context);
	REQUIRE(started.has_value());
	CHECK(started->code.own() == nullptr);
	CHECK_FALSE(started->recipe);
	const auto code = ear::makePassThrough(config->ports(), started->code.pairs());
	code->prepare(format);

	// Inputs: play.1..3 then from_card.1..2; each channel carries its own distinct ramp.
	// Outputs: capture.1..2 then to_card.1..3, primed with a value no input carries.
	auto ramp = [](std::uint32_t channel, std::uint32_t frame) {
		return static_cast<float>(channel + 1) + static_cast<float>(frame) / 1000.0F;
	};
	std::vector<std::vector<float>> in(playback + capture, std::vector<float>(format.maxBlock));
	for (std::uint32_t c = 0; c < in.size(); ++c) {
		for (std::uint32_t f = 0; f < format.maxBlock; ++f) {
			in[c][f] = ramp(c, f);
		}
	}
	std::vector<std::vector<float>> out(capture + playback, std::vector<float>(frames, -9.0F));
	std::vector<std::span<const float>> inSpans;
	for (const auto& channel : in) {
		inSpans.emplace_back(channel.data(), frames);
	}
	std::vector<std::span<float>> outSpans;
	for (auto& channel : out) {
		outSpans.emplace_back(channel);
	}
	code->process(inSpans, outSpans, frames, ear::GraphCycle{0, 0});
	code->release();

	for (std::uint32_t i = 0; i < playback; ++i) {
		INFO("play." << i + 1 << " -> to_card." << i + 1);
		CHECK(out[capture + i] == std::vector<float>(in[i].begin(), in[i].begin() + frames));
	}
	for (std::uint32_t j = 0; j < capture; ++j) {
		INFO("from_card." << j + 1 << " -> capture." << j + 1);
		CHECK(out[j] == std::vector<float>(in[playback + j].begin(), in[playback + j].begin() + frames));
	}
}
