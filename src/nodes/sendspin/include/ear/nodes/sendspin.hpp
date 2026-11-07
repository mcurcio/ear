// Frozen interface (docs/design/sendspin.md section 14, S1; moved and renamed by section 16's erratum of 2026-11-06).
// Change it only through an erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "sendspin". `folders.config`: the graph file folder, for each node's key file. Outputs "from.left", "from.right";
/// inputs "to.left", "to.right". Settings, each optional: docs/design/sendspin.md section 4 (with "server" the
/// node dials it; without it, it listens on "port"). Narrows every key file in the folder, whatever its node (section 8).
std::unique_ptr<const NodeType> makeSendspinNodeType(const NodeFolders& folders);
} // namespace ear::nodes
