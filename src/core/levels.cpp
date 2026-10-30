// What a level is (docs/design/batch2.md section 4.2, levels.hpp): a port's sample peak and RMS
// over one window, and the window's length on the graph clock.
#include "ear/core/levels.hpp"

#include <algorithm>
#include <cmath>

namespace ear {

std::uint32_t levelWindowFrames(std::uint32_t rate) noexcept {
  return (rate + levelWindowsPerSecond - 1) / levelWindowsPerSecond;
}

void LevelMeter::add(std::span<const float> samples) noexcept {
  for (const float sample : samples) {
    peak_ = std::max(peak_, std::fabs(sample));
    sumSquares_ += static_cast<double>(sample) * static_cast<double>(sample);
  }
  frames_ += samples.size();
}

Level LevelMeter::take() noexcept {
  const Level level{peak_, frames_ == 0 ? 0.0F
                                        : static_cast<float>(std::sqrt(
                                              sumSquares_ / static_cast<double>(frames_)))};
  peak_ = 0.0F;
  sumSquares_ = 0.0;
  frames_ = 0;
  return level;
}

} // namespace ear
