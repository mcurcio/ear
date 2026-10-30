// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "white": output "MONO", uniform noise in [-1, 1); settings {} (any key: invalid-settings).
std::unique_ptr<const NodeType> makeWhiteNodeType();
/// "file": inputs "FL", "FR"; settings {"path": non-empty string}. Truncates at start;
/// 32-bit float stereo WAV at the graph frequency; complete once the node is gone.
/// Unopenable path: start fails with startFailed. Ring: 2 s at the graph frequency.
std::unique_ptr<const NodeType> makeFileNodeType();
/// "card": a sound card. Settings: {"card": a CardId (required), "playback": channels 0-64
/// (default 0), "capture": channels 0-64 (default 0)}; at least one above 0; unknown keys
/// invalid; a "card" outside CardId's grammar is invalid. Ports: inputs
/// "playback_1".."playback_N", outputs "capture_1".."capture_M"; inside outputs "to_card_1"..,
/// inside inputs "from_card_1"... Audio passes straight through (playback_i -> to_card_i,
/// from_card_j -> capture_j). Start asks the grant for one card per direction used. The rate is
/// the graph frequency (R5), never a setting.
std::unique_ptr<const NodeType> makeCardNodeType();
std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes();   // white, file, card
}
