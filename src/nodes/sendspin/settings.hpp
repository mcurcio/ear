// A sendspin node's settings (docs/design/sendspin.md section 4): its table and the one parse. Pure.
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "ear/core/command_error.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
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
inline constexpr std::string_view typeName = "sendspin";
inline constexpr std::uint16_t defaultListenPort = 8928;
/// Every port pair of the node and each direction's audio: stereo (section 4).
inline constexpr std::uint32_t nodeChannels = 2;
struct SendspinSettings {
	std::optional<ServerUrl> server; // with it, ear dials; without it, ear listens on `port`
	std::uint16_t port;
	std::string name;
};
/// The first failure is told, in this order: object, unknown keys (in the object's order), server, port, name. `node`: the
/// node's name, the default of "name".
std::expected<SendspinSettings, std::string> parseSettings(const NodeName& node, const NodeSettings& settings);
/// NodeType::settingKinds(): the table's.
std::span<const SettingDeclaration> settingDeclarations() noexcept;
} // namespace ear::sendspin
