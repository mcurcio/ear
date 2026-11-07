// The one encoder of standard output (docs/design/node-framework.md section 8.8): every record ear prints, its
// fields and its human sentence, have their one home here. The schema is test/protocol/output-records.json.
#include "ear/control/status_log.hpp"

#include <format>
#include <utility>

#include <nlohmann/json.hpp>

namespace ear::control {

namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;

constexpr int recordVersion = 1; // the records' "v": additive changes keep it, a breaking one bumps it (8.7)

/// One value as JSON text. Peers' and libraries' text may not be UTF-8: a bad byte becomes U+FFFD, never an
/// exception or a broken line; control characters are escaped.
template <typename Value> std::string json(const Value& value) {
	return nlohmann::json(value).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// An object whose keys keep the order given (the vendored nlohmann/json sorts its own).
std::string object(const Fields& fields) {
	std::string text = "{";
	for (const auto& [key, value] : fields) {
		if (text.size() > 1) text += ',';
		text += json(key) + ':' + value;
	}
	return text + '}';
}

std::string array(const std::vector<std::string>& values) {
	std::string text = "[";
	for (const std::string& value : values) {
		if (text.size() > 1) text += ',';
		text += value;
	}
	return text + ']';
}

std::string writeFailedText(const std::string& detail) { return std::format("graph-file write-failed {}", detail); }

std::string timestamp(std::chrono::system_clock::time_point at) {
	return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::microseconds>(at));
}

std::string portText(const PortRef& port) { return std::format("{}:{}", port.node, port.port); }

/// The plain lines a graph file change printed before records, in order: the first always, then each that applies.
std::vector<std::string> graphFileLines(const GraphFileChanged& change) {
	const GraphFile& file = change.file;
	std::vector<std::string> lines;
	lines.push_back(std::format("graph-file {} {} running={}", causeName(change.cause), file.version,
	                            file.running.value_or("none")));
	for (const Diagnostic& d : file.diagnostics)
		if (d.severity == Severity::error)
			lines.push_back(
				std::format("graph-file error {}:{} {}: {}", d.place.line, d.place.column, d.code, d.message));
	if (file.refusal) {
		std::string refused = std::format("graph-file refused {}", errorCode(file.refusal->error));
		if (file.refusal->place)
			refused += std::format(" at {}:{}", file.refusal->place->line, file.refusal->place->column);
		lines.push_back(std::move(refused));
	}
	if (change.droppedEdits > 0) lines.push_back(std::format("graph-file dropped {} edits", change.droppedEdits));
	if (!file.pendingRestart.empty()) {
		std::string pending = "graph-file pending-restart";
		for (const FormatKey key : file.pendingRestart) pending += std::format(" {}", formatKeyName(key));
		lines.push_back(std::move(pending));
	}
	if (file.writeFailed) lines.push_back(writeFailedText(*file.writeFailed));
	return lines;
}

} // namespace

StatusLog::StatusLog(OutputLines& out, WallClock clock) : out_(out), clock_(std::move(clock)) {}

void StatusLog::emit(std::string_view kind, Fields fields, std::string msg) {
	const std::scoped_lock lock(mutex_);
	Fields record{
		{"v", json(recordVersion)}, {"seq", json(++seq_)}, {"t", json(timestamp(clock_()))}, {"kind", json(kind)}};
	for (auto& field : fields) record.push_back(std::move(field));
	if (dropped_ > 0) {
		record.emplace_back("dropped", json(dropped_));
		msg += std::format(" ({} records dropped before this)", dropped_);
	}
	record.emplace_back("msg", json(msg));
	if (out_.offer(object(record))) dropped_ = 0;
	else ++dropped_;
}

void StatusLog::nodeChanged(const NodeStatus& status) {
	Fields fields{{"node", json(status.name)}, {"state", json(stateName(status.state))}};
	if (status.state == NodeState::failed && status.reason) {
		const std::string_view code = failureCode(status.reason->code);
		fields.emplace_back("failure", object({{"code", json(code)}, {"detail", json(status.reason->detail)}}));
		emit("node", std::move(fields), std::format("node {} failed {}: {}", status.name, code, status.reason->detail));
		return;
	}
	emit("node", std::move(fields), std::format("node {} {}", status.name, stateName(status.state)));
}

