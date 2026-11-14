#include "ear/core/rate_text.hpp"

#include <cstdint>
#include <format>
#include <optional>

namespace ear {
namespace {

constexpr std::uint64_t largest = 4'294'967'295; // SampleRate's largest

bool digit(char c) { return c >= '0' && c <= '9'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// Whether `text` ends with `unit` (written in lower case), in any case.
bool endsWithUnit(std::string_view text, std::string_view unit) {
	if (text.size() < unit.size()) return false;
	const auto tail = text.substr(text.size() - unit.size());
	for (std::size_t at = 0; at < unit.size(); ++at)
		if (lower(tail[at]) != unit[at]) return false;
	return true;
}

/// A number as the grammar writes it: digits, then optionally '.' and digits.
struct Number {
	std::string_view whole;
	std::string_view fraction; // empty when there is no '.'
};

bool allDigits(std::string_view part) {
	if (part.empty()) return false;
	for (char c : part)
		if (!digit(c)) return false;
	return true;
}

std::optional<Number> numberOf(std::string_view text) {
	const auto dot = text.find('.');
	const auto whole = text.substr(0, dot);
	const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
	if (!allDigits(whole)) return std::nullopt;
	if (dot != std::string_view::npos && !allDigits(fraction)) return std::nullopt;
	return Number{whole, fraction};
}

/// The number times 10^scaleDigits (1 or 1000), exactly, in integers; nothing when that is not whole. A number above
/// every rate (more than 12 significant digits before the point) comes back as one past the largest.
std::optional<std::uint64_t> hertzOf(const Number& number, std::size_t scaleDigits) {
	for (std::size_t at = scaleDigits; at < number.fraction.size(); ++at)
		if (number.fraction[at] != '0') return std::nullopt;
	std::string_view whole = number.whole;
	while (whole.size() > 1 && whole.front() == '0') whole.remove_prefix(1);
	if (whole.size() > 12) return largest + 1;
	std::uint64_t hertz = 0;
	for (char c : whole) hertz = hertz * 10 + static_cast<std::uint64_t>(c - '0');
	for (std::size_t at = 0; at < scaleDigits; ++at)
		hertz = hertz * 10 + (at < number.fraction.size() ? static_cast<std::uint64_t>(number.fraction[at] - '0') : 0);
	return hertz;
}

std::optional<SampleRate> rateOf(std::uint64_t hertz) {
	if (hertz > largest) return std::nullopt;
	return SampleRate::of(static_cast<std::uint32_t>(hertz));
}

constexpr std::string_view notARate =
	"is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or 48000hz";

/// Why a number written with no unit is refused, with what to write instead.
std::string noUnit(std::string_view text, const Number& bare) {
	const auto hertz = hertzOf(bare, 0);
	const auto rate = hertz ? rateOf(*hertz) : std::nullopt;
	if (!rate) return std::format("{} has no unit: write hz or khz after it, such as 48khz", text);
	const std::string asHertz = std::format("{}hz", text);
	const std::string written = rateText(*rate);
	if (written == asHertz) return std::format("{} has no unit: write {}", text, asHertz);
	return std::format("{} has no unit: write {} or {}", text, asHertz, written);
}

} // namespace

std::expected<SampleRate, std::string> readRate(std::string_view text, RateRange range) {
	std::size_t scaleDigits = 0;
	std::string_view number = text;
	if (endsWithUnit(text, "khz")) {
		scaleDigits = 3;
		number.remove_suffix(3);
	} else if (endsWithUnit(text, "hz")) {
		number.remove_suffix(2);
	} else {
		if (const auto bare = numberOf(text)) return std::unexpected(noUnit(text, *bare));
		return std::unexpected(std::format("{} {}", text, notARate));
	}
	const auto parsed = numberOf(number);
	if (!parsed) return std::unexpected(std::format("{} {}", text, notARate));
	const auto hertz = hertzOf(*parsed, scaleDigits);
	if (!hertz) return std::unexpected(std::format("{} is not a whole number of hertz", text));
	const auto rate = rateOf(*hertz);
	if (!rate || *rate < range.low || *rate > range.high)
		return std::unexpected(std::format("{} is outside {}", text, rangeText(range)));
	return *rate;
}

std::string rateText(SampleRate rate) {
	const std::uint32_t hertz = rate.hz();
	if (hertz % 1000 == 0) return std::format("{}khz", hertz / 1000);
	if (hertz % 100 == 0) return std::format("{}.{}khz", hertz / 1000, hertz % 1000 / 100);
	return std::format("{}hz", hertz);
}

NodeSettings rateSetting(SampleRate rate) { return rate.hz(); }

std::optional<SampleRate> rateOfSetting(const NodeSettings& value) {
	if (value.is_number_unsigned()) return rateOf(value.get<std::uint64_t>());
	if (value.is_number_integer() && value.get<std::int64_t>() > 0)
		return rateOf(static_cast<std::uint64_t>(value.get<std::int64_t>()));
	return std::nullopt;
}

std::string rangeText(RateRange range) { return std::format("{} to {}", rateText(range.low), rateText(range.high)); }

} // namespace ear
