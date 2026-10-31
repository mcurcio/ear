// Sound cards as ALSA adapter nodes with their own links (docs/design/batch2.md sections 4.3
// and 5, sound_cards.hpp). One card is one state machine, the one owner of "is this card in
// the graph's clock": it runs alone until its PCM runs, then links its channels to its port
// block, and on any failure destroys its links and its engine node before it reports the loss.
#include "ear/pipewire/sound_cards.hpp"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "engine_names.hpp"
#include "link_proxy.hpp"

namespace ear::pipewire {
namespace {

/// The ALSA device a card id names: device 0 of the card. The one home of this form; the card
/// id's grammar (CardId) keeps every other ALSA device-name form out of reach.
std::string alsaPath(const CardId& card) { return "hw:CARD=" + card.text() + ",DEV=0"; }

/// "[ AUX0 AUX1 ... ]": the card's channels carry no speaker positions.
std::string auxPositions(std::size_t channels) {
  std::string positions = "[";
  for (std::size_t i = 0; i < channels; ++i) positions += " AUX" + std::to_string(i);
  return positions + " ]";
}

/// The adapter's port for channel `channel` (from 0), as its dsp port configuration names it.
std::string channelPort(CardDirection direction, std::size_t channel) {
  return (direction == CardDirection::playback ? "playback_AUX" : "capture_AUX") +
         std::to_string(channel);
}

class AlsaSoundCard final : public OutsideNode, public LinkEvents {
public:
  AlsaSoundCard(Engine& engine, const NodeName& engineNode, const NodeName& blockNode,
                const SoundCardRequest& request, const AudioFormat& format,
                OutsideNodeEvents& events)
      : engine_(engine), engineNode_(engineNode), blockNode_(blockNode),
        direction_(request.direction), blockPorts_(request.blockPorts), rate_(format.rate),
        events_(events) {
    later_ = pw_loop_add_event(engine_.mainLoop(), &AlsaSoundCard::onLater, this);
    pw_core_add_listener(engine_.core(), &coreHook_, &coreEvents(), this);
    coreListening_ = true;

    const auto channels = std::to_string(blockPorts_.size());
    const auto rate = std::to_string(format.rate);
    const auto priority = std::to_string(cardPriority);
    // No node.group: the card collects no port block until its own links join it to one.
    pw_properties* props = pw_properties_new(
        SPA_KEY_FACTORY_NAME,
        direction_ == CardDirection::playback ? "api.alsa.pcm.sink" : "api.alsa.pcm.source",
        "api.alsa.path", alsaPath(request.card).c_str(),
        "audio.channels", channels.c_str(),
        "audio.rate", rate.c_str(),
        "audio.position", auxPositions(blockPorts_.size()).c_str(),
        PW_KEY_NODE_NAME, engineNode_.c_str(),
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_PRIORITY_DRIVER, priority.c_str(),
        nullptr);
    node_ = static_cast<pw_proxy*>(pw_core_create_object(
        engine_.core(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
    pw_properties_free(props);
    if (node_ == nullptr) {
      fail(std::string("cannot ask for the card: ") + spa_strerror(-errno));
      return;
    }
    pw_proxy_add_listener(node_, &proxyHook_, &proxyEvents(), this);
    pw_proxy_add_object_listener(node_, &nodeHook_, &nodeEvents(), this);
    nodeListening_ = true;
    configurePorts();
  }

  ~AlsaSoundCard() override {
    silence();
    links_.clear();
    if (node_ != nullptr) pw_proxy_destroy(node_);
    if (later_ != nullptr) pw_loop_destroy_source(engine_.mainLoop(), later_);
  }

  AlsaSoundCard(const AlsaSoundCard&) = delete;
  AlsaSoundCard& operator=(const AlsaSoundCard&) = delete;

private:
  enum class State { opening, linking, ready, failing, lost };

  static const pw_core_events& coreEvents() {
    static const pw_core_events table = [] {
      pw_core_events events{};
      events.version = PW_VERSION_CORE_EVENTS;
      events.done = &AlsaSoundCard::onCoreDone;
      return events;
    }();
    return table;
  }
  static const pw_proxy_events& proxyEvents() {
    static const pw_proxy_events table = [] {
      pw_proxy_events events{};
      events.version = PW_VERSION_PROXY_EVENTS;
      events.removed = &AlsaSoundCard::onRemoved;
      events.error = &AlsaSoundCard::onError;
      return events;
    }();
    return table;
  }
  static const pw_node_events& nodeEvents() {
    static const pw_node_events table = [] {
      pw_node_events events{};
      events.version = PW_VERSION_NODE_EVENTS;
      events.info = &AlsaSoundCard::onInfo;
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
    raw.channels = static_cast<std::uint32_t>(blockPorts_.size());
    for (std::uint32_t i = 0; i < raw.channels && i < SPA_AUDIO_MAX_CHANNELS; ++i) {
      raw.position[i] = SPA_AUDIO_CHANNEL_START_Aux + i;
    }
    spa_pod* format = spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &raw);
    auto* config = static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
        SPA_PARAM_PORT_CONFIG_direction,
        SPA_POD_Id(direction_ == CardDirection::playback ? SPA_DIRECTION_INPUT
                                                         : SPA_DIRECTION_OUTPUT),
        SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
        SPA_PARAM_PORT_CONFIG_monitor, SPA_POD_Bool(false),
        SPA_PARAM_PORT_CONFIG_control, SPA_POD_Bool(false),
        SPA_PARAM_PORT_CONFIG_format, SPA_POD_Pod(format)));
    pw_node_set_param(reinterpret_cast<pw_node*>(node_), SPA_PARAM_PortConfig, 0, config);
  }

  // opening --RUNNING--> linking; any state --ERROR--> lost.
  static void onInfo(void* data, const pw_node_info* info) {
    auto* self = static_cast<AlsaSoundCard*>(data);
    if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) == 0) return;
    if (info->state == PW_NODE_STATE_ERROR) {
      self->fail(std::string("the card failed: ") +
                 (info->error != nullptr ? info->error : "engine error"));
    } else if (info->state == PW_NODE_STATE_RUNNING && self->state_ == State::opening) {
      self->link();
    }
  }
  static void onError(void* data, int /*seq*/, int res, const char* message) {
    static_cast<AlsaSoundCard*>(data)->fail(std::string("the card failed: ") +
                                            (message != nullptr ? message : spa_strerror(res)));
  }
  static void onRemoved(void* data) {
    static_cast<AlsaSoundCard*>(data)->fail("the engine removed the card");
  }

