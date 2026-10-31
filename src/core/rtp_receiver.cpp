// The RTP address, the one way anything names where an RTP receiver listens (docs/design/rtp.md
// sections 3 and 7.1, rtp_receiver.hpp). Its grammar is closed: "0.0.0.0" or an IPv4 multicast
// group as a plain dotted quad, so no host name (a blocking lookup on the main loop) and no
// unicast address (which PipeWire 1.4.2 reads as the one sender to hear) reaches PipeWire.
#include "ear/core/rtp_receiver.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace ear {
namespace {

/// The four parts of a dotted quad: each 1-3 decimal digits, no leading zero, 0-255. nullopt
/// for anything else (a sign, a space, hex, a prefix length, three or five parts).
std::optional<std::array<unsigned, 4>> dottedQuad(std::string_view text) {
	std::array<unsigned, 4> parts{};
	std::size_t part = 0;
	std::size_t digits = 0;
	unsigned value = 0;
	for (std::size_t i = 0; i <= text.size(); ++i) {
		if (i == text.size() || text[i] == '.') {
			if (digits == 0 || part == parts.size()) return std::nullopt;
			parts[part++] = value;
			digits = 0;
			value = 0;
			continue;
		}
		const char c = text[i];
		if (c < '0' || c > '9') return std::nullopt;
		if (digits == 1 && value == 0) return std::nullopt;   // a leading zero
		value = value * 10 + static_cast<unsigned>(c - '0');
		if (++digits > 3 || value > 255) return std::nullopt;
	}
	if (part != parts.size()) return std::nullopt;
	return parts;
}

constexpr std::string_view anyAddress = "0.0.0.0";

} // namespace

std::optional<RtpAddress> RtpAddress::parse(std::string_view text) {
	const auto parts = dottedQuad(text);
	if (!parts) return std::nullopt;
	const bool any = text == anyAddress;
	const bool multicast = (*parts)[0] >= 224 && (*parts)[0] <= 239;   // 224.0.0.0/4
	if (!any && !multicast) return std::nullopt;
	return RtpAddress(std::string(text));
}

const std::string& RtpAddress::text() const noexcept { return text_; }

bool RtpAddress::multicast() const noexcept { return text_ != anyAddress; }

RtpAddress::RtpAddress(std::string text) : text_(std::move(text)) {}

} // namespace ear
