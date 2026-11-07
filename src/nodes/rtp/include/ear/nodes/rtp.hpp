// Frozen interface (docs/design/batch2.md section 4; "rtp" by docs/design/rtp.md section 7.3; split from
// nodes/builtin.hpp by section 11's erratum of 2026-11-06; its settings' prose moved to their rows by its erratum
// "NodeType, 2026-11-06"). Change it only through an erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_folders.hpp"
#include "ear/core/node_type.hpp"
namespace ear::nodes {
/// "rtp": one RTP stream received (docs/design/rtp.md section 3). Its settings are the rows in rtp.cpp, which hold their
/// ranges and defaults. Ports: outputs "stream.1".."stream.N", the inside input bank "from_rtp"
/// ("from_rtp.1".."from_rtp.N", N = channels). The framework passes audio straight through
/// (from_rtp -> stream). Start acquires one receiver joined to the inside bank.
/// No packets: the node stays running and its outputs are silent.
std::unique_ptr<const NodeType> makeRtpNodeType(const NodeFolders& folders);
} // namespace ear::nodes
