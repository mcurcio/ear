// The counters' one pacer (ear/core/counter_watch.hpp; docs/engine.md#reporting-the-counters).
#include "ear/core/counter_watch.hpp"

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

} // namespace

CounterWatch::CounterWatch(const Timers& timers, const GraphCounterSource& graph, const NodeCounterSource& nodes,
                           AudioFormat format)
	: timers_(timers), graph_(graph), nodes_(nodes), format_(format) {}

void CounterWatch::subscribe(CounterListener& listener) { listeners_.push_back(&listener); }

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

void CounterWatch::tick() {
	const Clock::time_point now = timers_.now();
	const GraphCounts graph = graph_.graphCounts();
	for (auto& [name, span] : spans_)
		if (const auto counts = nodes_.counts(name)) span.read = *counts;

	// The wire's sample: every loss moved since the last, whole, at most once per wirePace.
	bool moved = !sameLosses(graph, graphSent_);
	std::vector<NamedCounts> changed;
	for (const auto& [name, span] : spans_)
		if (span.read != span.sent) changed.push_back({name, span.read});
	moved = moved || !changed.empty();
	if (moved && (!sentAt_ || now - *sentAt_ >= wirePace)) {
		for (CounterListener* listener : listeners_) listener->countersChanged(graph, lostMs(graph.cutCycles), changed);
		graphSent_ = graph;
		for (auto& [name, span] : spans_) span.sent = span.read;
		sentAt_ = now;
	}

	// The records: what moved since each was last written, once it held still for a tick.
	if (due(graphRecord_, graph.cutCycles != graphRecord_.recorded.cutCycles,
	        graph.cutCycles == graphRecord_.previous.cutCycles, graphRecordPace, now))
		recordGraph(graph, now);
	graphRecord_.previous = graph;
	for (auto& [name, span] : spans_) {
		if (due(span.record, !sameMisses(span.read, span.record.recorded), sameMisses(span.read, span.record.previous),
		        nodeRecordPace, now))
			recordNode(name, span, now);
		span.record.previous = span.read;
	}
}

void CounterWatch::flush() {
	const GraphCounts graph = graph_.graphCounts();
	if (graph.cutCycles != graphRecord_.recorded.cutCycles) recordGraph(graph, std::nullopt);
	for (auto& [name, span] : spans_) {
		if (const auto counts = nodes_.counts(name)) span.read = *counts;
		if (!sameMisses(span.read, span.record.recorded)) recordNode(name, span, std::nullopt);
	}
}

void CounterWatch::countsBegan(const NodeName& node) {
	// An older node of the name, superseded by this one: what it moved since its last record, as last read.
	if (const auto older = spans_.find(node); older != spans_.end()) {
		if (!sameMisses(older->second.read, older->second.record.recorded))
			recordNode(node, older->second, std::nullopt);
		spans_.erase(older);
	}
	spans_.emplace(node, Span{});
}

void CounterWatch::countsEnded(const NodeName& node, const NodeCounts& total) {
	const auto span = spans_.find(node);
	if (span == spans_.end()) return;
	span->second.read = total;
	if (!sameMisses(total, span->second.record.recorded)) recordNode(node, span->second, std::nullopt);
	spans_.erase(span);
}

} // namespace ear
