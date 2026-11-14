// Statistics from the count (header-only; the target ear_statistics puts it on the include path): a statistical check
// takes its bound from the samples it measured, never a fixed margin sized to the host it ran on
// (docs/testing.md#statistics-from-the-count).
#pragma once

#include <cmath>
#include <cstddef>

namespace ear::test {

/// Uniform noise on [-1, 1) (the white node's): its mean square is 1/3 and var(x^2) = 4/45, so the RMS of `frames`
/// independent samples of it is sqrt(1/3) with a standard deviation of sqrt(4/45) / (2 sqrt(1/3)) / sqrt(frames), by
/// the delta method: 0.258 / sqrt(frames).
inline constexpr double kNoiseRms = 0.57735026918962576; // sqrt(1/3)

/// The standard deviation of the RMS of `frames` samples of that noise; infinite for none.
inline double noiseRmsSigma(std::size_t frames) noexcept {
	if (frames == 0) return HUGE_VAL;
	return std::sqrt(4.0 / 45.0) / (2.0 * std::sqrt(1.0 / 3.0)) / std::sqrt(static_cast<double>(frames));
}

/// Whether `rms`, measured over `frames` samples, is that noise's within `sigmas` of its standard deviations. No frames
/// is never within: a check that measured nothing fails.
inline bool noiseRmsWithin(double rms, std::size_t frames, double sigmas) noexcept {
	if (frames == 0) return false;
	return std::abs(rms - kNoiseRms) <= sigmas * noiseRmsSigma(frames);
}

} // namespace ear::test
