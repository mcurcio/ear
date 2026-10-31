// The confined folder (confined_folder.hpp): one directory descriptor, opened at boot, beneath
// which every path is resolved by the kernel. openat2 (Linux 5.6) with RESOLVE_BENEATH refuses
// absolute paths, ".." that climbs out and symlinks that lead out, at every step of the lookup;
// RESOLVE_NO_SYMLINKS refuses any symlink at all and RESOLVE_NO_MAGICLINKS /proc-style links.
// glibc has no wrapper for openat2, so it is called through syscall(2).
//
// replaceIf is a compare-and-swap on a file: the new text is staged beside the target, swapped in
// with renameat2(RENAME_EXCHANGE) (or put there with RENAME_NOREPLACE when nothing is), and then
// what it replaced is compared with what the caller expected. A difference swaps it back. So the
// one compare happens after the swap and no read before it can race a writer.
#include "ear/folder/confined_folder.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ear::folder {
namespace {

constexpr std::uint64_t noSymlinks = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

/// Closes the descriptor it holds.
class Descriptor {
public:
	explicit Descriptor(int fd = -1) noexcept : fd_(fd) {}
	Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
	Descriptor& operator=(Descriptor&&) = delete;
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	~Descriptor() {
		if (fd_ >= 0) ::close(fd_);
	}
	int get() const noexcept { return fd_; }
	explicit operator bool() const noexcept { return fd_ >= 0; }

private:
	int fd_;
};

/// openat2 beneath `dirfd`; -1 with errno set on failure.
int openBeneath(int dirfd, const char* relative, std::uint64_t flags, std::uint64_t mode, std::uint64_t resolve) {
	open_how how{};
	how.flags = flags;
	how.mode = mode;
	how.resolve = resolve;
	return static_cast<int>(::syscall(SYS_openat2, dirfd, relative, &how, sizeof how));
}

int renameAt(int dirfd, const std::string& from, const std::string& to, unsigned int flags) {
	return static_cast<int>(::syscall(SYS_renameat2, dirfd, from.c_str(), dirfd, to.c_str(), flags));
}

/// errno as a reason, in the folder's words where the resolve flags chose it.
std::string reason(int error) {
	switch (error) {
	case EXDEV: return "it leads outside the folder";   // how RESOLVE_BENEATH refuses
	case ELOOP: return "a symlink is on its path";        // how RESOLVE_NO_SYMLINKS refuses
	default: return std::strerror(error);
	}
}

FolderError failure(const std::string& relative, const std::string& why) { return FolderError{relative + ": " + why}; }

/// The rest of an open regular file, at most maxBytes; error if it is longer.
std::expected<std::string, std::string> readAll(int fd, std::size_t maxBytes) {
	std::string text;
	std::array<char, 65536> buffer{};
	for (;;) {
		const ssize_t got = ::read(fd, buffer.data(), buffer.size());
		if (got < 0) {
			if (errno == EINTR) continue;
			return std::unexpected(std::string(std::strerror(errno)));
		}
		if (got == 0) return text;
		if (text.size() + static_cast<std::size_t>(got) > maxBytes) {
			return std::unexpected("larger than " + std::to_string(maxBytes) + " bytes");
		}
		text.append(buffer.data(), static_cast<std::size_t>(got));
	}
}

/// The regular file `name` beneath `dirfd` with no symlinks: its text, nullopt when it is missing,
/// or why it cannot be read.
std::expected<std::optional<std::string>, std::string> readFile(int dirfd, const char* name, std::size_t maxBytes) {
	// O_NONBLOCK: opening a FIFO must not wait for a writer; it is refused below as another kind.
	const Descriptor fd(openBeneath(dirfd, name, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC, 0, noSymlinks));
	if (!fd) {
		if (errno == ENOENT) return std::optional<std::string>{};
		return std::unexpected(reason(errno));
	}
	struct stat status{};
	if (::fstat(fd.get(), &status) != 0) return std::unexpected(std::string(std::strerror(errno)));
	if (!S_ISREG(status.st_mode)) return std::unexpected(std::string("not a regular file"));
	if (static_cast<std::uint64_t>(status.st_size) > maxBytes) {
		return std::unexpected("larger than " + std::to_string(maxBytes) + " bytes");
	}
	auto text = readAll(fd.get(), maxBytes);
	if (!text) return std::unexpected(text.error());
	return std::optional<std::string>(std::move(*text));
}

bool writeAll(int fd, std::string_view text) {
	while (!text.empty()) {
		const ssize_t wrote = ::write(fd, text.data(), text.size());
		if (wrote < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		text.remove_prefix(static_cast<std::size_t>(wrote));
	}
	return true;
}

/// One device and inode: the identity of a directory, whatever path names it.
struct Identity {
	dev_t device;
	ino_t inode;
	friend bool operator==(const Identity&, const Identity&) = default;
};

/// The identities of every directory on the resolved path of `folder`, from the root to it; the
/// folder's own is last. Empty when it does not exist.
std::expected<std::vector<Identity>, FolderError> lineage(const std::filesystem::path& folder) {
	std::error_code error;
	const auto resolved = std::filesystem::canonical(folder, error);
	if (error) {
		if (error == std::errc::no_such_file_or_directory) return std::vector<Identity>{};
		return std::unexpected(FolderError{"folder " + folder.string() + ": cannot resolve: " + error.message()});
	}
	std::vector<Identity> identities;
	std::filesystem::path prefix;
	for (const auto& component : resolved) {
		prefix /= component;
		struct stat status{};
		if (::stat(prefix.c_str(), &status) != 0) {
			return std::unexpected(FolderError{"folder " + folder.string() + ": cannot stat " + prefix.string() + ": " +
			                                   std::strerror(errno)});
		}
		identities.push_back({status.st_dev, status.st_ino});
	}
	return identities;
}

bool contains(const std::vector<Identity>& lineage, const Identity& identity) {
	for (const auto& each : lineage) {
		if (each == identity) return true;
	}
	return false;
}

}  // namespace

std::expected<ConfinedFolder, FolderError> ConfinedFolder::open(const std::filesystem::path& path) {
	if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		return std::unexpected(FolderError{"folder " + path.string() + ": cannot create: " + std::strerror(errno)});
	}
	const int fd = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return std::unexpected(FolderError{"folder " + path.string() + ": cannot open: " + std::strerror(errno)});
	}
	return ConfinedFolder(fd, path);
}

