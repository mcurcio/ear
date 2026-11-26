#include "ear/core/decimal_text.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <system_error>

namespace ear {

std::string decimalText(double value) {
	if (!std::isfinite(value)) throw std::logic_error("decimalText: a decimal is finite");
	if (value == 0.0) return "0"; // a negative zero too: ear.conf never shows -0
	// The longest fixed form of a finite double is under 330 characters (DBL_MAX has 309 digits; the smallest
	// subnormal 324 places after the point, with its sign and "0.").
	std::array<char, 400> text{};
	const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
	if (error != std::errc{}) throw std::logic_error("decimalText: the buffer is too short");
	return std::string(text.data(), end);
}

} // namespace ear
