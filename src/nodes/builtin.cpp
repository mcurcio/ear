// The node types EAR ships with; main builds its catalogue from this list.
#include "ear/nodes/builtin.hpp"

#include <utility>

namespace ear::nodes {

std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes(std::shared_ptr<const folder::ConfinedFolder> recordings) {
	std::vector<std::unique_ptr<const NodeType>> types;
	types.push_back(makeWhiteNodeType());
	types.push_back(makeFileNodeType(std::move(recordings)));
	types.push_back(makeCardNodeType());
	types.push_back(makeRtpNodeType());
	return types;
}

}  // namespace ear::nodes
