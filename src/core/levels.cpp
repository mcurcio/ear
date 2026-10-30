// What a level is (docs/design/batch2.md section 4.2, levels.hpp). Slice s0 stub: slice E1 builds
// the meter and the window size; until then every level reads silent.
#include "ear/core/levels.hpp"

namespace ear {

std::uint32_t levelWindowFrames(std::uint32_t) noexcept { return 0; }   // E1

void LevelMeter::add(std::span<const float>) noexcept {}   // E1

Level LevelMeter::take() noexcept { return {0.0F, 0.0F}; }   // E1

} // namespace ear
