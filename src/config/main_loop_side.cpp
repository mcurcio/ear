// The graph file keeper's main-loop side (main_loop_side.hpp): each offer is planned against the
// desired graph and committed in the same turn, after the listeners heard of the file change, so a
// file change always comes before the graph events it causes (design rule 3).
#include "main_loop_side.hpp"

#include <algorithm>
#include <format>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace ear::config {
namespace {

template <class... Fs> struct Overloaded : Fs... {
	using Fs::operator()...;
};

std::optional<control::TextPlace> at(const std::vector<control::TextPlace>& places, std::size_t index) {
	if (index >= places.size()) return std::nullopt;
	return places[index];
}

template <class Match> std::optional<control::TextPlace> nthNode(const GraphSpec& wanted, const Placements& placements,
                                                                 std::size_t nth, Match match) {
	for (std::size_t i = 0; i < wanted.nodes.size(); ++i)
		if (match(wanted.nodes[i]) && nth-- == 0) return at(placements.nodes, i);
	return std::nullopt;
}

template <class Match>
std::optional<control::TextPlace> firstRoute(const GraphSpec& wanted, const Placements& placements, Match match) {
	for (std::size_t i = 0; i < wanted.routes.size(); ++i)
		if (match(wanted.routes[i])) return at(placements.routes, i);
	return std::nullopt;
}

} // namespace

std::optional<control::TextPlace> placeOf(const CommandError& error, const GraphSpec& wanted,
                                          const Placements& placements) {
	auto named = [&](const NodeName& node) {
		return nthNode(wanted, placements, 0, [&](const NodeSpec& n) { return n.name == node; });
	};
	return std::visit(Overloaded{
		[&](const UnknownNodeType& e) {
			return nthNode(wanted, placements, 0, [&](const NodeSpec& n) { return n.type == e.type; });
		},
		[&](const DuplicateNode& e) {
			return nthNode(wanted, placements, 1, [&](const NodeSpec& n) { return n.name == e.node; });
		},
		[&](const UnknownNode& e) {
			return firstRoute(wanted, placements,
		                      [&](const Route& r) { return r.from.node == e.node || r.to.node == e.node; });
		},
		[&](const UnknownPort& e) {
			return firstRoute(wanted, placements, [&](const Route& r) { return r.from == e.port || r.to == e.port; });
		},
		[&](const WrongDirection& e) {
			return firstRoute(wanted, placements, [&](const Route& r) { return r == e.route; });
		},
		[&](const UnknownRoute& e) {
			return firstRoute(wanted, placements, [&](const Route& r) { return r == e.route; });
		},
		[](const Stopping&) { return std::optional<control::TextPlace>{}; },
		[](const UnknownGesture&) { return std::optional<control::TextPlace>{}; },
		// Every other refusal names one node (InvalidName, InvalidSettings, ...): its line.
		[&]<class E>(const E& e) -> std::optional<control::TextPlace>
			requires requires(const E& x) { x.node; }
		{ return named(e.node); },
		},
		error);
}

control::CommandResult resultOf(const NodeName& node, const LifecycleReply& reply) {
	using control::AppliedKind;
	if (reply) {
		switch (*reply) {
		case LifecycleApplied::start:
			return control::Applied{AppliedKind::start};
		case LifecycleApplied::stop:
			return control::Applied{AppliedKind::stop};
		case LifecycleApplied::restart:
			return control::Applied{AppliedKind::restart};
		case LifecycleApplied::retry:
			return control::Applied{AppliedKind::retry};
		case LifecycleApplied::unchanged:
			return control::Applied{AppliedKind::unchanged};
		}
		return control::Applied{AppliedKind::unchanged};
	}
	using Code = LifecycleRefusal::Code;
	using Outcome = std::expected<void, CommandError>;
	switch (reply.error().code) {
	case Code::unknownNode:
		return Outcome{std::unexpected(CommandError{UnknownNode{node}})};
	case Code::nodeDisabled:
		return control::ControlRefusal{control::NodeDisabled{node}};
	case Code::notRestartable:
		return control::ControlRefusal{
			control::NotRestartable{node, reply.error().reason == LifecycleRefusal::Reason::blockLost
		                                      ? control::NotRestartable::Reason::blockLost
		                                      : control::NotRestartable::Reason::notStarted}};
	case Code::stopping:
		break;
	}
	return Outcome{std::unexpected(CommandError{Stopping{}})};
}

