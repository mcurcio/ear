// knobly places (bytes of UTF-8, from 0) to the wire's (UTF-16 code units, from 1): place.hpp.
#include "place.hpp"

#include <algorithm>
#include <cstddef>

namespace ear::config {
namespace {

bool continuation(unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

/// The bytes of the character starting at `at`, and its UTF-16 code units. A byte that does not start
/// a well-formed sequence is one character of one unit (the browser shows it as one replacement).
std::pair<std::size_t, std::uint32_t> character(std::string_view line, std::size_t at) {
	const auto lead = static_cast<unsigned char>(line[at]);
	std::size_t length = 1;
	if (lead >= 0xF0U && lead <= 0xF4U) length = 4;
	else if (lead >= 0xE0U && lead <= 0xEFU) length = 3;
	else if (lead >= 0xC2U && lead <= 0xDFU) length = 2;
	if (at + length > line.size()) return {1, 1};
	for (std::size_t i = 1; i < length; ++i)
		if (!continuation(static_cast<unsigned char>(line[at + i]))) return {1, 1};
	return {length, length == 4 ? 2U : 1U};
}

} // namespace

control::TextPlace placeIn(std::string_view text, std::uint32_t line0, std::uint32_t byteColumn0) {
	// A line ends after each line feed (knobly's rule), so a text has one more line than it has line feeds.
	const auto lastLine = static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\n'));
	const std::uint32_t line = std::min(line0, lastLine);
	std::size_t start = 0;
	for (std::uint32_t i = 0; i < line; ++i) start = text.find('\n', start) + 1;
	std::string_view rest = text.substr(start);
	rest = rest.substr(0, rest.find('\n'));

	std::uint32_t units = 0;
	for (std::size_t at = 0; at < rest.size() && at < byteColumn0;) {
		const auto [bytes, width] = character(rest, at);
		units += width;
		at += bytes;
	}
	return control::TextPlace{line + 1, units + 1};
}

} // namespace ear::config
