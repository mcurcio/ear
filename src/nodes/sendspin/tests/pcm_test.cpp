// PCM to float and back, exact (docs/node-types.md#sendspin).
#include <array>
#include <cstdint>
#include <vector>

#include "catch2/catch.hpp"
#include "pcm.hpp"

namespace pcm = ear::sendspin::pcm;

TEST_CASE("Every 16-bit value round-trips exact", "[sendspin][pcm]") {
	std::uint64_t wrong = 0;
	for (std::int32_t value = -32768; value <= 32767; ++value) {
		const auto word = static_cast<std::uint16_t>(value);
		const std::array<std::uint8_t, 2> in{static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8U)};
		float sample = 9.0F;
		pcm::toFloat(in.data(), 1, 2, &sample);
		if (sample != static_cast<float>(value) / 32768.0F) ++wrong;
		std::array<std::uint8_t, 2> out{};
		pcm::fromFloat(&sample, 1, 2, out.data());
		if (out != in) ++wrong;
	}
	CHECK(wrong == 0);
}

TEST_CASE("24-bit edges round-trip exact", "[sendspin][pcm]") {
	for (const std::int32_t value :
	     {-8388608, -8388607, -4194304, -65536, -256, -1, 0, 1, 255, 256, 65535, 4194304, 8388606, 8388607}) {
		INFO(value);
		const auto word = static_cast<std::uint32_t>(value);
		const std::array<std::uint8_t, 3> in{static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8U),
		                                     static_cast<std::uint8_t>(word >> 16U)};
		float sample = 9.0F;
		pcm::toFloat(in.data(), 1, 3, &sample);
		CHECK(sample == static_cast<float>(value) / 8388608.0F);
		std::array<std::uint8_t, 3> out{};
		pcm::fromFloat(&sample, 1, 3, out.data());
		CHECK(out == in);
	}
}

TEST_CASE("A 16-bit sample carried in 24 bits is the same float", "[sendspin][pcm]") {
	const std::array<std::uint8_t, 3> wide{0x00, 0x34, 0x12};
	const std::array<std::uint8_t, 2> narrow{0x34, 0x12};
	float fromWide = 0.0F;
	float fromNarrow = 0.0F;
	pcm::toFloat(wide.data(), 1, 3, &fromWide);
	pcm::toFloat(narrow.data(), 1, 2, &fromNarrow);
	CHECK(fromWide == fromNarrow);
}

TEST_CASE("Going back clamps what lies outside the range", "[sendspin][pcm]") {
	const std::array<float, 2> loud{1.5F, -1.5F};
	std::array<std::uint8_t, 4> out{};
	pcm::fromFloat(loud.data(), 2, 2, out.data());
	CHECK(out == std::array<std::uint8_t, 4>{0xFF, 0x7F, 0x00, 0x80});
}

TEST_CASE("Several samples go in order", "[sendspin][pcm]") {
	const std::vector<std::uint8_t> in{0x00, 0x40, 0x00, 0xC0};
	std::array<float, 2> samples{};
	pcm::toFloat(in.data(), 2, 2, samples.data());
	CHECK(samples == std::array<float, 2>{0.5F, -0.5F});
}
