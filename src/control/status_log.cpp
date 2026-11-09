// The one encoder of standard output (docs/output-records.md): every record ear prints, its
// fields and its human sentence, have their one home here. The schema is test/protocol/output-records.json.
#include "ear/control/status_log.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include <nlohmann/json.hpp>

#include "ear/control/graph_json.hpp"

namespace ear::control {

namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;

constexpr int recordVersion = 1; // the records' "v": additive changes keep it, a breaking one bumps it

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

std::string portText(const PortRef& port) { return std::format("{}:{}", port.node, port.port); }

std::string strings(const std::vector<std::string>& values) {
	std::vector<std::string> quoted;
	for (const std::string& value : values) quoted.push_back(json(value));
	return array(quoted);
}

std::string joinedKeys(const std::vector<std::string>& keys) {
	std::string text;
	for (const std::string& key : keys) text += (text.empty() ? "" : ", ") + key;
	return text;
}

/// A cause as the record carries it (docs/output-records.md#node): {"kind"}, with the restart's own kind under
/// "restart" and a settings change's keys.
std::string causeJson(const NodeCause& cause) {
	Fields fields{{"kind", json(causeName(cause.kind))}};
	if (cause.kind == NodeCause::Kind::restarting && cause.restart) {
		Fields restart{{"kind", json(causeName(*cause.restart))}};
		if (*cause.restart == NodeCause::Kind::settings) restart.emplace_back("keys", strings(cause.keys));
		fields.emplace_back("restart", object(restart));
	} else if (cause.kind == NodeCause::Kind::settings) {
		fields.emplace_back("keys", strings(cause.keys));
	}
	return object(fields);
}

/// A restart's own kind, or a configuring cause, in words (docs/output-records.md#node).
std::string restartText(NodeCause::Kind kind, const std::vector<std::string>& keys) {
	switch (kind) {
	case NodeCause::Kind::settings:
		return keys.empty() ? std::string("settings changed") : "settings changed: " + joinedKeys(keys);
	case NodeCause::Kind::action:
		return "restart invoked";
	default:
		return std::string(causeName(kind));
	}
}

/// The cause after `stopping: ` or `starting: `: `stop`, `disabling`, `removing`, `restarting (<restart>)`, or a
/// configuring cause's words.
std::string causeText(const NodeCause& cause) {
	if (cause.kind == NodeCause::Kind::restarting)
		return cause.restart ? std::format("restarting ({})", restartText(*cause.restart, cause.keys))
		                     : std::string("restarting");
	return restartText(cause.kind, cause.keys);
}

std::string waitingJson(const std::vector<WaitingFor>& waits) {
	std::vector<std::string> items;
	for (const WaitingFor& wait : waits) {
		Fields fields{{"kind", json(waitingKindName(wait.kind))}};
		if (!wait.name.empty()) fields.emplace_back("name", json(wait.name));
		items.push_back(object(fields));
	}
	return array(items);
}

/// What a still record says the node waits for (docs/output-records.md#node).
std::string waitingText(const std::vector<WaitingFor>& waits) {
	std::string text;
	for (const WaitingFor& wait : waits) {
		std::string one;
		switch (wait.kind) {
		case WaitingFor::Kind::olderNode:
			one = "the previous " + wait.name;
			break;
		case WaitingFor::Kind::block:
			one = "the engine";
			break;
		case WaitingFor::Kind::companion:
			one = wait.name;
			break;
		case WaitingFor::Kind::consumer:
			one = "its consumer";
			break;
		}
		text += (text.empty() ? "" : ", ") + one;
	}
	return text;
}

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

StatusLog::StatusLog(OutputLines& out, WallClock clock, PaceClock pace)
	: out_(out), clock_(std::move(clock)), pace_(std::move(pace)) {}

void StatusLog::emit(std::string_view kind, Fields fields, std::string msg) {
	const std::scoped_lock lock(mutex_);
	Fields record{
		{"v", json(recordVersion)}, {"seq", json(++seq_)}, {"t", json(utcMicros(clock_()))}, {"kind", json(kind)}};
	for (auto& field : fields) record.push_back(std::move(field));
	if (dropped_ > 0) {
		record.emplace_back("dropped", json(dropped_));
		msg += std::format(" ({} records dropped before this)", dropped_);
	}
	record.emplace_back("msg", json(msg));
	if (out_.offer(object(record))) dropped_ = 0;
	else ++dropped_;
}

void StatusLog::nodeChanged(const NodeHealth& health) {
	enum class Write { none, state, still };
	Write write = Write::none;
	std::int64_t seconds = 0;
	{
		const std::scoped_lock lock(mutex_);
		const auto now = pace_();
		// A superseded node (an older node of a name whose newer node was added) is kept apart from the newer one:
		// its first superseded tell takes over what was seen of the name until then.
		auto& seenOf = health.superseded ? superseded_ : nodes_;
		if (health.superseded && !superseded_.contains(health.name)) {
			if (auto held = nodes_.extract(health.name)) superseded_.insert(std::move(held));
		}
		const auto known = seenOf.find(health.name);
		const bool first = known == seenOf.end();
		Seen& seen = first ? seenOf.emplace(health.name, Seen{health, now}).first->second : known->second;
		if (!first && seen.health.phase != health.phase) {
			seen.since = now;
			seen.still.reset();
			seen.written.clear();
		}
		const bool colour = first || seen.health.state != health.state;
		const bool reason = health.state == NodeState::failed && !first && seen.health.reason != health.reason;
		if (colour || reason) {
			write = Write::state;
		} else if (!health.waitingFor.empty() && health.waitingFor != seen.written &&
		           (!seen.still || now - *seen.still >= stillPace)) {
			// A list told inside the pace is written by the first tell after it, if it still differs.
			write = Write::still;
			seen.still = now;
			seen.written = health.waitingFor;
			seconds = std::chrono::duration_cast<std::chrono::seconds>(now - seen.since).count();
		}
		seen.health = health;
	}
	Fields fields{{"node", json(health.name)},
	              {"state", json(stateName(health.state))},
	              {"phase", json(phaseName(health.phase))}};
	if (write == Write::still) {
		fields.emplace_back("waitingFor", waitingJson(health.waitingFor));
		emit("node", std::move(fields),
		     std::format("node {} still {} after {}s: waiting for {}", health.name, phaseName(health.phase), seconds,
		                 waitingText(health.waitingFor)));
		return;
	}
	if (write == Write::none) return;
	std::string msg = std::format("node {} {}", health.name, stateName(health.state));
	switch (health.state) {
	case NodeState::starting:
		// added: `starting`; configuring after a restart, a retry or an enable: `starting: <cause>`; none else (the
		// colour's other phases, W4).
		if (health.phase == NodePhase::configuring && health.cause) {
			fields.emplace_back("cause", causeJson(*health.cause));
			msg += ": " + causeText(*health.cause);
		} else if (health.phase != NodePhase::added) {
			return;
		}
		break;
	case NodeState::stopping:
		if (health.cause) {
			fields.emplace_back("cause", causeJson(*health.cause));
			msg += ": " + causeText(*health.cause);
		}
		break;
	case NodeState::failed:
		if (health.reason) {
			const std::string_view code = failureCode(health.reason->code);
			fields.emplace_back("failure", object({{"code", json(code)}, {"detail", json(health.reason->detail)}}));
			msg = std::format("node {} failed {}: {}", health.name, code, health.reason->detail);
		}
		break;
	case NodeState::running:
	case NodeState::stopped:
	case NodeState::disabled:
		break;
	}
	emit("node", std::move(fields), std::move(msg));
}

void StatusLog::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	{
		const std::scoped_lock lock(mutex_);
		nodes_.erase(node);
	}
	removed(node, droppedFrames);
}