control::ActionRecord recordOf(control::LifecycleVerb verb, const NodeName& node, const LifecycleReply& reply) {
	control::ActionRecord record{node, std::string(control::verbName(verb)), "ok", std::nullopt};
	if (reply) return record;
	const control::CommandResult result = resultOf(node, reply);
	if (const auto* refusal = std::get_if<control::ControlRefusal>(&result)) {
		record.result = std::string(control::controlRefusalCode(*refusal));
		if (const auto* lost = std::get_if<control::NotRestartable>(refusal))
			record.detail = std::string(control::reasonName(lost->reason));
	} else {
		record.result = std::string(errorCode(std::get<std::expected<void, CommandError>>(result).error()));
	}
	return record;
}

control::CommandResult resultOf(const NodeName& node, std::string_view action, const ActionReply& reply) {
	using Outcome = std::expected<void, CommandError>;
	if (reply) return Outcome{};
	const ActionRefusal& refusal = reply.error();
	const std::string named(action);
	using Code = ActionRefusal::Code;
	switch (refusal.code) {
	case Code::unknownNode:
		return Outcome{std::unexpected(CommandError{UnknownNode{node}})};
	case Code::unknownAction:
		return control::ControlRefusal{control::UnknownAction{node, named}};
	case Code::invalidArgs:
		return control::ControlRefusal{control::InvalidArgs{node, named, refusal.detail, refusal.key}};
	case Code::notRunning:
		return control::ControlRefusal{control::NotRunning{node, refusal.phase.value_or(NodePhase::added)}};
	case Code::restarted:
		return control::ControlRefusal{control::Restarted{node}};
	case Code::failed:
		return control::ControlRefusal{control::ActionFailed{node, named, refusal.detail}};
	case Code::timeout:
		return control::ControlRefusal{control::ActionTimeout{node, named}};
	case Code::stopping:
		break;
	}
	return Outcome{std::unexpected(CommandError{Stopping{}})};
}

control::ActionRecord recordOf(const NodeName& node, std::string_view action, const ActionReply& reply) {
	control::ActionRecord record{node, std::string(action), "ok", std::nullopt};
	if (reply) return record;
	const control::CommandResult result = resultOf(node, action, reply);
	if (const auto* refusal = std::get_if<control::ControlRefusal>(&result)) {
		record.result = std::string(control::controlRefusalCode(*refusal));
		if (const auto* invalid = std::get_if<control::InvalidArgs>(refusal))
			record.detail = std::string(control::argsWords(*invalid));
		if (const auto* failed = std::get_if<control::ActionFailed>(refusal)) record.detail = failed->detail;
		if (const auto* notRunning = std::get_if<control::NotRunning>(refusal))
			record.detail = std::string(phaseName(notRunning->phase));
	} else {
		record.result = std::string(errorCode(std::get<std::expected<void, CommandError>>(result).error()));
	}
	return record;
}

MainLoopSide::MainLoopSide(DesiredGraph& graph, const GraphReader& reader, NodeOperations& operations)
	: graph_(graph), reader_(reader), operations_(operations) {}

void MainLoopSide::subscribe(control::GraphFileListener& listener) { listeners_.push_back(&listener); }

void MainLoopSide::subscribeActions(control::ActionListener& listener) { actionListeners_.push_back(&listener); }

void MainLoopSide::tellAction(const control::ActionRecord& record) {
	for (auto* listener : actionListeners_) listener->actionAnswered(record);
}

control::CommandResult MainLoopSide::operate(control::LifecycleVerb verb, const NodeName& node, bool stopping) {
	if (stopping) {
		const LifecycleReply refused = std::unexpected(LifecycleRefusal{LifecycleRefusal::Code::stopping});
		tellAction(recordOf(verb, node, refused));
		return resultOf(node, refused);
	}
	bool told = false;
	auto judged = [&](const LifecycleReply& reply) {
		told = true;
		tellAction(recordOf(verb, node, reply));
	};
	LifecycleReply reply = std::unexpected(LifecycleRefusal{LifecycleRefusal::Code::unknownNode});
	switch (verb) {
	case control::LifecycleVerb::start:
		reply = operations_.start(node, judged);
		break;
	case control::LifecycleVerb::stop:
		reply = operations_.stop(node, judged);
		break;
	case control::LifecycleVerb::restart:
		reply = operations_.restart(node, judged);
		break;
	}
	if (!told) tellAction(recordOf(verb, node, reply)); // an implementation that never called back
	return resultOf(node, reply);
}

void MainLoopSide::answerActions(NodeActions& actions) { actions_ = &actions; }

