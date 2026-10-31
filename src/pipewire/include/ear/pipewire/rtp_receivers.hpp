// Frozen interface (docs/design/rtp.md section 7.2). Change it only through an erratum in that
// page's Errata section and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One receiver = PipeWire's libpipewire-module-rtp-source loaded into the engine's context
/// (pw_context_load_module) with the arguments docs/design/rtp.md section 5 lists, every one
/// built here from the request (the one home of that form): remote.name = internal (the module
/// finds no core in the context and would otherwise look for a daemon's socket), source.ip,
/// source.port, sess.media audio, sess.latency.msec, sess.ignore-ssrc = true, audio.format
/// S16BE or S24BE, audio.rate, audio.channels, audio.position AUX0.., node.name =
/// node.description = media.name = engineNode, and stream.props { node.always-process = true,
/// adapter.auto-port-config dsp }. Its node (Stream/Output/Audio, a pw_stream on the module's own
/// internal connection) has no node.group, never drives, and runs alone on the graph's driver
/// until joined. Its ports: "receive_AUX<i>".
/// Lifecycle: the one outside-node lifecycle (outside_nodes.hpp), with this kind's body: a
/// registry binding and a pw_core_sync first, so a node of the same name still going (a
/// restart's old receiver) is known as stale; then the module is loaded; the first node global
/// named engineNode that is not stale is this receiver's, bound for its info. RUNNING (the
/// module binds its socket when its stream starts, before the node runs) lets the channels be
/// linked; node ERROR, the node's global removed or the module destroyed fail it; a module that
/// will not load fails it (told later, never inside make()). A socket that cannot be bound
/// gives no event on PipeWire 1.4.2: the node never runs, so it is never linked or ready, and
/// the EAR node stays starting (docs/design/rtp.md section 4, finding g).
class RtpReceivers final : public RtpReceiverFactory {
public:
  explicit RtpReceivers(Engine& engine);   // engine outlives it
  std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
      const RtpReceiverRequest& request, OutsideNodeEvents& events) override;
private:
  Engine& engine_;
};
}
