// Frozen interface (docs/design/knobly-config.md, the confined folder). Change it only through an
// erratum and a re-cut, never a local edit.
#pragma once
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
namespace ear::folder {
struct FolderError {
	std::string detail;   // a sentence naming the path and the reason
	friend bool operator==(const FolderError&, const FolderError&) = default;
};
struct Replaced {};                                        // it held `expected`; it now holds the new text
struct Changed { std::optional<std::string> current; };   // it did not; it holds `current` (nullopt: nothing), as before the call
using ReplaceOutcome = std::variant<Replaced, Changed>;
/// One folder EAR reads and writes beneath, held open as a directory for EAR's life. Every path is
/// resolved by the kernel inside it (openat2 RESOLVE_BENEATH), so an absolute path or a '..' out is
/// refused. Renames use a descriptor of the parent directory opened that way and a bare name, so
/// they stay beneath too. Any thread; a const folder may be shared.
class ConfinedFolder {
public:
	/// Creates the directory `path` if it is missing (never its parents), then opens it.
	static std::expected<ConfinedFolder, FolderError> open(const std::filesystem::path& path);
	ConfinedFolder(ConfinedFolder&& other) noexcept;
	ConfinedFolder& operator=(ConfinedFolder&& other) noexcept;
	ConfinedFolder(const ConfinedFolder&) = delete;
	ConfinedFolder& operator=(const ConfinedFolder&) = delete;
	~ConfinedFolder();
	const std::filesystem::path& path() const noexcept;
	/// "/proc/self/fd/<fd>": a path that names the held directory itself, for calls that take a path
	/// (inotify_add_watch). Valid while the folder lives.
	std::string heldPath() const;
	/// RecordingsFolder::create's contract, unchanged: RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS (an
	/// inner symlink that stays beneath is followed); created 0644 or truncated; a descriptor the
	/// caller owns (O_CLOEXEC).
	std::expected<int, FolderError> create(const std::string& relative) const;
	/// The regular file's text, reached with no symlink anywhere on the path (RESOLVE_BENEATH |
	/// RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS); std::nullopt when nothing is there. A symlink, a
	/// directory, another kind, or more than maxBytes is a FolderError.
	std::expected<std::optional<std::string>, FolderError> read(const std::string& relative, std::size_t maxBytes) const;
	/// Puts `text` at `relative` only if it held `expected` (nullopt: nothing there), resolved as
	/// read() resolves. Error: nothing changed, except as the error says.
	std::expected<ReplaceOutcome, FolderError> replaceIf(const std::string& relative,
	    const std::optional<std::string>& expected, std::string_view text, std::size_t maxBytes) const;

private:
	ConfinedFolder(int fd, std::filesystem::path path) noexcept;
	int fd_;
	std::filesystem::path path_;
};
/// Boot check: no two of `folders` are one directory, or one inside another. It is judged by device and
/// inode along each resolved path, so a symlinked alias is caught. A path that does not exist is
/// skipped. The error names both.
std::expected<void, FolderError> requireApart(std::span<const std::filesystem::path> folders);
}
