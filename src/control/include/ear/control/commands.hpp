// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>
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
using Command = std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute>;
using CommandResult = std::variant<GraphView, std::expected<void, CommandError>>;
/// Control thread. Named params only, decoded with graph_json. Unknown method: methodNotFound;
/// a missing, extra or mistyped key: invalidParams whose message is the JsonError
/// ("/from/port: missing").
std::expected<Command, RpcError> decodeCommand(std::string_view method,
                                               const nlohmann::json& params);
/// Main thread; called only by MainLoopGateway.
CommandResult runCommand(const Command& command, GraphControl& control,
                         const GraphReader& reader);
/// Control thread. GraphView -> encodeGraph; success -> {}; CommandError ->
/// RpcError{commandRefused, describe(e), {"code": errorCode(e)}}.
std::expected<nlohmann::json, RpcError> encodeCommandResult(const CommandResult& result);
nlohmann::json encodeGraph(const GraphView& view);
}
