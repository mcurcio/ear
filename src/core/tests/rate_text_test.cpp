// A rate as ear.conf and --rate write it (docs/graph-file.md#rates): the grammar, its refusals word for word, the
// ranges, and the form ear writes, read back.
#include "ear/core/rate_text.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include "catch2/catch.hpp"

namespace {

std::uint32_t hertz(std::string_view text, ear::RateRange range = ear::anyRate) {
	const auto rate = ear::readRate(text, range);
	INFO(text << ": " << (rate ? std::string("read") : rate.error()));
	REQUIRE(rate.has_value());
	return rate->hz();
}

std::string refusal(std::string_view text, ear::RateRange range = ear::anyRate) {
	const auto rate = ear::readRate(text, range);
	INFO(text);
	REQUIRE_FALSE(rate.has_value());
	return rate.error();
}

} // namespace

TEST_CASE("A rate is a number and hz or khz, in any case, giving whole hertz", "[rate_text]") {
	CHECK(hertz("48khz") == 48'000);
	CHECK(hertz("44.1khz") == 44'100);
	CHECK(hertz("48000hz") == 48'000);
	CHECK(hertz("48KHZ") == 48'000);
	CHECK(hertz("44.1kHz") == 44'100);
	CHECK(hertz("48000Hz") == 48'000);
	CHECK(hertz("11.025khz") == 11'025);
	CHECK(hertz("48.000khz") == 48'000); // zeros past whole hertz are whole
	CHECK(hertz("48000.0hz") == 48'000); // the same rule for hz
	CHECK(hertz("0048khz") == 48'000);   // leading zeros
	CHECK(hertz("4294967.295khz") == 4'294'967'295);
}

TEST_CASE("A rate that is not a number and a unit is refused with what to write", "[rate_text]") {
	CHECK(refusal("48000") == "48000 has no unit: write 48000hz or 48khz");
	CHECK(refusal("44100") == "44100 has no unit: write 44100hz or 44.1khz");
	CHECK(refusal("11025") == "11025 has no unit: write 11025hz");
	CHECK(refusal("44.1") == "44.1 has no unit: write hz or khz after it, such as 48khz");
	const std::string notARate = " is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or 48000hz";
	for (const std::string_view text : {"48mhz", "fast", "khz", "hz", ".5khz", "48.khz", "-48khz", "+48khz", "4.8e4hz",
	                                    "48 khz", "48k", "48_khz", "1.2.3khz", ""})
		CHECK(refusal(text) == std::string(text) + notARate);
}

TEST_CASE("A rate that is not a whole number of hertz is refused", "[rate_text]") {
	CHECK(refusal("44.1003khz") == "44.1003khz is not a whole number of hertz");
	CHECK(refusal("48000.5hz") == "48000.5hz is not a whole number of hertz");
}

TEST_CASE("A rate outside the range, 0 and one beyond every rate are refused naming the range", "[rate_text]") {
	CHECK(hertz("8khz", ear::graphRates) == 8'000);
	CHECK(hertz("768khz", ear::graphRates) == 768'000);
	CHECK(refusal("7999hz", ear::graphRates) == "7999hz is outside 8khz to 768khz");
	CHECK(refusal("768001hz", ear::graphRates) == "768001hz is outside 8khz to 768khz");
	CHECK(refusal("0hz") == "0hz is outside 1hz to 4294967295hz");
	CHECK(refusal("4294967296hz") == "4294967296hz is outside 1hz to 4294967295hz");
	CHECK(refusal("99999999999999999999khz") == "99999999999999999999khz is outside 1hz to 4294967295hz");
}

TEST_CASE("ear writes khz for whole kHz and hundreds of Hz, else hz, and reads it back", "[rate_text]") {
	const auto text = [](std::uint32_t hz) { return ear::rateText(*ear::SampleRate::of(hz)); };
	CHECK(text(48'000) == "48khz");
	CHECK(text(44'100) == "44.1khz");
	CHECK(text(88'200) == "88.2khz");
	CHECK(text(11'025) == "11025hz");
	CHECK(text(22'050) == "22050hz");
	CHECK(text(1) == "1hz");
	for (const std::uint32_t hz : {1u, 8'000u, 11'025u, 22'050u, 44'100u, 48'000u, 768'000u, 4'294'967'295u})
		CHECK(hertz(text(hz)) == hz);
	CHECK(ear::rangeText(ear::graphRates) == "8khz to 768khz");
}

TEST_CASE("A rate setting is a JSON whole number of hertz, both ways", "[rate_text]") {
	CHECK(ear::rateSetting(ear::SampleRate::fixed<44'100>()) == ear::NodeSettings(44'100));
	CHECK(ear::rateOfSetting(ear::NodeSettings(44'100)) == ear::SampleRate::fixed<44'100>());
	CHECK(ear::rateOfSetting(ear::NodeSettings(std::uint64_t{4'294'967'295})) ==
	      ear::SampleRate::fixed<4'294'967'295>());
	for (const auto& bad :
	     {ear::NodeSettings(0), ear::NodeSettings(-48'000), ear::NodeSettings(std::uint64_t{1} << 32),
	      ear::NodeSettings(48'000.0), ear::NodeSettings("48khz"), ear::NodeSettings(true), ear::NodeSettings()})
		CHECK_FALSE(ear::rateOfSetting(bad).has_value());
}
