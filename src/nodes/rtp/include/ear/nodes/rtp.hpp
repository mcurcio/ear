// Frozen interface (docs/design/batch2.md section 4; "rtp" by docs/design/rtp.md section 7.3; split from
// nodes/builtin.hpp by section 11's erratum of 2026-11-06; its settings' prose moved to their rows by its erratum
// "NodeType, 2026-11-06"). Change it only through an erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "rtp": one RTP stream received (docs/design/rtp.md section 3). Its settings are the rows in rtp.cpp, which hold their
/// ranges and defaults. Ports: outputs "stream.1".."stream.N", inside
/// inputs "from_rtp_1".."from_rtp_N" (N = channels). Audio passes straight through
/// (from_rtp_i -> stream.i). Start asks the grant for one receiver with blockPorts = the inside
/// inputs. No packets: the node stays running and its outputs are silent.
std::unique_ptr<const NodeType> makeRtpNodeType();
} // namespace ear::nodes
