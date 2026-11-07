// The "card" node type: a sound card behind a port block (docs/design/batch2.md section 4.4, ear/nodes/card.hpp;
// docs/design/node-framework.md sections 7.1 and 11). Its spec and behaviour are card_spec.hpp's.
#include <memory>

#include "card_spec.hpp"
#include "ear/nodes/card.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeCardNodeType() { return makeNodeType<card::spec, card::Card>(); }

} // namespace ear::nodes