void StatusLog::supersededGone(const NodeName& node, std::uint64_t droppedFrames) {
	{
		const std::scoped_lock lock(mutex_);
		superseded_.erase(node);
	}
	removed(node, droppedFrames);
}

void StatusLog::removed(const NodeName& node, std::uint64_t droppedFrames) {
	emit("node",
	     {{"node", json(node)},
	      {"state", json(stateName(NodeState::stopping))},
	      {"phase", json("removed")},
	      {"droppedFrames", json(droppedFrames)}},
	     std::format("node {} removed dropped={}", node, droppedFrames));
}

void StatusLog::nodeSays(const NodeName& node, const std::string& text) {
	emit("note", {{"node", json(node)}, {"text", json(text)}}, std::format("node {} says {}", node, text));
}

void StatusLog::nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail,
                               std::span<const DiagnosticField> fields) {
	Fields record{{"node", json(node)}, {"code", json(code)}};
	for (const DiagnosticField& field : fields) record.emplace_back(std::string(field.key), json(field.value));
	record.emplace_back("detail", json(detail));
	emit("diagnostic", std::move(record), std::format("node {} {}: {}", node, code, detail));
}

void StatusLog::engineDiagnostic(const EngineDiagnostic& diagnostic) {
	Fields record{{"code", json(diagnostic.code)}};
	for (const DiagnosticField& field : diagnostic.fields)
		record.emplace_back(std::string(field.key), json(field.value));
	record.emplace_back("detail", json(diagnostic.detail));
	emit("diagnostic", std::move(record), std::format("{}: {}", diagnostic.code, diagnostic.detail));
}

