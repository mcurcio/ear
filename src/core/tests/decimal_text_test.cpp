// A decimal as ear writes it (docs/graph-file.md#settings-and-their-kinds): the shortest fixed text that reads back the
// same.
#include "ear/core/decimal_text.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "catch2/catch.hpp"

namespace {

double readBack(const std::string& text) {
	double value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	REQUIRE(error == std::errc{});
	REQUIRE(end == text.data() + text.size());
	return value;
}

} // namespace

TEST_CASE("A decimal is written as the shortest fixed text, a whole one with no point", "[decimal-text]") {
	CHECK(ear::decimalText(-3.5) == "-3.5");
	CHECK(ear::decimalText(-6.0) == "-6");
	CHECK(ear::decimalText(0.25) == "0.25");
	CHECK(ear::decimalText(1e3) == "1000");
	CHECK(ear::decimalText(1e-4) == "0.0001");
	CHECK(ear::decimalText(-0.04) == "-0.04"); // no rounding: only a gesture's end rounds
	CHECK(ear::decimalText(0.1) == "0.1");
	CHECK(ear::decimalText(24) == "24");
}

TEST_CASE("A negative zero is written 0", "[decimal-text]") {
	CHECK(ear::decimalText(-0.0) == "0");
	CHECK(ear::decimalText(0.0) == "0");
}

TEST_CASE("Every written decimal reads back as the same double, the extremes too", "[decimal-text]") {
	for (const double value : {0.1, -3.5, 1.0 / 3.0, 123456.789, 1e-300, std::numeric_limits<double>::max(),
	                           std::numeric_limits<double>::lowest(), std::numeric_limits<double>::denorm_min()}) {
		const std::string text = ear::decimalText(value);
		INFO(text);
		CHECK(text.find('e') == std::string::npos);
		CHECK(readBack(text) == value);
	}
}

TEST_CASE("A decimal that is not finite is no decimal ear holds", "[decimal-text]") {
	CHECK_THROWS_AS(ear::decimalText(std::numeric_limits<double>::infinity()), std::logic_error);
	CHECK_THROWS_AS(ear::decimalText(std::nan("")), std::logic_error);
}
