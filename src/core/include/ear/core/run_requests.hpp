// What every acquire of a run checks and names (docs/design/node-framework.md sections 6, I9, and 7.3), the one home
// of both for NodeHost and the type harness (ear/testing/node_harness.hpp), so a type the harness accepts is one
// NodeHost accepts, and the reverse. Pure.
#pragma once
#include <optional>
#include <string_view>
#include "ear/core/node_type.hpp"
namespace ear {
/// The suffix of a run's outside node: "playback" or "capture" (a sound card), "rtp" (an RTP receiver), so its engine
/// node is "<node>.<suffix>" (I9; a node name never holds '.', so they cannot collide with a node). Nothing for a
/// handoff or a recording, which make no engine node.
std::optional<std::string_view> outsideSuffix(const ResourceRequest& request) noexcept;
/// The bank `request` names, checked against the node's ports: an outside node joins an inside bank of its direction
/// (a playback card an inside output, a capture card or an RTP receiver an inside input); a handoff takes an input bank
/// and a consumer. Null for a recording, which names none. Throws std::logic_error naming `node` (a type bug) for a bank
/// the node lacks or of the wrong reach or direction, or a handoff with no consumer.
const Ports::Bank* requestedBank(const Ports& ports, const NodeName& node, const ResourceRequest& request);
} // namespace ear
