// Sound cards as ALSA adapter nodes with their own links (docs/engine.md, sound_cards.hpp). A card is one kind of
// engine companion: its body here makes the ALSA adapter and hears from it through its own proxy; the one
// engine-companion lifecycle (engine_companions.hpp, docs/engine.md#engine-companions) runs it alone until its PCM
// runs, then links its channels to its port block, and on any failure destroys its links and its engine node before it
// reports the loss. Linked, it follows ear's internal clock, never drives the blocks.
#include "ear/pipewire/sound_cards.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "engine_names.hpp"
#include "engine_companions.hpp"

namespace ear::pipewire {
namespace {

constexpr char kind[] = "card"; // names it in failure details

/// The ALSA device a card id names: device 0 of the card. The one home of this form; the card
/// id's grammar (CardId) keeps every other ALSA device-name form out of reach.
std::string alsaPath(const CardId& card) { return "hw:CARD=" + card.text() + ",DEV=0"; }

/// The adapter's port for channel `channel` (from 0), as its dsp port configuration names it.
std::string channelPort(Direction bank, std::size_t channel) {
	return (bank == Direction::output ? "playback_AUX" : "capture_AUX") + std::to_string(channel);
}

/// One card's engine node: the ALSA adapter, made on EAR's connection and heard from through its
/// own proxy. The lifecycle (joinEngineCompanion) links it once it runs and tears it down.
class AlsaCardBody final : public CompanionBody {
public:
	AlsaCardBody(Engine& engine, const CompanionTarget& target, const SoundCardShape& shape,
	             const AlsaSoundCards::Properties& extra, CompanionBodyEvents& events)
		: bank_(target.bank), channels_(target.blockPorts.size()), rate_(target.format.rate), events_(events) {
		const NodeName& engineNode = target.engineNode;
		const AudioFormat& format = target.format;
		const auto channels = std::to_string(channels_);
		const auto rate = std::to_string(format.rate);
		const auto priority = std::to_string(cardPriority);
		const auto periodSize = std::to_string(format.maxBlock);
		const auto periods = std::to_string(cardPeriods(format.maxBlock));
		// No node.group: a card in the group would follow from creation and never start before it is linked, which
		// the lifecycle (link once RUNNING) cannot do. Alone it drives itself until its links join it to the blocks,
		// where the internal clock outranks it and it follows (docs/engine.md#the-internal-clock).
		pw_properties* props = pw_properties_new(
			SPA_KEY_FACTORY_NAME, bank_ == Direction::output ? "api.alsa.pcm.sink" : "api.alsa.pcm.source",
			"api.alsa.path", alsaPath(shape.card).c_str(), "audio.channels", channels.c_str(), "audio.rate",
			rate.c_str(), "audio.position", auxPositions(channels_).c_str(), PW_KEY_NODE_NAME, engineNode.c_str(),
			PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_PRIORITY_DRIVER, priority.c_str(),
			// The ALSA buffer from a period size and count, never from clock.quantum-limit, which module-adapter
			// sets to the graph's (the block size, docs/engine.md#the-quantum-guard): with no period count alsa-pcm.c
			// caps the buffer at 4 x that limit, 4 blocks (cardPeriods).
			"api.alsa.period-size", periodSize.c_str(), "api.alsa.period-num", periods.c_str(), nullptr);
		for (const auto& [key, value] : extra) pw_properties_set(props, key.c_str(), value.c_str());
		node_ = static_cast<pw_proxy*>(
			pw_core_create_object(engine.core(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
		pw_properties_free(props);
		if (node_ == nullptr) {
			events_.failed(std::string("cannot ask for the card: ") + spa_strerror(-errno));
			return;
		}
		pw_proxy_add_listener(node_, &proxyHook_, &proxyEvents(), this);
		pw_proxy_add_object_listener(node_, &nodeHook_, &nodeEvents(), this);
		configurePorts();
	}

	~AlsaCardBody() override {
		if (node_ == nullptr) return;
		spa_hook_remove(&proxyHook_);
		spa_hook_remove(&nodeHook_);
		pw_proxy_destroy(node_); // no linger: the engine node goes with the proxy
	}

	AlsaCardBody(const AlsaCardBody&) = delete;
	AlsaCardBody& operator=(const AlsaCardBody&) = delete;

private:
	static const pw_proxy_events& proxyEvents() {
		static const pw_proxy_events table = [] {
			pw_proxy_events events{};
			events.version = PW_VERSION_PROXY_EVENTS;
			events.removed = &AlsaCardBody::onRemoved;
			events.error = &AlsaCardBody::onError;
			return events;
		}();
		return table;
	}
	static const pw_node_events& nodeEvents() {
		static const pw_node_events table = [] {
			pw_node_events events{};
			events.version = PW_VERSION_NODE_EVENTS;
			events.info = &AlsaCardBody::onInfo;
			return events;
		}();
		return table;
	}

	/// One 32-bit float mono port per channel, named by its AUX position ("playback_AUX0"..), set
	/// on the node as a session manager would. PipeWire 1.4.2's adapter.auto-port-config left the
	/// adapter with one unconfigured port whose PCM failed to start (E2's spike), so the port
	/// configuration is sent here instead. A refusal arrives as the proxy's error.
	void configurePorts() {
		std::uint8_t buffer[1024];
		spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		spa_audio_info_raw raw{};
		raw.format = SPA_AUDIO_FORMAT_F32P;
		raw.rate = rate_;
		raw.channels = static_cast<std::uint32_t>(channels_);
		for (std::uint32_t i = 0; i < raw.channels && i < SPA_AUDIO_MAX_CHANNELS; ++i) {
			raw.position[i] = SPA_AUDIO_CHANNEL_START_Aux + i;
		}
		spa_pod* format = spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &raw);
		auto* config = static_cast<spa_pod*>(spa_pod_builder_add_object(
			&builder, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig, SPA_PARAM_PORT_CONFIG_direction,
			SPA_POD_Id(bank_ == Direction::output ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT),
			SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp), SPA_PARAM_PORT_CONFIG_monitor,
			SPA_POD_Bool(false), SPA_PARAM_PORT_CONFIG_control, SPA_POD_Bool(false), SPA_PARAM_PORT_CONFIG_format,
			SPA_POD_Pod(format)));
		pw_node_set_param(reinterpret_cast<pw_node*>(node_), SPA_PARAM_PortConfig, 0, config);
	}

	// RUNNING: the PCM runs, so the card may be linked, by its global id; ERROR: failed.
	static void onInfo(void* data, const pw_node_info* info) {
		auto* self = static_cast<AlsaCardBody*>(data);
		tellNodeState(*info, kind, pw_proxy_get_bound_id(self->node_), self->events_);
	}
	static void onError(void* data, int /*seq*/, int res, const char* message) {
		static_cast<AlsaCardBody*>(data)->events_.failed(std::string("the card failed: ") +
		                                                 (message != nullptr ? message : spa_strerror(res)));
	}
	static void onRemoved(void* data) {
		static_cast<AlsaCardBody*>(data)->events_.failed("the engine removed the card");
	}

	Direction bank_;
	std::size_t channels_;
	std::uint32_t rate_;
	CompanionBodyEvents& events_;
	pw_proxy* node_ = nullptr;
	spa_hook proxyHook_{};
	spa_hook nodeHook_{};
};

} // namespace

namespace {
/// The properties that would let one card change every block's quantum or rate, or take driving from the internal clock
/// (docs/engine.md#the-quantum-guard, docs/engine.md#the-internal-clock): ear sets none of the first four, sets
/// priority.driver itself and leaves the rest unset, and its test seam may set none of them.
AlsaSoundCards::Properties refuseClockProperties(AlsaSoundCards::Properties extra) {
	constexpr std::array<std::string_view, 8> refused{"node.force-quantum", "node.force-rate", "node.lock-quantum",
	                                                  "node.lock-rate",     "priority.driver", "node.driver",
	                                                  "node.group",         "node.link-group"};
	for (const auto& [key, value] : extra)
		if (std::ranges::find(refused, key) != refused.end())
			throw std::invalid_argument("a sound card may not carry " + key +
			                            ": it would change every block's clock or what drives it");
	return extra;
}
} // namespace

AlsaSoundCards::AlsaSoundCards(Engine& engine, Properties extra)
	: engine_(engine), extra_(refuseClockProperties(std::move(extra))) {}

std::uint32_t cardPeriods(std::uint32_t blockSize) noexcept {
	const std::uint64_t buffer = 4ULL * std::max(blockSize, 8192U); // the buffer the old limit gave
	const std::uint64_t halfBlock = std::max(blockSize / 2, 1U);    // a batch card's period (alsa-pcm.c)
	return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(buffer / halfBlock, 2, 1024));
}

AlsaSoundCards::Properties clocklessCardProperties() { return {{"clock.name", internalClockName}}; }

std::unique_ptr<EngineCompanion> AlsaSoundCards::make(const CompanionTarget& target, const SoundCardShape& shape,
                                                      EngineCompanionEvents& events) {
	std::vector<PortName> cardPorts;
	for (std::size_t channel = 0; channel < target.blockPorts.size(); ++channel) {
		cardPorts.push_back(channelPort(target.bank, channel));
	}
	return joinEngineCompanion(
		engine_,
		CompanionJoin{.kind = kind,
	                  .blockId = target.blockId,
	                  .flow = target.bank == Direction::output ? ChannelFlow::blockToNode : ChannelFlow::nodeToBlock,
	                  .nodePorts = std::move(cardPorts),
	                  .blockPorts = target.blockPorts},
		events, [&](CompanionBodyEvents& body) -> std::unique_ptr<CompanionBody> {
			return std::make_unique<AlsaCardBody>(engine_, target, shape, extra_, body);
		});
}

} // namespace ear::pipewire
