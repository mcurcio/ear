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

double roundedDecimal(double value, unsigned decimals) {
	std::string text = decimalText(value);
	const bool negative = text.front() == '-';
	if (negative) text.erase(0, 1);
	const auto point = text.find('.');
	if (point != std::string::npos && text.size() - point - 1 > decimals) {
		const bool up = text[point + 1 + decimals] >= '5';
		text.resize(decimals == 0 ? point : point + 1 + decimals);
		if (up) { // add one in the last place kept, carrying left past the point
			std::size_t at = text.size();
			while (true) {
				if (at == 0) {
					text.insert(text.begin(), '1');
					break;
				}
				--at;
				if (text[at] == '.') continue;
				if (text[at] != '9') {
					++text[at];
					break;
				}
				text[at] = '0';
			}
		}
	}
	double rounded = 0.0;
	std::from_chars(text.data(), text.data() + text.size(), rounded);
	return rounded == 0.0 ? 0.0 : (negative ? -rounded : rounded);
}

} // namespace ear
