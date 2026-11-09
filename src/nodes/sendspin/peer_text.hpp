// What a character of someone else's text may be on one status line (docs/node-types.md#sendspin). The one
// judge, for a `name` setting and for what a peer sends. Pure.
#pragma once
#include <cstddef>
#include <optional>
#include <string_view>
namespace ear::sendspin {
struct Character {
	char32_t code;
	std::size_t bytes;
};
/// The character starting at `at`, by RFC 3629's UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF), or nothing
/// when the bytes there are not one.
inline std::optional<Character> characterAt(std::string_view text, std::size_t at) noexcept {
	const auto byte = [&](std::size_t where) { return static_cast<unsigned char>(text[where]); };
	const unsigned char lead = byte(at);
	if (lead < 0x80) return Character{lead, 1};
	std::size_t continuation = 0;
	unsigned char low = 0x80;
	unsigned char high = 0xBF;
	char32_t code = 0;
	if (lead >= 0xC2 && lead <= 0xDF) {
		continuation = 1;
		code = lead & 0x1FU;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		continuation = 2;
		code = lead & 0x0FU;
		if (lead == 0xE0) low = 0xA0;
		if (lead == 0xED) high = 0x9F;
	} else if (lead >= 0xF0 && lead <= 0xF4) {
		continuation = 3;
		code = lead & 0x07U;
		if (lead == 0xF0) low = 0x90;
		if (lead == 0xF4) high = 0x8F;
	} else {
		return std::nullopt;
	}
	if (at + continuation >= text.size()) return std::nullopt;
	for (std::size_t c = 1; c <= continuation; ++c) {
		if (byte(at + c) < low || byte(at + c) > high) return std::nullopt;
		code = (code << 6U) | (byte(at + c) & 0x3FU);
		low = 0x80;
		high = 0xBF;
	}
	return Character{code, continuation + 1};
}
/// C0, DEL, C1 and the Unicode line and paragraph separators: a terminal or a log would act on them, or break the line.
inline bool isControl(char32_t code) noexcept {
	return code < 0x20 || (code >= 0x7F && code <= 0x9F) || code == 0x2028 || code == 0x2029;
}
} // namespace ear::sendspin
