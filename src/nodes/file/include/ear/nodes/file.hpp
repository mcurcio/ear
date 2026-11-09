// Frozen interface (docs/design/batch2.md section 4; split from nodes/builtin.hpp by section 11's erratum of
// 2026-11-06; no argument since docs/design/node-framework.md slice 2, its recording being a resource; on the interim
// sink by docs/design/node-lifecycle-modules.md 4.5, step L-4b). Change it only through an erratum and a re-cut, never a
// local edit.
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "file": inputs "record.left", "record.right"; settings {"path": a path relative to the recordings folder}: non-empty,
/// not absolute, no ".." component (else invalid). Opened at configure as a recording (RecordingRequest), so beneath the
/// folder only and truncated; a path the folder refuses or cannot open fails the node start-failed, "cannot open
/// <path>: <why>". Recorded through an interim sink on its inside output bank "writer" (2 s ring at the graph
/// frequency): the WAV is opened on the sink's own thread, and a libsndfile refusal fails the node start-failed before
/// it is configured. 32-bit float stereo WAV at the graph frequency, every cycle the node's code drove (none while
/// stopped); complete once its configuration is torn down (a restart, a disable, a removal).
std::unique_ptr<const NodeType> makeFileNodeType();
} // namespace ear::nodes
