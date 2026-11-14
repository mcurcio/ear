#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "sendspin". Outputs "from.left", "from.right"; inputs "to.left", "to.right". Settings, each optional:
/// docs/node-types.md#sendspin (with "server" the node dials it; without it, it listens on "port"). Each node's key
/// file is its store "key" (sendspin-<node>.key in the graph file folder).
std::unique_ptr<const NodeType> makeSendspinNodeType();
} // namespace ear::nodes
