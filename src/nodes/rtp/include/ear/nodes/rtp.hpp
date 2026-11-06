// Frozen interface (docs/design/batch2.md section 4; "rtp" by docs/design/rtp.md section 7.3; split from
// nodes/builtin.hpp by section 11's erratum of 2026-11-06). Change it only through an erratum and a re-cut, never a
// local edit.
#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "rtp": one RTP stream received (docs/design/rtp.md section 3). Settings: {"address": an
/// RtpAddress ("0.0.0.0" or an IPv4 multicast group), "port": 1-65535, "format": "L16" or
/// "L24", "channels": 1-64, "rate": 8000-192000 Hz (default: the graph frequency, taken at
/// start), "latencyMs": 1-1000 (default 100, this parser being its one home)}; all but rate and
/// latencyMs required; unknown keys invalid. Ports: outputs "stream.1".."stream.N", inside
/// inputs "from_rtp_1".."from_rtp_N" (N = channels). Audio passes straight through
/// (from_rtp_i -> stream.i). Start asks the grant for one receiver with blockPorts = the inside
/// inputs. No packets: the node stays running and its outputs are silent.
std::unique_ptr<const NodeType> makeRtpNodeType();
} // namespace ear::nodes
