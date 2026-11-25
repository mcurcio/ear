// The lifecycle's own wire values in ear_control (docs/control-port.md#methods): what a lifecycle request applied, the
// two refusals that are not the desired graph's or the file's, and the `action` record each node.start, node.stop and
// node.restart writes (docs/output-records.md#action). commands.hpp includes this header for Command's five lifecycle
// commands and CommandResult's Applied and ControlRefusal. Pure: no socket, no knobly.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/node_type.hpp"
namespace ear::control {

/// A lifecycle request's `applied` (docs/control-port.md#methods): node.start (start, retry, unchanged), node.stop
/// (stop, unchanged), node.restart (restart, retry, unchanged), node.disable (disable, unchanged), node.enable (enable,
/// unchanged), and an edit's outcome where it is answered (node.disable, node.enable, node.set: `reconfigure`,
/// `reshape` and `stored` among them, docs/control-port.md#methods); the one list of names has one home.
enum class AppliedKind { start, stop, restart, retry, disable, enable, stored, unchanged, reconfigure, reshape };
/// "start" | "stop" | "restart" | "retry" | "disable" | "enable" | "stored" | "unchanged" | "reconfigure" | "reshape":
/// the one home of these strings.
std::string_view appliedName(AppliedKind kind) noexcept;
/// A request's result: {"applied": appliedName(kind)}, and for a change of the node's ports
/// (docs/control-port.md#changes-of-ports) "inPlace" (a reshape's), "ports" and the routes it removed ("routesDropped"),
/// or, for a dry run, would remove ("routes").
struct Applied {
	AppliedKind kind;
	std::optional<bool> inPlace{};
	std::optional<PortsMoved> ports{};
	std::optional<std::vector<Route>> routesDropped{};
	std::optional<std::vector<Route>> routes{};
	friend bool operator==(const Applied&, const Applied&) = default;
};

/// node.start, node.stop or node.restart on a node `ear.conf` disables.
struct NodeDisabled {
	NodeName node;
	friend bool operator==(const NodeDisabled&, const NodeDisabled&) = default;
};
/// node.start or node.restart on a node whose block is lost; or node.restart on a node still `added`.
struct NotRestartable {
	enum class Reason { blockLost, notStarted };
	NodeName node;
	Reason reason;
	friend bool operator==(const NotRestartable&, const NotRestartable&) = default;
};
/// An edit whose new ports drop ports that wanted routes name, without `dropRoutes`
/// (docs/control-port.md#changes-of-ports): those ports, and every such route.
struct PortsInUse {
	NodeName node;
	std::vector<PortName> ports;
	std::vector<Route> routes;
	friend bool operator==(const PortsInUse&, const PortsInUse&) = default;
};
/// node.invoke of an action the node's type does not have (docs/control-port.md#actions).
struct UnknownAction {
	NodeName node;
	std::string action;
	friend bool operator==(const UnknownAction&, const UnknownAction&) = default;
};
/// node.invoke whose arguments the action's table refuses: the walk's whole text and the key it names (none for an
/// argument list that is not an object).
struct InvalidArgs {
	NodeName node;
	std::string action;
	std::string text;
	std::string key{};
	friend bool operator==(const InvalidArgs&, const InvalidArgs&) = default;
};
/// node.invoke on a node that cannot take the action now: its phase.
struct NotRunning {
	NodeName node;
	NodePhase phase;
	friend bool operator==(const NotRunning&, const NotRunning&) = default;
};
/// node.invoke of a call whose configuration was torn down before it ran.
struct Restarted {
	NodeName node;
	friend bool operator==(const Restarted&, const Restarted&) = default;
};
/// node.invoke whose handler refused: its text.
struct ActionFailed {
	NodeName node;
	std::string action;
	std::string detail;
	friend bool operator==(const ActionFailed&, const ActionFailed&) = default;
};
/// node.invoke whose handler did not answer within 10 s (actionDeadline).
struct ActionTimeout {
	NodeName node;
	std::string action;
	friend bool operator==(const ActionTimeout&, const ActionTimeout&) = default;
};
/// The control plane's own refusals (docs/control-port.md#error-codes ControlRefusal): the lifecycle's, a change of
/// ports', and the actions' (docs/control-port.md#actions).
using ControlRefusal = std::variant<NodeDisabled, NotRestartable, PortsInUse, UnknownAction, InvalidArgs, NotRunning,
                                    Restarted, ActionFailed, ActionTimeout>;
/// "node-disabled" | "not-restartable" | "ports-in-use" | "unknown-action" | "invalid-args" | "not-running" |
/// "restarted" | "action-failed" | "action-timeout": the one home of these strings.
std::string_view controlRefusalCode(const ControlRefusal& refusal) noexcept;
/// invalid-args' words: its text without the key it names ("must be a whole number, 1-10"), as invalid-settings'.
std::string_view argsWords(const InvalidArgs& refusal) noexcept;
/// "block-lost" | "not-started".
std::string_view reasonName(NotRestartable::Reason reason) noexcept;
/// The code plus what it names: "node-disabled porch", "not-restartable desk: block-lost",
/// "ports-in-use desk: play.2 (1 route)", "unknown-action amp: forget", "invalid-args amp add: amount: missing",
/// "not-running amp: stopped", "restarted amp", "action-timeout amp add"; action-failed's is its text alone.
std::string describe(const ControlRefusal& refusal);

/// The three lifecycle requests that are not edits (docs/control-port.md#methods).
enum class LifecycleVerb { start, stop, restart };
/// "start" | "stop" | "restart".
std::string_view verbName(LifecycleVerb verb) noexcept;

/// One `action` record (docs/output-records.md#action): an operator's request that edits nothing, answered.
struct ActionRecord {
	NodeName node;
	std::string action; // verbName (a lifecycle verb), or a type action's name (node.invoke)
	std::string result; // "ok", or the refusal's code ("node-disabled", "not-restartable", "unknown-node",
						// "stopping", or an action's: docs/output-records.md#action)
	std::optional<std::string> detail; // not-restartable's reason; invalid-args' words, action-failed's text,
									   // not-running's phase
	friend bool operator==(const ActionRecord&, const ActionRecord&) = default;
};
/// Who writes each answered lifecycle request on standard output (StatusLog). Told on the main loop by the graph file
/// keeper's main-loop side, before the request's first health change is told, so the record precedes the `node`
/// records it causes (docs/output-records.md#node).
class ActionListener {
public:
	virtual void actionAnswered(const ActionRecord& record) = 0;

protected:
	~ActionListener() = default;
};

} // namespace ear::control
