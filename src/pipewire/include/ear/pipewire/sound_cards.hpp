#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One card = pw_core_create_object("adapter") with factory.name api.alsa.pcm.sink (playback) or api.alsa.pcm.source
/// (capture), api.alsa.path = "hw:CARD=<id>,DEV=0" (built here from the CardId: the one home of that form),
/// audio.channels, audio.rate = format.rate, audio.position AUX0.., node.name = engineNode, node.always-process = true,
/// priority.driver = cardPriority, no node.group, then the extra properties it was made with (a test seam, below). It
/// runs alone as its own driver until its PCM runs; once linked it follows ear's internal clock, which always drives
/// (docs/engine.md#the-internal-clock), and matches its rate to it. Its ports: a dsp PortConfig with AUX positions sent
/// by pw_node_set_param, as a session manager does (adapter.auto-port-config leaves one port, playback_1, whose start
/// fails with EINVAL on PipeWire 1.4.2). Its state machine (one owner of "is this card in the clock"; the one
/// outside-node lifecycle, docs/engine.md#outside-nodes):
///   opening --node info RUNNING--> linking: one link per channel through link-factory, the
///     card and the block each named by its global id, each port by name
///     (link.output.node/port, link.input.node/port), owned by EAR's connection (link_proxy.hpp)
///   linking --every link ACTIVE--> ready (events.ready)
///   any state --node ERROR, proxy error, node removed, a link failed or gone--> destroy the
///     links, destroy the adapter proxy, then events.lost(detail)
/// channelPorts: "playback_AUX<i>" / "capture_AUX<i>".
class AlsaSoundCards final : public SoundCardAdapter {
public:
	using Properties = std::vector<std::pair<std::string, std::string>>;
	/// `extra`: adapter properties added to every card's, after its own (a test seam: clocklessCardProperties).
	explicit AlsaSoundCards(Engine& engine, Properties extra = {}); // engine outlives it
	std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const SoundCardRequest& request,
	                                  OutsideNodeEvents& events) override;

private:
	Engine& engine_;
	Properties extra_;
};
/// The extra properties for a card with no clock of its own (CI's test card, a null PCM): its clock.name is the
/// internal clock's, so as a follower it turns its rate matching off, which would otherwise never settle against a
/// delay of 0 (docs/engine.md#the-internal-clock). A real card is never made with these: it keeps matching on.
AlsaSoundCards::Properties clocklessCardProperties();
} // namespace ear::pipewire
