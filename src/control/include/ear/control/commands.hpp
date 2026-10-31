// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit. The graphFile.* commands and
// results are docs/design/knobly-config.md's (slices S3 and S5, which deleted runCommand: the graph
// file keeper runs commands); their erratum to batch2.md lands in S7.
#pragma once
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>
#include "ear/control/graph_file_state.hpp"
#include "ear/control/rpc.hpp"
#include "ear/core/graph_view.hpp"
// The control port's methods (docs/design/batch2.md section 6) as typed commands.
namespace ear::control {
struct GetGraph {};                                        // graph.get, graph.subscribe
struct AddNode { NodeSpec node; };
struct UpdateNode { NodeName node; NodeSettings settings; };
struct RemoveNode { NodeName node; };
struct AddRoute { Route route; };
struct RemoveRoute { Route route; };
struct GetGraphFile {};                                   // graphFile.get, graphFile.subscribe
struct SetGraphFile { std::string text; std::string version; };   // graphFile.set
using Command = std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute, GetGraphFile, SetGraphFile>;
using CommandResult = std::variant<GraphView, std::expected<void, CommandError>, GraphFile, FileRefusal>;
/// Control thread. Named params only, decoded with graph_json. Unknown method: methodNotFound;
/// a missing, extra or mistyped key: invalidParams whose message is the JsonError
/// ("/from/port: missing"). graphFile.get and graphFile.subscribe (params absent or {}) are
/// GetGraphFile; graphFile.set is SetGraphFile (text and version, each a required string). The
/// graph file keeper runs every command (requests.hpp).
std::expected<Command, RpcError> decodeCommand(std::string_view method,
                                               const nlohmann::json& params);
/// Control thread. GraphView -> encodeGraph; success -> {}; CommandError ->
/// RpcError{commandRefused, describe(e), {"code": errorCode(e)}}; GraphFile -> encodeGraphFile;
/// FileRefusal -> RpcError{commandRefused, describe(r), {"code": fileRefusalCode(r)}, plus
/// "diagnostics" (InvalidText) or "line", "column" (SharedDefinition)}.
std::expected<nlohmann::json, RpcError> encodeCommandResult(const CommandResult& result);
nlohmann::json encodeGraph(const GraphView& view);
/// {"version", "text", "diagnostics": [{"severity", "code", "message", "line", "column"}], "running"?,
/// "refusal"?: {"code", "message", "line"?, "column"?}, "pendingRestart", "writeFailed"?}: an absent
/// optional is left out, never null.
nlohmann::json encodeGraphFile(const GraphFile& file);
}
