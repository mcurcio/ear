// Frozen interface (docs/design/batch2.md section 4.4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <filesystem>
#include <string>
namespace ear::nodes {
/// The one folder "file" nodes write into (--recordings-dir), held open as a directory for
/// EAR's life, so no client-given path can name a file outside it.
class RecordingsFolder {
public:
	/// Creates the directory `path` if it is missing (never its parents) and opens it. The error
	/// is a sentence naming `path` and the reason.
	static std::expected<RecordingsFolder, std::string> open(const std::filesystem::path& path);
	RecordingsFolder(RecordingsFolder&& other) noexcept;
	RecordingsFolder& operator=(RecordingsFolder&& other) noexcept;
	RecordingsFolder(const RecordingsFolder&) = delete;
	RecordingsFolder& operator=(const RecordingsFolder&) = delete;
	~RecordingsFolder();
	/// Opens `relative` beneath the folder for writing: created (mode 0644) or truncated. The
	/// kernel resolves it inside the folder only (openat2 RESOLVE_BENEATH |
	/// RESOLVE_NO_MAGICLINKS), so an absolute path, a ".." or a symlink that leads out is
	/// refused. A descriptor the caller owns (O_CLOEXEC), or the reason it was refused.
	std::expected<int, std::string> create(const std::string& relative) const;
	const std::filesystem::path& path() const noexcept { return path_; }

private:
	RecordingsFolder(int fd, std::filesystem::path path) noexcept;
	int fd_;
	std::filesystem::path path_;
};
}
