// Route links through the engine's link-factory (docs/architecture.md#packages,
// links.hpp).
#include "ear/pipewire/links.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "callback_boundary.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "engine_property_set.hpp"
#include "link_proxy.hpp"

namespace ear::pipewire {

/// Every event ends a link's story or moves it on; a link that is done (failed or gone) is
/// silenced at once and its proxy destroyed later on the main loop, outside the proxy's own
/// event and outside any listener's call into destroyLink.
struct Links::Impl final : LinkEvents {
	explicit Impl(Engine& engine) : engine(engine) {
		addEvent(later, engine.mainLoop(), guarded<&Impl::onLater>, this);
	}
	~Impl() {
		if (dataLoopWedged(engine)) leakAll();
		live.clear();
		done.clear();
		later.reset();
	}

	void active(LinkId link) override {
		tell([&](LinkListener& listener) { listener.linkActive(link); });
	}
	void failed(LinkId link, std::string detail) override {
		retire(link);
		tell([&](LinkListener& listener) { listener.linkFailed(link, detail); });
	}
	void gone(LinkId link) override {
		retire(link);
		tell([&](LinkListener& listener) { listener.linkGone(link); });
	}

	/// Tells every listener (the reconciler, and through it the route status listeners: core's state, not this
	/// link's) inside a scope of the main loop's activity, so a throw of theirs leaves a place and the callback boundary
	/// takes it as core's bug, never as the link proxy's whose event told it (docs/architecture.md#faults-by-owner).
	template <class Tell> void tell(Tell each) {
		const MainLoopActivity::Scope scope("tell route links", {});
		for (LinkListener* listener : listeners) each(*listener);
	}

	void retire(LinkId link) {
		auto found = live.find(link);
		if (found == live.end()) return;
		found->second->silence();
		done.push_back(std::move(found->second));
		live.erase(found);
		pw_loop_signal_event(engine.mainLoop(), later.get());
	}

	/// On a wedged data loop a link proxy is never destroyed (data_loop_wedge.hpp): its destruction would release port
	/// mixes with a blocking invoke into that loop. It is silenced and kept for good.
	void leakAll() {
		for (auto& [link, proxy] : live) {
			proxy->silence();
			static_cast<void>(proxy.release());
		}
		live.clear();
		for (auto& proxy : done) static_cast<void>(proxy.release());
		done.clear();
	}

	void onLater(std::uint64_t /*count*/) {
		if (dataLoopWedged(engine)) {
			for (auto& proxy : done) static_cast<void>(proxy.release());
		}
		done.clear();
		auto told = std::exchange(refused, {});
		for (auto& [link, detail] : told) {
			tell([&](LinkListener& listener) { listener.linkFailed(link, detail); });
		}
	}

	Engine& engine;
	std::vector<LinkListener*> listeners;
	std::map<LinkId, std::unique_ptr<LinkProxy>> live;
	std::vector<std::unique_ptr<LinkProxy>> done;
	std::vector<std::pair<LinkId, std::string>> refused; // told later, never inside createLink
	LoopSource later;
};

Links::Links(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}

Links::~Links() = default;

void Links::subscribe(LinkListener& listener) { impl_->listeners.push_back(&listener); }

void Links::createLink(LinkId link, EnginePortId output, EnginePortId input) {
	if (dataLoopWedged(impl_->engine)) { // making it would wait on the wedged loop: refused, told later
		impl_->refused.emplace_back(link, "the data loop is wedged");
		pw_loop_signal_event(impl_->engine.mainLoop(), impl_->later.get());
		return;
	}
	const PropertySet<Home::link> props({{property<Home::link>("link.output.port"), std::to_string(output)},
	                                     {property<Home::link>("link.input.port"), std::to_string(input)}});
	OwnedProxy proxy(
		createObject(impl_->engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, props));
	if (proxy == nullptr) {
		impl_->refused.emplace_back(link, std::string("cannot ask for the link: ") + spa_strerror(-errno));
		pw_loop_signal_event(impl_->engine.mainLoop(), impl_->later.get());
		return;
	}
	impl_->live[link] = std::make_unique<LinkProxy>(link, std::move(proxy), *impl_);
}

void Links::destroyLink(LinkId link) {
	impl_->retire(link);
	std::erase_if(impl_->refused, [link](const auto& entry) { return entry.first == link; });
}

} // namespace ear::pipewire
