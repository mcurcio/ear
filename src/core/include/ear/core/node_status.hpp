// What NodeHost tells about a node's status and events (docs/architecture.md#status-and-events): the status view's
// changes, as written on standard output (paced per field) and as sent on the wire (coalesced per tick), its reset
// when a configuration closes, and each event. Main thread.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ear/core/graph_spec.hpp"
#include "ear/core/status_spec.hpp"
namespace ear {
/// One field of a node's status view: its name (the spec's, static) and its value.
struct FieldValue {
	std::string_view name;
	StatusValue value;
	friend bool operator==(const FieldValue&, const FieldValue&) = default;
};
/// One change of a field as standard output writes it (docs/output-records.md#status): `from` is the value the field's
/// last record wrote (its initial value before any), `to` the view's. A trailing record, written once the field's pace
/// allows after changes it held back, carries their count in `heldBack`, so the field's last record always matches the
/// view.
struct StatusWrite {
	std::string_view field;
	StatusValue from;
	StatusValue to;
	std::uint64_t heldBack = 0;
	std::chrono::steady_clock::time_point at{};
};
/// One event a node raised (docs/writing-a-node-type.md#events), past the node's event pace: its code, severity and
/// arguments by name (a choice's as its text), `msg` its sentence with each place filled, `at` when it was written.
struct RaisedEvent {
	std::string_view code;
	EventSeverity severity;
	std::vector<FieldValue> args;
	std::string msg;
	std::chrono::steady_clock::time_point at{};
};
/// The JSON-free text of a value, as an event's msg and a status record's msg give it: a whole number in decimal, a
/// decimal with `decimals` places, a flag `true` or `false`, a text as it is, null `unknown`.
std::string valueText(const StatusValue& value, std::uint8_t decimals = 2);

/// A text value made safe on its writer's thread (docs/writing-a-node-type.md#status-fields): bytes that are not UTF-8
/// become U+FFFD, a tab, newline, carriage return, U+2028 or U+2029 a space and any other control character (C0, DEL
/// and C1) U+FFFD, and a text past `maxBytes` is cut at a character's boundary so that with "…" appended it fits.
std::string safeText(std::string_view text, std::size_t maxBytes);

class NodeStatusListener {
public:
	/// A field's change as standard output writes it, paced per node and field.
	virtual void statusWritten(const NodeName& node, const StatusWrite& write) {
		static_cast<void>(node);
		static_cast<void>(write);
	}
	/// The fields changed since the node's last statusChanged, each at its value now: at most one per node per tick,
	/// and before an event or a loss that followed them.
	virtual void statusChanged(const NodeName& node, std::span<const FieldValue> fields) {
		static_cast<void>(node);
		static_cast<void>(fields);
	}
	/// A configuration closed (or leaked) after its last values were told: every field at its initial value.
	virtual void statusReset(const NodeName& node, std::span<const FieldValue> fields) {
		static_cast<void>(node);
		static_cast<void>(fields);
	}
	virtual void eventRaised(const NodeName& node, const RaisedEvent& event) {
		static_cast<void>(node);
		static_cast<void>(event);
	}
	/// Whether a superseded node's status and events reach this listener (as NodeHealthListener's).
	virtual bool hearsSuperseded() const noexcept { return true; }

protected:
	~NodeStatusListener() = default;
};
} // namespace ear
