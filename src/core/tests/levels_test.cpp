// What a level is (docs/design/batch2.md section 4.2, levels.hpp; acceptance criterion E1(1)):
// a port's peak and RMS over a window, and the window's length at each rate.
#include <cmath>
#include <numbers>
#include <vector>

#include "catch2/catch.hpp"
#include "ear/core/levels.hpp"

using namespace ear;

TEST_CASE("A constant signal's peak and RMS are its value", "[levels]") {
  LevelMeter meter;
  const std::vector<float> samples(1600, 0.5F);
  meter.add(samples);
  const Level level = meter.take();
  CHECK(level.peak == 0.5F);
  CHECK(level.rms == Approx(0.5F).margin(1e-6));
}

TEST_CASE("A full-scale sine's RMS is 1/sqrt(2), across several adds", "[levels]") {
  LevelMeter meter;
  constexpr std::size_t kFrames = 48000;   // a whole number of periods of 1 kHz at 48 kHz
  std::vector<float> samples(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    samples[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
  }
  for (std::size_t at = 0; at < kFrames; at += 256) {
    meter.add(std::span<const float>(samples).subspan(at, std::min<std::size_t>(256, kFrames - at)));
  }
  const Level level = meter.take();
  CHECK(level.peak == Approx(1.0F).margin(1e-6));
  CHECK(level.rms == Approx(0.7071F).margin(1e-4));
}

TEST_CASE("take() starts a new window, and a window with no samples is silent", "[levels]") {
  LevelMeter meter;
  const std::vector<float> loud{-0.75F, 0.25F};
  meter.add(loud);
  CHECK(meter.take().peak == 0.75F);
  const Level empty = meter.take();
  CHECK(empty.peak == 0.0F);
  CHECK(empty.rms == 0.0F);
  const std::vector<float> quiet(4, 0.125F);
  meter.add(quiet);
  const Level next = meter.take();
  CHECK(next.peak == 0.125F);
  CHECK(next.rms == Approx(0.125F).margin(1e-6));
}

TEST_CASE("A window is 1/30 s of the graph clock, rounded up", "[levels]") {
  CHECK(levelWindowFrames(48000) == 1600);
  CHECK(levelWindowFrames(44100) == 1470);
  CHECK(levelWindowFrames(96000) == 3200);
  CHECK(levelWindowFrames(22050) == 735);
  CHECK(levelWindowFrames(1) == 1);
}
