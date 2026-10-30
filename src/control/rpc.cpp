// The JSON-RPC 2.0 envelope (docs/design/batch2.md section 4.5, rpc.hpp). Slice s0 stub: slice
// R1 builds it.
#include "ear/control/rpc.hpp"

#include <stdexcept>

namespace ear::control {
namespace {

[[noreturn]] void notBuilt(const char* what) {
	throw std::logic_error(std::string(what) + ": not built yet (slice R1)");
}

} // namespace

RpcMessage parseMessage(std::string_view) { notBuilt("parseMessage"); }

std::string encodeResult(const RpcId&, const nlohmann::json&) { notBuilt("encodeResult"); }

std::string encodeError(const RpcId&, const RpcError&) { notBuilt("encodeError"); }

std::string encodeNotification(std::string_view, const nlohmann::json&) { notBuilt("encodeNotification"); }

std::string encodeBatch(std::span<const std::string>) { notBuilt("encodeBatch"); }

} // namespace ear::control
