// Statistics from the count (test/support/statistics.hpp): the noise's RMS bound follows the frames measured, refuses a
// measurement of nothing, and holds real uniform noise at every length while it refuses a gain off by 2% once the count
// can see it.
#include <cmath>
#include <cstddef>
#include <random>

#include "catch2/catch.hpp"
#include "statistics.hpp"

namespace {
using ear::test::kNoiseRms;
using ear::test::noiseRmsSigma;
using ear::test::noiseRmsWithin;

/// The RMS of `frames` samples of uniform noise on [-1, 1) times `gain`, from a fixed seed: the same every run.
double rmsOf(std::size_t frames, double gain, unsigned seed) {
	std::mt19937 generator(seed);
	std::uniform_real_distribution<double> noise(-1.0, 1.0);
	double squares = 0.0;
	for (std::size_t i = 0; i < frames; ++i) {
		const double sample = gain * noise(generator);
		squares += sample * sample;
	}
	return std::sqrt(squares / static_cast<double>(frames));
}

} // namespace

TEST_CASE("The noise's RMS deviation shrinks with the square root of the frames measured", "[statistics]") {
	CHECK(kNoiseRms == Approx(std::sqrt(1.0 / 3.0)));
	CHECK(noiseRmsSigma(1600) == Approx(0.006455).margin(1e-6)); // one level window
	CHECK(noiseRmsSigma(22400) == Approx(noiseRmsSigma(1600) / std::sqrt(14.0)));
	CHECK(std::isinf(noiseRmsSigma(0)));
	CHECK(noiseRmsWithin(kNoiseRms, 1, 0.0));
	CHECK(noiseRmsWithin(kNoiseRms + 4.9 * noiseRmsSigma(1600), 1600, 5.0));
	CHECK_FALSE(noiseRmsWithin(kNoiseRms + 5.1 * noiseRmsSigma(1600), 1600, 5.0));
	CHECK_FALSE(noiseRmsWithin(kNoiseRms - 5.1 * noiseRmsSigma(1600), 1600, 5.0));
	CHECK_FALSE(noiseRmsWithin(kNoiseRms, 0, 5.0)); // nothing measured is never within
}

TEST_CASE("Real uniform noise is within 5 deviations at every length, and a gain off by 2% is not once the count "
          "can see it",
          "[statistics]") {
	for (const std::size_t frames : {256U, 1600U, 22400U, 48000U, 480000U}) {
		for (unsigned seed = 1; seed <= 20; ++seed) {
			INFO(frames << " frames, seed " << seed << ": rms " << rmsOf(frames, 1.0, seed));
			CHECK(noiseRmsWithin(rmsOf(frames, 1.0, seed), frames, 5.0));
		}
	}
	// 2% of 0.577 is 0.0115: 1.8 deviations at 1600 frames, 6.7 at 22400 (the control port's recording).
	CHECK(noiseRmsWithin(rmsOf(1600, 1.02, 7), 1600, 5.0));
	CHECK_FALSE(noiseRmsWithin(rmsOf(22400, 1.02, 7), 22400, 5.0));
	CHECK_FALSE(noiseRmsWithin(rmsOf(22400, 0.98, 7), 22400, 5.0));
	CHECK_FALSE(noiseRmsWithin(0.0, 22400, 5.0)); // silence
}
