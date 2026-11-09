// The one encoder of standard output (docs/design/node-framework.md section 8.8, slice 4; it replaces the plain
// lines of docs/design/m1-white-to-file.md section 2, knobly-config.md and sendspin.md section 9). The records'
// schema is the fixture test/protocol/output-records.json.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ear/control/requests.hpp"
#include "ear/core/log.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
namespace ear::control {
/// Where the encoder's lines go, one record a line, without its newline. Never blocks: false when the line was
/// dropped (a full queue). Called with the encoder's lock held, from any thread.
class OutputLines {
public:
	virtual bool offer(std::string line) = 0;

protected:
	~OutputLines() = default;
};
using WallClock = std::function<std::chrono::system_clock::time_point()>;
/// What paces log records: steady, so a wall clock set back never holds a source's records for good.
using PaceClock = std::function<std::chrono::steady_clock::time_point()>;
/// Every line ear writes on standard output, one JSON object each:
///   {"v":1,"seq":<n>,"t":"<UTC, microseconds>","kind":"<kind>",<the kind's fields>,"msg":"<a human sentence>"}
/// msg is today's plain line, so `jq -r .msg` reads as ear always did:
///   engine              engine: realtime <detail> | engine: not realtime (<detail>)
///   control             control listening on <address>:<port>
///   node                node <n> starting|running|stopping | node <n> failed <code>: <detail>
///                       | node <n> stopped dropped=<frames>
///   note                node <n> says <text>
///   route               route <a>:<p> -> <b>:<q> pending|linked|refused[: <detail>]
///   graph-file          one record per change, every diagnostic in it; msg is its lines, joined by newlines:
///                         graph-file <edit|disk|write> <version> running=<version|none>
///                         graph-file error <line>:<column> <code>: <message>      (one per error)
///                         graph-file refused <code>[ at <line>:<column>]
///                         graph-file dropped <n> edits
///                         graph-file pending-restart <graphFrequency|blockSize>...
///                         graph-file write-failed <detail>
///   graph-file-written  graph-file written <version>
///   graph-file-write-failed  graph-file write-failed <detail>   (the last write's, as ear exits)
///   log                 [node <n> ]<source>[ <topic>] <level>: <text>
///   exit                ear: <reason>
///   diagnostic          node <n> <code>: <detail>   (the framework's own observation about a node: ear.close-overdue,
///                       ear.fade-unacknowledged, ear.barrier-overdue; its fields, such as "ms", between code and detail)
///                       | <code>: <detail>   (one that names no node: ear.data-loop-wedged, with no "node" field;
///                       ear.main-loop-stalled with "ms" and "in" (the call the main loop is in, when known) and
///                       ear.main-loop-resumed with "ms", the stall's length, both from the watchdog's thread)
/// Records a full queue dropped are counted: the next record carries "dropped":<n>, and its msg says so.
/// Log records are paced per node, or per source when unattributed (captured stderr, PipeWire, a library): a burst of
/// logBurst passes, then one a second, so a library that floods its output cannot push node states, routes and notes
/// out of the queue. What was held back is counted: the source's next record that passes carries "heldBack":<n>, and
/// its msg says so (a flood that stops is counted on that source's next line, not before).
/// Thread-safe: every call takes one lock while it encodes and offers, never while anything is written.
class StatusLog final : public NodeHealthListener,
						public NodeNoteListener,
						public RouteStatusListener,
						public GraphFileListener,
						public NodeDiagnosticListener,
						public EngineDiagnosticListener,
						public LogSink {
public:
	static constexpr int logBurst = 32;
	explicit StatusLog(OutputLines& out, WallClock clock = &std::chrono::system_clock::now,
	                   PaceClock pace = &std::chrono::steady_clock::now);
	void nodeChanged(const NodeHealth& health) override;
	void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
	void nodeSays(const NodeName& node, const std::string& text) override;
	void nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail,
	                    std::span<const DiagnosticField> fields) override;
	void engineDiagnostic(const EngineDiagnostic& diagnostic) override;
	void routeChanged(const RouteStatus& status) override;
	void graphFileChanged(const GraphFileChanged& change) override;
	void graphFileWritten(const std::string& version) override;
	void graphFileWriteFailed(const std::string& detail);
	void log(const LogEntry& entry) override;
	/// The main-loop watchdog (StallWatch, node-lifecycle.md 5.12), from its own thread: a look found the heartbeat
	/// `ms` old, the main loop `in` a call (MainLoopActivity::current(); empty when between calls); and the beats back
	/// after a gap of `ms` between the beats either side.
	void mainLoopStalled(std::int64_t ms, std::string_view in);
	void mainLoopResumed(std::int64_t ms);
	void engineScheduling(bool realtime, std::string_view detail);
	void controlListening(std::string_view address, std::uint16_t port);
	/// Why ear exits with `status` (a folder it cannot open, a port it cannot bind).
	void exiting(int status, std::string_view reason);

private:
	/// A record's own fields, in order: each a key and its value as JSON text.
	using Fields = std::vector<std::pair<std::string, std::string>>;
	/// The record's envelope around `fields`, offered under the lock.
	void emit(std::string_view kind, Fields fields, std::string msg);

	/// One node's or source's pace: tokens up to logBurst, one more a second.
	struct Pace {
		double tokens;
		std::chrono::steady_clock::time_point at;
		std::uint64_t heldBack = 0;
	};

	OutputLines& out_;
	WallClock clock_;
	PaceClock pace_;
	std::map<std::string, Pace, std::less<>> paces_; // under mutex_
	std::mutex mutex_;
	std::uint64_t seq_ = 0;
	std::uint64_t dropped_ = 0;
};
} // namespace ear::control
