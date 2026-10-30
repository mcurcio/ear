// Route links through the engine's link-factory (docs/design/m1-white-to-file.md section 2,
// links.hpp).
#include "ear/pipewire/links.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

namespace ear::pipewire {
namespace {

/// What a link's proxy tells Links, on the main loop.
class LinkEvents {
public:
  virtual void active(LinkId link) = 0;
  virtual void failed(LinkId link, std::string detail) = 0;
  virtual void gone(LinkId link) = 0;

protected:
  ~LinkEvents() = default;
};

/// One link, owned by EAR's connection: the engine removes the link when this proxy goes
/// (no object.linger).
class LinkProxy {
public:
  LinkProxy(LinkId id, pw_proxy* proxy, LinkEvents& events)
      : id_(id), proxy_(proxy), events_(events) {
    pw_proxy_add_listener(proxy_, &proxyHook_, &proxyEvents(), this);
    pw_proxy_add_object_listener(proxy_, &linkHook_, &linkEvents(), this);
  }
  ~LinkProxy() {
    silence();
    pw_proxy_destroy(proxy_);
  }
  LinkProxy(const LinkProxy&) = delete;
  LinkProxy& operator=(const LinkProxy&) = delete;

  /// No further events from this proxy.
  void silence() {
    if (!listening_) return;
    listening_ = false;
    spa_hook_remove(&proxyHook_);
    spa_hook_remove(&linkHook_);
  }

private:
  static const pw_proxy_events& proxyEvents() {
    static const pw_proxy_events table = [] {
      pw_proxy_events events{};
      events.version = PW_VERSION_PROXY_EVENTS;
      events.removed = &LinkProxy::onRemoved;
      events.error = &LinkProxy::onError;
      return events;
    }();
    return table;
  }
  static const pw_link_events& linkEvents() {
    static const pw_link_events table = [] {
      pw_link_events events{};
      events.version = PW_VERSION_LINK_EVENTS;
      events.info = &LinkProxy::onInfo;
      return events;
    }();
    return table;
  }

  static void onInfo(void* data, const pw_link_info* info) {
    auto* self = static_cast<LinkProxy*>(data);
    if ((info->change_mask & PW_LINK_CHANGE_MASK_STATE) == 0) return;
    if (info->state == PW_LINK_STATE_ACTIVE && !self->active_) {
      self->active_ = true;
      self->events_.active(self->id_);
    } else if (info->state == PW_LINK_STATE_ERROR && !self->active_) {
      // The link-factory destroys a link in error; an active one reports removed next.
      self->events_.failed(self->id_, info->error != nullptr ? info->error : "link error");
    }
  }

  static void onError(void* data, int /*seq*/, int res, const char* message) {
    auto* self = static_cast<LinkProxy*>(data);
    if (self->active_) return;
    self->events_.failed(self->id_, message != nullptr ? message : spa_strerror(res));
  }

  static void onRemoved(void* data) {
    auto* self = static_cast<LinkProxy*>(data);
    if (self->active_) {
      self->events_.gone(self->id_);
    } else {
      self->events_.failed(self->id_, "the engine removed the link before it was active");
    }
  }

  LinkId id_;
  pw_proxy* proxy_;
  LinkEvents& events_;
  spa_hook proxyHook_{};
  spa_hook linkHook_{};
  bool listening_ = true;
  bool active_ = false;
};

}  // namespace

/// Every event ends a link's story or moves it on; a link that is done (failed or gone) is
/// silenced at once and its proxy destroyed later on the main loop, outside the proxy's own
/// event and outside any listener's call into destroyLink.
struct Links::Impl final : LinkEvents {
  explicit Impl(Engine& engine) : engine(engine) {
    later = pw_loop_add_event(engine.mainLoop(), &Impl::onLater, this);
  }
  ~Impl() {
    live.clear();
    done.clear();
    if (later != nullptr) pw_loop_destroy_source(engine.mainLoop(), later);
  }

  void active(LinkId link) override {
    for (LinkListener* listener : listeners) listener->linkActive(link);
  }
  void failed(LinkId link, std::string detail) override {
    retire(link);
    for (LinkListener* listener : listeners) listener->linkFailed(link, detail);
  }
  void gone(LinkId link) override {
    retire(link);
    for (LinkListener* listener : listeners) listener->linkGone(link);
  }

  void retire(LinkId link) {
    auto found = live.find(link);
    if (found == live.end()) return;
    found->second->silence();
    done.push_back(std::move(found->second));
    live.erase(found);
    pw_loop_signal_event(engine.mainLoop(), later);
  }

  static void onLater(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<Impl*>(data);
    self->done.clear();
    auto refused = std::exchange(self->refused, {});
    for (auto& [link, detail] : refused) {
      for (LinkListener* listener : self->listeners) listener->linkFailed(link, detail);
    }
  }

  Engine& engine;
  std::vector<LinkListener*> listeners;
  std::map<LinkId, std::unique_ptr<LinkProxy>> live;
  std::vector<std::unique_ptr<LinkProxy>> done;
  std::vector<std::pair<LinkId, std::string>> refused;   // told later, never inside createLink
  spa_source* later = nullptr;
};

Links::Links(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}

Links::~Links() = default;

void Links::subscribe(LinkListener& listener) { impl_->listeners.push_back(&listener); }

void Links::createLink(LinkId link, EnginePortId output, EnginePortId input) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input);
  auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
      impl_->engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
      &props->dict, 0));
  pw_properties_free(props);
  if (proxy == nullptr) {
    impl_->refused.emplace_back(link, std::string("cannot ask for the link: ") +
                                          spa_strerror(-errno));
    pw_loop_signal_event(impl_->engine.mainLoop(), impl_->later);
    return;
  }
  impl_->live[link] = std::make_unique<LinkProxy>(link, proxy, *impl_);
}

void Links::destroyLink(LinkId link) {
  impl_->retire(link);
  std::erase_if(impl_->refused, [link](const auto& entry) { return entry.first == link; });
}

}  // namespace ear::pipewire
