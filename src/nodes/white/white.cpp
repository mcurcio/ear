// The "white" node type: one output, "noise.mono", carrying uniform white noise in [-1, 1). It takes
// no settings (docs/node-types.md#white, ear/nodes/white.hpp; written against its spec,
// white_spec.hpp, docs/writing-a-node-type.md#the-spec).
#include <memory>

#include "ear/nodes/white.hpp"
#include "white_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeWhiteNodeType() { return makeNodeType<white::spec, white::White>(); }

} // namespace ear::nodes
