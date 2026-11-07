#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// One of every folder in src/nodes, made with these folders, in folder order; generated at configure.
/// NodeCatalogue::names() sorts, so this order reaches nothing.
std::vector<std::unique_ptr<const NodeType>> allNodeTypes(const NodeFolders& folders);
} // namespace ear::nodes
