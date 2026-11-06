// Little-endian 16 and 24 bit PCM to float and back, exact: a sample divided by 2^15 or 2^23 fits a float's 24-bit
// significand. Pure; no lock, allocation or waiting.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace ear::sendspin::pcm {
inline constexpr float scale16 = 32768.0F;
inline constexpr float scale24 = 8388608.0F;
/// `samples` samples of `bytes` (2 or 3) each from `in`, to `out`.
inline void toFloat(const std::uint8_t* in, std::size_t samples, std::uint32_t bytes, float* out) noexcept {
  if (bytes == 2) {
    for (std::size_t i = 0; i < samples; ++i, in += 2) {
      const auto value = static_cast<std::int16_t>(static_cast<std::uint16_t>(in[0] | (in[1] << 8U)));
      out[i] = static_cast<float>(value) / scale16;
    }
    return;
  }
  for (std::size_t i = 0; i < samples; ++i, in += 3) {
    // The top byte into the sign: the unsigned 32-bit value shifted down arithmetically.
    const auto word = (static_cast<std::uint32_t>(in[0]) << 8U) | (static_cast<std::uint32_t>(in[1]) << 16U) |
                      (static_cast<std::uint32_t>(in[2]) << 24U);
    out[i] = static_cast<float>(static_cast<std::int32_t>(word) >> 8) / scale24;
  }
}
/// `samples` floats from `in`, to samples of `bytes` (2 or 3) each at `out`; what lies outside [-1, 1) is clamped.
inline void fromFloat(const float* in, std::size_t samples, std::uint32_t bytes, std::uint8_t* out) noexcept {
  const float scale = bytes == 2 ? scale16 : scale24;
  for (std::size_t i = 0; i < samples; ++i) {
    const auto value = static_cast<std::int32_t>(std::lrint(std::clamp(static_cast<double>(in[i]) * scale, -static_cast<double>(scale), static_cast<double>(scale) - 1.0)));
    for (std::uint32_t b = 0; b < bytes; ++b) *out++ = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> (8U * b));
  }
}
}
