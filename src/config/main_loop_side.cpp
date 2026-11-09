// The graph file keeper's main-loop side (main_loop_side.hpp): each offer is planned against the
// desired graph and committed in the same turn, after the listeners heard of the file change, so a
// file change always comes before the graph events it causes (design rule 3).
#include "main_loop_side.hpp"

#include <algorithm>
#include <cstddef>
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

Judged MainLoopSide::offer(Offer offer) {
	auto plan = graph_.plan(offer.wanted.nodes, offer.wanted.routes);
	if (!plan) {
		if (std::holds_alternative<Stopping>(plan.error())) return {Verdict::stopping, std::unexpected(plan.error())};
		if (offer.cause == control::FileChangeCause::disk) {
			refusal_ = Recorded{offer.file.version,
			                    control::Refusal{plan.error(), placeOf(plan.error(), offer.wanted, offer.placements)}};
			tell(std::move(offer.file), offer.cause, offer.droppedEdits);
		}
		return {Verdict::refused, std::unexpected(plan.error())};
	}
	running_ = offer.file.version;
	tell(std::move(offer.file), offer.cause, offer.droppedEdits);
	auto outcomes = plan->outcomes();
	// Planned in this turn, so the graph cannot have closed or changed since: commit cannot refuse.
	auto committed = graph_.commit(std::move(*plan));
	if (!committed) return {Verdict::stopping, std::unexpected(committed.error())};
	return {Verdict::accepted, {}, std::move(outcomes)};
}

void MainLoopSide::announce(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits) {
	tell(std::move(file), cause, droppedEdits);
}

control::GraphFile MainLoopSide::graphFile(FileState file) const {
	control::GraphFile out{
		std::move(file.version),        std::move(file.text),       std::move(file.diagnostics), running_, std::nullopt,
		std::move(file.pendingRestart), std::move(file.writeFailed)};
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

void MainLoopSide::tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits) {
	const control::GraphFileChanged change{graphFile(std::move(file)), cause, droppedEdits};
	for (auto* listener : listeners_) listener->graphFileChanged(change);
}

} // namespace ear::config
