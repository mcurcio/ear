// Frozen interface (docs/design/batch2.md section 4; returns the outside node of
// docs/design/rtp.md section 7.1, batch2.md section 11's erratum). Change it only through an
// erratum and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One card = pw_core_create_object("adapter") with factory.name api.alsa.pcm.sink (playback) or
/// api.alsa.pcm.source (capture), api.alsa.path = "hw:CARD=<id>,DEV=0" (built here from the
/// CardId: the one home of that form), audio.channels, audio.rate = format.rate, audio.position
/// AUX0.., node.name = engineNode, node.always-process = true, priority.driver = cardPriority,
/// no node.group. Its ports: a dsp PortConfig with AUX positions sent by pw_node_set_param, as a
/// session manager does (adapter.auto-port-config leaves one port, playback_1, whose start fails
/// with EINVAL on PipeWire 1.4.2).
/// Its state machine (one owner of "is this card in the clock"; the one outside-node lifecycle,
/// docs/design/rtp.md section 6):
///   opening --node info RUNNING--> linking: one link per channel through link-factory, the
///     card named by its global id and the block by its node name, each port by name
///     (link.output.node/port, link.input.node/port), owned by EAR's connection (link_proxy.hpp)
///   linking --every link ACTIVE--> ready (events.ready)
///   any state --node ERROR, proxy error, node removed, a link failed or gone--> destroy the
///     links, destroy the adapter proxy, then events.lost(detail)
/// channelPorts: "playback_AUX<i>" / "capture_AUX<i>".
class AlsaSoundCards final : public SoundCardFactory {
public:
  explicit AlsaSoundCards(Engine& engine);   // engine outlives it
  std::unique_ptr<OutsideNode> make(const NodeName& engineNode, const NodeName& blockNode,
      const SoundCardRequest& request, const AudioFormat& format,
      OutsideNodeEvents& events) override;
private:
  Engine& engine_;
};
}
