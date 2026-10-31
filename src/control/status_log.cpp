// M1's status lines: one flushed line per node or route event (docs/design/m1-white-to-file.md
// section 2, status_log.hpp), and the graph file's (docs/design/knobly-config.md): their one home.
#include "ear/control/status_log.hpp"

#include <format>

namespace ear::control {

StatusLog::StatusLog(std::ostream& out) : out_(out) {}

void StatusLog::nodeChanged(const NodeStatus& status) {
	if (status.state == NodeState::failed && status.reason)
		out_ << std::format("node {} failed {}: {}\n", status.name, failureCode(status.reason->code),
		                    status.reason->detail);
	else
		out_ << std::format("node {} {}\n", status.name, stateName(status.state));
	out_.flush();
}

void StatusLog::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	out_ << std::format("node {} stopped dropped={}\n", node, droppedFrames);
	out_.flush();
}

void StatusLog::routeChanged(const RouteStatus& status) {
	const Route& route = status.route;
	out_ << std::format("route {}:{} -> {}:{} {}", route.from.node, route.from.port, route.to.node,
	                    route.to.port, matchName(status.match));
	if (status.match == RouteMatch::refused && !status.detail.empty()) out_ << ": " << status.detail;
	out_ << '\n';
	out_.flush();
}

void StatusLog::graphFileChanged(const GraphFileChanged& change) {
	const GraphFile& file = change.file;
	const auto line = [&](const std::string& text) {
		out_ << "graph-file " << text << '\n';
		out_.flush();
	};
	line(std::format("{} {} running={}", causeName(change.cause), file.version, file.running.value_or("none")));
	for (const Diagnostic& d : file.diagnostics)
		if (d.severity == Severity::error)
			line(std::format("error {}:{} {}: {}", d.place.line, d.place.column, d.code, d.message));
	if (file.refusal) {
		std::string refused = std::format("refused {}", errorCode(file.refusal->error));
		if (file.refusal->place) refused += std::format(" at {}:{}", file.refusal->place->line, file.refusal->place->column);
		line(refused);
	}
	if (change.droppedEdits > 0) line(std::format("dropped {} edits", change.droppedEdits));
	if (!file.pendingRestart.empty()) {
		std::string pending = "pending-restart";
		for (const FormatKey key : file.pendingRestart) pending += std::format(" {}", formatKeyName(key));
		line(pending);
	}
	if (file.writeFailed) line(std::format("write-failed {}", *file.writeFailed));
}

void StatusLog::graphFileWritten(const std::string& version) {
	out_ << std::format("graph-file written {}\n", version);
	out_.flush();
}

} // namespace ear::control
