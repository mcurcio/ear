// Frozen interface (docs/design/batch2.md section 4; split from nodes/builtin.hpp by section 11's erratum of
// 2026-11-06). Change it only through an erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "file": inputs "record.left", "record.right"; settings {"path": a path relative to `folders.recordings`}: non-empty,
/// not absolute, no ".." component (else invalid). Opened beneath the folder only
/// (folder::ConfinedFolder::create), truncated at start; 32-bit float stereo WAV at the graph
/// frequency; complete once the node is gone. A path the folder refuses or cannot open: start
/// fails with startFailed. Ring: 2 s at the graph frequency.
std::unique_ptr<const NodeType> makeFileNodeType(const NodeFolders& folders);
} // namespace ear::nodes
