// RTP receivers through PipeWire's rtp-source module (docs/design/rtp.md sections 4, 5 and 7.2,
// rtp_receivers.hpp). A receiver is one kind of outside node: its body here loads the module
// into the engine's context and finds the module's node through the registry; the one
// outside-node lifecycle (outside_nodes.hpp) links it once it runs and tears it down.
#include "ear/pipewire/rtp_receivers.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "outside_nodes.hpp"

namespace ear::pipewire {
namespace {

constexpr char moduleName[] = "libpipewire-module-rtp-source";

/// The receiver's port for channel `channel` (from 0): a node.network stream's output ports are
/// "receive_<position>" (impl-port.c), and its positions are AUX0...
std::string channelPort(std::size_t channel) { return "receive_AUX" + std::to_string(channel); }

/// The module's arguments (docs/design/rtp.md section 5): the one home of this form. Every value
/// is a number, a closed enum, an RtpAddress or the engine node's name ([A-Za-z0-9_.-]), so no
/// user text reaches PipeWire's parsers.
std::string moduleArguments(const NodeName& engineNode, const RtpReceiverRequest& request) {
  std::string positions = "[";
  for (std::size_t i = 0; i < request.blockPorts.size(); ++i) positions += " AUX" + std::to_string(i);
  positions += " ]";
  const std::string name = "\"" + engineNode + "\"";
  return "{ remote.name = \"internal\"" +
         std::string(" source.ip = \"") + request.address.text() + "\"" +
         " source.port = " + std::to_string(request.port) +
         " sess.media = \"audio\"" +
         " sess.latency.msec = " + std::to_string(request.latencyMs) +
         " sess.ignore-ssrc = true" +
         " audio.format = \"" + (request.encoding == RtpEncoding::l24 ? "S24BE" : "S16BE") + "\"" +
         " audio.rate = " + std::to_string(request.rate) +
         " audio.channels = " + std::to_string(request.blockPorts.size()) +
         " audio.position = " + positions +
         " node.name = " + name + " node.description = " + name + " media.name = " + name +
         " stream.props = { node.always-process = true"
         " adapter.auto-port-config = { mode = dsp monitor = false control = false position = preserve } } }";
}

/// One receiver's engine node: the rtp-source module loaded into the engine's context, its
/// node found through a registry binding on EAR's connection.
///   1. A registry and a pw_core_sync: node globals named engineNode announced before the sync's
///      done are stale (a restart's old receiver, still going); a stale id is forgotten when its
///      global goes, as the engine may give the id again.
///   2. On the done: the module is loaded. The first node global named engineNode that is not
///      stale is this receiver's; it is bound for its info.
///   3. Its info RUNNING (its socket is bound, docs/design/rtp.md section 4, finding g): running.
class RtpReceiverBody final : public OutsideBody {
public:
  RtpReceiverBody(Engine& engine, NodeName engineNode, std::string arguments, OutsideBodyEvents& events)
      : engine_(engine), engineNode_(std::move(engineNode)), arguments_(std::move(arguments)), events_(events) {
    registry_ = pw_core_get_registry(engine_.core(), PW_VERSION_REGISTRY, 0);
    if (registry_ == nullptr) {
      events_.failed(std::string("cannot watch the engine for the RTP receiver: ") + spa_strerror(-errno));
      return;
    }
    pw_registry_add_listener(registry_, &registryHook_, &registryEvents(), this);
    pw_core_add_listener(engine_.core(), &coreHook_, &coreEvents(), this);
    coreListening_ = true;
    syncSeq_ = pw_core_sync(engine_.core(), PW_ID_CORE, 0);
    if (syncSeq_ < 0) load();   // nothing to wait for
  }

  ~RtpReceiverBody() override {
    if (moduleListening_) spa_hook_remove(&moduleHook_);
    unbindNode();
    if (coreListening_) spa_hook_remove(&coreHook_);
    if (registry_ != nullptr) {
      spa_hook_remove(&registryHook_);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    }
    if (module_ != nullptr) pw_impl_module_destroy(module_);
  }

  RtpReceiverBody(const RtpReceiverBody&) = delete;
  RtpReceiverBody& operator=(const RtpReceiverBody&) = delete;

private:
  static const pw_core_events& coreEvents() {
    static const pw_core_events table = [] {
      pw_core_events events{};
      events.version = PW_VERSION_CORE_EVENTS;
      events.done = &RtpReceiverBody::onCoreDone;
      return events;
    }();
    return table;
  }
  static const pw_registry_events& registryEvents() {
    static const pw_registry_events table = [] {
      pw_registry_events events{};
      events.version = PW_VERSION_REGISTRY_EVENTS;
      events.global = &RtpReceiverBody::onGlobal;
      events.global_remove = &RtpReceiverBody::onGlobalRemove;
      return events;
    }();
    return table;
  }
  static const pw_node_events& nodeEvents() {
    static const pw_node_events table = [] {
      pw_node_events events{};
      events.version = PW_VERSION_NODE_EVENTS;
      events.info = &RtpReceiverBody::onInfo;
      return events;
    }();
    return table;
  }
  static const pw_impl_module_events& moduleEvents() {
    static const pw_impl_module_events table = [] {
      pw_impl_module_events events{};
      events.version = PW_VERSION_IMPL_MODULE_EVENTS;
      events.destroy = &RtpReceiverBody::onModuleDestroy;
      return events;
    }();
    return table;
  }

