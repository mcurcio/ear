// The "rtp" node type: one RTP stream received behind a port block (docs/design/rtp.md sections 3
// and 7.3, ear/nodes/rtp.hpp). The framework joins the receiver's channels to the inside inputs
// "from_rtp_1"..; they pass straight through to the outputs "stream.1"... Every setting is
// checked by the table, so nothing reaches PipeWire unparsed: the address is an RtpAddress, the
// format a closed enum, every number ranged.
#include <algorithm>
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

/// The receive buffer when the settings name none: about 100 ms (R2). The one home of this
/// default; the framework always passes the value on.
constexpr std::uint32_t defaultLatencyMs = 100;

/// The one home of the formats' names.
std::optional<RtpEncoding> encodingOf(std::string_view format) {
	if (format == "L16") return RtpEncoding::l16;
	if (format == "L24") return RtpEncoding::l24;
	return std::nullopt;
}

constexpr auto addressRow = setting::required(
	"address", setting::text<RtpAddress::parse>(
				   R"("0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)"));
constexpr auto portRow = setting::required("port", setting::number(1, 65535));
constexpr auto formatRow = setting::required("format", setting::text<encodingOf>(R"("L16" or "L24")"));
constexpr auto channelsRow = setting::required("channels", setting::number(1, 64));
constexpr auto rateRow = setting::optional("rate", setting::number(8000, 192000, "Hz"));
constexpr auto latencyRow = setting::defaulted("latencyMs", setting::number(1, 1000, "milliseconds"), defaultLatencyMs);
constexpr SettingsTable<addressRow, portRow, formatRow, channelsRow, rateRow, latencyRow> table{"rtp"};

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

class RtpType final : public NodeTypeOf<table> {
	MadeConfig make(const NodeName&, const Checked& settings) const override {
		return std::make_unique<const RtpConfig>(
			RtpSettings{.address = settings.get<addressRow>(),
		                .port = static_cast<std::uint16_t>(settings.get<portRow>()),
		                .encoding = settings.get<formatRow>(),
		                .channels = settings.get<channelsRow>(),
		                .rate = settings.get<rateRow>(),
		                .latencyMs = settings.get<latencyRow>()});
	}
};

} // namespace

std::unique_ptr<const NodeType> makeRtpNodeType() { return std::make_unique<const RtpType>(); }

} // namespace ear::nodes
