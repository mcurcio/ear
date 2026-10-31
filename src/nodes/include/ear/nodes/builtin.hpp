// Frozen interface (docs/design/batch2.md section 4; "rtp" by docs/design/rtp.md section 7.3,
// batch2.md section 11's erratum). Change it only through an erratum and a re-cut, never a
// local edit.
#pragma once
#include <memory>
#include <vector>
#include "ear/core/node_type.hpp"
#include "ear/folder/confined_folder.hpp"
namespace ear::nodes {
/// "white": output "MONO", uniform noise in [-1, 1); settings {} (any key: invalid-settings).
std::unique_ptr<const NodeType> makeWhiteNodeType();
/// "file": inputs "FL", "FR"; settings {"path": a path relative to `recordings`}: non-empty,
/// not absolute, no ".." component (else invalid). Opened beneath the folder only
/// (folder::ConfinedFolder::create), truncated at start; 32-bit float stereo WAV at the graph
/// frequency; complete once the node is gone. A path the folder refuses or cannot open: start
/// fails with startFailed. Ring: 2 s at the graph frequency.
std::unique_ptr<const NodeType> makeFileNodeType(std::shared_ptr<const folder::ConfinedFolder> recordings);
/// "card": a sound card. Settings: {"card": a CardId (required), "playback": channels 0-64
/// (default 0), "capture": channels 0-64 (default 0)}; at least one above 0; unknown keys
/// invalid; a "card" outside CardId's grammar is invalid. Ports: inputs
/// "playback_1".."playback_N", outputs "capture_1".."capture_M"; inside outputs "to_card_1"..,
/// inside inputs "from_card_1"... Audio passes straight through (playback_i -> to_card_i,
/// from_card_j -> capture_j). Start asks the grant for one card per direction used. The rate is
/// the graph frequency (R5), never a setting.
std::unique_ptr<const NodeType> makeCardNodeType();
/// "rtp": one RTP stream received (docs/design/rtp.md section 3). Settings: {"address": an
/// RtpAddress ("0.0.0.0" or an IPv4 multicast group), "port": 1-65535, "format": "L16" or
/// "L24", "channels": 1-64, "rate": 8000-192000 Hz (default: the graph frequency, taken at
/// start), "latencyMs": 1-1000 (default 100, this parser being its one home)}; all but rate and
/// latencyMs required; unknown keys invalid. Ports: outputs "channel_1".."channel_N", inside
/// inputs "from_rtp_1".."from_rtp_N" (N = channels). Audio passes straight through
/// (from_rtp_i -> channel_i). Start asks the grant for one receiver with blockPorts = the inside
/// inputs. No packets: the node stays running and its outputs are silent.
std::unique_ptr<const NodeType> makeRtpNodeType();
/// white, file (writing into `recordings`), card and rtp.
std::vector<std::unique_ptr<const NodeType>> builtinNodeTypes(std::shared_ptr<const folder::ConfinedFolder> recordings);
}
