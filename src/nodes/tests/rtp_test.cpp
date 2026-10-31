// The "rtp" node type through its NodeType contract, with a fake grant that records the receivers
// asked for (docs/design/rtp.md section 3, R1n criteria 1-5). The engine side of a receiver
// (RtpReceivers) is tested in src/pipewire/tests/rtp_receiver_test.cpp.
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/builtin.hpp"

namespace {

using Names = std::vector<ear::PortName>;
using ear::NodeSettings;

/// Records every RTP receiver asked for; a handoff or a sound card request fails the test.
class RecordingGrant final : public ear::NodeGrant {
public:
	explicit RecordingGrant(std::uint32_t rate = 48000) : rate_(rate) {}
	ear::AudioFormat format() const override { return {.rate = rate_, .maxBlock = 256}; }
	ear::RtWriter& handoff(std::uint32_t, std::uint32_t, ear::HandoffConsumer&) override {
		throw std::logic_error("rtp requested a handoff");
	}
	void soundCard(const ear::SoundCardRequest&) override { throw std::logic_error("rtp asked for a sound card"); }
	void rtpReceiver(const ear::RtpReceiverRequest& request) override { requests.push_back(request); }

	std::vector<ear::RtpReceiverRequest> requests;

private:
	std::uint32_t rate_;
};

/// Valid settings: every key, then `change` applied (a null value removes the key).
NodeSettings settingsWith(const NodeSettings& change = NodeSettings::object()) {
	NodeSettings settings = {{"address", "239.69.0.1"}, {"port", 5004}, {"format", "L24"},
	                         {"channels", 2},           {"rate", 48000}, {"latencyMs", 100}};
	for (const auto& [key, value] : change.items()) {
		if (value.is_null()) {
			settings.erase(key);
		} else {
			settings[key] = value;
		}
	}
	return settings;
}

std::unique_ptr<const ear::NodeConfig> parseOk(const NodeSettings& settings) {
	auto config = ear::nodes::makeRtpNodeType()->parse("fpp", settings);
	INFO(settings.dump());
	if (!config) FAIL("refused: " << config.error().detail);
	return std::move(*config);
}

ear::RtpReceiverRequest startOk(const NodeSettings& settings, std::uint32_t graphRate = 48000) {
	RecordingGrant grant(graphRate);
	REQUIRE(parseOk(settings)->start(grant).has_value());
	REQUIRE(grant.requests.size() == 1);
	return grant.requests[0];
}

}  // namespace

TEST_CASE("Rtp accepts a full settings object, and its ports are channel_i out and from_rtp_i inside", "[rtp]") {
	CHECK(ear::nodes::makeRtpNodeType()->name() == "rtp");
	const auto config = parseOk(settingsWith({{"channels", 3}}));
	const ear::Ports& ports = config->ports();
	CHECK(ports.inputs.empty());
	CHECK(ports.outputs == Names{"channel_1", "channel_2", "channel_3"});
	CHECK(ports.insideInputs == Names{"from_rtp_1", "from_rtp_2", "from_rtp_3"});
	CHECK(ports.insideOutputs.empty());

	const auto wide = parseOk(settingsWith({{"channels", 64}, {"address", "0.0.0.0"}}));
	CHECK(wide->ports().outputs.size() == 64);
	CHECK(wide->ports().insideInputs.back() == "from_rtp_64");
}

TEST_CASE("Rtp start asks for exactly one receiver with its settings and its inside inputs", "[rtp]") {
	const auto request = startOk(settingsWith({{"port", 6000}, {"rate", 96000}, {"latencyMs", 250}}));
	CHECK(request.address == *ear::RtpAddress::parse("239.69.0.1"));
	CHECK(request.port == 6000);
	CHECK(request.encoding == ear::RtpEncoding::l24);
	CHECK(request.rate == 96000);
	CHECK(request.latencyMs == 250);
	CHECK(request.blockPorts == Names{"from_rtp_1", "from_rtp_2"});

	SECTION("an absent rate is the graph frequency, taken at start") {
		const auto defaulted = startOk(settingsWith({{"rate", nullptr}}), 44100);
		CHECK(defaulted.rate == 44100);
	}
	SECTION("an absent latencyMs is 100") {
		CHECK(startOk(settingsWith({{"latencyMs", nullptr}})).latencyMs == 100);
	}
}

TEST_CASE("Rtp maps L24 to l24 and L16 to l16", "[rtp]") {
	CHECK(startOk(settingsWith({{"format", "L24"}})).encoding == ear::RtpEncoding::l24);
	CHECK(startOk(settingsWith({{"format", "L16"}})).encoding == ear::RtpEncoding::l16);
}

