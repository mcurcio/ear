// The graph notifications as typed events (docs/design/batch2.md section 4.5, events.hpp).
// Slice s0 stub: slice R1 builds the encoders.
#include "ear/control/events.hpp"

#include <stdexcept>

namespace ear::control {

std::string encodeEvent(const GraphEvent&) {
	throw std::logic_error("encodeEvent: not built yet (slice R1)");
}

std::string encodeLevels(std::span<const NodeLevels>) {
	throw std::logic_error("encodeLevels: not built yet (slice R1)");
}

} // namespace ear::control
