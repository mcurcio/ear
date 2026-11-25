// What a type's status fields and events are, as the framework reads them (docs/writing-a-node-type.md#status-and-events):
// the declarations status_spec.hpp's makers fill in, which NodeType's schema and NodeHost read. Only the standard
// library and the log's levels, so node_type.hpp can name them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include "ear/core/log.hpp"
namespace ear {
/// Where a client shows a status field (docs/writing-a-node-type.md#status-fields): only in the node's panel, also on
/// its box, or as a banner while it is not at its initial value.
enum class Display { panel, box, callout };
std::string_view displayName(Display display) noexcept; // "panel" | "box" | "callout"
/// How an event reads to an operator. Not a graph file diagnostic's Severity.
enum class EventSeverity { info, warn, error };
std::string_view severityName(EventSeverity severity) noexcept; // "info" | "warn" | "error"
/// The one vocabulary of value kinds for status fields and event arguments (docs/control-port.md#type-schemas).
enum class ValueKind { flag, wholeNumber, decimal, choice, text };
std::string_view valueKindName(ValueKind kind) noexcept; // "flag" | "wholeNumber" | "decimal" | "choice" | "text"

namespace writer {
/// Who writes a status field: the node's code on the realtime thread (through Io, slice 5.3), one of the spec's loops
/// (through Report, from that loop's thread), or a companion kind's adapter (slice 8).
struct Writer {
	enum class Kind { rt, loop, companion };
	Kind kind;
	std::string_view loop = {}; // Kind::loop: the loop's name
};
} // namespace writer

/// A status value without its field's type: unknown (null), a flag, a whole number, a decimal or a text. A choice's
/// value is its text.
using StatusValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// When a field is judged flapping (ear.status-flapping): more than `transitions` changes within `windowMs`.
struct FlapsAbove {
	std::uint32_t transitions = 10;
	std::uint32_t windowMs = 10'000;
};

/// One status field as the framework reads it: what the type schema sends and the status view starts from. Every view
/// is of static storage (the type's spec).
struct StatusDeclaration {
	std::string_view name;
	ValueKind kind;
	writer::Writer writer;
	Display display = Display::panel;
	std::string_view help = {};
	FlapsAbove flapsAbove = {};
	std::int64_t low = 0;  // wholeNumber
	std::int64_t high = 0; // wholeNumber
	double lowDecimal = 0; // decimal
	double highDecimal = 0;
	std::uint8_t decimals = 2;
	std::string_view unit = {};
	std::span<const std::string_view> values = {}; // choice
	std::uint32_t maxBytes = 0;                    // text
	bool initialUnknown = false;
	bool initialFlag = false;
	std::int64_t initialNumber = 0;
	double initialDecimal = 0;
	std::size_t initialChoice = 0;
	std::string_view initialText = {};
	/// The field's value until its writer writes one (docs/writing-a-node-type.md#status-fields).
	StatusValue initial() const;
};

/// One argument of an event or a log code as the framework reads it.
struct ArgDeclaration {
	std::string_view name;
	ValueKind kind;
	std::int64_t low = 0;
	std::int64_t high = 0;
	std::uint8_t decimals = 2;
	std::span<const std::string_view> values = {}; // choice
	std::uint32_t maxBytes = 0;                    // text
};

/// One event as the framework reads it.
struct EventDeclaration {
	std::string_view code;
	EventSeverity severity;
	std::string_view sentence; // its record's msg and the editor's line, with {arg} places
	std::span<const ArgDeclaration> args;
};

/// One realtime log code as the framework reads it (docs/writing-a-node-type.md#log-codes): what Io::log writes.
struct LogCodeDeclaration {
	std::string_view code;
	LogLevel level;
	std::string_view sentence;
	std::span<const ArgDeclaration> args;
};

} // namespace ear
