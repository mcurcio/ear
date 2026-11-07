// The one encoder of standard output (docs/design/node-framework.md section 8.8, slice 4; it replaces the plain
// lines of docs/design/m1-white-to-file.md section 2, knobly-config.md and sendspin.md section 9). The records'
// schema is the fixture test/protocol/output-records.json.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
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
/// Records a full queue dropped are counted: the next record carries "dropped":<n>, and its msg says so.
/// Thread-safe: every call takes one lock while it encodes and offers, never while anything is written.
class StatusLog final : public NodeStatusListener,
						public NodeNoteListener,
						public RouteStatusListener,
						public GraphFileListener,
						public LogSink {
public:
	explicit StatusLog(OutputLines& out, WallClock clock = &std::chrono::system_clock::now);
	void nodeChanged(const NodeStatus& status) override;
	void nodeGone(const NodeName& node, std::uint64_t droppedFrames) override;
	void nodeSays(const NodeName& node, const std::string& text) override;
	void routeChanged(const RouteStatus& status) override;
	void graphFileChanged(const GraphFileChanged& change) override;
	void graphFileWritten(const std::string& version) override;
	void graphFileWriteFailed(const std::string& detail);
	void log(const LogEntry& entry) override;
	void engineScheduling(bool realtime, std::string_view detail);
	void controlListening(std::string_view address, std::uint16_t port);
	/// Why ear exits with `status` (a folder it cannot open, a port it cannot bind).
	void exiting(int status, std::string_view reason);

private:
	/// A record's own fields, in order: each a key and its value as JSON text.
	using Fields = std::vector<std::pair<std::string, std::string>>;
	/// The record's envelope around `fields`, offered under the lock.
	void emit(std::string_view kind, Fields fields, std::string msg);

	OutputLines& out_;
	WallClock clock_;
	std::mutex mutex_;
	std::uint64_t seq_ = 0;
	std::uint64_t dropped_ = 0;
};
} // namespace ear::control
