// The protocol per connection, and the hub of sessions (docs/design/batch2.md sections 4.5 and 6,
// session.hpp). Slice s0 stub: slice R1 builds them.
#include "ear/control/session.hpp"

#include <stdexcept>
#include <string>

namespace ear::control {
namespace {

[[noreturn]] void notBuilt(const char* what) {
	throw std::logic_error(std::string(what) + ": not built yet (slice R1)");
}

} // namespace

struct ControlSession::Impl {};

ControlSession::ControlSession(ControlHub&, Outbox&) { notBuilt("ControlSession"); }

ControlSession::~ControlSession() = default;

void ControlSession::received(std::string, std::move_only_function<void()>) { notBuilt("ControlSession::received"); }

struct ControlHub::Impl {};

ControlHub::ControlHub(GraphGateway&, LevelReader&) { notBuilt("ControlHub"); }

ControlHub::~ControlHub() = default;

void ControlHub::graphChanged(GraphEvent) { notBuilt("ControlHub::graphChanged"); }

void ControlHub::levelsPublished() { notBuilt("ControlHub::levelsPublished"); }

} // namespace ear::control
