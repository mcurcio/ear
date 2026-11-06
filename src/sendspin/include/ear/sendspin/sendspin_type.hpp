// Frozen interface (docs/design/sendspin.md section 14, S1). Change it only through an erratum and a re-cut, never a
// local edit.
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
#include "ear/folder/confined_folder.hpp"
namespace ear::sendspin {
/// "sendspin". `folder`: the graph file folder, for each node's key file. Outputs "from_server_FL", "from_server_FR";
/// inputs "to_server_FL", "to_server_FR". Settings: {"server": ws://<host>[:<port>]/<path> (required), "name": what
/// Music Assistant shows (default: the node's name)}.
std::unique_ptr<const NodeType> makeSendspinNodeType(std::shared_ptr<const folder::ConfinedFolder> folder);
}
