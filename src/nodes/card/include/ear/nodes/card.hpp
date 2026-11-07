// Frozen interface (docs/design/batch2.md section 4; split from nodes/builtin.hpp by section 11's erratum of
// 2026-11-06; its settings' prose moved to their rows by its erratum "NodeType, 2026-11-06"). Change it only through an
// erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "card": a sound card. Its settings ("card", "playback", "capture") are the rows in card.cpp, which hold their ranges
/// and defaults; at least one channel count above 0. Ports: inputs
/// "play.1".."play.N", outputs "capture.1".."capture.M"; inside outputs "to_card_1"..,
/// inside inputs "from_card_1"... Audio passes straight through (play.i -> to_card_i,
/// from_card_j -> capture.j). Start asks the grant for one card per direction used. The rate is
/// the graph frequency (R5), never a setting.
std::unique_ptr<const NodeType> makeCardNodeType(const NodeFolders& folders);
} // namespace ear::nodes
