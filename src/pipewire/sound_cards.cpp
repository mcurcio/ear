// Sound cards as ALSA adapter nodes with their own links (docs/design/batch2.md section 4.3,
// sound_cards.hpp). Slice s0 stub: slice E2 builds the adapter and its state machine.
#include "ear/pipewire/sound_cards.hpp"

#include <stdexcept>

namespace ear::pipewire {

AlsaSoundCards::AlsaSoundCards(Engine& engine) : engine_(engine) {}

std::unique_ptr<SoundCard> AlsaSoundCards::make(const NodeName&, const NodeName&,
                                                const SoundCardRequest&, const AudioFormat&,
                                                SoundCardEvents&) {
  throw std::logic_error("AlsaSoundCards::make is not built yet (slice E2)");
}

}  // namespace ear::pipewire
