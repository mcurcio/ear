// One link's proxy and its events (docs/engine.md), moved out of links.cpp
// unchanged so Links (route links) and AlsaSoundCards (a card's own links) share it. Private to
// ear_pipewire: its sources include it by this path; no public header does.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
#include "ear/core/internal_error.hpp"
#include "ear/core/reconciler.hpp"

namespace ear::pipewire {

/// What a link's proxy tells Links, on the main loop.
class LinkEvents {
public:
	virtual void active(LinkId link) = 0;
	virtual void failed(LinkId link, std::string detail) = 0;
	virtual void gone(LinkId link) = 0;
	/// The link's own callback threw (ear's bug, docs/architecture.md#faults-by-owner): nothing more is heard from it,
	/// and it is failed with `detail` (internalErrorDetail's), as a link the engine refused.
	virtual void faulted(LinkId link, std::string detail) { failed(link, std::move(detail)); }

protected:
	~LinkEvents() = default;
};

/// One link, owned by EAR's connection: the engine removes the link when this proxy goes
/// (no object.linger). Its state is its own, so a throw out of its callback fails only it: its route, or the engine
/// companion (and so the node, `node`) whose channel it carries (docs/architecture.md#faults-by-owner).
class LinkProxy final : public FaultOwner {
public:
	/// Takes `proxy`, already held, so nothing between its making and this leaks it, and it goes with this, even when
	/// the constructor throws.
	LinkProxy(LinkId id, OwnedProxy proxy, LinkEvents& events, std::string node = {})
		: id_(id), proxy_(std::move(proxy)), events_(events), node_(std::move(node)) {
		listen<ProxyCallbacks<LinkProxy>{.removed = guarded<&LinkProxy::onRemoved>,
		                                 .error = guarded<&LinkProxy::onError>}>(proxy_.get(), proxyListener_, this);
		listen<LinkCallbacks<LinkProxy>{.info = guarded<&LinkProxy::onInfo>}>(proxy_.get(), linkListener_, this);
	}
	~LinkProxy() { silence(); } // the listeners first, then the proxy (declared before them)
	LinkProxy(const LinkProxy&) = delete;
	LinkProxy& operator=(const LinkProxy&) = delete;

	/// No further events from this proxy.
	void silence() noexcept {
		proxyListener_.remove();
		linkListener_.remove();
	}

	std::string_view faultNode() const noexcept override { return node_; }
	void faulted(std::string_view callback, std::string_view detail) override {
		silence();
		events_.faulted(id_, internalErrorDetail(callback, detail));
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
	OwnedProxy proxy_;
	LinkEvents& events_;
	std::string node_; // the node whose engine companion this link joins; empty for a route's
	Listener proxyListener_;
	Listener linkListener_;
	bool active_ = false;
};

} // namespace ear::pipewire
