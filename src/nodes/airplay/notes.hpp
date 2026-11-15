// The texts an airplay node says (docs/node-types.md#airplay): each one's one home. status_log.cpp frames them as
// `node <n> says <text>`.
#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
namespace ear::nodes::airplay::notes {
/// A listing is on the network, once probing found its names free: the instance it holds (shairport-sync's
/// `<device id>@<speaker name>` for _raop._tcp, the speaker name for AirPlay 2's _airplay._tcp, or its rename) and its
/// RTSP port. Said once per service.
inline std::string listed(std::string_view instance, std::uint16_t port) {
	return std::format("listed {} on port {}", instance, port);
}
/// Another responder held the instance shairport-sync asked for, so the listing goes by `published` (RFC 6762 section
/// 9's "<name> (n)"; docs/node-types.md#mdns). Said before its listed.
inline std::string renamed(std::string_view asked, std::string_view published) {
	return std::format("renamed {} to {}", asked, published);
}
/// Its listing could not be announced (the advertiser's words, or the report's): it plays to a sender that knows it.
inline std::string advertiseFailed(std::string_view why) { return std::format("advertise-failed {}", why); }
/// A sender began playing to it, and stopped: shairport-sync's sessions, never the node's state.
inline constexpr std::string_view playing = "playing", idle = "idle";
} // namespace ear::nodes::airplay::notes