void StatusLog::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	emit("node", {{"node", json(node)}, {"state", json("stopped")}, {"droppedFrames", json(droppedFrames)}},
	     std::format("node {} stopped dropped={}", node, droppedFrames));
}

void StatusLog::nodeSays(const NodeName& node, const std::string& text) {
	emit("note", {{"node", json(node)}, {"text", json(text)}}, std::format("node {} says {}", node, text));
}

void StatusLog::routeChanged(const RouteStatus& status) {
	const std::string from = portText(status.route.from);
	const std::string to = portText(status.route.to);
	Fields fields{{"from", json(from)}, {"to", json(to)}, {"status", json(matchName(status.match))}};
	std::string msg = std::format("route {} -> {} {}", from, to, matchName(status.match));
	if (status.match == RouteMatch::refused && !status.detail.empty()) {
		fields.emplace_back("detail", json(status.detail));
		msg += ": " + status.detail;
	}
	emit("route", std::move(fields), std::move(msg));
}

void StatusLog::graphFileChanged(const GraphFileChanged& change) {
	const GraphFile& file = change.file;
	std::vector<std::string> diagnostics;
	for (const Diagnostic& d : file.diagnostics)
		diagnostics.push_back(object({{"severity", json(severityName(d.severity))},
		                              {"line", json(d.place.line)},
		                              {"column", json(d.place.column)},
		                              {"code", json(d.code)},
		                              {"message", json(d.message)}}));
	Fields fields{{"cause", json(causeName(change.cause))},
	              {"version", json(file.version)},
	              {"running", file.running ? json(*file.running) : "null"},
	              {"diagnostics", array(diagnostics)}};
	if (file.refusal) {
		Fields refused{{"code", json(errorCode(file.refusal->error))}};
		if (file.refusal->place) {
			refused.emplace_back("line", json(file.refusal->place->line));
			refused.emplace_back("column", json(file.refusal->place->column));
		}
		fields.emplace_back("refused", object(refused));
	}
	fields.emplace_back("droppedEdits", json(change.droppedEdits));
	std::vector<std::string> pending;
	for (const FormatKey key : file.pendingRestart) pending.push_back(json(formatKeyName(key)));
	fields.emplace_back("pendingRestart", array(pending));
	if (file.writeFailed) fields.emplace_back("writeFailed", json(*file.writeFailed));

	std::string msg;
	for (const std::string& line : graphFileLines(change)) msg += (msg.empty() ? "" : "\n") + line;
	emit("graph-file", std::move(fields), std::move(msg));
}

void StatusLog::graphFileWritten(const std::string& version) {
	emit("graph-file-written", {{"version", json(version)}}, std::format("graph-file written {}", version));
}

void StatusLog::graphFileWriteFailed(const std::string& detail) {
	emit("graph-file-write-failed", {{"detail", json(detail)}}, writeFailedText(detail));
}

void StatusLog::log(const LogEntry& entry) {
	Fields fields;
	if (entry.node) fields.emplace_back("node", json(*entry.node));
	else fields.emplace_back("unattributed", "true");
	if (entry.run) fields.emplace_back("run", json(*entry.run));
	fields.emplace_back("level", json(logLevelName(entry.level)));
	fields.emplace_back("source", json(entry.source));
	if (!entry.topic.empty()) fields.emplace_back("topic", json(entry.topic));
	fields.emplace_back("text", json(entry.text));
	if (entry.cut) fields.emplace_back("cut", "true");
	std::string msg = entry.node ? std::format("node {} ", *entry.node) : std::string();
	msg += entry.source;
	if (!entry.topic.empty()) msg += " " + entry.topic;
	msg += std::format(" {}: {}", logLevelName(entry.level), entry.text);
	emit("log", std::move(fields), std::move(msg));
}

void StatusLog::engineScheduling(bool realtime, std::string_view detail) {
	emit("engine", {{"realtime", json(realtime)}, {"scheduling", json(detail)}},
	     realtime ? std::format("engine: realtime {}", detail) : std::format("engine: not realtime ({})", detail));
}

void StatusLog::controlListening(std::string_view address, std::uint16_t port) {
	emit("control", {{"address", json(address)}, {"port", json(port)}},
	     std::format("control listening on {}:{}", address, port));
}

void StatusLog::exiting(int status, std::string_view reason) {
	emit("exit", {{"status", json(status)}, {"reason", json(reason)}}, std::format("ear: {}", reason));
}

} // namespace ear::control
