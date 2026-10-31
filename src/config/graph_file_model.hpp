// The graph file model (docs/design/knobly-config.md, "File format"): one knobly graph of ear.conf
// and its read with ear's directives. A keeper-thread object; no knobly type is in this header, and
// ear_config's headers are its own (no other package includes them).
#pragma once
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ear/control/graph_file_state.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_type.hpp"
namespace ear::config {
inline constexpr std::string_view rootDocument = "ear.conf";
// ear's own diagnostic codes (knobly program codes must hold a '/'): their one home
inline constexpr std::string_view missingFileCode = "ear/missing-file";
inline constexpr std::string_view unreadableFileCode = "ear/unreadable-file";
inline constexpr std::string_view includeUnsupportedCode = "ear/include-unsupported";
inline constexpr std::string_view unknownDirectiveCode = "ear/unknown-directive";
inline constexpr std::string_view badDirectiveCode = "ear/bad-directive";
inline constexpr std::string_view missingDirectiveCode = "ear/missing-directive";
/// ear.conf as read from disk: its text; std::nullopt when it is not there; or why it cannot be read.
using DiskText = std::expected<std::optional<std::string>, std::string>;
/// Where each wanted item is written: parallel to GraphSpec::nodes and GraphSpec::routes.
struct Placements { std::vector<control::TextPlace> nodes; std::vector<control::TextPlace> routes; };
struct GraphFileRead {
  std::vector<control::Diagnostic> diagnostics;  // knobly's in its order, then ear's in text order
  std::optional<AudioFormat> format;             // both lines read, with no diagnostic placed on them
  std::optional<GraphSpec> wanted;               // only when no diagnostic is an error
  Placements placements;                         // empty when wanted is absent
  bool hasErrors() const noexcept;
};
/// One knobly graph of ear.conf and its read. Used on one thread at a time (knobly handles are not
/// thread-safe); it may be moved to a thread before that thread starts.
class GraphFileModel {
public:
  /// Pure (no I/O): knobly's processor with no extension registered, over knobly's memory source
  /// holding only rootDocument. Missing: text "", exactly one error missingFileCode at 1:1
  /// ("ear.conf is not there"). Unreadable: text "", exactly one error unreadableFileCode at 1:1
  /// ("ear.conf cannot be read: <detail>"). `catalogue` outlives the model.
  static GraphFileModel load(const DiskText& disk, const NodeCatalogue& catalogue);
  GraphFileModel(GraphFileModel&& other) noexcept;
  GraphFileModel& operator=(GraphFileModel&& other) noexcept;
  ~GraphFileModel();
  const std::string& text() const noexcept;      // "" when missing or unreadable
  const GraphFileRead& read() const noexcept;    // worked out at load
private:
  struct Impl; std::unique_ptr<Impl> impl_;
  explicit GraphFileModel(std::unique_ptr<Impl> impl) noexcept;
};
}
