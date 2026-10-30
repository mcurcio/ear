// One link's proxy and its events (docs/design/batch2.md section 4.3), moved out of links.cpp
// unchanged so Links (route links) and AlsaSoundCards (a card's own links) share it. Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

#include <string>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "ear/core/reconciler.hpp"

namespace ear::pipewire {

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

}  // namespace ear::pipewire
