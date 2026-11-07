// Frozen interface (docs/design/batch2.md section 4; split from nodes/builtin.hpp by section 11's erratum of
// 2026-11-06). Change it only through an erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "white": output "noise.mono", uniform noise in [-1, 1); settings {} (any key: invalid-settings).
std::unique_ptr<const NodeType> makeWhiteNodeType(const NodeFolders& folders);
} // namespace ear::nodes
