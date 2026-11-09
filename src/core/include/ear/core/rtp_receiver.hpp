#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/graph_spec.hpp"
namespace ear {
/// Where an RTP receiver listens: "0.0.0.0" (any sender, sent to any of this host's addresses)
/// or an IPv4 multicast group (224.0.0.0 to 239.255.255.255, joined). Dotted quad only: four
/// decimal parts 0-255, no leading zeros, nothing else. The only way anything names an RTP
/// address, so no user text reaches PipeWire's address parser (which resolves a host name with
/// a blocking lookup on the main loop) and no unicast address does: PipeWire 1.4.2 reads one
/// as "the sender's address and source port", which a sender with its own source port never
/// matches (docs/engine.md#rtp-receivers).
class RtpAddress {
public:
	static std::optional<RtpAddress> parse(std::string_view text); // nullopt outside the grammar
	const std::string& text() const noexcept;                      // as parsed (already canonical)
	bool multicast() const noexcept;                               // false only for "0.0.0.0"
	friend bool operator==(const RtpAddress&, const RtpAddress&) = default;

private:
	explicit RtpAddress(std::string text);
	std::string text_;
};
/// The RTP payload: big-endian signed PCM, RFC 3551's L16 and RFC 3190's L24.
enum class RtpEncoding { l16, l24 };
} // namespace ear
