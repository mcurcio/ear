#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "file": inputs "record.left", "record.right"; settings {"path": a path relative to the recordings folder}: non-empty,
/// not absolute, no ".." component (else invalid). A recording (RecordingRequest): its path claimed, so a second node on
/// it fails start-failed "cannot open <path>: held by node <n>", then opened beneath the folder only and truncated, off
/// the main loop; a path the folder refuses or cannot open fails the node start-failed, "cannot open <path>: <why>".
/// Recorded through a sink on its inside output bank "writer" (2 s ring at the graph
/// frequency): the WAV is opened on the sink's own thread, and a libsndfile refusal fails the node start-failed before
/// it is configured. 32-bit float stereo WAV at the graph frequency, every cycle the node's code drove (none while
/// stopped); complete once its configuration is torn down (a restart, a disable, a removal).
std::unique_ptr<const NodeType> makeFileNodeType();
} // namespace ear::nodes
