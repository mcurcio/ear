#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>

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

/// RFC 3629's UTF-8: no overlong form, no surrogate, nothing above U+10FFFF.
bool validUtf8(std::string_view text) noexcept {
	const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
	for (std::size_t i = 0; i < text.size();) {
		const unsigned char lead = byte(i);
		std::size_t continuation = 0;
		unsigned char low = 0x80;
		unsigned char high = 0xBF;
		if (lead < 0x80) {
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			continuation = 1;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			continuation = 2;
			if (lead == 0xE0) low = 0xA0;
			if (lead == 0xED) high = 0x9F;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			continuation = 3;
			if (lead == 0xF0) low = 0x90;
			if (lead == 0xF4) high = 0x8F;
		} else {
			return false;
		}
		if (i + continuation >= text.size() + (continuation == 0 ? 1 : 0)) return false;
		for (std::size_t c = 1; c <= continuation; ++c) {
			if (byte(i + c) < low || byte(i + c) > high) return false;
			low = 0x80;
			high = 0xBF;
		}
		i += continuation + 1;
	}
	return true;
}

bool validName(std::string_view name) noexcept {
	return !name.empty() && name.size() <= maxNameBytes && validUtf8(name) &&
	       std::ranges::none_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
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
	if (!std::ranges::all_of(host, [](char c) { return isAlphanumeric(c) || c == '-' || c == '.'; })) return std::nullopt;
	if (colon != std::string_view::npos && !validPort(authority.substr(colon + 1))) return std::nullopt;

	const bool goodPath = std::ranges::all_of(path, [](char c) {
		return isAlphanumeric(c) || c == '/' || c == '.' || c == '_' || c == '~' || c == '-';
	});
	if (!goodPath) return std::nullopt;
	return ServerUrl(std::string(text));
}

std::expected<SendspinSettings, std::string> parseSettings(const NodeName& node, const NodeSettings& settings) {
	if (!settings.is_object()) {
		return std::unexpected(R"(settings: must be an object with optional "server", "port" and "name")");
	}
	for (const auto& [key, value] : settings.items()) {
		if (key != serverKey && key != portKey && key != nameKey) return std::unexpected("unknown key " + key);
	}

	std::optional<ServerUrl> url;
	if (const auto server = settings.find(serverKey); server != settings.end()) {
		url = server->is_string() ? ServerUrl::parse(server->get_ref<const std::string&>()) : std::nullopt;
		if (!url) return std::unexpected("server: must be ws://<host>[:<port>]/<path>");
	}

	std::uint16_t port = defaultListenPort;
	if (const auto given = settings.find(portKey); given != settings.end()) {
		if (url) return std::unexpected(R"(port: only without "server")");
		if (!given->is_number_integer() || *given < 1 || *given > 65535) return std::unexpected("port: must be a whole number, 1-65535");
		port = given->get<std::uint16_t>();
	}

	std::string name = node;
	if (const auto given = settings.find(nameKey); given != settings.end()) {
		if (!given->is_string() || !validName(given->get_ref<const std::string&>())) {
			return std::unexpected("name: must be 1 to 64 bytes of UTF-8 with no control characters");
		}
		name = given->get<std::string>();
	}
	return SendspinSettings{.server = std::move(url), .port = port, .name = std::move(name)};
}

} // namespace ear::sendspin