void StatusLog::mainLoopStalled(std::int64_t ms, std::string_view in) {
	constexpr std::string_view code = "ear.main-loop-stalled";
	Fields record{{"code", json(code)}, {"ms", json(ms)}};
	std::string detail = std::format("no main-loop heartbeat for {} ms", ms);
	if (!in.empty()) {
		record.emplace_back("in", json(in));
		detail += std::format(", in {}", in);
	}
	record.emplace_back("detail", json(detail));
	emit("diagnostic", std::move(record), std::format("{}: {}", code, detail));
}

void StatusLog::mainLoopResumed(std::int64_t ms) {
	constexpr std::string_view code = "ear.main-loop-resumed";
	const std::string detail = std::format("the main-loop heartbeat came back after a gap of {} ms", ms);
	emit("diagnostic", {{"code", json(code)}, {"ms", json(ms)}, {"detail", json(detail)}},
	     std::format("{}: {}", code, detail));
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
	std::uint64_t heldBack = 0;
	{
		// Paced under the lock: a refused entry costs no encoding. A node's sources share its pace.
		const std::scoped_lock lock(mutex_);
		const std::string key = entry.node ? "node " + *entry.node : entry.source;
		const auto now = pace_();
		Pace& pace = paces_.try_emplace(key, Pace{static_cast<double>(logBurst), now}).first->second;
		if (now > pace.at) {
			const double refill = std::chrono::duration<double>(now - pace.at).count();
			pace.tokens = std::min(static_cast<double>(logBurst), pace.tokens + refill);
		}
		pace.at = now;
		if (pace.tokens < 1.0) {
			++pace.heldBack;
			return;
		}
		pace.tokens -= 1.0;
		heldBack = std::exchange(pace.heldBack, 0);
	}
	Fields fields;
	if (entry.node) fields.emplace_back("node", json(*entry.node));
	else fields.emplace_back("unattributed", "true");
	fields.emplace_back("level", json(logLevelName(entry.level)));
	fields.emplace_back("source", json(entry.source));
	if (!entry.topic.empty()) fields.emplace_back("topic", json(entry.topic));
	fields.emplace_back("text", json(entry.text));
	if (entry.cut) fields.emplace_back("cut", "true");
	if (heldBack > 0) fields.emplace_back("heldBack", json(heldBack));
	std::string msg = entry.node ? std::format("node {} ", *entry.node) : std::string();
	msg += entry.source;
	if (!entry.topic.empty()) msg += " " + entry.topic;
	msg += std::format(" {}: {}", logLevelName(entry.level), entry.text);
	if (heldBack > 0) msg += std::format(" ({} more held back before this: over {} at once)", heldBack, logBurst);
	emit("log", std::move(fields), std::move(msg));
}

void StatusLog::actionAnswered(const ActionRecord& record) {
	Fields fields{{"node", json(record.node)}, {"action", json(record.action)}, {"result", json(record.result)}};
	if (record.detail) fields.emplace_back("detail", json(*record.detail));
	std::string msg = record.result == "ok"
	                      ? std::format("node {} {} invoked", record.node, record.action)
	                      : std::format("node {} {} refused: {}", record.node, record.action, record.result);
	if (record.result != "ok" && record.detail) msg += std::format(" ({})", *record.detail);
	emit("action", std::move(fields), std::move(msg));
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
