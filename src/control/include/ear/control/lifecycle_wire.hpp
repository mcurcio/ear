// The lifecycle's own wire values in ear_control (docs/design/control-plane.md 4.2, 4.5 and 9, the wire's home;
// docs/design/node-lifecycle-modules.md section 5, the home of these declarations, step L-6 of slice 3a): what a
// lifecycle request applied, the two refusals that are not the desired graph's or the file's, and the `action` record
// each node.start, node.stop and node.restart writes. commands.hpp includes this header from step L-6, when Command
// gains the five lifecycle commands and CommandResult gains Applied and ControlRefusal; until then nothing includes it.
// Pure: no socket, no knobly.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/graph_spec.hpp"
namespace ear::control {

/// A lifecycle request's `applied` (control-plane.md 4.2): node.start (start, retry, unchanged), node.stop (stop,
/// unchanged), node.restart (restart, retry, unchanged), node.disable (disable, unchanged), node.enable (enable,
/// unchanged). `stored` is node.update's on a disabled node, from slice 3d's step L3 (D-M9); it is here so the one list
/// of names has one home.
enum class AppliedKind { start, stop, restart, retry, disable, enable, stored, unchanged };
/// "start" | "stop" | "restart" | "retry" | "disable" | "enable" | "stored" | "unchanged": the one home of these strings.
std::string_view appliedName(AppliedKind kind) noexcept;
/// A lifecycle request's result: {"applied": appliedName(kind)}.
struct Applied {
	AppliedKind kind;
	friend bool operator==(const Applied&, const Applied&) = default;
};

/// node.start, node.stop or node.restart on a node `ear.conf` disables (D-F2).
struct NodeDisabled {
	NodeName node;
	friend bool operator==(const NodeDisabled&, const NodeDisabled&) = default;
};
/// node.start or node.restart on a node whose block is lost (I1); or node.restart on a node still `added`.
struct NotRestartable {
	enum class Reason { blockLost, notStarted };
	NodeName node;
	Reason reason;
	friend bool operator==(const NotRestartable&, const NotRestartable&) = default;
};
/// The control plane's own refusals (control-plane.md 4.5's ControlRefusal; slice 5 adds its other codes here).
using ControlRefusal = std::variant<NodeDisabled, NotRestartable>;
/// "node-disabled" | "not-restartable": the one home of these strings.
std::string_view controlRefusalCode(const ControlRefusal& refusal) noexcept;
/// "block-lost" | "not-started".
std::string_view reasonName(NotRestartable::Reason reason) noexcept;
/// The code plus what it names: "node-disabled porch", "not-restartable desk: block-lost".
std::string describe(const ControlRefusal& refusal);

/// The three lifecycle requests that are not edits (control-plane.md 7.6).
enum class LifecycleVerb { start, stop, restart };
/// "start" | "stop" | "restart".
std::string_view verbName(LifecycleVerb verb) noexcept;

/// One `action` record (control-plane.md 9): an operator's request that edits nothing, answered.
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
/// records it causes (control-plane.md 9.1).
class ActionListener {
public:
	virtual void actionAnswered(const ActionRecord& record) = 0;

protected:
	~ActionListener() = default;
};

} // namespace ear::control
