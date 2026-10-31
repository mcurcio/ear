// The "rtp" node type: one RTP stream received behind a port block (docs/design/rtp.md sections 3
// and 7.3, builtin.hpp). The framework joins the receiver's channels to the inside inputs
// "from_rtp_1"..; they pass straight through to the outputs "channel_1"... Every setting is
// checked here, so nothing reaches PipeWire unparsed: the address is an RtpAddress, the format a
// closed enum, every number ranged.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ear/nodes/builtin.hpp"

namespace ear::nodes {
namespace {

constexpr std::string_view typeName = "rtp";
constexpr std::string_view addressKey = "address";
constexpr std::string_view portKey = "port";
constexpr std::string_view formatKey = "format";
constexpr std::string_view channelsKey = "channels";
constexpr std::string_view rateKey = "rate";
constexpr std::string_view latencyKey = "latencyMs";
/// The receive buffer when the settings name none: about 100 ms (R2). The one home of this
/// default; the framework always passes the value on.
constexpr std::uint32_t defaultLatencyMs = 100;

/// "<prefix>1".."<prefix><count>".
std::vector<PortName> numbered(std::string_view prefix, std::uint32_t count) {
	std::vector<PortName> names;
	names.reserve(count);
	for (std::uint32_t i = 1; i <= count; ++i) {
		names.push_back(std::string(prefix) + std::to_string(i));
	}
	return names;
}

/// Copies from_rtp_i to channel_i. The node has no public inputs, so its inputs are the inside
/// inputs alone; its outputs are channel_1..N.
class PassThrough final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames) noexcept override {
		for (std::size_t i = 0; i < outputs.size(); ++i) {
			std::ranges::copy(inputs[i].first(frames), outputs[i].begin());
		}
	}

	void release() noexcept override {}
};

class RtpInsides final : public NodeInsides {
public:
	AudioCode& audioCode() override { return code_; }

private:
	PassThrough code_;
};

struct RtpSettings {
	RtpAddress address;
	std::uint16_t port;
	RtpEncoding encoding;
	std::uint32_t channels;
	std::optional<std::uint32_t> rate;   // absent: the graph frequency, taken at start
	std::uint32_t latencyMs;
};

class RtpConfig final : public NodeConfig {
public:
	explicit RtpConfig(RtpSettings settings)
	    : settings_(std::move(settings)),
	      ports_{.inputs = {},
	             .outputs = numbered("channel_", settings_.channels),
	             .insideInputs = numbered("from_rtp_", settings_.channels)} {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		grant.rtpReceiver({.address = settings_.address,
		                   .port = settings_.port,
		                   .encoding = settings_.encoding,
		                   .rate = settings_.rate.value_or(grant.format().rate),
		                   .latencyMs = settings_.latencyMs,
		                   .blockPorts = ports_.insideInputs});
		return std::make_unique<RtpInsides>();
	}

private:
	RtpSettings settings_;
	Ports ports_;
};

/// The key's whole number when it lies in [low, high]; nullopt otherwise (absent included).
std::optional<std::uint32_t> wholeNumber(const NodeSettings& settings, std::string_view key, std::int64_t low,
                                         std::int64_t high) {
	const auto value = settings.find(key);
	if (value == settings.end() || !value->is_number_integer()) {
		return std::nullopt;
	}
	const auto number = value->get<std::int64_t>();
	if (number < low || number > high) {
		return std::nullopt;
	}
	return static_cast<std::uint32_t>(number);
}

class RtpType final : public NodeType {
public:
	std::string_view name() const override { return typeName; }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		const auto invalid = [&](std::string detail) {
			return std::unexpected(InvalidSettings{node, std::move(detail)});
		};
		if (!settings.is_object()) {
			return invalid(R"(settings: must be an object with "address", "port", "format" and "channels")");
		}
		for (const auto& [key, value] : settings.items()) {
			if (key != addressKey && key != portKey && key != formatKey && key != channelsKey && key != rateKey &&
			    key != latencyKey) {
				return invalid("unknown key " + key);
			}
		}

		const auto address = settings.find(addressKey);
		if (address == settings.end()) {
			return invalid("address: missing");
		}
		std::optional<RtpAddress> rtpAddress =
		    address->is_string() ? RtpAddress::parse(address->get_ref<const std::string&>()) : std::nullopt;
		if (!rtpAddress) {
			return invalid(R"(address: must be "0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)");
		}

		if (!settings.contains(portKey)) {
			return invalid("port: missing");
		}
		const auto port = wholeNumber(settings, portKey, 1, 65535);
		if (!port) {
			return invalid("port: must be a whole number, 1-65535");
		}

		const auto format = settings.find(formatKey);
		if (format == settings.end()) {
			return invalid("format: missing");
		}
		std::optional<RtpEncoding> encoding;
		if (*format == "L16") encoding = RtpEncoding::l16;
		if (*format == "L24") encoding = RtpEncoding::l24;
		if (!encoding) {
			return invalid(R"(format: must be "L16" or "L24")");
		}

		if (!settings.contains(channelsKey)) {
			return invalid("channels: missing");
		}
		const auto channels = wholeNumber(settings, channelsKey, 1, 64);
		if (!channels) {
			return invalid("channels: must be a whole number, 1-64");
		}

		std::optional<std::uint32_t> rate;
		if (settings.contains(rateKey)) {
			rate = wholeNumber(settings, rateKey, 8000, 192000);
			if (!rate) {
				return invalid("rate: must be a whole number of Hz, 8000-192000");
			}
		}

		std::uint32_t latencyMs = defaultLatencyMs;
		if (settings.contains(latencyKey)) {
			const auto latency = wholeNumber(settings, latencyKey, 1, 1000);
			if (!latency) {
				return invalid("latencyMs: must be a whole number of milliseconds, 1-1000");
			}
			latencyMs = *latency;
		}

		return std::make_unique<const RtpConfig>(RtpSettings{.address = std::move(*rtpAddress),
		                                                     .port = static_cast<std::uint16_t>(*port),
		                                                     .encoding = *encoding,
		                                                     .channels = *channels,
		                                                     .rate = rate,
		                                                     .latencyMs = latencyMs});
	}
};

}  // namespace

std::unique_ptr<const NodeType> makeRtpNodeType() { return std::make_unique<const RtpType>(); }

}  // namespace ear::nodes
