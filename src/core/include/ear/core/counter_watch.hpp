// The diagnostics counters reported (docs/engine.md#reporting-the-counters): CounterWatch reads the graph's counters and
// every node's at the main loop's heartbeat, and tells its listeners what moved, at the paces the wire and standard
// output each take: the control port's counters.changed (docs/control-port.md#counters), and the records
// ear.graph-lost-cycles and ear.node-missed-cycles (docs/output-records.md#diagnostic). Main thread only: the data thread
// writes the counters and is never asked anything; this reads them through the lock-free reads the counters give.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
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

protected:
	~NodeCounterSource() = default;
};

/// Told where a node's counts begin and end (docs/engine.md#counters), by NodeHost, which alone knows a node's life:
/// a name's counts are one span from a node's add to its end, a port-caused replace carrying them on. Main thread.
class NodeCountsListener {
public:
	/// A node of this name begins counting from zero: added, or replacing an older node of the name by another type or a
	/// removal and add. Not told for a port-caused replace, whose newer node carries the older node's counts on.
	virtual void countsBegan(const NodeName& node) = 0;
	/// The node is gone, these its final counts, which NodeHost::counts no longer answers. Told just before its
	/// nodeGone; not for a node superseded by a newer node of its name (the name's span is the newer node's), nor for the
	/// older node of a port-caused replace (its newer node carries its counts).
	virtual void countsEnded(const NodeName& node, const NodeCounts& total) = 0;

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

protected:
	~CounterListener() = default;
};

/// The one pacer of the counters' reports (docs/engine.md#reporting-the-counters). Main thread. tick() is called at the
/// main loop's heartbeat, which already wakes the loop every 100 ms for the watchdog, so reading the counters adds no
/// wake-up and nothing runs on the data thread for it. Each tick reads the graph's counters and those of every node
/// whose span it holds (opened by countsBegan, closed by countsEnded) and tells:
/// - countersChanged when a loss (cutCycles, stalls, or any node counter: `cycles` moves every cycle and is never a
///   change on its own) moved since the last one, at most once per wirePace;
/// - graphLostCycles when cutCycles moved since the last such record and the cuts held still over the last tick (a
///   stall's cuts all come in the catch-up burst after it, so a long stall is one record) or have kept moving for
///   holdBound, at most once per graphRecordPace;
/// - nodeMissedCycles likewise for a node's missedCycles and overruns, at most once per nodeRecordPace per node, and at
///   its end at once, pace and hold waived, for what moved since its last record.
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
	/// The time `cutCycles` cut cycles lost, at this graph's rate and block size.
	std::uint64_t lostMs(std::uint64_t cutCycles) const noexcept;
	/// At each heartbeat.
	void tick();
	/// Every record still held back, written now.
	void flush();
	void countsBegan(const NodeName& node) override;
	void countsEnded(const NodeName& node, const NodeCounts& total) override;

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
		NodeCounts read{}; // this tick's reading
		NodeCounts sent{}; // what the client holds: node.added's zeros, then what countersChanged named
		Paced<NodeCounts> record{};
	};
	/// Whether a record is due at `now`: moved since it was recorded; settled (no move since the last tick) or moving for
	/// holdBound; and its pace passed since the last record. Notes when the move was first seen.
	template <class Counts>
	bool due(Paced<Counts>& paced, bool moved, bool settled, std::chrono::milliseconds pace, Clock::time_point now);
	void recordGraph(const GraphCounts& now, std::optional<Clock::time_point> at);
	void recordNode(const NodeName& name, Span& span, std::optional<Clock::time_point> at);

	const Timers& timers_;
	const GraphCounterSource& graph_;
	const NodeCounterSource& nodes_;
	AudioFormat format_;
	std::vector<CounterListener*> listeners_;
	std::map<NodeName, Span> spans_;
	GraphCounts graphSent_{}; // what countersChanged last carried; ear starts from zero

	std::optional<Clock::time_point> sentAt_;
	Paced<GraphCounts> graphRecord_{};
};

} // namespace ear
