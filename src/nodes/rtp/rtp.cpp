// The "rtp" node type: one RTP stream received behind a port block (docs/node-types.md#rtp,
// ear/nodes/rtp.hpp; docs/writing-a-node-type.md#the-spec). Its spec and behaviour are rtp_spec.hpp's.
#include <memory>

#include "ear/nodes/rtp.hpp"
#include "rtp_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeRtpNodeType() { return makeNodeType<rtp::spec, rtp::Rtp>(); }

} // namespace ear::nodes
