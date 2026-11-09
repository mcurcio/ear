// The lifecycle's own wire values in ear_control (docs/control-port.md#methods): what a lifecycle request applied, the
// two refusals that are not the desired graph's or the file's, and the `action` record each node.start, node.stop and
// node.restart writes (docs/output-records.md#action). commands.hpp includes this header for Command's five lifecycle
// commands and CommandResult's Applied and ControlRefusal. Pure: no socket, no knobly.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/graph_spec.hpp"
namespace ear::control {

/// A lifecycle request's `applied` (docs/control-port.md#methods): node.start (start, retry, unchanged), node.stop
/// (stop, unchanged), node.restart (restart, retry, unchanged), node.disable (disable, unchanged), node.enable (enable,
/// unchanged). `stored` is node.update's on a disabled node, from slice 3d (docs/design/reconfigure.md); it is here so
/// the one list of names has one home.
enum class AppliedKind { start, stop, restart, retry, disable, enable, stored, unchanged };
/// "start" | "stop" | "restart" | "retry" | "disable" | "enable" | "stored" | "unchanged": the one home of these strings.
std::string_view appliedName(AppliedKind kind) noexcept;
/// A lifecycle request's result: {"applied": appliedName(kind)}.
struct Applied {
	AppliedKind kind;
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
/// The control plane's own refusals (docs/control-port.md#error-codes ControlRefusal; slice 5 adds its other codes
/// here).
using ControlRefusal = std::variant<NodeDisabled, NotRestartable>;
/// "node-disabled" | "not-restartable": the one home of these strings.
std::string_view controlRefusalCode(const ControlRefusal& refusal) noexcept;
/// "block-lost" | "not-started".
std::string_view reasonName(NotRestartable::Reason reason) noexcept;
/// The code plus what it names: "node-disabled porch", "not-restartable desk: block-lost".
std::string describe(const ControlRefusal& refusal);

/// The three lifecycle requests that are not edits (docs/control-port.md#methods).
enum class LifecycleVerb { start, stop, restart };
/// "start" | "stop" | "restart".
std::string_view verbName(LifecycleVerb verb) noexcept;

/// One `action` record (docs/output-records.md#action): an operator's request that edits nothing, answered.
struct ActionRecord {
	NodeName node;
	std::string action; // verbName in 3a; slice 5 adds a type's action names
	std::string result; // "ok", or the refusal's code ("node-disabled", "not-restartable", "unknown-node",
						// "stopping")
	std::optional<std::string> detail; // not-restartable's reason; slice 5: an action's refusal text
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