void MainLoopSide::invoke(const NodeName& node, const std::string& action, const NodeSettings& args, bool stopping,
                          ActionResult result) {
	if (stopping) {
		const ActionReply refused = std::unexpected(ActionRefusal{ActionRefusal::Code::stopping});
		tellAction(recordOf(node, action, refused));
		return result(resultOf(node, action, refused));
	}
	if (actions_ == nullptr) throw std::logic_error("node.invoke with no NodeActions given: a wiring bug");
	actions_->invoke(node, action, args,
	                 [this, node, action, result = std::move(result)](const ActionReply& reply) mutable {
						 tellAction(recordOf(node, action, reply));
						 result(resultOf(node, action, reply));
					 });
}

namespace {

control::GestureRefused refusedWith(std::string_view code, std::string message,
                                    nlohmann::json data = nlohmann::json::object()) {
	return control::GestureRefused{std::string(code), std::move(message), std::move(data)};
}

std::string_view changeName(setting::Change change) {
	switch (change) {
	case setting::Change::live:
		return "live";
	case setting::Change::inPlace:
		return "inPlace";
	case setting::Change::restart:
		break;
	}
	return "restart";
}

bool gesturable(const ReachRow& row) {
	return row.change == setting::Change::live && row.ramped && row.decimal && row.member.has_value();
}

/// The abort reason a related change gives an open gesture, by the kind of offer (docs/control-port.md#gestures-and-the-graph-file);
/// none for an edit that must be refused instead (a held value changed), or a change not related.
std::optional<std::string> abortFor(OfferKind kind, Relation relation) {
	if (relation == Relation::none) return std::nullopt;
	if (kind == OfferKind::disk) return "hand-edit";
	if (kind == OfferKind::fileSet) return "file-saved";
	if (relation == Relation::nodeRemoved) return "node-removed";
	if (relation == Relation::keyRemoved) return "key-removed";
	if (relation == Relation::reach) return "edited";
	return std::nullopt; // a value: refused parameter-held
}

} // namespace

void MainLoopSide::holdGestures(Gestures& gestures) { gestures_ = &gestures; }

control::GestureAnswer MainLoopSide::beginGesture(SessionToken session, const control::BeginGesture& begin,
                                                  bool stopping) {
	if (stopping || gestures_ == nullptr)
		return control::CommandResult{std::expected<void, CommandError>{std::unexpected(Stopping{})}};
	if (newestHasErrors_) return control::CommandResult{control::FileRefusal{control::FileInvalid{}}};
	if (newestRefused_)
		return refusedWith(control::fileRefusedCode, "file-refused: the graph refused the newest text of ear.conf");
	const GestureKey asked{begin.node, begin.key};
	const bool nodeThere = runningReach_ && std::ranges::any_of(runningReach_->entries(), [&](const ReachEntry& entry) {
							   return entry.at.node == begin.node;
						   });
	if (!nodeThere)
		return control::CommandResult{std::expected<void, CommandError>{std::unexpected(UnknownNode{begin.node})}};
	const ReachEntry* own = runningReach_->find(asked);
	if (own == nullptr)
		return refusedWith(control::notLiveCode,
		                   std::format("not-live {}: {} is not a setting of its type", begin.node, begin.key),
		                   {{"key", begin.key}});
	if (!gesturable(own->row))
		return refusedWith(control::notLiveCode,
		                   std::format("not-live {}: {} is not a live ramped decimal", begin.node, begin.key),
		                   {{"key", begin.key}, {"change", std::string(changeName(own->row.change))}});
	GestureOpen open{session, {}, own->row.low, own->row.high, own->row.decimals, own->value.get<double>()};
	nlohmann::json with = nlohmann::json::array();
	for (const GestureKey& at : runningReach_->reachOf(asked)) {
		const ReachEntry* entry = runningReach_->find(at);
		std::string why;
		if (!gesturable(entry->row)) why = "kind";
		else if (entry->row.unit != own->row.unit) why = "unit";
		else {
			open.low = std::max(open.low, entry->row.low);
			open.high = std::min(open.high, entry->row.high);
			open.decimals = std::min(open.decimals, entry->row.decimals);
			if (open.low > open.high) why = "range";
		}
		if (!why.empty()) with.push_back({{"name", at.node}, {"key", at.key}, {"why", why}});
		else open.reach.push_back(GesturePair{at, *entry->row.member});
	}
	if (!with.empty())
		return refusedWith(
			control::reachMismatchCode,
			std::format("reach-mismatch {}: {} shares its definition with rows a gesture cannot move together",
		                begin.node, begin.key),
			{{"line", own->place.line}, {"column", own->place.column}, {"with", std::move(with)}});
	for (const GesturePair& pair : open.reach)
		if (const auto held = gestures_->heldAt(pair.at))
			return control::CommandResult{std::expected<void, CommandError>{std::unexpected(ParameterHeld{
				pair.at.node, pair.at.key, held->gesture, static_cast<std::uint64_t>(held->silent.count())})}};
	control::GestureBegun begun{"", open.committed, {}, open.low, open.high, open.decimals};
	for (const GesturePair& pair : open.reach) begun.reach.push_back(pair.at);
	begun.gesture = gestures_->open(std::move(open));
	return begun;
}