  // 1 -> 2: every node of this name the engine had before the sync is stale.
  static void onCoreDone(void* data, std::uint32_t id, int seq) {
    auto* self = static_cast<RtpReceiverBody*>(data);
    if (id != PW_ID_CORE || seq != self->syncSeq_ || self->loading_) return;
    self->coreListening_ = false;
    spa_hook_remove(&self->coreHook_);
    self->load();
  }

  void load() {
    loading_ = true;
    module_ = pw_context_load_module(engine_.context(), moduleName, arguments_.c_str(), nullptr);
    if (module_ == nullptr) {
      events_.failed(std::string("cannot load PipeWire's rtp-source module: ") + std::strerror(errno));
      return;
    }
    pw_impl_module_add_listener(module_, &moduleHook_, &moduleEvents(), this);
    moduleListening_ = true;
  }

  static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/, const char* type,
                       std::uint32_t /*version*/, const spa_dict* props) {
    auto* self = static_cast<RtpReceiverBody*>(data);
    if (props == nullptr || std::string_view(type) != PW_TYPE_INTERFACE_Node) return;
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == nullptr || self->engineNode_ != name) return;
    if (!self->loading_) {
      self->stale_.insert(id);
    } else if (self->node_ == nullptr && !self->stale_.contains(id)) {
      self->bindNode(id);
    }
  }

  static void onGlobalRemove(void* data, std::uint32_t id) {
    auto* self = static_cast<RtpReceiverBody*>(data);
    self->stale_.erase(id);
    if (self->node_ != nullptr && id == self->nodeId_) {
      self->unbindNode();
      self->events_.failed("the engine removed the RTP receiver");
    }
  }

  void bindNode(std::uint32_t id) {
    node_ = static_cast<pw_proxy*>(pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node_ == nullptr) {
      events_.failed(std::string("cannot watch the RTP receiver: ") + spa_strerror(-errno));
      return;
    }
    nodeId_ = id;
    pw_proxy_add_object_listener(node_, &nodeHook_, &nodeEvents(), this);
  }

  void unbindNode() {
    if (node_ == nullptr) return;
    spa_hook_remove(&nodeHook_);
    pw_proxy_destroy(node_);
    node_ = nullptr;
  }

  // 3: RUNNING lets the channels be linked, by the node's global id; ERROR fails it.
  static void onInfo(void* data, const pw_node_info* info) {
    auto* self = static_cast<RtpReceiverBody*>(data);
    if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) == 0) return;
    if (info->state == PW_NODE_STATE_ERROR) {
      self->events_.failed(std::string("the RTP receiver failed: ") +
                           (info->error != nullptr ? info->error : "engine error"));
    } else if (info->state == PW_NODE_STATE_RUNNING) {
      self->events_.running(self->nodeId_);
    }
  }

  static void onModuleDestroy(void* data) {
    auto* self = static_cast<RtpReceiverBody*>(data);
    spa_hook_remove(&self->moduleHook_);
    self->moduleListening_ = false;
    self->module_ = nullptr;
    self->events_.failed("the engine removed the RTP receiver's module");
  }

  Engine& engine_;
  NodeName engineNode_;
  std::string arguments_;
  OutsideBodyEvents& events_;
  pw_registry* registry_ = nullptr;
  spa_hook registryHook_{};
  spa_hook coreHook_{};
  bool coreListening_ = false;
  int syncSeq_ = 0;
  bool loading_ = false;               // the sync is done: a node of this name now is ours
  std::set<std::uint32_t> stale_;      // node ids of this name from before the sync
  pw_impl_module* module_ = nullptr;
  spa_hook moduleHook_{};
  bool moduleListening_ = false;
  pw_proxy* node_ = nullptr;
  std::uint32_t nodeId_ = SPA_ID_INVALID;
  spa_hook nodeHook_{};
};

}  // namespace

RtpReceivers::RtpReceivers(Engine& engine) : engine_(engine) {}

std::unique_ptr<OutsideNode> RtpReceivers::make(const NodeName& engineNode, const NodeName& blockNode,
                                                const RtpReceiverRequest& request, OutsideNodeEvents& events) {
  std::vector<PortName> receiverPorts;
  for (std::size_t channel = 0; channel < request.blockPorts.size(); ++channel) {
    receiverPorts.push_back(channelPort(channel));
  }
  return joinOutsideNode(
      engine_,
      OutsideJoin{.kind = "RTP receiver",
                  .blockNode = blockNode,
                  .flow = ChannelFlow::nodeToBlock,
                  .nodePorts = std::move(receiverPorts),
                  .blockPorts = request.blockPorts},
      events, [&](OutsideBodyEvents& body) -> std::unique_ptr<OutsideBody> {
        return std::make_unique<RtpReceiverBody>(engine_, engineNode, moduleArguments(engineNode, request), body);
      });
}

}  // namespace ear::pipewire
