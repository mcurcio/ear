// The "card" node type: a sound card behind a port block (docs/design/batch2.md section 4.4,
// builtin.hpp). Slice s0 stub: slice E2 builds the type and adds it to builtinNodeTypes().
#include "ear/nodes/builtin.hpp"

#include <stdexcept>

namespace ear::nodes {

std::unique_ptr<const NodeType> makeCardNodeType() {
	throw std::logic_error("the card node type is not built yet (slice E2)");
}

}  // namespace ear::nodes
