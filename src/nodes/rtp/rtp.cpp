// The "rtp" node type: one RTP stream received behind a port block (docs/design/rtp.md sections 3
// and 7.3, ear/nodes/rtp.hpp). The framework joins the receiver's channels to the inside inputs
// "from_rtp_1"..; they pass straight through to the outputs "stream.1"... Every setting is
// checked by the table, so nothing reaches PipeWire unparsed: the address is an RtpAddress, the
// format a closed enum, every number ranged.
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ear/core/settings_table.hpp"
#include "ear/nodes/rtp.hpp"

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

/// The one home of the formats' names.
std::optional<RtpEncoding> encodingOf(std::string_view format) {
	if (format == "L16") return RtpEncoding::l16;
	if (format == "L24") return RtpEncoding::l24;
	return std::nullopt;
}

using enum SettingKind;
constexpr SettingsTable table{
	typeName,
	std::array{
		SettingRule{.key = addressKey,
                    .kind = text,
                    .required = true,
                    .mustBe = R"("0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)",
                    .means = [](std::string_view value) { return RtpAddress::parse(value).has_value(); }},
		SettingRule{.key = portKey, .kind = wholeNumber, .required = true, .low = 1, .high = 65535},
		SettingRule{.key = formatKey,
                    .kind = text,
                    .required = true,
                    .mustBe = R"("L16" or "L24")",
                    .means = [](std::string_view value) { return encodingOf(value).has_value(); }},
		SettingRule{.key = channelsKey, .kind = wholeNumber, .required = true, .low = 1, .high = 64},
		SettingRule{.key = rateKey, .kind = wholeNumber, .low = 8000, .high = 192000, .unit = "Hz"},
		SettingRule{.key = latencyKey,
                    .kind = wholeNumber,
                    .low = 1,
                    .high = 1000,
                    .unit = "milliseconds",
                    .byDefault = defaultLatencyMs}}};

/// Copies from_rtp_i to stream.i. The node has no public inputs, so its inputs are the inside
/// inputs alone; its outputs are stream.1..N.
class PassThrough final : public AudioCode {
public:
	void prepare(const AudioFormat&) override {}

	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle&) noexcept override {
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
	std::optional<std::uint32_t> rate; // absent: the graph frequency, taken at start
	std::uint32_t latencyMs;
};

class RtpConfig final : public NodeConfig {
public:
	explicit RtpConfig(RtpSettings settings)
		: settings_(std::move(settings)), ports_{{{Direction::output, "stream", numbered(settings_.channels)}},
	                                             numbered(settings_.channels, "from_rtp_")} {}

	const Ports& ports() const override { return ports_; }

	std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const override {
		grant.rtpReceiver({.address = settings_.address,
		                   .port = settings_.port,
		                   .encoding = settings_.encoding,
		                   .rate = settings_.rate.value_or(grant.format().rate),
		                   .latencyMs = settings_.latencyMs,
		                   .blockPorts = ports_.insideInputs()});
		return std::make_unique<RtpInsides>();
	}

private:
	RtpSettings settings_;
	Ports ports_;
};

class RtpType final : public NodeType {
public:
	std::string_view name() const override { return typeName; }

	std::span<const SettingDeclaration> settingKinds() const override { return table.kinds(); }

	std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
	parse(const NodeName& node, const NodeSettings& settings) const override {
		auto checked = table.check(settings);
		if (!checked) return std::unexpected(InvalidSettings{node, std::move(checked.error())});
		return std::make_unique<const RtpConfig>(
			RtpSettings{.address = *RtpAddress::parse(*checked->text(addressKey)),
		                .port = static_cast<std::uint16_t>(*checked->wholeNumber(portKey)),
		                .encoding = *encodingOf(*checked->text(formatKey)),
		                .channels = *checked->wholeNumber(channelsKey),
		                .rate = checked->wholeNumber(rateKey),
		                .latencyMs = *checked->wholeNumber(latencyKey)});
	}
};

} // namespace

std::unique_ptr<const NodeType> makeRtpNodeType() { return std::make_unique<const RtpType>(); }

} // namespace ear::nodes
