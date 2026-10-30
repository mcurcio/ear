// The node types EAR ships with; main builds its catalogue from this list.
#include "ear/nodes/builtin.hpp"

namespace ear::nodes {

std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes() {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(makeWhiteNodeType());
	types.push_back(makeFileNodeType());
	types.push_back(makeCardNodeType());
	return types;
}

}  // namespace ear::nodes
