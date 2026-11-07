#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// One of every folder in src/nodes, in folder order (only sendspin reads `folders`, until slice 3 of
/// docs/design/node-framework.md gives it stores); generated at configure, into
/// out/src/all_node_types/all_node_types.cpp (no source file calls make<Type>NodeType by name).
/// NodeCatalogue::names() sorts, so this order reaches nothing.
std::vector<std::unique_ptr<const NodeType>> allNodeTypes(const NodeFolders& folders);
} // namespace ear::nodes