TEST_CASE("Rtp refuses each setting outside section 3's grammar with exactly its detail", "[rtp]") {
	const std::string notObject = R"(settings: must be an object with "address", "port", "format" and "channels")";
	const std::string badAddress =
	    R"(address: must be "0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)";
	const std::string badPort = "port: must be a whole number, 1-65535";
	const std::string badFormat = R"(format: must be "L16" or "L24")";
	const std::string badChannels = "channels: must be a whole number, 1-64";
	const std::string badRate = "rate: must be a whole number of Hz, 8000-192000";
	const std::string badLatency = "latencyMs: must be a whole number of milliseconds, 1-1000";

	const std::vector<std::pair<NodeSettings, std::string>> cases{
	    {NodeSettings{}, notObject},
	    {NodeSettings(3), notObject},
	    {NodeSettings::array({1}), notObject},
	    {settingsWith({{"device", "x"}}), "unknown key device"},
	    {settingsWith({{"address", nullptr}}), "address: missing"},
	    {settingsWith({{"address", "192.168.1.10"}}), badAddress},
	    {settingsWith({{"address", "fpp.local"}}), badAddress},
	    {settingsWith({{"address", 239}}), badAddress},
	    {settingsWith({{"port", nullptr}}), "port: missing"},
	    {settingsWith({{"port", 0}}), badPort},
	    {settingsWith({{"port", 65536}}), badPort},
	    {settingsWith({{"port", "5004"}}), badPort},
	    {settingsWith({{"port", 5004.5}}), badPort},
	    {settingsWith({{"format", nullptr}}), "format: missing"},
	    {settingsWith({{"format", "l24"}}), badFormat},
	    {settingsWith({{"format", "S24BE"}}), badFormat},
	    {settingsWith({{"format", 24}}), badFormat},
	    {settingsWith({{"channels", nullptr}}), "channels: missing"},
	    {settingsWith({{"channels", 0}}), badChannels},
	    {settingsWith({{"channels", 65}}), badChannels},
	    {settingsWith({{"channels", "2"}}), badChannels},
	    {settingsWith({{"rate", 7999}}), badRate},
	    {settingsWith({{"rate", 192001}}), badRate},
	    {settingsWith({{"rate", 48000.5}}), badRate},
	    {settingsWith({{"latencyMs", 0}}), badLatency},
	    {settingsWith({{"latencyMs", 1001}}), badLatency},
	    {settingsWith({{"latencyMs", true}}), badLatency},
	};
	const auto type = ear::nodes::makeRtpNodeType();
	for (const auto& [settings, detail] : cases) {
		INFO(settings.dump());
		const auto config = type->parse("fpp", settings);
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "fpp");
		CHECK(config.error().detail == detail);
	}

	// The ends of every range are accepted.
	for (const auto& edge : {settingsWith({{"port", 1}, {"channels", 1}, {"rate", 8000}, {"latencyMs", 1}}),
	                         settingsWith({{"port", 65535}, {"channels", 64}, {"rate", 192000}, {"latencyMs", 1000}})}) {
		INFO(edge.dump());
		CHECK(type->parse("fpp", edge).has_value());
	}

	// Checks run in section 3's order: the first failure is the one told.
	CHECK(type->parse("fpp", settingsWith({{"address", "1.2.3.4"}, {"port", 0}, {"other", 1}})).error().detail ==
	      "unknown key other");
	CHECK(type->parse("fpp", settingsWith({{"address", "1.2.3.4"}, {"port", 0}})).error().detail == badAddress);
	CHECK(type->parse("fpp", settingsWith({{"port", 0}, {"format", "x"}})).error().detail == badPort);
	CHECK(type->parse("fpp", settingsWith({{"format", "x"}, {"channels", 0}})).error().detail == badFormat);
	CHECK(type->parse("fpp", settingsWith({{"channels", 0}, {"rate", 1}})).error().detail == badChannels);
	CHECK(type->parse("fpp", settingsWith({{"rate", 1}, {"latencyMs", 0}})).error().detail == badRate);
}

TEST_CASE("Rtp passes from_rtp_i to channel_i sample for sample", "[rtp]") {
	for (const std::uint32_t channels : {1U, 3U}) {
		INFO(channels << " channels");
		constexpr std::uint32_t frames = 200;
		constexpr ear::AudioFormat format{.rate = 48000, .maxBlock = 256};
		const auto config = parseOk(settingsWith({{"channels", channels}}));
		RecordingGrant grant;
		auto insides = config->start(grant);
		REQUIRE(insides.has_value());
		ear::AudioCode& code = (*insides)->audioCode();
		code.prepare(format);

		// Inputs: from_rtp_1..N, each its own ramp. Outputs: channel_1..N, primed with a value no
		// input carries.
		std::vector<std::vector<float>> in(channels, std::vector<float>(format.maxBlock));
		for (std::uint32_t c = 0; c < channels; ++c) {
			for (std::uint32_t f = 0; f < format.maxBlock; ++f) {
				in[c][f] = static_cast<float>(c + 1) + static_cast<float>(f) / 1000.0F;
			}
		}
		std::vector<std::vector<float>> out(channels, std::vector<float>(frames, -9.0F));
		std::vector<std::span<const float>> inSpans;
		for (const auto& channel : in) inSpans.emplace_back(channel.data(), frames);
		std::vector<std::span<float>> outSpans;
		for (auto& channel : out) outSpans.emplace_back(channel);
		code.process(inSpans, outSpans, frames);
		code.release();

		for (std::uint32_t c = 0; c < channels; ++c) {
			INFO("from_rtp_" << c + 1 << " -> channel_" << c + 1);
			CHECK(out[c] == std::vector<float>(in[c].begin(), in[c].begin() + frames));
		}
	}
}
