// The level bell: the eventfd the realtime thread rings when levels are published
// (docs/design/batch2.md sections 2 and 4.6, control_port.hpp). Slice s0 stub: slice R2 builds it.
#include "ear/server/control_port.hpp"

#include <stdexcept>

namespace ear::server {

struct LevelBell::Impl {};

LevelBell::LevelBell() { throw std::logic_error("LevelBell is not built yet (slice R2)"); }

LevelBell::~LevelBell() = default;

void LevelBell::levelsPublished() noexcept {}   // R2

int LevelBell::fd() const noexcept { return -1; }   // R2

} // namespace ear::server
