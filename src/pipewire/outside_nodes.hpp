// The one outside-node lifecycle (docs/design/rtp.md section 7.2): how every kind of outside
// node (a sound card, an RTP receiver) is joined to its port block, reported and torn down. A
// kind only says how its PipeWire node is made, how EAR hears from it and which ports it has.
// Private to ear_pipewire: its sources include it by this path; no public header does.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"

struct pw_node_info;

namespace ear::pipewire {

/// Which way an outside node's channels flow.
enum class ChannelFlow {
  blockToNode,   // a playback card: block inside output -> the node's input port
  nodeToBlock,   // a capture card, an RTP receiver: the node's output port -> block inside input
};

/// How one outside node is joined to its port block, channel for channel.
struct OutsideJoin {
  std::string kind;                   // "card", "RTP receiver": names it in loss details
  NodeName blockNode;                 // its port block's node.name
  ChannelFlow flow;
  std::vector<PortName> nodePorts;    // channel i's port on the outside node
  std::vector<PortName> blockPorts;   // channel i's inside port on the block; same size
};

/// What a kind's body tells the lifecycle, on the main loop.
class OutsideBodyEvents {
public:
  /// The engine node runs (its info state is RUNNING): its channels may be linked. `nodeId` is
  /// its global id, which its links name (never its node.name: link-factory also matches a
  /// name against other nodes' nick, description and object path, and a node of the same name
  /// that is still going would match first). Repeats are ignored.
  virtual void running(std::uint32_t nodeId) = 0;
  /// The engine node failed or went (refused, ERROR, removed, its module destroyed). May be told
  /// inside the body's constructor; the lifecycle reports the loss later, never inside make().
  virtual void failed(std::string detail) = 0;

protected:
  ~OutsideBodyEvents() = default;
};

/// "[ AUX0 AUX1 ... ]", the audio.position of an outside node of `channels` channels: its
/// channels carry no speaker positions. The one home of this form for every kind.
std::string auxPositions(std::size_t channels);

/// The one reading of an outside node's info, for every kind's body: a state change to ERROR
/// tells failed("the <kind> failed: <error>"), one to RUNNING tells running(nodeId) (failed if
/// the engine gave the node no id); anything else tells nothing.
void tellNodeState(const pw_node_info& info, const std::string& kind, std::uint32_t nodeId,
                   OutsideBodyEvents& events);

/// One kind's PipeWire object for one outside node. Destroying it (main thread) removes the
/// engine node and tells nothing more, in any state.
class OutsideBody {
public:
  virtual ~OutsideBody() = default;
};

/// Makes a kind's body, which reports to the events it is given (they outlive the body).
using OutsideBodyMaker = std::move_only_function<std::unique_ptr<OutsideBody>(OutsideBodyEvents&)>;

/// The one lifecycle, for every kind (batch2.md section 5's card state machine, generalised):
///   opening --body running(id)--> linking: one link per channel through link-factory, the
///     outside node named by its global id and the block by its node.name, each port by name
///     (link.output.node/port, link.input.node/port as `join.flow` says), owned by EAR's
///     connection (link_proxy.hpp)
///   linking --every link ACTIVE--> ready (events.ready)
///   any state --body failed, a link failed or gone--> failing: nothing more is heard; later, on
///     the main loop (never inside a proxy's own event, never inside make()), the links and the
///     body are destroyed, then a pw_core_sync round trip, and on its done events.lost(detail)
/// Destroying the returned node (main thread) in any state destroys links and body and tells
/// nothing.
/// Precondition: join.nodePorts.size() == join.blockPorts.size() > 0.
std::unique_ptr<OutsideNode> joinOutsideNode(Engine& engine, OutsideJoin join, OutsideNodeEvents& events,
                                             OutsideBodyMaker makeBody);

}  // namespace ear::pipewire
