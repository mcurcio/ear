#pragma once
#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>
#include "ear/control/companions_wire.hpp"
#include "ear/control/graph_file_state.hpp"
#include "ear/control/lifecycle_wire.hpp"
#include "ear/control/rpc.hpp"
#include "ear/core/graph_view.hpp"
// The control port's methods (docs/control-port.md) as typed commands.
namespace ear::control {
struct GetGraph {}; // graph.get, graph.subscribe
struct AddNode {
	NodeSpec node;
};
struct UpdateNode {
	NodeName node;
	NodeSettings settings;
	bool dropRoutes =
		false; // feature reshape: remove the routes its new ports strand (docs/control-port.md#changes-of-ports)
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
struct SetNodeText {
	NodeName node;
	std::string text;
	std::string version;
}; // node.setText {"name", "text", "version"} (feature node-text)
struct AddNodeText {
	std::string text;
	std::string version;
}; // node.addText {"text", "version"} (feature node-text)
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
/// node.set {"name", "settings", "dryRun"?, "dropRoutes"?} (feature node.set; dryRun, feature dryRun): `settings` is a
/// JSON merge patch of the node's settings, null removing a key (docs/control-port.md#nodeset-and-dry-runs).
struct SetNode {
	NodeName node;
	NodeSettings settings;
	bool dryRun = false;
	bool dropRoutes = false;
};
/// node.invoke {"name", "action", "args"?} (feature actions): one of the node's type actions, `args` an object or null
/// when absent (docs/control-port.md#actions).
struct InvokeNode {
	NodeName node;
	std::string action;
	NodeSettings args = nullptr;
};
using Command = std::variant<GetGraph, AddNode, UpdateNode, RemoveNode, AddRoute, RemoveRoute, GetGraphFile,
                             SetGraphFile, StartNode, StopNode, RestartNode, DisableNode, EnableNode, SetNodeText,
                             AddNodeText, SetNode, InvokeNode>;
using CommandResult =
	std::variant<GraphView, std::expected<void, CommandError>, GraphFile, FileRefusal, Applied, ControlRefusal>;
/// The wire's frame (docs/control-port.md#protocol-and-features): graph.get's "protocol" and "features".
inline constexpr int wireProtocol = 1;
/// A node's own knobly text: nodeTexts on the GraphFile and node.setText (docs/control-port.md#a-nodes-own-text).
inline constexpr std::string_view nodeTextFeature = "node-text";
/// A change of a node's ports kept as the same node: applied reshape, node.updated's ports, dropRoutes and ports-in-use
/// (docs/control-port.md#changes-of-ports).
inline constexpr std::string_view reshapeFeature = "reshape";
/// node.set, a merge of a node's settings answered with what it applied, and its dry runs
/// (docs/control-port.md#nodeset-and-dry-runs).
inline constexpr std::string_view nodeSetFeature = "node.set";
inline constexpr std::string_view dryRunFeature = "dryRun";
/// The node types' schemas: types.get and the Graph's typeSchemaHash (docs/control-port.md#type-schemas); with it,
/// invalid-settings carries data.key and data.detail (docs/control-port.md#error-codes).
inline constexpr std::string_view typesFeature = "types";
/// node.invoke and each type schema's actions (docs/control-port.md#actions).
inline constexpr std::string_view actionsFeature = "actions";
inline constexpr std::array<std::string_view, 10> wireFeatures{
	"lifecycle",    "counters",    companionsFeature, nodeTextFeature, reshapeFeature,
	nodeSetFeature, dryRunFeature, typesFeature,      "status",        actionsFeature};
/// Control thread. Named params only, decoded with graph_json. Unknown method: methodNotFound;
/// a missing, extra or mistyped key: invalidParams whose message is the JsonError
/// ("/from/port: missing") and whose data is {"pointer"} (the JSON pointer it names). graphFile.get and
/// graphFile.subscribe (params absent or {}) are GetGraphFile; graphFile.set is SetGraphFile (text and version, each a
/// required string); node.setText is SetNodeText (name, text and version); node.addText is AddNodeText (text and
/// version); node.start, node.stop, node.restart, node.disable and node.enable take {"name"}; node.invoke is
/// InvokeNode ({"name", "action", "args"?}: action a non-empty string, args an object). The
/// graph file keeper runs every command (requests.hpp).
std::expected<Command, RpcError> decodeCommand(std::string_view method, const nlohmann::json& params);
/// Control thread. GraphView -> encodeGraph; success -> {}; CommandError ->
/// RpcError{commandRefused, describe(e), {"code": errorCode(e)}}; GraphFile -> encodeGraphFile;
/// FileRefusal -> RpcError{commandRefused, describe(r), {"code": fileRefusalCode(r)}, plus
/// "diagnostics" (InvalidText) or "line", "column" (SharedDefinition)}; Applied -> {"applied"};
/// ControlRefusal -> RpcError{commandRefused, describe(r), {"code": controlRefusalCode(r)}, plus "reason"
/// (NotRestartable), "ports" and "routes" (PortsInUse), "action" (UnknownAction), "key" and "detail" (InvalidArgs),
/// "phase" (NotRunning), "detail" (ActionFailed)}.
std::expected<nlohmann::json, RpcError> encodeCommandResult(const CommandResult& result);
/// {"protocol", "features", "graphFrequency", ..., "nodes": [each with its health, docs/control-port.md#node-health],
/// "routes"}.
nlohmann::json encodeGraph(const GraphView& view);
/// {"version", "text", "diagnostics": [{"severity", "code", "message", "line", "column"}], "running"?,
/// "refusal"?: {"code", "message", "line"?, "column"?}, "pendingRestart", "writeFailed"?, "nodeTexts"?: [{"name", "start": {"line", "column"}, "end": {...},
/// "shared"?: true}]}: an absent
/// optional is left out, never null.
nlohmann::json encodeGraphFile(const GraphFile& file);
} // namespace ear::control
