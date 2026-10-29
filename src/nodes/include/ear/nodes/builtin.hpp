// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
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
std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes();   // white, file
}
