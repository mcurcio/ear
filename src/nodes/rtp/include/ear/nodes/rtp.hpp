#pragma once
#include <memory>
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "rtp": one RTP stream received (docs/node-types.md#rtp). Its settings are the rows in rtp.cpp, which hold their
/// ranges and defaults. Ports: outputs "stream.1".."stream.N", the inside input bank "from_rtp"
/// ("from_rtp.1".."from_rtp.N", N = channels). The framework passes audio straight through
/// (from_rtp -> stream). Start acquires one receiver joined to the inside bank.
/// No packets: the node stays running and its outputs are silent.
std::unique_ptr<const NodeType> makeRtpNodeType();
} // namespace ear::nodes
