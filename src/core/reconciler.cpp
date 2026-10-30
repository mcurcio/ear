// Route links from events (docs/design/m1-white-to-file.md section 2, reconciler.hpp): it holds
// the wanted routes and the live ports, asks the LinkMaker for one link per wanted route whose
// ends are both live, and keeps each route's status from the link events. It never knows node
// state and never touches audio.
#include "ear/core/reconciler.hpp"

#include <map>
#include <optional>
#include <utility>

namespace ear {

std::string_view matchName(RouteMatch match) noexcept {
	switch (match) {
	case RouteMatch::pending: return "pending";
	case RouteMatch::linked: return "linked";
	case RouteMatch::refused: return "refused";
	}
	return "unknown";
}

struct Reconciler::Impl {
	/// The engine ports one link attempt joined; kept after a refusal so an end going can make
	/// the route pending again.
	struct Ends {
		EnginePortId output;
		EnginePortId input;
		bool uses(EnginePortId id) const noexcept { return output == id || input == id; }
	};
	struct WantedRoute {
		RouteMatch match = RouteMatch::pending;
		std::string detail;
		std::optional<LinkId> link;   // set while a link was asked for and not failed or gone
		std::optional<Ends> ends;     // the ports of the last attempt, while both are live
	};
	using PortKey = std::pair<Direction, PortRef>;

	LinkMaker& links;
	LinkId nextLink = 1;
	std::map<Route, WantedRoute> routes;
	std::map<EnginePortId, PortKey> livePorts;
	std::map<PortKey, EnginePortId> liveIds;
	std::vector<RouteStatusListener*> listeners;

	explicit Impl(LinkMaker& l) : links(l) {}

	std::optional<EnginePortId> liveId(Direction direction, const PortRef& port) const {
		const auto it = liveIds.find(PortKey{direction, port});
		if (it == liveIds.end()) return std::nullopt;
		return it->second;
	}

	void tell(const Route& route, const WantedRoute& wanted) {
		const RouteStatus status{route, wanted.match, wanted.detail};
		for (auto* listener : listeners) listener->routeChanged(status);
	}

	void set(const Route& route, WantedRoute& wanted, RouteMatch match, std::string detail = {}) {
		if (wanted.match == match && wanted.detail == detail) return;
		wanted.match = match;
		wanted.detail = std::move(detail);
		tell(route, wanted);
	}

	/// Asks for the route's link when it has none, no refused attempt stands, and both ends are live.
	void linkIfLive(const Route& route, WantedRoute& wanted) {
		if (wanted.link || wanted.ends) return;
		const auto output = liveId(Direction::output, route.from);
		const auto input = liveId(Direction::input, route.to);
		if (!output || !input) return;
		const LinkId link = nextLink++;
		wanted.link = link;
		wanted.ends = Ends{*output, *input};
		links.createLink(link, *output, *input);
	}

	std::pair<const Route, WantedRoute>* routeOf(LinkId link) {
		for (auto& entry : routes)
			if (entry.second.link == link) return &entry;
		return nullptr;
	}
};

Reconciler::Reconciler(LinkMaker& links) : impl_(std::make_unique<Impl>(links)) {}

Reconciler::~Reconciler() = default;

void Reconciler::subscribe(RouteStatusListener& listener) { impl_->listeners.push_back(&listener); }

void Reconciler::routeAdded(const Route& route) {
	const auto [it, added] = impl_->routes.try_emplace(route);
	if (!added) return;
	impl_->tell(it->first, it->second);
	impl_->linkIfLive(it->first, it->second);
}

void Reconciler::portAppeared(const LivePort& port) {
	if (impl_->livePorts.contains(port.id)) portGone(port.id);
	const Impl::PortKey key{port.direction, port.port};
	impl_->livePorts.emplace(port.id, key);
	impl_->liveIds.insert_or_assign(key, port.id);   // the latest id of a port name is the one new links use
	for (auto& [route, wanted] : impl_->routes) impl_->linkIfLive(route, wanted);
}

void Reconciler::portGone(EnginePortId id) {
	const auto it = impl_->livePorts.find(id);
	if (it == impl_->livePorts.end()) return;
	if (const auto named = impl_->liveIds.find(it->second); named != impl_->liveIds.end() && named->second == id)
		impl_->liveIds.erase(named);
	impl_->livePorts.erase(it);
	for (auto& [route, wanted] : impl_->routes) {
		if (!wanted.ends || !wanted.ends->uses(id)) continue;
		if (wanted.link) impl_->links.destroyLink(*wanted.link);
		wanted.link.reset();
		wanted.ends.reset();
		impl_->set(route, wanted, RouteMatch::pending);
	}
}

void Reconciler::linkActive(LinkId link) {
	auto* entry = impl_->routeOf(link);
	if (entry == nullptr) return;
	impl_->set(entry->first, entry->second, RouteMatch::linked);
}

void Reconciler::linkFailed(LinkId link, std::string detail) {
	auto* entry = impl_->routeOf(link);
	if (entry == nullptr) return;
	entry->second.link.reset();   // keeps ends: no new attempt until an end goes and comes back
	impl_->set(entry->first, entry->second, RouteMatch::refused, std::move(detail));
}

void Reconciler::linkGone(LinkId link) {
	auto* entry = impl_->routeOf(link);
	if (entry == nullptr) return;
	entry->second.link.reset();
	entry->second.ends.reset();
	impl_->set(entry->first, entry->second, RouteMatch::pending);
	impl_->linkIfLive(entry->first, entry->second);
}

std::vector<RouteStatus> Reconciler::status() const {
	std::vector<RouteStatus> all;
	all.reserve(impl_->routes.size());
	for (const auto& [route, wanted] : impl_->routes) all.push_back(RouteStatus{route, wanted.match, wanted.detail});
	return all;
}

} // namespace ear
