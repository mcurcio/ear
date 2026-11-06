#include "settings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <utility>

#include "ear/core/settings_table.hpp"
#include "peer_text.hpp"

namespace ear::sendspin {
namespace {

constexpr std::string_view serverKey = "server";
constexpr std::string_view portKey = "port";
constexpr std::string_view nameKey = "name";
constexpr std::size_t maxHostBytes = 253;
constexpr std::size_t maxNameBytes = 64;
constexpr std::string_view scheme = "ws://";

bool isAlphanumeric(char c) noexcept {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool validPort(std::string_view text) noexcept {
	std::uint32_t port = 0;
	const auto* const end = text.data() + text.size();
	const auto [stopped, error] = std::from_chars(text.data(), end, port);
	return error == std::errc{} && stopped == end && port >= 1 && port <= 65535;
}

bool validName(std::string_view name) noexcept {
	if (name.empty() || name.size() > maxNameBytes) return false;
	for (std::size_t at = 0; at < name.size();) {
		const auto character = characterAt(name, at);
		if (!character || isControl(character->code)) return false;
		at += character->bytes;
	}
	return true;
}

using enum SettingKind;
constexpr SettingsTable table{
	typeName,
	std::array{SettingRule{.key = serverKey,
                           .kind = text,
                           .mustBe = "ws://<host>[:<port>]/<path>",
                           .means = [](std::string_view value) { return ServerUrl::parse(value).has_value(); }},
               SettingRule{.key = portKey,
                           .kind = wholeNumber,
                           .low = 1,
                           .high = 65535,
                           .byDefault = defaultListenPort,
                           .across = [](const NodeSettings& settings) -> std::optional<std::string_view> {
							   if (settings.contains(serverKey)) return R"(port: only without "server")";
							   return std::nullopt;
						   }},
               SettingRule{.key = nameKey,
                           .kind = text,
                           .mustBe = "1 to 64 bytes of UTF-8 with no control characters",
                           .means = validName}}};

} // namespace

std::optional<ServerUrl> ServerUrl::parse(std::string_view text) {
	if (!text.starts_with(scheme)) return std::nullopt;
	const std::string_view rest = text.substr(scheme.size());
	const std::size_t slash = rest.find('/');
	if (slash == std::string_view::npos) return std::nullopt;
	const std::string_view authority = rest.substr(0, slash);
	const std::string_view path = rest.substr(slash);

	const std::size_t colon = authority.find(':');
	const std::string_view host = authority.substr(0, colon);
	if (host.empty() || host.size() > maxHostBytes) return std::nullopt;
	if (!std::ranges::all_of(host, [](char c) { return isAlphanumeric(c) || c == '-' || c == '.'; }))
		return std::nullopt;
	if (colon != std::string_view::npos && !validPort(authority.substr(colon + 1))) return std::nullopt;

	const bool goodPath = std::ranges::all_of(
		path, [](char c) { return isAlphanumeric(c) || c == '/' || c == '.' || c == '_' || c == '~' || c == '-'; });
	if (!goodPath) return std::nullopt;
	return ServerUrl(std::string(text));
}

std::expected<SendspinSettings, std::string> parseSettings(const NodeName& node, const NodeSettings& settings) {
	auto checked = table.check(settings);
	if (!checked) return std::unexpected(std::move(checked.error()));
	std::optional<ServerUrl> url;
	if (const auto server = checked->text(serverKey)) url = ServerUrl::parse(*server);
	return SendspinSettings{.server = std::move(url),
	                        .port = static_cast<std::uint16_t>(*checked->wholeNumber(portKey)),
	                        .name = std::string(checked->text(nameKey).value_or(node))};
}

std::span<const SettingDeclaration> settingDeclarations() noexcept { return table.kinds(); }

} // namespace ear::sendspin
