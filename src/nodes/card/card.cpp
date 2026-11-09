// The "card" node type: a sound card behind a port block (docs/node-types.md#card, ear/nodes/card.hpp;
// docs/writing-a-node-type.md#the-spec). Its spec and behaviour are card_spec.hpp's.
#include <memory>

#include "card_spec.hpp"
#include "ear/nodes/card.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeCardNodeType() { return makeNodeType<card::spec, card::Card>(); }

} // namespace ear::nodes