ConfinedFolder::ConfinedFolder(int fd, std::filesystem::path path) noexcept : fd_(fd), path_(std::move(path)) {}

ConfinedFolder::ConfinedFolder(ConfinedFolder&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

ConfinedFolder& ConfinedFolder::operator=(ConfinedFolder&& other) noexcept {
	if (this != &other) {
		if (fd_ >= 0) ::close(fd_);
		fd_ = std::exchange(other.fd_, -1);
		path_ = std::move(other.path_);
	}
	return *this;
}

ConfinedFolder::~ConfinedFolder() {
	if (fd_ >= 0) ::close(fd_);
}

const std::filesystem::path& ConfinedFolder::path() const noexcept { return path_; }

std::string ConfinedFolder::heldPath() const { return "/proc/self/fd/" + std::to_string(fd_); }

std::expected<int, FolderError> ConfinedFolder::create(const std::string& relative) const {
	const int fd = openBeneath(fd_, relative.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644,
	                           RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS);
	if (fd < 0) {
		// The node prefixes the path it was given; the reason alone is the detail.
		return std::unexpected(FolderError{errno == EXDEV ? std::string("it leads outside the folder")
		                                                  : std::string(std::strerror(errno))});
	}
	return fd;
}

std::expected<std::optional<std::string>, FolderError> ConfinedFolder::read(const std::string& relative,
                                                                            std::size_t maxBytes) const {
	auto text = readFile(fd_, relative.c_str(), maxBytes);
	if (!text) return std::unexpected(failure(relative, text.error()));
	return std::move(*text);
}

std::expected<ReplaceOutcome, FolderError> ConfinedFolder::replaceIf(const std::string& relative,
                                                                    const std::optional<std::string>& expected,
                                                                    std::string_view text, std::size_t maxBytes) const {
	const std::filesystem::path target(relative);
	const std::string name = target.filename().string();
	if (name.empty() || name == "." || name == "..") return std::unexpected(failure(relative, "not a file name"));
	const std::string parent = target.has_parent_path() ? target.parent_path().string() : std::string(".");

	// 1. The parent, beneath and with no symlinks; then what is at the name, not followed.
	const Descriptor directory(openBeneath(fd_, parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, noSymlinks));
	if (!directory) return std::unexpected(failure(relative, reason(errno)));
	struct stat status{};
	bool exists = true;
	if (::fstatat(directory.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno != ENOENT) return std::unexpected(failure(relative, std::strerror(errno)));
		exists = false;
	} else if (!S_ISREG(status.st_mode)) {
		return std::unexpected(failure(relative, "not a regular file"));
	}

	// 2. The new text, staged beside the target with the target's mode and owner, and synced.
	const std::string staging = "." + name + ".ear-staging";
	const auto removeStaging = [&] { ::unlinkat(directory.get(), staging.c_str(), 0); };
	{
		const mode_t mode = exists ? (status.st_mode & 07777) : 0644;
		const Descriptor staged(
		    openBeneath(directory.get(), staging.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode, noSymlinks));
		if (!staged) return std::unexpected(failure(relative, "cannot stage the new text: " + reason(errno)));
		// The umask narrowed the mode at creation, and a staging file left over kept its own.
		bool written = ::fchmod(staged.get(), mode) == 0;
		if (written && exists && ::fchown(staged.get(), status.st_uid, status.st_gid) != 0 && errno != EPERM) {
			written = false;
		}
		written = written && writeAll(staged.get(), text) && ::fsync(staged.get()) == 0;
		if (!written) {
			const std::string why = std::strerror(errno);
			removeStaging();
			return std::unexpected(failure(relative, "cannot stage the new text: " + why));
		}
	}

	// 3. The swap. A target that appeared or went since step 1 is met as it is now, once.
	bool exchanged = false;
	for (int attempt = 0;; ++attempt) {
		const unsigned int how = exists ? RENAME_EXCHANGE : RENAME_NOREPLACE;
		if (renameAt(directory.get(), staging, name, how) == 0) {
			exchanged = exists;
			break;
		}
		const bool metAsItIsNow = attempt == 0 && errno == (exists ? ENOENT : EEXIST);
		if (!metAsItIsNow) {
			const std::string why = std::strerror(errno);
			removeStaging();
			return std::unexpected(failure(relative, "cannot swap the new text in: " + why));
		}
		exists = !exists;
	}

	// 4. The one compare: what was replaced against what the caller expected.
	const auto syncDirectory = [&] { ::fsync(directory.get()); };
	const std::string mayHold = "; the file may hold the new text";
	if (!exchanged) {
		// Nothing was there.
		if (!expected) {
			syncDirectory();
			return ReplaceOutcome{Replaced{}};
		}
		if (renameAt(directory.get(), name, staging, RENAME_NOREPLACE) != 0) {
			return std::unexpected(failure(relative, std::string("cannot undo: ") + std::strerror(errno) + mayHold));
		}
		removeStaging();
		syncDirectory();
		return ReplaceOutcome{Changed{std::nullopt}};
	}
	auto replaced = readFile(directory.get(), staging.c_str(), maxBytes);
	if (replaced && *replaced && *replaced == expected) {
		removeStaging();
		syncDirectory();
		return ReplaceOutcome{Replaced{}};
	}
	if (renameAt(directory.get(), staging, name, RENAME_EXCHANGE) != 0) {
		return std::unexpected(failure(relative, std::string("cannot undo: ") + std::strerror(errno) + mayHold));
	}
	removeStaging();
	syncDirectory();
	if (!replaced) return std::unexpected(failure(relative, replaced.error()));
	if (!*replaced) return std::unexpected(failure(relative, "it went while being replaced"));
	return ReplaceOutcome{Changed{std::move(**replaced)}};
}

std::expected<void, FolderError> requireApart(std::span<const std::filesystem::path> folders) {
	std::vector<std::vector<Identity>> lineages;
	lineages.reserve(folders.size());
	for (const auto& folder : folders) {
		auto identities = lineage(folder);
		if (!identities) return std::unexpected(identities.error());
		lineages.push_back(std::move(*identities));
	}
	for (std::size_t a = 0; a < folders.size(); ++a) {
		for (std::size_t b = a + 1; b < folders.size(); ++b) {
			if (lineages[a].empty() || lineages[b].empty()) continue;
			// One is the other or inside it when its own identity is on the other's path.
			if (contains(lineages[b], lineages[a].back()) || contains(lineages[a], lineages[b].back())) {
				return std::unexpected(FolderError{"folders " + folders[a].string() + " and " + folders[b].string() +
				                                   " overlap: one is the other or inside it"});
			}
		}
	}
	return {};
}

}  // namespace ear::folder