  /// One link per channel, by node and port name, owned by EAR's connection.
  void link() {
    state_ = State::linking;
    const bool playback = direction_ == CardDirection::playback;
    for (std::size_t channel = 0; channel < blockPorts_.size(); ++channel) {
      const std::string cardPort = channelPort(direction_, channel);
      pw_properties* props = pw_properties_new(
          PW_KEY_LINK_OUTPUT_NODE, playback ? blockNode_.c_str() : engineNode_.c_str(),
          PW_KEY_LINK_OUTPUT_PORT, playback ? blockPorts_[channel].c_str() : cardPort.c_str(),
          PW_KEY_LINK_INPUT_NODE, playback ? engineNode_.c_str() : blockNode_.c_str(),
          PW_KEY_LINK_INPUT_PORT, playback ? cardPort.c_str() : blockPorts_[channel].c_str(),
          nullptr);
      auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
          engine_.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict,
          0));
      pw_properties_free(props);
      if (proxy == nullptr) {
        fail(std::string("cannot ask for the card's link: ") + spa_strerror(-errno));
        return;
      }
      links_.push_back(std::make_unique<LinkProxy>(channel, proxy, *this));
    }
  }

  // linking --every link ACTIVE--> ready; any link failed or gone --> lost.
  void active(LinkId /*link*/) override {
    if (state_ != State::linking || ++activeLinks_ < links_.size()) return;
    state_ = State::ready;
    events_.ready();
  }
  void failed(LinkId link, std::string detail) override {
    fail("the card's link for channel " + std::to_string(link + 1) + " failed: " + detail);
  }
  void gone(LinkId link) override {
    fail("the card's link for channel " + std::to_string(link + 1) + " is gone");
  }

  /// The loss path, from any state: nothing more is heard from the card, and the teardown runs
  /// later on the main loop (never inside a proxy's own event, never inside make()).
  void fail(std::string detail) {
    if (state_ == State::failing || state_ == State::lost) return;
    state_ = State::failing;
    detail_ = std::move(detail);
    silenceCard();
    if (later_ != nullptr) pw_loop_signal_event(engine_.mainLoop(), later_);
  }

  /// Destroys the links and the adapter (no linger: its engine node goes with the proxy), then
  /// syncs with the engine, so the removal is done before the loss is told.
  static void onLater(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<AlsaSoundCard*>(data);
    if (self->state_ != State::failing || self->syncing_) return;
    self->syncing_ = true;
    self->links_.clear();
    if (self->node_ != nullptr) {
      pw_proxy_destroy(self->node_);
      self->node_ = nullptr;
    }
    self->syncSeq_ = pw_core_sync(self->engine_.core(), PW_ID_CORE, 0);
    if (self->syncSeq_ < 0) self->tellLost();   // nothing to wait for
  }

  static void onCoreDone(void* data, std::uint32_t id, int seq) {
    auto* self = static_cast<AlsaSoundCard*>(data);
    if (id != PW_ID_CORE || !self->syncing_ || self->state_ != State::failing ||
        seq != self->syncSeq_) {
      return;
    }
    self->tellLost();
  }

  void tellLost() {
    state_ = State::lost;
    silence();
    events_.lost(detail_);
  }

  void silenceCard() {
    if (nodeListening_) {
      nodeListening_ = false;
      spa_hook_remove(&proxyHook_);
      spa_hook_remove(&nodeHook_);
    }
    for (const auto& link : links_) link->silence();
  }
  void silence() {
    silenceCard();
    if (coreListening_) {
      coreListening_ = false;
      spa_hook_remove(&coreHook_);
    }
  }

  Engine& engine_;
  NodeName engineNode_;
  NodeName blockNode_;
  CardDirection direction_;
  std::vector<PortName> blockPorts_;
  std::uint32_t rate_;
  OutsideNodeEvents& events_;
  State state_ = State::opening;
  std::string detail_;
  pw_proxy* node_ = nullptr;
  spa_hook proxyHook_{};
  spa_hook nodeHook_{};
  bool nodeListening_ = false;
  spa_hook coreHook_{};
  bool coreListening_ = false;
  std::vector<std::unique_ptr<LinkProxy>> links_;   // link id = channel
  std::size_t activeLinks_ = 0;
  spa_source* later_ = nullptr;
  bool syncing_ = false;
  int syncSeq_ = 0;
};

}  // namespace

AlsaSoundCards::AlsaSoundCards(Engine& engine) : engine_(engine) {}

std::unique_ptr<OutsideNode> AlsaSoundCards::make(const NodeName& engineNode,
                                                const NodeName& blockNode,
                                                const SoundCardRequest& request,
                                                const AudioFormat& format,
                                                OutsideNodeEvents& events) {
  return std::make_unique<AlsaSoundCard>(engine_, engineNode, blockNode, request, format,
                                         events);
}

}  // namespace ear::pipewire
