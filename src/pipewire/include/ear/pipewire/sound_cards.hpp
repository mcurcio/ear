#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ear/core/companions.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One card = pw_core_create_object("adapter") with factory.name api.alsa.pcm.sink (playback: the bank is an output)
/// or api.alsa.pcm.source (capture: an input), api.alsa.path = "hw:CARD=<id>,DEV=0" (built here from the CardId: the
/// one home of that form), audio.channels, audio.rate = format.rate, audio.position AUX0.., node.name = engineNode,
/// node.always-process = true, priority.driver = cardPriority, api.alsa.period-size = format.maxBlock and
/// api.alsa.period-num = cardPeriods(format.maxBlock) (its buffer), no node.group, then the extra properties it was made
/// with (a test seam, below, which refuses clock properties). It runs alone as its own driver until its PCM runs; once
/// linked it follows ear's internal clock, which always drives (docs/engine.md#the-internal-clock), and matches its rate
/// to it. Its ports: a dsp
/// PortConfig with AUX positions sent by pw_node_set_param, as a session manager does (adapter.auto-port-config leaves
/// one port, playback_1, whose start fails with EINVAL on PipeWire 1.4.2). Its state machine (one owner of "is this
/// card in the clock"; the one engine-companion lifecycle, docs/engine.md#engine-companions):
///   opening --node info RUNNING--> linking: one link per channel through link-factory, the
///     card and the block each named by its global id, each port by name
///     (link.output.node/port, link.input.node/port), owned by EAR's connection (link_proxy.hpp)
///   linking --every link ACTIVE--> ready (events.ready)
///   any state --node ERROR, proxy error, node removed, a link failed or gone--> destroy the
///     links, destroy the adapter proxy, then events.lost(detail)
/// A card whose PCM never runs tells nothing here: NodeHost's readiness deadline (10 s) fails its node engine-refused
/// and destroys it (docs/engine.md#readiness-deadlines).
/// channelPorts: "playback_AUX<i>" / "capture_AUX<i>".
class AlsaSoundCards final : public SoundCardCompanions {
public:
	using Properties = std::vector<std::pair<std::string, std::string>>;
	/// `extra`: adapter properties added to every card's, after its own (a test seam: clocklessCardProperties).
	/// std::invalid_argument when it names node.force-quantum, node.force-rate, node.lock-quantum or node.lock-rate:
	/// no companion may change every block's clock (docs/engine.md#the-quantum-guard).
	explicit AlsaSoundCards(Engine& engine, Properties extra = {}); // engine outlives it
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const SoundCardShape& shape,
	                                      EngineCompanionEvents& events) override;

private:
	Engine& engine_;
	Properties extra_;
};
/// The api.alsa.period-num a card is made with, beside api.alsa.period-size = the block size, so its ALSA buffer is
/// what it was when the graph's quantum limit was max(block, 8192), not 4 blocks (docs/engine.md#the-quantum-guard):
/// alsa-pcm.c halves a batch (USB) card's period, so a batch card gets block / 2 x this = 4 x max(block, 8192) frames,
/// a card that is not batch twice that; both capped at the hardware's maximum (1024 periods at most, 2 at least).
std::uint32_t cardPeriods(std::uint32_t blockSize) noexcept;
/// The extra properties for a card with no clock of its own (CI's test card, a null PCM): its clock.name is the
/// internal clock's, so as a follower it turns its rate matching off, which would otherwise never settle against a
/// delay of 0 (docs/engine.md#the-internal-clock). A real card is never made with these: it keeps matching on.
AlsaSoundCards::Properties clocklessCardProperties();
} // namespace ear::pipewire
