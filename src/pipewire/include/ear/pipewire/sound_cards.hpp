// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One card = pw_core_create_object("adapter") with factory.name api.alsa.pcm.sink (playback) or
/// api.alsa.pcm.source (capture), api.alsa.path = "hw:CARD=<id>,DEV=0" (built here from the
/// CardId: the one home of that form), audio.channels, audio.rate = format.rate, audio.position
/// AUX0.., node.name = engineNode, node.always-process = true, priority.driver = cardPriority,
/// no node.group, adapter.auto-port-config = { mode = dsp monitor = false control = false
/// position = aux }.
/// Its state machine (one owner of "is this card in the clock"):
///   opening --node info RUNNING--> linking: one link per channel through link-factory, by
///     node and port name (link.output.node/port, link.input.node/port), owned by EAR's
///     connection (link_proxy.hpp)
///   linking --every link ACTIVE--> ready (events.ready)
///   any state --node ERROR, proxy error, node removed, a link failed or gone--> destroy the
///     links, destroy the adapter proxy, then events.lost(detail)
/// channelPorts: "playback_AUX<i>" / "capture_AUX<i>".
class AlsaSoundCards final : public SoundCardFactory {
public:
  explicit AlsaSoundCards(Engine& engine);   // engine outlives it
  std::unique_ptr<SoundCard> make(const NodeName& engineNode, const NodeName& blockNode,
      const SoundCardRequest& request, const AudioFormat& format,
      SoundCardEvents& events) override;
private:
  Engine& engine_;
};
}
