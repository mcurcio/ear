// The one home of node and route JSON (docs/design/batch2.md section 4.5, graph_json.hpp).
// Slice s0 stub: slice R1 builds it and moves graph_file.cpp onto it.
#include "ear/control/graph_json.hpp"

#include <stdexcept>

namespace ear::control {
namespace {

[[noreturn]] void notBuilt(const char* what) {
	throw std::logic_error(std::string(what) + ": not built yet (slice R1)");
}

} // namespace

std::expected<void, JsonError> requireObject(const nlohmann::json&, const JsonPointer&,
                                             std::initializer_list<std::string_view>) {
	notBuilt("requireObject");
}

std::expected<const nlohmann::json*, JsonError> requireMember(const nlohmann::json&, const JsonPointer&,
                                                              const std::string&) {
	notBuilt("requireMember");
}

std::expected<std::string, JsonError> requireName(const nlohmann::json&, const JsonPointer&, const std::string&) {
	notBuilt("requireName");
}

std::expected<NodeSpec, JsonError> decodeNodeSpec(const nlohmann::json&, const JsonPointer&) {
	notBuilt("decodeNodeSpec");
}

std::expected<Route, JsonError> decodeRoute(const nlohmann::json&, const JsonPointer&) { notBuilt("decodeRoute"); }

nlohmann::json encodePortRef(const PortRef&) { notBuilt("encodePortRef"); }

nlohmann::json encodeRoute(const Route&) { notBuilt("encodeRoute"); }

} // namespace ear::control
