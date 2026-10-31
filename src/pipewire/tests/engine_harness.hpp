// The engine tests' shared harness (docs/design/rtp.md section 7.4): the in-process engine with no
// daemon, runtime directory or D-Bus; a Waiter that runs its main loop until the events a test
// waits for have arrived (the deadline only fails the test); port block events; and the
// registry as a test reads it. Moved unchanged out of sound_card_test.cpp so the card's and the
// RTP receiver's engine tests share one copy. The Waiter and makeEngine follow engine_test.cpp's
// (the pipewire-engine skill's pattern).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>

#include "catch2/catch.hpp"
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/port_blocks.hpp"

namespace ear::pipewire::testing {
using namespace std::chrono_literals;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;
constexpr AudioFormat kFormat{kRate, kBlock};

inline std::unique_ptr<Engine> makeEngine() {
  for (const char* name : {"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE",
                           "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS"}) {
    unsetenv(name);
  }
  auto engine = Engine::create(EngineSettings{EAR_ENGINE_CONF, kRate, kBlock});
  if (!engine) FAIL("Engine::create: " << engine.error().detail);
  return std::move(*engine);
}

/// Runs the engine's main loop until `done` holds. Every event a test listens for calls
/// wake() (from any thread), which re-checks `done` on the main loop.
class Waiter {
public:
  explicit Waiter(Engine& engine) : engine_(engine) {
    wake_ = pw_loop_add_event(engine.mainLoop(), &Waiter::onWake, this);
  }
  ~Waiter() { pw_loop_destroy_source(engine_.mainLoop(), wake_); }

  void wake() noexcept { pw_loop_signal_event(engine_.mainLoop(), wake_); }

  bool until(std::function<bool()> done, std::chrono::seconds deadline = 5s) {
    done_ = std::move(done);
    if (done_()) return true;
    timedOut_ = false;
    spa_source* timer = pw_loop_add_timer(engine_.mainLoop(), &Waiter::onTimeout, this);
    timespec value{static_cast<time_t>(deadline.count()), 0};
    pw_loop_update_timer(engine_.mainLoop(), timer, &value, nullptr, false);
    engine_.run();
    pw_loop_destroy_source(engine_.mainLoop(), timer);
    return !timedOut_ && done_();
  }

private:
  static void onWake(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<Waiter*>(data);
    if (self->done_ && self->done_()) self->engine_.quit();
  }
  static void onTimeout(void* data, std::uint64_t /*expirations*/) {
    auto* self = static_cast<Waiter*>(data);
    self->timedOut_ = true;
    self->engine_.quit();
  }

