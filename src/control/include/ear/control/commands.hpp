// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. The graphFile.* commands and
// results are docs/design/knobly-config.md's (slices S3 and S5, which deleted runCommand: the graph
// file keeper runs commands); their erratum to batch2.md lands in S7. The lifecycle's five commands, Applied,
// ControlRefusal, `disabled` on node.add, data.pointer and the frame (wireProtocol, wireFeatures) are
// docs/design/node-lifecycle-modules.md 5.1's (slice 3a, step L-6; control-plane.md the wire's home from protocol 1,
// batch2.md section 11's erratum).
#pragma once
#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>
#include "ear/control/graph_file_state.hpp"
#include "ear/control/lifecycle_wire.hpp"
#include "ear/control/rpc.hpp"
#include "ear/core/graph_view.hpp"
// The control port's methods (docs/design/batch2.md section 6) as typed commands.
namespace ear::control {
struct GetGraph {}; // graph.get, graph.subscribe
struct AddNode {
	NodeSpec node;
};
struct UpdateNode {
	NodeName node;
	NodeSettings settings;
};
struct RemoveNode {
	NodeName node;
};
struct AddRoute {
	Route route;
};
struct RemoveRoute {
	Route route;
};
struct GetGraphFile {}; // graphFile.get, graphFile.subscribe
struct SetGraphFile {
	std::string text;
	std::string version;
}; // graphFile.set
struct StartNode {
	NodeName node;
}; // node.start {"name"}
struct StopNode {
	NodeName node;
}; // node.stop {"name"}
struct RestartNode {
	NodeName node;
}; // node.restart {"name"}
struct DisableNode {
	NodeName node;
}; // node.disable {"name"}
struct EnableNode {
	NodeName node;
}; // node.enable {"name"}
using Command = std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute, GetGraphFile,
                             SetGraphFile, StartNode, StopNode, RestartNode, DisableNode, EnableNode>;
using CommandResult =
	std::variant<GraphView, std::expected<void, CommandError>, GraphFile, FileRefusal, Applied, ControlRefusal>;
/// The wire's frame (control-plane.md 4.1): graph.get's "protocol" and "features" (D-M22).
inline constexpr int wireProtocol = 1;
inline constexpr std::array<std::string_view, 1> wireFeatures{"lifecycle"};
/// Control thread. Named params only, decoded with graph_json. Unknown method: methodNotFound;
/// a missing, extra or mistyped key: invalidParams whose message is the JsonError
/// ("/from/port: missing") and whose data is {"pointer"} (the JSON pointer it names). graphFile.get and
/// graphFile.subscribe (params absent or {}) are GetGraphFile; graphFile.set is SetGraphFile (text and version, each a
/// required string); node.start, node.stop, node.restart, node.disable and node.enable take {"name"}. The
/// graph file keeper runs every command (requests.hpp).
std::expected<Command, RpcError> decodeCommand(std::string_view method, const nlohmann::json& params);
/// Control thread. GraphView -> encodeGraph; success -> {}; CommandError ->
/// RpcError{commandRefused, describe(e), {"code": errorCode(e)}}; GraphFile -> encodeGraphFile;
/// FileRefusal -> RpcError{commandRefused, describe(r), {"code": fileRefusalCode(r)}, plus
/// "diagnostics" (InvalidText) or "line", "column" (SharedDefinition)}; Applied -> {"applied"};
/// ControlRefusal -> RpcError{commandRefused, describe(r), {"code": controlRefusalCode(r)}, plus "reason"
/// (NotRestartable)}.
std::expected<nlohmann::json, RpcError> encodeCommandResult(const CommandResult& result);
/// {"protocol", "features", "graphFrequency", ..., "nodes": [each with its health, node-lifecycle-modules.md 5.3],
/// "routes"}.
nlohmann::json encodeGraph(const GraphView& view);
/// {"version", "text", "diagnostics": [{"severity", "code", "message", "line", "column"}], "running"?,
/// "refusal"?: {"code", "message", "line"?, "column"?}, "pendingRestart", "writeFailed"?}: an absent
/// optional is left out, never null.
nlohmann::json encodeGraphFile(const GraphFile& file);
} // namespace ear::control
