// The graph file keeper's one file (docs/design/knobly-config.md, design rule 1): ear.conf in the
// graph file folder, read and replaced only if it still holds what the keeper last saw. The seam
// the keeper's tests may replace (S4).
#pragma once
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include "ear/folder/confined_folder.hpp"
#include "graph_file_model.hpp"
namespace ear::config {
inline constexpr std::size_t maxDocumentBytes = 1 << 20;
class GraphFileDisk {
public:
  virtual DiskText read() = 0;   // ear.conf's text, nullopt when missing, or why it cannot be read
  /// Puts `text` in place only if ear.conf still holds `expected` (ConfinedFolder::replaceIf). Error: nothing changed.
  virtual std::expected<folder::ReplaceOutcome, std::string> replaceIf(const std::optional<std::string>& expected, std::string_view text) = 0;
protected:
  ~GraphFileDisk() = default;
};
class FolderGraphFileDisk final : public GraphFileDisk {   // rootDocument in `folder`, maxDocumentBytes; FolderError -> its detail
public:
  explicit FolderGraphFileDisk(const folder::ConfinedFolder& folder);   // folder outlives it
  DiskText read() override;
  std::expected<folder::ReplaceOutcome, std::string> replaceIf(const std::optional<std::string>& expected, std::string_view text) override;
private:
  const folder::ConfinedFolder& folder_;
};
}
