// M1's status lines: one flushed line per node or route event (docs/design/m1-white-to-file.md
// section 2, status_log.hpp).
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

} // namespace ear::control
