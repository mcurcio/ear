// A sendspin node's settings (docs/design/sendspin.md section 4): its table and what the type makes of it. Pure.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "ear/core/graph_spec.hpp"
#include "ear/core/settings_table.hpp"
namespace ear::sendspin {
/// ws://<host>[:<port>]/<path>: a host of letters, digits, '-' and '.' (a dotted quad among them), at most 253; a port
/// of 1-65535; a path of '/' and [A-Za-z0-9._~-]. No wss://, no user, query or fragment. Only parse() makes one.
class ServerUrl {
public:
	static std::optional<ServerUrl> parse(std::string_view text);
	const std::string& text() const noexcept { return text_; }
	friend bool operator==(const ServerUrl&, const ServerUrl&) = default;

private:
	explicit ServerUrl(std::string text) : text_(std::move(text)) {}
	std::string text_;
};
inline constexpr std::uint16_t defaultListenPort = 8928;
/// Every port pair of the node and each direction's audio: stereo (section 4).
inline constexpr std::uint32_t nodeChannels = 2;
struct SendspinSettings {
	std::optional<ServerUrl> server; // with it, ear dials; without it, ear listens on `port`
	std::uint16_t port;
	std::string name;
};
/// The name, nothing outside 1-64 bytes of UTF-8 or with a control character.
std::optional<std::string> parseName(std::string_view text);
namespace rows {
inline constexpr auto server =
	setting::optional("server", setting::text<ServerUrl::parse>("ws://<host>[:<port>]/<path>"));
inline constexpr auto port =
	setting::defaulted("port", setting::number(1, 65535), defaultListenPort).onlyWithout(server);
inline constexpr auto name =
	setting::optional("name", setting::text<parseName>("1 to 64 bytes of UTF-8 with no control characters"));
} // namespace rows
inline constexpr SettingsTable<rows::server, rows::port, rows::name> settingsTable{"sendspin"};
/// Pure. `node`: the node's name, the default of "name".
SendspinSettings settingsFrom(const NodeName& node, const decltype(settingsTable)::Checked& checked);
} // namespace ear::sendspin
