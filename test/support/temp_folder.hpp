// The tests' temporary folder (header-only; the target ear_temp_folder puts it on the include path):
// one fresh folder for one test, removed with everything in it, however a test left it.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <stdlib.h>

namespace ear::test {

/// A fresh folder for one test, made with mkdtemp as <tmp>/ear-<label>-XXXXXX (mode 0700) and removed with everything in it.
class TempFolder {
public:
	/// Throws std::runtime_error when mkdtemp fails, which Catch reports as the test's failure.
	explicit TempFolder(std::string_view label = "test") {
		std::string pattern =
			(std::filesystem::temp_directory_path() / ("ear-" + std::string(label) + "-XXXXXX")).string();
		if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed: " + pattern);
		path_ = pattern;
	}

	/// noexcept in effect: every step takes an error_code, and a failure to remove is ignored.
	/// First gives every directory beneath it, at any depth, owner read, write and search, so a test may
	/// leave a folder read-only; then removes the tree.
	~TempFolder() {
		std::error_code ignored;
		openUp(path_);
		std::filesystem::remove_all(path_, ignored);
	}

	TempFolder(const TempFolder&) = delete;
	TempFolder& operator=(const TempFolder&) = delete;

	const std::filesystem::path& path() const noexcept { return path_; }

private:
	/// Opens `directory`'s permissions, then descends into its subdirectories. symlink_status never follows a
	/// link, so a symlink (the tests make ones that lead out of the folder on purpose) is neither chmodded nor entered.
	static void openUp(const std::filesystem::path& directory) {
		namespace fs = std::filesystem;
		std::error_code ignored;
		if (!fs::is_directory(fs::symlink_status(directory, ignored))) return;
		fs::permissions(directory, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
		                fs::perm_options::add | fs::perm_options::nofollow, ignored);
		fs::directory_iterator entries(directory, ignored);
		for (; !ignored && entries != fs::directory_iterator(); entries.increment(ignored)) openUp(entries->path());
	}

	std::filesystem::path path_;
};

} // namespace ear::test
