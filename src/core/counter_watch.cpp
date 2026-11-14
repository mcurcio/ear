// The counters' one pacer (ear/core/counter_watch.hpp; docs/engine.md#reporting-the-counters).
#include "ear/core/counter_watch.hpp"

#include <algorithm>
#include <utility>

namespace ear {
namespace {

/// What a node's record reports: the two counters that say the node itself fell behind.
bool sameMisses(const NodeCounts& a, const NodeCounts& b) noexcept {
	return a.missedCycles == b.missedCycles && a.overruns == b.overruns;
}

/// A loss the wire reports: `cycles` moves every cycle and is never a change on its own.
bool sameLosses(const GraphCounts& a, const GraphCounts& b) noexcept {
	return a.cutCycles == b.cutCycles && a.stalls == b.stalls;
}

NodeCounts minus(const NodeCounts& a, const NodeCounts& b) noexcept {
	return NodeCounts{.missedCycles = a.missedCycles - b.missedCycles,
	                  .overruns = a.overruns - b.overruns,
	                  .repositions = a.repositions - b.repositions,
	                  .driverChanges = a.driverChanges - b.driverChanges};
}

/// A ring companion's one counter: a sink's dropped frames, a source's shortfall; none for an engine companion.
std::optional<std::uint64_t> counterOf(const CompanionView& companion) noexcept {
	if (companion.dropped) return companion.dropped;
	return companion.shortfall;
}

} // namespace

CounterWatch::CounterWatch(const Timers& timers, const GraphCounterSource& graph, const NodeCounterSource& nodes,
                           AudioFormat format)
	: timers_(timers), graph_(graph), nodes_(nodes), format_(format) {}

void CounterWatch::subscribe(CounterListener& listener) { listeners_.push_back(&listener); }

void CounterWatch::subscribeCompanions(NodeCompanionsListener& listener) { companionListeners_.push_back(&listener); }

std::uint64_t CounterWatch::lostMs(std::uint64_t cutCycles) const noexcept {
	return GraphCounts{.cutCycles = cutCycles}.lostMs(FrameClock(format_.rate), format_.maxBlock);
}

template <class Counts> bool CounterWatch::due(Paced<Counts>& paced, bool moved, bool settled,
                                               std::chrono::milliseconds pace, Clock::time_point now) {
	if (!moved) {
		paced.movingSince.reset();
		return false;
	}
	if (!paced.movingSince) paced.movingSince = now;
	const bool held = settled || now - *paced.movingSince >= holdBound;
	const bool paced_ = !paced.recordedAt || now - *paced.recordedAt >= pace;
	return held && paced_;
}

void CounterWatch::recordGraph(const GraphCounts& now, std::optional<Clock::time_point> at) {
	const GraphCounts& last = graphRecord_.recorded;
	const GraphCounts since{.cycles = now.cycles - last.cycles,
	                        .cutCycles = now.cutCycles - last.cutCycles,
	                        .stalls = now.stalls - last.stalls};
	const std::uint64_t ms = lostMs(now.cutCycles) - lostMs(last.cutCycles);
	for (CounterListener* listener : listeners_) listener->graphLostCycles(now, since, ms);
	graphRecord_.recorded = now;
	if (at) graphRecord_.recordedAt = at;
	graphRecord_.movingSince.reset();
}

void CounterWatch::recordNode(const NodeName& name, Span& span, std::optional<Clock::time_point> at) {
	const NodeCounts since = minus(span.read, span.record.recorded);
	for (CounterListener* listener : listeners_) listener->nodeMissedCycles(name, span.read, since);
	span.record.recorded = span.read;
	if (at) span.record.recordedAt = at;
	span.record.movingSince.reset();
}

void CounterWatch::recordCompanion(const NodeName& node, const CompanionView& companion, std::uint64_t total,
                                   Paced<std::uint64_t>& paced, std::optional<Clock::time_point> at) {
	for (CounterListener* listener : listeners_)
		listener->companionFramesLost(node, companion, total - paced.recorded, total);
	paced.recorded = total;
	if (at) paced.recordedAt = at;
	paced.movingSince.reset();
}

void CounterWatch::tickCompanions(Span& span, std::span<const CompanionView> companions, Clock::time_point now) {
	// The records: each counter once it held still for a beat, at most once per companionDropDiagnosticPace.
	for (const CompanionView& companion : companions) {
		const auto total = counterOf(companion);
		if (!total) continue;
		Paced<std::uint64_t>& paced = span.companionRecords[companion.name];
		if (due(paced, *total != paced.recorded, *total == paced.previous, companionDropDiagnosticPace, now))
			recordCompanion(span.name, companion, *total, paced, now);
		paced.previous = *total;
	}
	// The wire's sample: the whole list when a counter moved from what the last list told, at most once per
	// companionCounterPace, for the node a client holds.
	if (span.superseded) return;
	const bool moved = std::ranges::any_of(companions, [&](const CompanionView& companion) {
		const auto total = counterOf(companion);
		if (!total) return false;
		const auto sent = span.companionsSent.find(companion.name);
		return *total != (sent == span.companionsSent.end() ? 0 : sent->second);
	});
	if (!moved || (span.companionsSentAt && now - *span.companionsSentAt < companionCounterPace)) return;
	for (const CompanionView& companion : companions)
		if (const auto total = counterOf(companion)) span.companionsSent[companion.name] = *total;
	span.companionsSentAt = now;
	for (NodeCompanionsListener* listener : companionListeners_) listener->companionsChanged(span.name, companions);
}

void CounterWatch::tick() {
	const Clock::time_point now = timers_.now();
	const GraphCounts graph = graph_.graphCounts();
	for (auto& [id, span] : spans_)
		if (const auto counts = nodes_.spanCounts(id)) span.read = *counts;

	// The wire's sample: every loss moved since the last, whole, at most once per wirePace. A superseded node is one
	// no client holds.
	bool moved = !sameLosses(graph, graphSent_);
	std::vector<NamedCounts> changed;
	for (const auto& [id, span] : spans_)
		if (!span.superseded && span.read != span.sent) changed.push_back({span.name, span.read});
	std::ranges::sort(changed, {}, &NamedCounts::name);
	moved = moved || !changed.empty();
	if (moved && (!sentAt_ || now - *sentAt_ >= wirePace)) {
		for (CounterListener* listener : listeners_) listener->countersChanged(graph, lostMs(graph.cutCycles), changed);
		graphSent_ = graph;
		for (auto& [id, span] : spans_) span.sent = span.read;
		sentAt_ = now;
	}

	// The records: what moved since each was last written, once it held still for a tick.
	if (due(graphRecord_, graph.cutCycles != graphRecord_.recorded.cutCycles,
	        graph.cutCycles == graphRecord_.previous.cutCycles, graphRecordPace, now))
		recordGraph(graph, now);
	graphRecord_.previous = graph;
	for (auto& [id, span] : spans_) {
		if (due(span.record, !sameMisses(span.read, span.record.recorded), sameMisses(span.read, span.record.previous),
		        nodeRecordPace, now))
			recordNode(span.name, span, now);
		span.record.previous = span.read;
	}
	for (auto& [id, span] : spans_) tickCompanions(span, nodes_.spanCompanions(id), now);
}

void CounterWatch::flush() {
	const GraphCounts graph = graph_.graphCounts();
	if (graph.cutCycles != graphRecord_.recorded.cutCycles) recordGraph(graph, std::nullopt);
	for (auto& [id, span] : spans_) {
		if (const auto counts = nodes_.spanCounts(id)) span.read = *counts;
		if (!sameMisses(span.read, span.record.recorded)) recordNode(span.name, span, std::nullopt);
		for (const CompanionView& companion : nodes_.spanCompanions(id)) {
			const auto total = counterOf(companion);
			Paced<std::uint64_t>& paced = span.companionRecords[companion.name];
			if (total && *total != paced.recorded) recordCompanion(span.name, companion, *total, paced, std::nullopt);
		}
	}
}

void CounterWatch::countsBegan(const NodeName& node, std::uint64_t span) {
	// An older node of the name is superseded: its span stays open, read and recorded, until it ends.
	for (auto& [id, older] : spans_)
		if (older.name == node) older.superseded = true;
	spans_.insert_or_assign(span, Span{.name = node});
}

void CounterWatch::countsEnded(const NodeName& node, std::uint64_t span, const NodeCounts& total) {
	static_cast<void>(node);
	const auto ended = spans_.find(span);
	if (ended == spans_.end()) return;
	ended->second.read = total;
	if (!sameMisses(total, ended->second.record.recorded)) recordNode(ended->second.name, ended->second, std::nullopt);
	spans_.erase(ended);
}

void CounterWatch::companionsEnded(const NodeName& node, std::uint64_t span, std::span<const CompanionView> finals) {
	const auto ended = spans_.find(span);
	if (ended == spans_.end()) return;
	Span& open = ended->second;
	// What moved since each companion's last record is written now, pace and hold waived; a later configuration's
	// companions of the same names count from zero, in the records and in what the wire last named alike.
	for (const CompanionView& companion : finals) {
		Paced<std::uint64_t>& paced = open.companionRecords[companion.name];
		if (const auto total = counterOf(companion); total && *total != paced.recorded)
			for (CounterListener* listener : listeners_)
				listener->companionFramesLost(node, companion, *total - paced.recorded, *total);
		open.companionRecords.erase(companion.name);
		open.companionsSent.erase(companion.name);
	}
}

} // namespace ear
