// RTP receivers through PipeWire's rtp-source module (docs/design/rtp.md sections 5 and 7.2,
// rtp_receivers.hpp). Interface stage: slice R1 builds the receiver's body on the one
// outside-node lifecycle (outside_nodes.hpp) and replaces this stub. No node type asks for a
// receiver until then (builtinNodeTypes has no "rtp" yet).
#include "ear/pipewire/rtp_receivers.hpp"

#include <stdexcept>

#include "outside_nodes.hpp"

namespace ear::pipewire {

RtpReceivers::RtpReceivers(Engine& engine) : engine_(engine) {}

std::unique_ptr<OutsideNode> RtpReceivers::make(const NodeName& engineNode, const NodeName&,
                                                const RtpReceiverRequest&, OutsideNodeEvents&) {
  throw std::logic_error("RTP receiver " + engineNode + ": arrives with docs/design/rtp.md slice R1");
}

}  // namespace ear::pipewire
