#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <utility>

#include "peer_text.hpp"

namespace ear::sendspin {
namespace {

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

std::optional<std::string> parseName(std::string_view text) {
	if (text.empty() || text.size() > maxNameBytes) return std::nullopt;
	for (std::size_t at = 0; at < text.size();) {
		const auto character = characterAt(text, at);
		if (!character || isControl(character->code)) return std::nullopt;
		at += character->bytes;
	}
	return std::string(text);
}

SendspinSettings settingsFrom(const NodeName& node, const decltype(settingsTable)::Checked& checked) {
	return SendspinSettings{.server = checked.get<rows::server>(),
	                        .port = static_cast<std::uint16_t>(checked.get<rows::port>()),
	                        .name = checked.get<rows::name>().value_or(node)};
}

} // namespace ear::sendspin
