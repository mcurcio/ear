// The "rtp" node type: one RTP stream received behind a port block (docs/design/rtp.md sections 3 and 7.3,
// ear/nodes/rtp.hpp; docs/design/node-framework.md sections 7.1 and 11). The framework joins the receiver's channels to
// the inside bank "from_rtp" and passes them straight through to the outputs "stream.1"..: the type writes no audio
// code. Every setting is checked by the table, so nothing reaches PipeWire unparsed: the address is an RtpAddress, the
// format a closed enum, every number ranged.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ear/core/type_spec.hpp"
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
constexpr auto stream = bank::output<"stream">;
constexpr auto fromRtp = bank::insideInput<"from_rtp">;
constexpr TypeSpec spec{.settings =
                            SettingsTable<addressRow, portRow, formatRow, channelsRow, rateRow, latencyRow>{"rtp"},
                        .banks = Banks<stream, fromRtp>{}};
using Checked = CheckedOf<spec>;

struct Rtp {
	static Members<spec> ports(const Checked& s) {
		return {stream(members(s.get<channelsRow>())), fromRtp(members(s.get<channelsRow>()))};
	}

	static StartResult start(StartContext& context, const Checked& s) {
		context.acquire(RtpReceiverRequest{.address = s.get<addressRow>(),
		                                   .port = static_cast<std::uint16_t>(s.get<portRow>()),
		                                   .encoding = s.get<formatRow>(),
		                                   .rate = s.get<rateRow>().value_or(context.format().rate),
		                                   .latencyMs = s.get<latencyRow>(),
		                                   .bank = std::string(fromRtp.name)});
		return passThrough({bankPair(fromRtp, stream)});
	}
};

} // namespace

std::unique_ptr<const NodeType> makeRtpNodeType() { return makeNodeType<spec, Rtp>(); }

} // namespace ear::nodes
