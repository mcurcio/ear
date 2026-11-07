// The framework's log, as node code and captured output reach it (docs/design/node-framework.md section 8.8): free
// text at a level, from a source, attributed to a node or not. The one encoder (ear_control's status_log.cpp) turns
// each entry into a `log` record on standard output.
//
// The seam for 8.8's node-facing primitives: a run handle's `debug`/`info`/`warn`/`error` fill an entry with its node
// and run and give it to a LogSink; `rtlog` and its coded ring are drained into entries the same way. Neither exists
// yet: both need the run handle (slice 1).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "ear/core/graph_spec.hpp"
namespace ear {
enum class LogLevel { debug, info, warn, error };
std::string_view logLevelName(LogLevel level) noexcept; // "debug" | "info" | "warn" | "error"
struct LogEntry {
	LogLevel level = LogLevel::info;
	std::string source;                 // who wrote it: "stdout", "stderr", "pipewire", a type or library's name
	std::string text;                   // free text, never interpreted
	std::string topic{};                // a library's own sub-source (a PipeWire log topic); empty: none
	std::optional<NodeName> node{};     // absent: unattributed (captured descriptors, PipeWire, a type's boot)
	std::optional<std::uint64_t> run{}; // the run's generation, once runs have one (slice 1)
	bool cut = false;                   // the text was longer than its bound and was cut
};
/// Where log entries go. Any thread; never blocks on output.
class LogSink {
public:
	virtual void log(const LogEntry& entry) = 0;

protected:
	~LogSink() = default;
};
} // namespace ear
