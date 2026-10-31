// The graph file keeper's main-loop side (docs/design/knobly-config.md, "Contracts"; S4): the
// desired graph's only editor, and the one holder of the running version and the last refusal.
// Main loop only. It never parses text: what it judges arrives as plain values.
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>
#include "ear/control/graph_file_state.hpp"
#include "ear/control/requests.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "graph_file_model.hpp"
namespace ear::config {
/// The working copy's file fields as they cross to the main loop (plain values).
struct FileState {
  std::string version;
  std::string text;
  std::vector<control::Diagnostic> diagnostics;
  std::vector<control::FormatKey> pendingRestart;   // the file's format against the engine's, FormatKey order
  std::optional<std::string> writeFailed;
};
struct Offer {
  FileState file;
  GraphSpec wanted;               // plan reads only nodes and routes
  Placements placements;
  control::FileChangeCause cause; // edit or disk
  std::uint32_t droppedEdits;     // disk only
};
enum class Verdict { accepted, refused, stopping };
struct Judged { Verdict verdict; std::expected<void, CommandError> result; };
/// Where a refusal points: the named node's line; the first route naming a refused node or port;
/// the route refused; the first node of an unknown type; the second of two nodes sharing a name. Pure.
std::optional<control::TextPlace> placeOf(const CommandError& error, const GraphSpec& wanted, const Placements& placements);
class MainLoopSide {
public:
  MainLoopSide(DesiredGraph& graph, const GraphReader& reader);   // both outlive it
  void subscribe(control::GraphFileListener& listener);           // main thread, before the first offer
  /// Plan. Accepted: running = offer.file.version, listeners told graphFileChanged, commit. Refused
  /// with cause disk: the refusal recorded for that version (place from placeOf), listeners told.
  /// Refused edit: nothing told. Stopping: nothing told.
  Judged offer(Offer offer);
  /// A file change with no plan (errors; a write state): listeners told graphFileChanged.
  void announce(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits);
  /// `file` plus running and refusal. Refusal is present only when it was recorded for file.version.
  control::GraphFile graphFile(FileState file) const;
  GraphView graph() const;                       // reader.read()
  void written(const std::string& version);      // listeners told graphFileWritten
private:
  struct Recorded { std::string version; control::Refusal refusal; };
  void tell(FileState file, control::FileChangeCause cause, std::uint32_t droppedEdits);
  DesiredGraph& graph_;
  const GraphReader& reader_;
  std::vector<control::GraphFileListener*> listeners_;
  std::optional<std::string> running_;
  std::optional<Recorded> refusal_;
};
}
