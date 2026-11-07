// The "rtp" node type: one RTP stream received behind a port block (docs/design/rtp.md sections 3 and 7.3,
// ear/nodes/rtp.hpp; docs/design/node-framework.md sections 7.1 and 11). Its spec and behaviour are rtp_spec.hpp's.
#include <memory>

#include "ear/nodes/rtp.hpp"
#include "rtp_spec.hpp"

namespace ear::nodes {

std::unique_ptr<const NodeType> makeRtpNodeType() { return makeNodeType<rtp::spec, rtp::Rtp>(); }

} // namespace ear::nodes
