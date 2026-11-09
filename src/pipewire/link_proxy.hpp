// One link's proxy and its events (docs/engine.md), moved out of links.cpp
// unchanged so Links (route links) and AlsaSoundCards (a card's own links) share it. Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

#include <string>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
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
	LinkProxy(LinkId id, pw_proxy* proxy, LinkEvents& events) : id_(id), proxy_(proxy), events_(events) {
		listen<ProxyCallbacks<LinkProxy>{.removed = guarded<&LinkProxy::onRemoved>,
		                                 .error = guarded<&LinkProxy::onError>}>(proxy_, proxyHook_, this);
		listen<LinkCallbacks<LinkProxy>{.info = guarded<&LinkProxy::onInfo>}>(proxy_, linkHook_, this);
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
	void onInfo(const pw_link_info* info) {
		if ((info->change_mask & PW_LINK_CHANGE_MASK_STATE) == 0) return;
		if (info->state == PW_LINK_STATE_ACTIVE && !active_) {
			active_ = true;
			events_.active(id_);
		} else if (info->state == PW_LINK_STATE_ERROR && !active_) {
			// The link-factory destroys a link in error; an active one reports removed next.
			events_.failed(id_, info->error != nullptr ? info->error : "link error");
		}
	}

	void onError(int /*seq*/, int res, const char* message) {
		if (active_) return;
		events_.failed(id_, message != nullptr ? message : spa_strerror(res));
	}

	void onRemoved() {
		if (active_) {
			events_.gone(id_);
		} else {
			events_.failed(id_, "the engine removed the link before it was active");
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

} // namespace ear::pipewire
