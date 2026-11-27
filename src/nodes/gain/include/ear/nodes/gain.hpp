#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "gain": inputs in.1..in.N, outputs out.N; settings {channels?, gainDb?, mute?, trimDb.N?, invert.N?}, every one live
/// but channels, a reshape in place (docs/node-types.md#gain).
std::unique_ptr<const NodeType> makeGainNodeType();
} // namespace ear::nodes
