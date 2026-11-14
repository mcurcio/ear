// The diagnostics counters reported (docs/engine.md#reporting-the-counters): CounterWatch reads the graph's counters and
// every node's at the main loop's heartbeat, and tells its listeners what moved, at the paces the wire and standard
// output each take: the control port's counters.changed (docs/control-port.md#counters), and the records
// ear.graph-lost-cycles and ear.node-missed-cycles (docs/output-records.md#diagnostic); and the node's companions'
// counters, re-told in the node's companion list and as ear.sink-dropped and ear.source-shortfall. Main thread only: the data thread
// writes the counters and is never asked anything; this reads them through the lock-free reads the counters give.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
#include "ear/core/companions.hpp"
#include "ear/core/cycle_counters.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/timers.hpp"
namespace ear {

/// Where the graph's counters come from: the engine (ear_pipewire's Engine), or a test's fake.
class GraphCounterSource {
public:
	/// Any thread.
	virtual GraphCounts graphCounts() const noexcept = 0;

protected:
	~GraphCounterSource() = default;
};

/// Where a node's counters come from: NodeHost (NodeHost::counts), or a test's fake.
class NodeCounterSource {
public:
	/// Main thread. The newest node of this name not yet gone, from its add; none when no such node is left.
	virtual std::optional<NodeCounts> counts(const NodeName& node) const = 0;
	/// Main thread. The counts of this span (NodeCountsListener), from its begin; none once it ended.
	virtual std::optional<NodeCounts> spanCounts(std::uint64_t span) const = 0;
	/// Main thread. The companions of this span's newest node not yet gone, as NodeHost::companionsOf lists them (a
	/// sink's `dropped` and a source's `shortfall` read now); empty once it ended or while it has none.
	virtual std::vector<CompanionView> spanCompanions(std::uint64_t span) const = 0;

protected:
	~NodeCounterSource() = default;
};

/// Told where a node's counts begin and end (docs/engine.md#reporting-the-counters), by NodeHost, which alone knows a
/// node's life. A span is one node's counts from its add to its end, keyed by the serial of the node that began it; a
/// port-caused replace carries its older node's span on. Two spans of a name are open at once while an older node of
/// the name, superseded by a newer one, still closes. Main thread.
class NodeCountsListener {
public:
	/// Span `span` of this name begins counting from zero: a node added, or replacing an older node of the name by
	/// another type, or by a removal and an add. Not told for a port-caused replace, which goes on in the older's span.
	virtual void countsBegan(const NodeName& node, std::uint64_t span) = 0;
	/// The span's last node is gone, these its final counts, which nothing answers after. Told just before its nodeGone
	/// or supersededGone, a superseded node's too; not for the older node of a port-caused replace, whose span goes on.
	virtual void countsEnded(const NodeName& node, std::uint64_t span, const NodeCounts& total) = 0;
	/// A configuration of a node of this span ended (its teardown's step 5, or it was leaked): its sinks' and sources'
	/// final counts, which nothing answers after (a companion's counters run per configuration). Told before its node's
	/// countsEnded; for every configuration, a superseded node's and the older node of a port-caused replace's too.
	virtual void companionsEnded(const NodeName& node, std::uint64_t span, std::span<const CompanionView> finals) {
		static_cast<void>(node);
		static_cast<void>(span);
		static_cast<void>(finals);
	}

protected:
	~NodeCountsListener() = default;
};

/// A node's counters, by name.
struct NamedCounts {
	NodeName name;
	NodeCounts counts;
	friend bool operator==(const NamedCounts&, const NamedCounts&) = default;
};

/// What CounterWatch tells, each at its own pace (docs/engine.md#reporting-the-counters). Main thread. A listener
/// overrides what it reports: the gateway the first, standard output the other two.
class CounterListener {
public:
	/// The control port's sample, counters.changed: the graph's counters whole (`lostMs` encoded from its cutCycles),
	/// and the whole counters of each node whose counters differ from those this call last named for it. At most once a
	/// second, only when a loss moved.
	virtual void countersChanged(const GraphCounts& graph, std::uint64_t lostMs, std::span<const NamedCounts> nodes) {
		static_cast<void>(graph);
		static_cast<void>(lostMs);
		static_cast<void>(nodes);
	}
	/// ear.graph-lost-cycles: the graph's totals now, and what was cut since its last such record (`since`'s cycles
	/// unused, `sinceMs` the time those cuts lost).
	virtual void graphLostCycles(const GraphCounts& total, const GraphCounts& since, std::uint64_t sinceMs) {
		static_cast<void>(total);
		static_cast<void>(since);
		static_cast<void>(sinceMs);
	}
	/// ear.node-missed-cycles: the node's totals now, and what moved since its last such record.
	virtual void nodeMissedCycles(const NodeName& node, const NodeCounts& total, const NodeCounts& since) {
		static_cast<void>(node);
		static_cast<void>(total);
		static_cast<void>(since);
	}
	/// ear.sink-dropped (a sink: frames its ring had no room for) or ear.source-shortfall (a source: frames played as
	/// zeros while its writer was not idle): `frames` since the companion's last such record, `totalFrames` since its
	/// configuration made it.
	virtual void companionFramesLost(const NodeName& node, const CompanionView& companion, std::uint64_t frames,
	                                 std::uint64_t totalFrames) {
		static_cast<void>(node);
		static_cast<void>(companion);
		static_cast<void>(frames);
		static_cast<void>(totalFrames);
	}

protected:
	~CounterListener() = default;
};

/// The one pacer of the counters' reports (docs/engine.md#reporting-the-counters). Main thread. tick() is called at the
/// main loop's heartbeat, which already wakes the loop every 100 ms for the watchdog, so reading the counters adds no
/// wake-up and nothing runs on the data thread for it. Each tick reads the graph's counters and those of every node
/// whose span is open (opened by countsBegan, closed by countsEnded, a superseded node's kept until it is gone) and
/// tells:
/// - countersChanged when a loss (cutCycles, stalls, or any node counter: `cycles` moves every cycle and is never a
///   change on its own) moved since the last one, at most once per wirePace, naming only each name's newest span (the
///   node a client holds);
/// - graphLostCycles when cutCycles moved since the last such record and the cuts held still over the last tick (a
///   stall's cuts all come in the catch-up burst after it, so a long stall is one record) or have kept moving for
///   holdBound, at most once per graphRecordPace;
/// - nodeMissedCycles likewise for a node's missedCycles and overruns, at most once per nodeRecordPace per node, and at
///   its end at once, pace and hold waived, for what moved since its last record;
/// - its NodeCompanionsListeners the node's whole companion list (spanCompanions) when a sink's `dropped` or a source's
///   `shortfall` in it differs from what the last list it told named (0 for a companion it never named), at most once
///   per companionCounterPace per node, only for each name's newest span (what moves a list otherwise, an entry added
///   or removed or made ready, is NodeHost's to tell: docs/engine.md#companions-in-a-nodes-list);
/// - companionFramesLost likewise for each sink's and source's count, at most once per companionDropDiagnosticPace per
///   companion, and at its configuration's end (companionsEnded) at once, pace and hold waived.
/// flush() writes every record still held back (at a stop, before ear's last record), so the records' sums are the
/// totals.
class CounterWatch final : public NodeCountsListener {
public:
	static constexpr std::chrono::milliseconds wirePace{1000};
	static constexpr std::chrono::milliseconds graphRecordPace{1000};
	static constexpr std::chrono::milliseconds nodeRecordPace{10'000};
	static constexpr std::chrono::milliseconds holdBound{10'000};
	/// All outlive it. `timers` gives the clock the paces count on; `format` encodes lostMs (one block per cut cycle).
	CounterWatch(const Timers& timers, const GraphCounterSource& graph, const NodeCounterSource& nodes,
	             AudioFormat format);
	CounterWatch(const CounterWatch&) = delete;
	CounterWatch& operator=(const CounterWatch&) = delete;
	~CounterWatch() = default;
	void subscribe(CounterListener& listener);
	/// Told a node's list when its companions' counters moved (the list's other changes are NodeHost's).
	void subscribeCompanions(NodeCompanionsListener& listener);
	/// The time `cutCycles` cut cycles lost, at this graph's rate and block size.
	std::uint64_t lostMs(std::uint64_t cutCycles) const noexcept;
	/// At each heartbeat.
	void tick();
	/// Every record still held back, written now.
	void flush();
	void countsBegan(const NodeName& node, std::uint64_t span) override;
	void countsEnded(const NodeName& node, std::uint64_t span, const NodeCounts& total) override;
	void companionsEnded(const NodeName& node, std::uint64_t span, std::span<const CompanionView> finals) override;

private:
	using Clock = std::chrono::steady_clock;
	/// One record's pacing: the totals at its last record, when it was written, the reading before this tick's, and
	/// since when what it has not recorded has kept moving.
	template <class Counts> struct Paced {
		Counts recorded{};
		Counts previous{};
		std::optional<Clock::time_point> recordedAt{};
		std::optional<Clock::time_point> movingSince{};
	};
	struct Span {
		NodeName name;
		bool superseded = false; // a newer span of its name began: the wire names that one
		NodeCounts read{};       // this tick's reading
		NodeCounts sent{};       // what the client holds: node.added's zeros, then what countersChanged named
		Paced<NodeCounts> record{};
		std::map<NodeName, std::uint64_t> companionsSent{}; // each counter as the last list told named it
		std::optional<Clock::time_point> companionsSentAt{};
		std::map<NodeName, Paced<std::uint64_t>> companionRecords{}; // by companion, this configuration's
	};
	/// Whether a record is due at `now`: moved since it was recorded; settled (no move since the last tick) or moving for
	/// holdBound; and its pace passed since the last record. Notes when the move was first seen.
	template <class Counts>
	bool due(Paced<Counts>& paced, bool moved, bool settled, std::chrono::milliseconds pace, Clock::time_point now);
	void recordGraph(const GraphCounts& now, std::optional<Clock::time_point> at);
	void recordNode(const NodeName& name, Span& span, std::optional<Clock::time_point> at);
	void recordCompanion(const NodeName& node, const CompanionView& companion, std::uint64_t total,
	                     Paced<std::uint64_t>& paced, std::optional<Clock::time_point> at);
	void tickCompanions(Span& span, std::span<const CompanionView> companions, Clock::time_point now);

	const Timers& timers_;
	const GraphCounterSource& graph_;
	const NodeCounterSource& nodes_;
	AudioFormat format_;
	std::vector<CounterListener*> listeners_;
	std::vector<NodeCompanionsListener*> companionListeners_;
	std::map<std::uint64_t, Span> spans_; // by span
	GraphCounts graphSent_{};             // what countersChanged last carried; ear starts from zero

	std::optional<Clock::time_point> sentAt_;
	Paced<GraphCounts> graphRecord_{};
};

} // namespace ear
