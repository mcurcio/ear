// Frozen interface (docs/design/batch2.md section 4; split from nodes/builtin.hpp by section 11's erratum of
// 2026-11-06; its settings' prose moved to their rows by its erratum "NodeType, 2026-11-06"; no argument since
// docs/design/node-framework.md slice 2, folders being resources). Change it only through an erratum and a re-cut,
// never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "card": a sound card. Its settings ("card", "playback", "capture") are the rows in card.cpp, which hold their ranges
/// and defaults; at least one channel count above 0. Ports: inputs
/// "play.1".."play.N", outputs "capture.1".."capture.M"; the inside output bank "to_card" ("to_card.1"..),
/// the inside input bank "from_card". The framework passes audio straight through (play -> to_card,
/// from_card -> capture). Start acquires one card per direction used, joined to its bank. The rate is
/// the graph frequency (R5), never a setting.
std::unique_ptr<const NodeType> makeCardNodeType();
} // namespace ear::nodes