bool MainLoopSide::stillOpen(const std::string& gesture) const {
	return gestures_ != nullptr && std::ranges::any_of(gestures_->openGestures(), [&](const OpenGesture& open) {
			   return open.gesture == gesture;
		   });
}

control::CommandResult MainLoopSide::closeGesture(const std::string& gesture, double value) {
	// Aborted after its end was read (a related change, a cancel by its id): the abort was told, so nothing closes.
	if (!stillOpen(gesture))
		return control::CommandResult{std::expected<void, CommandError>{std::unexpected(UnknownGesture{gesture})}};
	gestures_->close(gesture, value);
	return control::GestureEnded{value};
}

/// The open gestures `offer` relates to, each with its abort reason; a held value it changes refuses it
/// (docs/control-port.md#gestures-and-the-graph-file). Nothing for a dry run.
std::expected<std::vector<std::pair<std::string, std::string>>, CommandError>
MainLoopSide::relatedTo(const Offer& offer) const {
	std::vector<std::pair<std::string, std::string>> aborts;
	if (gestures_ == nullptr || offer.dryRun) return aborts;
	for (const OpenGesture& open : gestures_->openGestures()) {
		if (offer.gesture && *offer.gesture == open.gesture) continue; // its own end
		Relation relation = Relation::none;
		if (!offer.file.reach) relation = Relation::value; // a text with errors: no end could commit onto it
		else if (runningReach_) relation = relationOf(*runningReach_, *offer.file.reach, open.reach);
		if (relation == Relation::none) continue;
		if (const auto reason = abortFor(offer.kind, relation)) {
			aborts.emplace_back(open.gesture, *reason);
			continue;
		}
		// An edit changing a held value: refused, naming the first held key it changes.
		for (const GestureKey& at : open.reach) {
			const ReachEntry* was = runningReach_->find(at);
			const ReachEntry* now = offer.file.reach->find(at);
			if (was && now && was->value != now->value)
				if (const auto held = gestures_->heldAt(at))
					return std::unexpected(ParameterHeld{at.node, at.key, held->gesture,
					                                     static_cast<std::uint64_t>(held->silent.count())});
		}
	}
	return aborts;
}

void MainLoopSide::abortAll(const std::string& reason) {
	if (gestures_ == nullptr) return;
	for (const OpenGesture& open : gestures_->openGestures()) gestures_->abort(open.gesture, reason);
}

void MainLoopSide::newest(const FileState& file) {
	newestHasErrors_ = std::ranges::any_of(
		file.diagnostics, [](const control::Diagnostic& d) { return d.severity == control::Severity::error; });
}

