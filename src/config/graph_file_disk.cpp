// ear.conf in the graph file folder (graph_file_disk.hpp): the confined folder's read and
// replace-if-unchanged, with its errors as their detail.
#include "graph_file_disk.hpp"

#include <utility>

namespace ear::config {

FolderGraphFileDisk::FolderGraphFileDisk(const folder::ConfinedFolder& folder) : folder_(folder) {}

DiskText FolderGraphFileDisk::read() {
	auto text = folder_.read(std::string(rootDocument), maxDocumentBytes);
	if (!text) return std::unexpected(std::move(text.error().detail));
	return std::move(*text);
}

std::expected<folder::ReplaceOutcome, std::string> FolderGraphFileDisk::replaceIf(const std::optional<std::string>& expected,
                                                                                 std::string_view text) {
	auto outcome = folder_.replaceIf(std::string(rootDocument), expected, text, maxDocumentBytes);
	if (!outcome) return std::unexpected(std::move(outcome.error().detail));
	return std::move(*outcome);
}

} // namespace ear::config
