// Interface page: docs/design/knobly-config.md (slices S1 and S3). The plain types of ear.conf's state on
// the wire: a place in the text and a problem placed there; the file's state as a client holds it and
// each change to it; and the graph file's own refusals. No knobly type is ever in this header.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "ear/core/command_error.hpp"
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
enum class FormatKey { graphFrequency, blockSize };
std::string_view formatKeyName(FormatKey key) noexcept;      // "graphFrequency" | "blockSize": the wire's pendingRestart
struct Refusal { CommandError error; std::optional<TextPlace> place; };   // the desired graph refused `version`
struct GraphFile {
  std::string version;                    // opaque; changes whenever the text changes
  std::string text;                       // ear.conf's text; "" when missing or unreadable
  std::vector<Diagnostic> diagnostics;    // any error means the file does not apply
  std::optional<std::string> running;     // the version the graph runs; absent: none yet
  std::optional<Refusal> refusal;         // only when the desired graph refused `version`
  std::vector<FormatKey> pendingRestart;  // FormatKey order
  std::optional<std::string> writeFailed; // the disk refused the last write
};
enum class FileChangeCause { edit, disk, write };
std::string_view causeName(FileChangeCause cause) noexcept;   // "edit" | "disk" | "write"
struct GraphFileChanged { GraphFile file; FileChangeCause cause; std::uint32_t droppedEdits; };  // > 0 only with disk
struct FileChanged {};                                        // graphFile.set: version is stale
struct InvalidText { std::vector<Diagnostic> diagnostics; };  // graphFile.set: the text has errors
struct FileInvalid {};                                        // node.*/route.*: the working copy has errors
struct SharedDefinition { TextPlace place; };                 // the line also makes other nodes or routes
using FileRefusal = std::variant<FileChanged, InvalidText, FileInvalid, SharedDefinition>;
/// "file-changed" | "invalid-text" | "file-invalid" | "shared-definition": the one home of these strings.
std::string_view fileRefusalCode(const FileRefusal& refusal) noexcept;
/// The code plus what it names: "shared-definition 6:5"; the others are the bare code.
std::string describe(const FileRefusal& refusal);
}
