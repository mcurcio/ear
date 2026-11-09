#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "sendspin". `folders.config`: the graph file folder, for each node's key file. Outputs "from.left", "from.right";
/// inputs "to.left", "to.right". Settings, each optional: docs/node-types.md#sendspin (with "server" the
/// node dials it; without it, it listens on "port"). Narrows every key file in the folder, whatever its node.
std::unique_ptr<const NodeType> makeSendspinNodeType(const NodeFolders& folders);
} // namespace ear::nodes
