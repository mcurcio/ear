// The "rtp" node type through the harness, which records the receivers it acquires (docs/design/rtp.md section 3, R1n
// criteria 1-5; docs/design/node-framework.md section 7.6). The engine side of a receiver
// (RtpReceivers) is tested in src/pipewire/tests/rtp_receiver_test.cpp.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/nodes/rtp.hpp"
#include "ear/testing/node_harness.hpp"

namespace {

using Names = std::vector<ear::PortName>;
using ear::NodeSettings;

const ear::NodeType& rtpType() {
	static const auto type = ear::nodes::makeRtpNodeType();
	return *type;
}

/// Valid settings: every key, then `change` applied (a null value removes the key).
NodeSettings settingsWith(const NodeSettings& change = NodeSettings::object()) {
	NodeSettings settings = {{"address", "239.69.0.1"}, {"port", 5004},  {"format", "L24"},
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

/// A harness on a node named "fpp" at `graphRate`, whose settings parsed.
std::unique_ptr<ear::testing::NodeHarness> parseOk(const NodeSettings& settings, std::uint32_t graphRate = 48000) {
	auto harness =
		std::make_unique<ear::testing::NodeHarness>(rtpType(), ear::AudioFormat{graphRate, 256}, nullptr, "fpp");
	const auto parsed = harness->parse(settings);
	INFO(settings.dump());
	if (!parsed) FAIL("refused: " << parsed.error());
	return harness;
}

ear::RtpReceiverRequest startOk(const NodeSettings& settings, std::uint32_t graphRate = 48000) {
	const auto harness = parseOk(settings, graphRate);
	REQUIRE(harness->start());
	REQUIRE(harness->requests().size() == 1);
	REQUIRE(harness->outsideName(0) == "fpp.rtp");
	return std::get<ear::RtpReceiverRequest>(harness->requests()[0]);
}

} // namespace

TEST_CASE("Rtp accepts a full settings object, and its ports are stream.i out and the inside bank from_rtp", "[rtp]") {
	CHECK(rtpType().name() == "rtp");
	const auto config = parseOk(settingsWith({{"channels", 3}}));
	const ear::Ports& ports = config->ports();
	CHECK(ports.inputs().empty());
	CHECK(ports.outputs() == Names{"stream.1", "stream.2", "stream.3"});
	CHECK(ports.insideInputs() == Names{"from_rtp.1", "from_rtp.2", "from_rtp.3"});
	CHECK(ports.insideOutputs().empty());
	CHECK(ports.bank("from_rtp").reach == ear::PortBank::Reach::inside);

	const auto wide = parseOk(settingsWith({{"channels", 64}, {"address", "0.0.0.0"}}));
	CHECK(wide->ports().outputs().size() == 64);
	CHECK(wide->ports().insideInputs().back() == "from_rtp.64");
}

TEST_CASE("Rtp start acquires exactly one receiver with its settings, joined to its inside bank", "[rtp]") {
	const auto request = startOk(settingsWith({{"port", 6000}, {"rate", 96000}, {"latencyMs", 250}}));
	CHECK(request.address == *ear::RtpAddress::parse("239.69.0.1"));
	CHECK(request.port == 6000);
	CHECK(request.encoding == ear::RtpEncoding::l24);
	CHECK(request.rate == 96000);
	CHECK(request.latencyMs == 250);
	CHECK(request.bank == "from_rtp");

	SECTION("an absent rate is the graph frequency, taken at start") {
		const auto defaulted = startOk(settingsWith({{"rate", nullptr}}), 44100);
		CHECK(defaulted.rate == 44100);
	}
	SECTION("an absent latencyMs is 100") { CHECK(startOk(settingsWith({{"latencyMs", nullptr}})).latencyMs == 100); }
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
	const ear::NodeType* type = &rtpType();
	for (const auto& [settings, detail] : cases) {
		INFO(settings.dump());
		const auto config = type->parse("fpp", settings);
		REQUIRE_FALSE(config.has_value());
		CHECK(config.error().node == "fpp");
		CHECK(config.error().detail == detail);
	}

	// The ends of every range are accepted.
	for (const auto& edge :
	     {settingsWith({{"port", 1}, {"channels", 1}, {"rate", 8000}, {"latencyMs", 1}}),
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

TEST_CASE("Rtp writes no audio code: the framework's pass-through carries from_rtp.i to stream.i sample for sample",
          "[rtp]") {
	for (const std::uint32_t channels : {1U, 3U}) {
		INFO(channels << " channels");
		constexpr std::uint32_t frames = 200;
		const auto harness = parseOk(settingsWith({{"channels", channels}}));
		REQUIRE(harness->start());
		CHECK(harness->state() == ear::testing::NodeHarness::State::starting);
		harness->ready(0);
		CHECK(harness->state() == ear::testing::NodeHarness::State::running);

		// Each from_rtp.i its own ramp; stream.i must carry it.
		const auto ramp = [](std::uint32_t c, std::uint32_t f) {
			return static_cast<float>(c + 1) + static_cast<float>(f) / 1000.0F;
		};
		for (std::uint32_t c = 0; c < channels; ++c) {
			const std::span<float> buffer = harness->input("from_rtp." + std::to_string(c + 1));
			for (std::uint32_t f = 0; f < buffer.size(); ++f) buffer[f] = ramp(c, f);
		}
		harness->process(ear::GraphCycle{0, 0}, frames);
		CHECK(harness->unwritten() == 0);

		for (std::uint32_t c = 0; c < channels; ++c) {
			INFO("from_rtp." << c + 1 << " -> stream." << c + 1);
			const std::span<const float> out = harness->output("stream." + std::to_string(c + 1));
			for (std::uint32_t f = 0; f < frames; ++f) CHECK(out[f] == ramp(c, f));
		}
	}
}
