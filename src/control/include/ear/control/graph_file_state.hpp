// Interface page: docs/design/knobly-config.md (slice S1). The plain types of ear.conf's state on the
// wire: a place in the text and a problem placed there. No knobly type is ever in this header.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace ear::control {
/// A place in ear.conf in the wire's units: line from 1; column from 1 in UTF-16 code units (the browser's).
struct TextPlace {
  std::uint32_t line;
  std::uint32_t column;
  friend bool operator==(const TextPlace&, const TextPlace&) = default;
};
enum class Severity { error, warning };
std::string_view severityName(Severity severity) noexcept;   // "error" | "warning"
/// One problem in ear.conf: knobly's ("E2001") or ear's own ("ear/<name>").
struct Diagnostic {
  Severity severity; std::string code; std::string message; TextPlace place;
  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};
}
