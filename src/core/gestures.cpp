// Gestures (gestures.hpp; docs/architecture.md#gestures).
#include "ear/core/gestures.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>

#include "ear/core/decimal_text.hpp"

namespace ear {
namespace {

/// 128 random bits in hex: never reused, not guessable, so holding one is the credential to end it.
std::string newGestureId() {
	static thread_local std::mt19937_64 bits = [] {
		std::random_device random;
		std::seed_seq seed{random(), random(), random(), random(), random(), random(), random(), random()};
		return std::mt19937_64(seed);
	}();
	return std::format("{:016x}{:016x}", bits(), bits());
}

} // namespace

struct Gestures::Impl {
	struct Gesture {
		std::string id;
		SessionToken owner;
		std::vector<GesturePair> reach;
		double low;
		double high;
		unsigned decimals;
		double committed;
		double accepted;                             // the last accepted value: what the slots hold
		std::uint64_t seq = 0;                       // the last taken
		bool ending = false;                         // its end was read
		std::optional<SessionToken> endedBy;         // the session whose end it is, when not its owner
		std::string endReason;                       // a release's
		std::chrono::steady_clock::time_point heard; // its owner's last message about it
		std::unique_ptr<Timer> stale;
		std::optional<std::chrono::steady_clock::time_point> refusedAt; // the last tick-refused told
		std::optional<std::chrono::steady_clock::time_point> toldAt;    // the last change told
		bool changePending = false;
		std::unique_ptr<Timer> pace; // the trailing change
	};
	LiveTarget& nodes;
	Timers& timers;
	std::chrono::milliseconds staleAfter;
	std::vector<GestureListener*> listeners;
	std::move_only_function<void(GestureEnding)> release;
	std::map<std::string, Gesture> open;
	std::vector<GesturePair> unsettled;

