// The "white" node type: one output, "noise.mono", carrying uniform white noise in [-1, 1). It takes
// no settings (docs/design/m1-white-to-file.md section 2, ear/nodes/white.hpp; written against its spec,
// white_spec.hpp, docs/design/node-framework.md section 7.1).
#include <memory>

#include "ear/nodes/white.hpp"
#include "white_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeWhiteNodeType() { return makeNodeType<white::spec, white::White>(); }

} // namespace ear::nodes
