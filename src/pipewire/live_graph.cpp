// Live ports from the engine's registry (docs/design/m1-white-to-file.md section 2,
// live_graph.hpp).
#include "ear/pipewire/live_graph.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

namespace ear::pipewire {
namespace {

/// A port global seen before, or without, its node's global.
struct PortGlobal {
  std::uint32_t node;
  PortName name;
  Direction direction;
};

std::optional<Direction> parseDirection(const char* value) {
  const std::string_view text = value != nullptr ? value : "";
  if (text == "out") return Direction::output;
  if (text == "in") return Direction::input;
  return std::nullopt;
}

/// One subscriber's view: its own registry binding, so the engine replays every existing global
/// to it as events (never inside subscribe()), and the join state that turns port and node
/// globals into LivePorts, whichever arrives first.
class Feed {
public:
  Feed(pw_core* core, LivePortListener& listener) : listener_(listener) {
    registry_ = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &hook_, &registryEvents(), this);
  }
  ~Feed() {
    spa_hook_remove(&hook_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
  }
  Feed(const Feed&) = delete;
  Feed& operator=(const Feed&) = delete;

private:
  static const pw_registry_events& registryEvents() {
    static const pw_registry_events table = [] {
      pw_registry_events events{};
      events.version = PW_VERSION_REGISTRY_EVENTS;
      events.global = &Feed::onGlobal;
      events.global_remove = &Feed::onGlobalRemove;
      return events;
    }();
    return table;
  }

  static void onGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                       const char* type, std::uint32_t /*version*/, const spa_dict* props) {
    auto* self = static_cast<Feed*>(data);
    if (props == nullptr) return;
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
      const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (name == nullptr) return;
      self->nodes_[id] = name;
      for (const auto& [portId, port] : self->ports_) {
        if (port.node == id) self->announce(portId, port);
      }
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
      const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
      const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
      const auto direction = parseDirection(spa_dict_lookup(props, PW_KEY_PORT_DIRECTION));
      std::uint32_t nodeId = 0;
      if (node == nullptr || name == nullptr || !direction || !spa_atou32(node, &nodeId, 10)) {
        return;
      }
      const PortGlobal port{nodeId, name, *direction};
      self->ports_[id] = port;
      if (self->nodes_.contains(nodeId)) self->announce(id, port);
    }
  }

  static void onGlobalRemove(void* data, std::uint32_t id) {
    auto* self = static_cast<Feed*>(data);
    self->nodes_.erase(id);
    self->ports_.erase(id);
    if (self->announced_.erase(id) > 0) self->listener_.portGone(id);
  }

  void announce(std::uint32_t id, const PortGlobal& port) {
    if (!announced_.insert(id).second) return;
    listener_.portAppeared(LivePort{PortRef{nodes_.at(port.node), port.name}, port.direction, id});
  }

  LivePortListener& listener_;
  pw_registry* registry_ = nullptr;
  spa_hook hook_{};
  std::map<std::uint32_t, NodeName> nodes_;
  std::map<std::uint32_t, PortGlobal> ports_;
  std::set<std::uint32_t> announced_;
};

}  // namespace

struct LiveGraph::Impl {
  explicit Impl(Engine& engine) : engine(engine) {}
  Engine& engine;
  std::vector<std::unique_ptr<Feed>> feeds;
};

LiveGraph::LiveGraph(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}

LiveGraph::~LiveGraph() = default;

void LiveGraph::subscribe(LivePortListener& listener) {
  impl_->feeds.push_back(std::make_unique<Feed>(impl_->engine.core(), listener));
}

}  // namespace ear::pipewire