	void tell(const GestureNote& note) {
		for (GestureListener* listener : listeners) listener->gestureChanged(note);
	}
	static std::vector<GestureKey> keysOf(const Gesture& g) {
		std::vector<GestureKey> keys;
		for (const GesturePair& pair : g.reach) keys.push_back(pair.at);
		return keys;
	}
	Gesture* find(const std::string& id) {
		const auto found = open.find(id);
		return found == open.end() ? nullptr : &found->second;
	}
	void arm(Gesture& g) {
		g.heard = timers.now();
		g.stale = timers.after(staleAfter, [this, id = g.id] { releaseOf(id, "stale"); });
	}
	/// Each reached node's writes of `value`, grouped by node in reach order.
	static std::vector<std::pair<NodeName, std::vector<LiveWrite>>> writesOf(const std::vector<GesturePair>& reach,
	                                                                         double value) {
		std::vector<std::pair<NodeName, std::vector<LiveWrite>>> out;
		for (const GesturePair& pair : reach) {
			auto it = std::ranges::find(out, pair.at.node, &std::pair<NodeName, std::vector<LiveWrite>>::first);
			if (it == out.end()) {
				out.emplace_back(pair.at.node, std::vector<LiveWrite>{});
				it = std::prev(out.end());
			}
			it->second.push_back(LiveWrite{pair.member, std::bit_cast<std::uint64_t>(value)});
		}
		return out;
	}
	void tellChange(Gesture& g) {
		g.changePending = false;
		g.toldAt = timers.now();
		tell(GestureNote{
			.gesture = g.id, .phase = GesturePhase::change, .owner = g.owner, .value = g.accepted, .reach = keysOf(g)});
	}
	void changed(Gesture& g) {
		const auto now = timers.now();
		if (!g.toldAt || now - *g.toldAt >= gestureChangePace) {
			g.pace.reset();
			tellChange(g);
			return;
		}
		g.changePending = true;
		if (!g.pace)
			g.pace = timers.after(
				std::chrono::duration_cast<std::chrono::milliseconds>(gestureChangePace - (now - *g.toldAt)),
				[this, id = g.id] {
					Gesture* again = find(id);
					if (again == nullptr) return;
					again->pace.reset();
					if (again->changePending) tellChange(*again);
				});
	}
	/// Every slot of `reach` set to its node's committed value; a node gone or with none is skipped.
	void settle(const std::vector<GesturePair>& reach) {
		for (const GesturePair& pair : reach) {
			const auto committed = nodes.committedLive(pair.at.node, pair.member);
			if (!committed) continue;
			const LiveWrite write{pair.member, *committed};
			nodes.setLive(pair.at.node, std::span(&write, 1));
		}
	}
	void releaseOf(const std::string& id, const std::string& reason) {
		Gesture* g = find(id);
		if (g == nullptr || g->ending) return;
		g->ending = true;
		g->stale.reset();
		g->endReason = reason;
		GestureEnding ending{g->id,   g->reach.front().at, roundedDecimal(g->accepted, g->decimals), keysOf(*g), reason,
		                     g->owner};
		if (release) release(std::move(ending));
		else abortOf(id, "refused", false, true); // nobody commits releases: never left open
	}
	void abortOf(const std::string& id, const std::string& reason, bool toldByChange, bool settleNow,
	             std::optional<SessionToken> by = std::nullopt) {
		const auto found = open.find(id);
		if (found == open.end()) return;
		Gesture g = std::move(found->second);
		open.erase(found);
		GestureNote note{.gesture = g.id,
		                 .phase = GesturePhase::aborted,
		                 .owner = g.owner,
		                 .reach = keysOf(g),
		                 .reason = reason,
		                 .handValue = g.accepted};
		if (by && *by != g.owner) note.by = by;
		else if (g.endedBy) note.by = g.endedBy;
		if (!toldByChange) note.value = g.committed;
		tell(note);
		if (settleNow) settle(g.reach);
		else unsettled.insert(unsettled.end(), g.reach.begin(), g.reach.end());
	}
};

Gestures::Gestures(LiveTarget& nodes, Timers& timers, std::chrono::milliseconds staleAfter)
	: impl_(std::make_unique<Impl>(Impl{nodes, timers, staleAfter, {}, {}, {}, {}})) {}

Gestures::~Gestures() = default;

void Gestures::subscribe(GestureListener& listener) { impl_->listeners.push_back(&listener); }

void Gestures::releaseThrough(std::move_only_function<void(GestureEnding)> release) {
	impl_->release = std::move(release);
}

std::optional<Gestures::Held> Gestures::heldAt(const GestureKey& at) const {
	for (const auto& [id, g] : impl_->open)
		if (std::ranges::any_of(g.reach, [&](const GesturePair& pair) { return pair.at == at; }))
			return Held{id, std::chrono::duration_cast<std::chrono::milliseconds>(impl_->timers.now() - g.heard)};
	return std::nullopt;
}

std::string Gestures::open(GestureOpen open) {
	if (open.reach.empty()) throw std::logic_error("gestures: a reach holds at least its own pair");
	std::string id = newGestureId();
	Impl::Gesture g{.id = id,
	                .owner = open.session,
	                .reach = std::move(open.reach),
	                .low = open.low,
	                .high = open.high,
	                .decimals = open.decimals,
	                .committed = open.committed,
	                .accepted = open.committed};
	Impl::Gesture& in = impl_->open.emplace(id, std::move(g)).first->second;
	impl_->arm(in);
	impl_->tell(GestureNote{.gesture = id,
	                        .phase = GesturePhase::begin,
	                        .owner = in.owner,
	                        .value = in.committed,
	                        .reach = Impl::keysOf(in)});
	return id;
}

void Gestures::move(const std::string& gesture, SessionToken session, std::uint64_t seq, double value) {
	Impl::Gesture* g = impl_->find(gesture);
	if (g == nullptr || g->ending || g->owner != session) return;
	impl_->arm(*g); // every message of its owner about it, a stale or refused one too
	if (seq <= g->seq) return;
	g->seq = seq;
	if (value == g->accepted) return;
	const auto writes = Impl::writesOf(g->reach, value);
	for (const auto& [node, nodeWrites] : writes) {
		auto checked = impl_->nodes.checkLive(node, nodeWrites);
		if (checked) continue;
		const auto now = impl_->timers.now();
		if (!g->refusedAt || now - *g->refusedAt >= gestureRefusalPace) {
			g->refusedAt = now;
			impl_->tell(GestureNote{.gesture = g->id,
			                        .phase = GesturePhase::tickRefused,
			                        .owner = g->owner,
			                        .code = "invalid-settings",
			                        .key = std::move(checked.error().key),
			                        .detail = std::move(checked.error().detail)});
		}
		return;
	}
	g->accepted = value;
	for (const auto& [node, nodeWrites] : writes) impl_->nodes.setLive(node, nodeWrites);
	impl_->changed(*g);
}

std::expected<GestureEnding, GestureRefusal> Gestures::end(const std::string& gesture, std::optional<double> value,
                                                           SessionToken by) {
	Impl::Gesture* g = impl_->find(gesture);
	if (g == nullptr || g->ending) return std::unexpected(GestureRefusal::unknownGesture);
	g->ending = true;
	g->stale.reset();
	if (by != g->owner) g->endedBy = by;
	return GestureEnding{
		g->id, g->reach.front().at, roundedDecimal(value.value_or(g->accepted), g->decimals), Impl::keysOf(*g), "", by};
}

void Gestures::close(const std::string& gesture, double value) {
	const auto found = impl_->open.find(gesture);
	if (found == impl_->open.end()) return;
	Impl::Gesture g = std::move(found->second);
	impl_->open.erase(found);
	impl_->settle(g.reach);
	impl_->tell(GestureNote{.gesture = g.id,
	                        .phase = GesturePhase::end,
	                        .owner = g.owner,
	                        .by = g.endedBy,
	                        .value = value,
	                        .reach = Impl::keysOf(g),
	                        .reason = g.endReason});
}

void Gestures::abort(const std::string& gesture, const std::string& reason, bool toldByChange, bool settleNow) {
	impl_->abortOf(gesture, reason, toldByChange, settleNow);
}

void Gestures::settle() {
	std::vector<GesturePair> unsettled = std::move(impl_->unsettled);
	impl_->unsettled.clear();
	impl_->settle(unsettled);
}

std::expected<void, GestureRefusal> Gestures::cancel(const std::string& gesture, SessionToken by) {
	if (impl_->find(gesture) == nullptr) return std::unexpected(GestureRefusal::unknownGesture);
	impl_->abortOf(gesture, "cancelled", false, true, by);
	return {};
}

void Gestures::sessionClosed(SessionToken session) {
	std::vector<std::string> ids;
	for (const auto& [id, g] : impl_->open)
		if (g.owner == session && !g.ending) ids.push_back(id);
	for (const std::string& id : ids) impl_->releaseOf(id, "session-closed");
}

void Gestures::drop() {
	impl_->open.clear();
	impl_->unsettled.clear();
}

std::vector<OpenGesture> Gestures::openGestures() const {
	std::vector<OpenGesture> out;
	for (const auto& [id, g] : impl_->open) out.push_back(OpenGesture{id, Impl::keysOf(g), g.ending});
	return out;
}

std::vector<Gestures::OnNode> Gestures::onNode(const NodeName& node) const {
	std::vector<OnNode> out;
	for (const auto& [id, g] : impl_->open)
		for (const GesturePair& pair : g.reach)
			if (pair.at.node == node) out.push_back(OnNode{id, pair.at.key, g.accepted});
	return out;
}

} // namespace ear