  Engine& engine_;
  spa_source* wake_ = nullptr;
  std::function<bool()> done_;
  bool timedOut_ = false;
};

class NoBell final : public LevelWake {
public:
  void levelsPublished() noexcept override {}
};

class BlockEvents final : public PortBlockEvents {
public:
  explicit BlockEvents(Waiter& waiter) : waiter_(waiter) {}
  void ready() override {
    isReady = true;
    waiter_.wake();
  }
  void lost(std::string) override { waiter_.wake(); }
  bool isReady = false;

private:
  Waiter& waiter_;
};

/// Engine nodes and links as the registry shows them; nodes are bound for their properties and
/// links for their state.
class Registry {
public:
  Registry(Engine& engine, Waiter& waiter) : waiter_(waiter) {
    registry_ = pw_core_get_registry(engine.core(), PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &hook_, &registryEvents(), this);
  }
  ~Registry() {
    for (Node& node : nodes_) {
      if (node.proxy == nullptr) continue;
      spa_hook_remove(&node.hook);
      pw_proxy_destroy(node.proxy);
    }
    for (auto& [id, link] : links_) unbindLink(link);
    spa_hook_remove(&hook_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
  }
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  /// Told on every node info, after the registry has recorded it.
  std::function<void(const NodeName&)> onNodeInfo;

  bool seen(const NodeName& name) const { return find(name, true) != nullptr; }
  bool present(const NodeName& name) const { return find(name, false) != nullptr; }
  std::optional<std::uint32_t> id(const NodeName& name) const {
    const Node* node = find(name, false);
    return node ? std::optional(node->id) : std::nullopt;
  }
  /// Every present node of this name, in the order the registry announced them.
  std::vector<std::uint32_t> ids(const NodeName& name) const {
    std::vector<std::uint32_t> found;
    for (const Node& node : nodes_) {
      if (node.name == name && !node.removed) found.push_back(node.id);
    }
    return found;
  }
  /// node.driver-id, once the node's info carried it.
  std::optional<std::uint32_t> driverId(const NodeName& name) const {
    const Node* node = find(name, false);
    if (node == nullptr) return std::nullopt;
    const auto found = node->props.find(PW_KEY_NODE_DRIVER_ID);
    if (found == node->props.end()) return std::nullopt;
    return static_cast<std::uint32_t>(std::stoul(found->second));
  }
  const std::map<std::string, std::string>* props(const NodeName& name) const {
    const Node* node = find(name, false);
    return node != nullptr && node->infoSeen ? &node->props : nullptr;
  }
  /// Links that touch node `id`, present now.
  std::vector<std::uint32_t> linksOf(std::uint32_t id) const {
    std::vector<std::uint32_t> found;
    for (const auto& [linkId, link] : links_) {
      if (link.output == id || link.input == id) found.push_back(linkId);
    }
    return found;
  }
  /// How many links that touch node `id` this connection has heard are ACTIVE (their info on
  /// this registry's binding, so it trails any proxy bound to the link before it).
  std::size_t activeLinksOf(std::uint32_t id) const {
    return static_cast<std::size_t>(std::ranges::count_if(links_, [&](const auto& entry) {
      const Link& link = entry.second;
      return (link.output == id || link.input == id) && link.state == PW_LINK_STATE_ACTIVE;
    }));
  }
  /// Destroys a global through the registry, as another client or the engine could.
  bool destroy(std::uint32_t id) { return pw_registry_destroy(registry_, id) >= 0; }
  /// The named nodes' props are unknown until their fresh info arrives (no wait here).
  void rebind(const std::vector<NodeName>& names) {
    for (Node& node : nodes_) {
      if (node.removed || std::ranges::find(names, node.name) == names.end()) continue;
      spa_hook_remove(&node.hook);
      pw_proxy_destroy(node.proxy);
      node.infoSeen = false;
      bindNode(node);
    }
  }
  std::optional<pw_node_state> state(const NodeName& name) const {
    const Node* node = find(name, false);
    return node != nullptr && node->infoSeen ? std::optional(node->state) : std::nullopt;
  }
  /// Binds the named nodes afresh: a bind sends the node's whole current info, the only way a
  /// client learns a node's new node.driver-id on PipeWire 1.4.2 (see onInfo). Then waits for it.
  bool reread(const std::vector<NodeName>& names) {
    rebind(names);
    return waiter_.until([&] {
      return std::ranges::all_of(names, [&](const NodeName& name) { return props(name) != nullptr; });
    });
  }

private:
  struct Node {
    Registry* owner = nullptr;
    std::uint32_t id = 0;
    std::string name;
    pw_proxy* proxy = nullptr;
    spa_hook hook{};
    bool infoSeen = false;
    bool removed = false;
    pw_node_state state = PW_NODE_STATE_CREATING;
    std::map<std::string, std::string> props;
  };
  struct Link {
    Registry* owner = nullptr;
    std::uint32_t output = SPA_ID_INVALID;
    std::uint32_t input = SPA_ID_INVALID;
    pw_proxy* proxy = nullptr;
    spa_hook hook{};
    pw_link_state state = PW_LINK_STATE_INIT;
  };

  const Node* find(const NodeName& name, bool includeRemoved) const {
    for (const Node& node : nodes_) {
      if (node.name == name && (includeRemoved || !node.removed)) return &node;
    }
    return nullptr;
  }

  static const pw_registry_events& registryEvents() {
    static const pw_registry_events table = [] {
      pw_registry_events events{};
      events.version = PW_VERSION_REGISTRY_EVENTS;
      events.global = &Registry::onGlobal;
      events.global_remove = &Registry::onGlobalRemove;
      return events;
    }();
    return table;
  }
  static const pw_link_events& linkEvents() {
    static const pw_link_events table = [] {
      pw_link_events events{};
      events.version = PW_VERSION_LINK_EVENTS;
      events.info = &Registry::onLinkInfo;
      return events;
    }();
    return table;
  }
  static const pw_node_events& nodeEvents() {
    static const pw_node_events table = [] {
      pw_node_events events{};
      events.version = PW_VERSION_NODE_EVENTS;
      events.info = &Registry::onInfo;
      return events;
    }();
    return table;
  }

  static std::uint32_t number(const spa_dict* props, const char* key) {
    const char* value = spa_dict_lookup(props, key);
    return value != nullptr ? static_cast<std::uint32_t>(std::stoul(value)) : SPA_ID_INVALID;
  }

  static void onGlobal(void* data, std::uint32_t id, std::uint32_t, const char* type,
                       std::uint32_t, const spa_dict* props) {
    auto* self = static_cast<Registry*>(data);
    if (props == nullptr) return;
    if (std::string_view(type) == PW_TYPE_INTERFACE_Link) {
      Link& link = self->links_[id];
      link.owner = self;
      link.output = number(props, PW_KEY_LINK_OUTPUT_NODE);
      link.input = number(props, PW_KEY_LINK_INPUT_NODE);
      link.proxy = static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0));
      if (link.proxy != nullptr) pw_proxy_add_object_listener(link.proxy, &link.hook, &linkEvents(), &link);
      self->waiter_.wake();
      return;
    }
    if (std::string_view(type) != PW_TYPE_INTERFACE_Node) return;
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == nullptr) return;
    Node& node = self->nodes_.emplace_back();
    node.owner = self;
    node.id = id;
    node.name = name;
    self->bindNode(node);
    self->waiter_.wake();
  }

  void bindNode(Node& node) {
    node.hook = {};
    node.proxy = static_cast<pw_proxy*>(
        pw_registry_bind(registry_, node.id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    pw_proxy_add_object_listener(node.proxy, &node.hook, &nodeEvents(), &node);
  }

  static void onGlobalRemove(void* data, std::uint32_t id) {
    auto* self = static_cast<Registry*>(data);
    if (const auto link = self->links_.find(id); link != self->links_.end()) {
      unbindLink(link->second);
      self->links_.erase(link);
    }
    for (Node& node : self->nodes_) {
      if (node.id != id || node.removed) continue;
      node.removed = true;
      spa_hook_remove(&node.hook);
      pw_proxy_destroy(node.proxy);
      node.proxy = nullptr;
    }
    self->waiter_.wake();
  }

  static void unbindLink(Link& link) {
    if (link.proxy == nullptr) return;
    spa_hook_remove(&link.hook);
    pw_proxy_destroy(link.proxy);
    link.proxy = nullptr;
  }

  static void onLinkInfo(void* data, const pw_link_info* info) {
    auto* link = static_cast<Link*>(data);
    if ((info->change_mask & PW_LINK_CHANGE_MASK_STATE) != 0) link->state = info->state;
    link->owner->waiter_.wake();
  }

  /// PipeWire 1.4.2 sets a node's node.driver-id when its driver changes but tells no client
  /// (impl-node.c pw_impl_node_set_driver emits the info on the driver's in-process listeners
  /// only), so a test reads it through reread().
  static void onInfo(void* data, const pw_node_info* info) {
    auto* node = static_cast<Node*>(data);
    Registry* self = node->owner;
    if (info->props != nullptr) {
      const spa_dict_item* item = nullptr;
      spa_dict_for_each(item, info->props) node->props[item->key] = item->value;
    }
    if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0) node->state = info->state;
    node->infoSeen = true;
    if (self->onNodeInfo) self->onNodeInfo(node->name);
    self->waiter_.wake();
  }

  Waiter& waiter_;
  pw_registry* registry_ = nullptr;
  spa_hook hook_{};
  std::list<Node> nodes_;
  std::map<std::uint32_t, Link> links_;   // a map's elements stay put: their hooks are registered
};

}  // namespace ear::pipewire::testing
