// Engine names and numbers EAR's adapters share (docs/design/batch2.md section 4.3). Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

namespace ear::pipewire {

/// node.group of every port block: one clock for all blocks (port_blocks.cpp).
inline constexpr char graphGroup[] = "ear.graph";

/// priority.driver of a sound card (sound_cards.cpp). It must stay below engine.conf's
/// Dummy-Driver (200000), so a card running alone is never the fallback driver.
inline constexpr int cardPriority = 1;

}  // namespace ear::pipewire