Judged MainLoopSide::offer(Offer offer) {
	// An end whose gesture was aborted while the end waited for the keeper (docs/control-port.md#gestures-and-the-graph-file):
	// the abort won and was told, so its value never commits over what aborted it.
	if (offer.kind == OfferKind::gestureEnd && offer.gesture && !stillOpen(*offer.gesture))
		return {Verdict::refused, std::unexpected(CommandError{UnknownGesture{*offer.gesture}})};
	auto related = relatedTo(offer);
	if (!related) return {Verdict::refused, std::unexpected(related.error())};
	auto plan = graph_.plan(offer.wanted.nodes, offer.wanted.routes);
	if (!plan) {
		if (std::holds_alternative<Stopping>(plan.error())) return {Verdict::stopping, std::unexpected(plan.error())};
		if (offer.cause == control::FileChangeCause::disk && !offer.dryRun) {
			// No end could commit onto a text the graph refused: every gesture is aborted, back to what it runs.
			abortAll("hand-edit");
			newest(offer.file);
			newestRefused_ = true;
			refusal_ = Recorded{offer.file.version,
			                    control::Refusal{plan.error(), placeOf(plan.error(), offer.wanted, offer.placements)}};
			tell(std::move(offer.file), offer.cause, offer.droppedEdits);
		}
		return {Verdict::refused, std::unexpected(plan.error())};
	}
	std::optional<NodeChange> change = offer.answerFor ? plan->change(*offer.answerFor) : std::nullopt;
	std::vector<control::NodeKey> also;
	if (offer.answerFor)
		for (const auto& [name, outcome] : plan->outcomes()) {
			if (name == *offer.answerFor) continue;
			const std::optional<NodeChange> other = plan->change(name);
			if (!other) continue;
			std::vector<std::string> keys = other->keys;
			for (const Retuned& retune : other->retuned) keys.push_back(retune.key);
			std::ranges::sort(keys);
			for (std::string& key : keys) also.push_back(control::NodeKey{name, std::move(key)});
		}
	// A dry run is advice: the plan's answer, and nothing more (docs/control-port.md#nodeset-and-dry-runs).
	if (offer.dryRun) return {Verdict::accepted, {}, std::move(change), std::move(also)};
	// Aborted before the change is told, so a client hears the abort first; the slots settle after the commit, last in
	// the turn, so the audio ramps once, to the new value (docs/control-port.md#gestures-and-the-graph-file).
	for (const auto& [gesture, reason] : *related)
		gestures_->abort(gesture, reason, reason == "hand-edit" || reason == "file-saved" || reason == "edited", false);
	// A retune of keys no gesture holds can still cross a hand on its node a check binds (lowHz below a held highHz):
	// those gestures are aborted crossed and set back first (docs/architecture.md#gestures).
	if (gestures_ != nullptr)
		for (const auto& [name, outcome] : plan->outcomes()) {
			const std::optional<NodeChange> moved = plan->change(name);
			if (!moved || moved->retuned.empty()) continue;
			std::vector<LiveWrite> writes;
			for (const Retuned& retune : moved->retuned)
				if (retune.member) writes.push_back(LiveWrite{*retune.member, retune.bits});
			gestures_->abortCrossing(name, writes,
			                         offer.kind == OfferKind::gestureEnd && offer.gesture ? *offer.gesture : "");
		}
	running_ = offer.file.version;
	runningReach_ = offer.file.reach;
	newest(offer.file);
	newestRefused_ = false;
	const std::optional<std::string> gesture = offer.kind == OfferKind::gestureEnd ? offer.gesture : std::nullopt;
	tell(std::move(offer.file), offer.cause, offer.droppedEdits, gesture, offer.gestureReason);
	// Planned in this turn, so the graph cannot have closed or changed since: commit cannot refuse.
	auto committed = graph_.commit(std::move(*plan));
	if (!committed) return {Verdict::stopping, std::unexpected(committed.error())};
	if (gestures_ != nullptr) {
		gestures_->settle();
		if (gesture) gestures_->close(*gesture, offer.gestureValue);
	}
	return {Verdict::accepted, {}, std::move(change), std::move(also)};
}

void MainLoopSide::announce(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits) {
	newest(file);
	if (newestHasErrors_ && cause == control::FileChangeCause::disk) abortAll("hand-edit");
	tell(std::move(file), cause, droppedEdits);
}

control::GraphFile MainLoopSide::graphFile(FileState file) const {
	control::GraphFile out{std::move(file.version),
	                       std::move(file.text),
	                       std::move(file.diagnostics),
	                       running_,
	                       std::nullopt,
	                       std::move(file.pendingRestart),
	                       std::move(file.writeFailed),
	                       std::move(file.nodeTexts)};
	if (refusal_ && refusal_->version == out.version) out.refusal = refusal_->refusal;
	return out;
}

GraphView MainLoopSide::graph() const {
	GraphView view = reader_.read();
	for (const auto* stamp : stamps_) stamp->stamp(view);
	return view;
}

void MainLoopSide::stampSnapshots(const control::SnapshotStamp& stamp) { stamps_.push_back(&stamp); }

void MainLoopSide::written(const std::string& version) {
	for (auto* listener : listeners_) listener->graphFileWritten(version);
}

void MainLoopSide::tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits,
                        std::optional<std::string> gesture, std::string gestureReason) {
	const control::GraphFileChanged change{graphFile(std::move(file)), cause, droppedEdits, std::move(gesture),
	                                       std::move(gestureReason)};
	for (auto* listener : listeners_) listener->graphFileChanged(change);
}

} // namespace ear::config
