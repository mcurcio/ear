// The plain types of ear.conf's state on the wire (docs/design/knobly-config.md, "Wire additions";
// graph_file_state.hpp): the one home of the severity names.
#include "ear/control/graph_file_state.hpp"

namespace ear::control {

std::string_view severityName(Severity severity) noexcept {
	switch (severity) {
	case Severity::error: return "error";
	case Severity::warning: return "warning";
	}
	return "error";
}

} // namespace ear::control
