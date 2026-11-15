// The texts an airplay node says (docs/node-types.md#airplay): each one's one home. status_log.cpp frames them as
// `node <n> says <text>`.
#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
namespace ear::nodes::airplay::notes {
/// Its listing is on the network: the instance shairport-sync named (`<device id>@<speaker name>`) and its RTSP port.
inline std::string listed(std::string_view instance, std::uint16_t port) {
	return std::format("listed {} on port {}", instance, port);
}
/// Its listing could not be announced (the advertiser's words, or the report's): it plays to a sender that knows it.
inline std::string advertiseFailed(std::string_view why) { return std::format("advertise-failed {}", why); }
/// A sender began playing to it, and stopped: shairport-sync's sessions, never the node's state.
inline constexpr std::string_view playing = "playing", idle = "idle";
} // namespace ear::nodes::airplay::notes
