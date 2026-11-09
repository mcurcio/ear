// Engine names and numbers EAR's adapters share (docs/engine.md). Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

namespace ear::pipewire {

/// node.group of every port block and of the internal clock (engine.conf's Dummy-Driver): one
/// clock for all blocks (port_blocks.cpp).
inline constexpr char graphGroup[] = "ear.graph";

/// priority.driver of a sound card (sound_cards.cpp). It must stay below engine.conf's
/// Dummy-Driver (200000), so the internal clock always drives and a linked card follows it
/// (docs/engine.md#the-internal-clock).
inline constexpr int cardPriority = 1;

/// The internal clock's clock.name: support.node.driver names it "clock.system.<clock.id>", and
/// engine.conf sets clock.id = monotonic-raw. A following ALSA node whose own clock.name is this
/// one believes it shares the driver's clock and turns its rate matching off (alsa-pcm.c,
/// setup_matching): only CI's test card, which has no clock, is made so (sound_cards.hpp).
inline constexpr char internalClockName[] = "clock.system.monotonic-raw";

} // namespace ear::pipewire
