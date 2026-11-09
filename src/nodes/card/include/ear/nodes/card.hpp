#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "card": a sound card. Its settings ("card", "playback", "capture") are the rows in card.cpp, which hold their ranges
/// and defaults; at least one channel count above 0. Ports: inputs
/// "play.1".."play.N", outputs "capture.1".."capture.M"; the inside output bank "to_card" ("to_card.1"..),
/// the inside input bank "from_card". The framework passes audio straight through (play -> to_card,
/// from_card -> capture). Start acquires one card per direction used, joined to its bank. The rate is
/// the graph frequency, never a setting.
std::unique_ptr<const NodeType> makeCardNodeType();
} // namespace ear::nodes
