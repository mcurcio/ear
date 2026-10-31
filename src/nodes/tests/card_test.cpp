// The "card" node type through its NodeType contract, with a fake grant that records the sound
// cards asked for (docs/design/batch2.md section 4.4, slice E2 criterion 6). The engine side of a
// card (AlsaSoundCards) is tested in src/pipewire/tests/sound_card_test.cpp.
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/builtin.hpp"

namespace {

using Names = std::vector<ear::PortName>;

/// Records every sound card asked for; a handoff request fails the test.
class RecordingGrant final : public ear::NodeGrant {
public:
	ear::AudioFormat format() const override { return {.rate = 48000, .maxBlock = 256}; }
	ear::RtWriter& handoff(std::uint32_t, std::uint32_t, ear::HandoffConsumer&) override {
		throw std::logic_error("card requested a handoff");
	}
	void soundCard(const ear::SoundCardRequest& request) override { requests.push_back(request); }
	void rtpReceiver(const ear::RtpReceiverRequest&) override {
		throw std::logic_error("card asked for an RTP receiver");
	}

	std::vector<ear::SoundCardRequest> requests;
};

std::unique_ptr<const ear::NodeConfig> parseOk(const ear::NodeSettings& settings) {
	auto config = ear::nodes::makeCardNodeType()->parse("motu", settings);
	REQUIRE(config.has_value());
	return std::move(*config);
}

}  // namespace

TEST_CASE("Card accepts a card id with playback and capture channel counts", "[card]") {
	CHECK(ear::nodes::makeCardNodeType()->name() == "card");
	const auto config = parseOk({{"card", "Pro24"}, {"playback", 3}, {"capture", 2}});
	const ear::Ports& ports = config->ports();
	CHECK(ports.inputs == Names{"playback_1", "playback_2", "playback_3"});
	CHECK(ports.insideOutputs == Names{"to_card_1", "to_card_2", "to_card_3"});
	CHECK(ports.outputs == Names{"capture_1", "capture_2"});
	CHECK(ports.insideInputs == Names{"from_card_1", "from_card_2"});

	// Each direction defaults to 0 channels.
	const auto playbackOnly = parseOk({{"card", "Test"}, {"playback", 1}});
	CHECK(playbackOnly->ports().inputs == Names{"playback_1"});
	CHECK(playbackOnly->ports().outputs.empty());
	CHECK(playbackOnly->ports().insideInputs.empty());
	const auto captureOnly = parseOk({{"card", "Test"}, {"capture", 64}});
	CHECK(captureOnly->ports().inputs.empty());
	CHECK(captureOnly->ports().outputs.size() == 64);
	CHECK(captureOnly->ports().outputs.back() == "capture_64");
}

TEST_CASE("Card rejects settings outside section 4.4's rules", "[card]") {
	const auto type = ear::nodes::makeCardNodeType();
	const std::vector<ear::NodeSettings> invalid{
	    // A raw ALSA device name: the file plugin's "|command" form, and hw:0 under an unknown key.
	    {{"card", "file:'|touch x'"}, {"playback", 2}},
	    {{"device", "hw:0"}},
	    {{"device", "hw:0"}, {"card", "Pro24"}, {"playback", 2}},
	    {{"card", "hw:0"}, {"playback", 2}},
	    {{"card", "0123456789abcdef"}, {"playback", 2}},   // 16 characters
	    {{"card", ""}, {"playback", 2}},
	    {{"card", 3}, {"playback", 2}},
	    {{"playback", 2}},                                  // no card
	    {{"card", "Pro24"}},                                // no channels
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

TEST_CASE("Card start asks one sound card per direction, its block ports in order", "[card]") {
	const auto config = parseOk({{"card", "Pro24"}, {"playback", 2}, {"capture", 3}});
	RecordingGrant grant;
	REQUIRE(config->start(grant).has_value());
	REQUIRE(grant.requests.size() == 2);
	const ear::CardId pro24 = *ear::CardId::parse("Pro24");
	CHECK(grant.requests[0].card == pro24);
	CHECK(grant.requests[0].direction == ear::CardDirection::playback);
	CHECK(grant.requests[0].blockPorts == Names{"to_card_1", "to_card_2"});
	CHECK(grant.requests[1].card == pro24);
	CHECK(grant.requests[1].direction == ear::CardDirection::capture);
	CHECK(grant.requests[1].blockPorts == Names{"from_card_1", "from_card_2", "from_card_3"});

	RecordingGrant playbackGrant;
	REQUIRE(parseOk({{"card", "Test"}, {"playback", 1}})->start(playbackGrant).has_value());
	REQUIRE(playbackGrant.requests.size() == 1);
	CHECK(playbackGrant.requests[0].direction == ear::CardDirection::playback);

	RecordingGrant captureGrant;
	REQUIRE(parseOk({{"card", "Test"}, {"capture", 1}})->start(captureGrant).has_value());
	REQUIRE(captureGrant.requests.size() == 1);
	CHECK(captureGrant.requests[0].direction == ear::CardDirection::capture);
}

TEST_CASE("Card passes playback_i to to_card_i and from_card_j to capture_j sample for sample", "[card]") {
	constexpr std::uint32_t playback = 3;
	constexpr std::uint32_t capture = 2;
	constexpr std::uint32_t frames = 200;
	constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 256};
	const auto config = parseOk({{"card", "Pro24"}, {"playback", playback}, {"capture", capture}});
	RecordingGrant grant;
	auto insides = config->start(grant);
	REQUIRE(insides.has_value());
	ear::AudioCode& code = (*insides)->audioCode();
	code.prepare(format);

	// Inputs: playback_1..3 then from_card_1..2; each channel carries its own distinct ramp.
	// Outputs: capture_1..2 then to_card_1..3, primed with a value no input carries.
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
	code.process(inSpans, outSpans, frames);
	code.release();

	for (std::uint32_t i = 0; i < playback; ++i) {
		INFO("playback_" << i + 1 << " -> to_card_" << i + 1);
		CHECK(out[capture + i] == std::vector<float>(in[i].begin(), in[i].begin() + frames));
	}
	for (std::uint32_t j = 0; j < capture; ++j) {
		INFO("from_card_" << j + 1 << " -> capture_" << j + 1);
		CHECK(out[j] == std::vector<float>(in[playback + j].begin(), in[playback + j].begin() + frames));
	}
}
