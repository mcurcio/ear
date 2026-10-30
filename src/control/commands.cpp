// The control port's methods as typed commands (docs/design/batch2.md section 4.5,
// commands.hpp). Slice s0 stub: slice R1 builds them.
#include "ear/control/commands.hpp"

#include <stdexcept>
#include <string>

namespace ear::control {
namespace {

[[noreturn]] void notBuilt(const char* what) {
	throw std::logic_error(std::string(what) + ": not built yet (slice R1)");
}

} // namespace

std::expected<Command, RpcError> decodeCommand(std::string_view, const nlohmann::json&) {
	notBuilt("decodeCommand");
}

CommandResult runCommand(const Command&, GraphControl&, const GraphReader&) { notBuilt("runCommand"); }

std::expected<nlohmann::json, RpcError> encodeCommandResult(const CommandResult&) {
	notBuilt("encodeCommandResult");
}

nlohmann::json encodeGraph(const GraphView&) { notBuilt("encodeGraph"); }

} // namespace ear::control
