// The graph file keeper's main-loop side (main_loop_side.hpp): each offer is planned against the
// desired graph and committed in the same turn, after the listeners heard of the file change, so a
// file change always comes before the graph events it causes (design rule 3).
#include "main_loop_side.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ear::config {
namespace {

template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };

std::optional<control::TextPlace> at(const std::vector<control::TextPlace>& places, std::size_t index) {
	if (index >= places.size()) return std::nullopt;
	return places[index];
}

template <class Match>
std::optional<control::TextPlace> nthNode(const GraphSpec& wanted, const Placements& placements, std::size_t nth, Match match) {
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

std::optional<control::TextPlace> placeOf(const CommandError& error, const GraphSpec& wanted, const Placements& placements) {
	auto named = [&](const NodeName& node) {
		return nthNode(wanted, placements, 0, [&](const NodeSpec& n) { return n.name == node; });
	};
	return std::visit(
	    Overloaded{
	        [&](const UnknownNodeType& e) {
		        return nthNode(wanted, placements, 0, [&](const NodeSpec& n) { return n.type == e.type; });
	        },
	        [&](const DuplicateNode& e) {
		        return nthNode(wanted, placements, 1, [&](const NodeSpec& n) { return n.name == e.node; });
	        },
	        [&](const UnknownNode& e) {
		        return firstRoute(wanted, placements, [&](const Route& r) { return r.from.node == e.node || r.to.node == e.node; });
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
	            requires requires(const E& x) { x.node; } { return named(e.node); },
	    },
	    error);
}

MainLoopSide::MainLoopSide(DesiredGraph& graph, const GraphReader& reader) : graph_(graph), reader_(reader) {}

void MainLoopSide::subscribe(control::GraphFileListener& listener) { listeners_.push_back(&listener); }

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
	// Planned in this turn, so the graph cannot have closed or changed since: commit cannot refuse.
	auto committed = graph_.commit(std::move(*plan));
	if (!committed) return {Verdict::stopping, std::unexpected(committed.error())};
	return {Verdict::accepted, {}};
}

void MainLoopSide::announce(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits) {
	tell(std::move(file), cause, droppedEdits);
}

control::GraphFile MainLoopSide::graphFile(FileState file) const {
	control::GraphFile out{std::move(file.version), std::move(file.text), std::move(file.diagnostics), running_,
	                       std::nullopt, std::move(file.pendingRestart), std::move(file.writeFailed)};
	if (refusal_ && refusal_->version == out.version) out.refusal = refusal_->refusal;
	return out;
}

GraphView MainLoopSide::graph() const { return reader_.read(); }

void MainLoopSide::written(const std::string& version) {
	for (auto* listener : listeners_) listener->graphFileWritten(version);
}

void MainLoopSide::tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits) {
	const control::GraphFileChanged change{graphFile(std::move(file)), cause, droppedEdits};
	for (auto* listener : listeners_) listener->graphFileChanged(change);
}

} // namespace ear::config
