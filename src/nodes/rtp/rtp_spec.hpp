// The "rtp" node type's spec and behaviour (docs/design/rtp.md sections 3 and 7.3; docs/design/node-framework.md
// sections 7.1, 7.6 and 11): private to the type, shared by rtp.cpp and its tests, which run it through
// ear::testing::TypeHarness<spec, Rtp> by the same bank handles. In the type's own named namespace, never an unnamed one
// (type_spec.hpp says why). The framework joins the receiver's channels to the inside bank "from_rtp" and passes them
// straight through to the outputs "stream.1"..: the type writes no audio code. Every setting is checked by the table,
// so nothing reaches PipeWire unparsed: the address is an RtpAddress, the format a closed enum, every number ranged.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ear/core/type_spec.hpp"

namespace ear::nodes::rtp {

/// The receive buffer when the settings name none: about 100 ms (R2). The one home of this
/// default; the framework always passes the value on.
inline constexpr std::uint32_t defaultLatencyMs = 100;

/// The one home of the formats' names.
inline std::optional<RtpEncoding> encodingOf(std::string_view format) {
	if (format == "L16") return RtpEncoding::l16;
	if (format == "L24") return RtpEncoding::l24;
	return std::nullopt;
}

inline constexpr auto addressRow = setting::required(
	"address", setting::text<RtpAddress::parse>(
				   R"("0.0.0.0" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad)"));
inline constexpr auto portRow = setting::required("port", setting::number(1, 65535));
inline constexpr auto formatRow = setting::required("format", setting::text<encodingOf>(R"("L16" or "L24")"));
inline constexpr auto channelsRow = setting::required("channels", setting::number(1, 64));
inline constexpr auto rateRow = setting::optional("rate", setting::number(8000, 192000, "Hz"));
inline constexpr auto latencyRow =
	setting::defaulted("latencyMs", setting::number(1, 1000, "milliseconds"), defaultLatencyMs);
inline constexpr auto stream = bank::output<"stream">;
inline constexpr auto fromRtp = bank::insideInput<"from_rtp">;
inline constexpr TypeSpec spec{
	.settings = SettingsTable<addressRow, portRow, formatRow, channelsRow, rateRow, latencyRow>{"rtp"},
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
		// The receive buffer holds the stream that long before it leaves; PipeWire's receiver reports none of it.
		const std::uint32_t buffered = s.get<latencyRow>() * context.format().rate / 1000;
		context.declareLatency(std::string(stream.name), Latency{buffered, buffered});
		return passThrough({bankPair(fromRtp, stream)});
	}
};

} // namespace ear::nodes::rtp
