// The "gain" node type: a gain per channel, from in.N to out.N, every adjustment live but the channel count
// (docs/node-types.md#gain, ear/nodes/gain.hpp; written against its spec, gain_spec.hpp,
// docs/writing-a-node-type.md#the-spec).
#include <memory>

#include "ear/nodes/gain.hpp"
#include "gain_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeGainNodeType() { return makeNodeType<gain::spec, gain::Gain>(); }

} // namespace ear::nodes
