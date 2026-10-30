// The recordings folder (recordings.hpp): one directory descriptor, opened at boot, beneath
// which every "file" node's path is resolved by the kernel. openat2 (Linux 5.6) with
// RESOLVE_BENEATH refuses absolute paths, ".." that climbs out and symlinks that lead out, at
// every step of the lookup; RESOLVE_NO_MAGICLINKS also refuses /proc-style links. glibc has no
// wrapper for openat2, so it is called through syscall(2).
#include "ear/nodes/recordings.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ear::nodes {

std::expected<RecordingsFolder, std::string> RecordingsFolder::open(const std::filesystem::path& path) {
	if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		return std::unexpected("recordings folder " + path.string() + ": cannot create: " + std::strerror(errno));
	}
	const int fd = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return std::unexpected("recordings folder " + path.string() + ": cannot open: " + std::strerror(errno));
	}
	return RecordingsFolder(fd, path);
}

RecordingsFolder::RecordingsFolder(int fd, std::filesystem::path path) noexcept : fd_(fd), path_(std::move(path)) {}

RecordingsFolder::RecordingsFolder(RecordingsFolder&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

RecordingsFolder& RecordingsFolder::operator=(RecordingsFolder&& other) noexcept {
	if (this != &other) {
		if (fd_ >= 0) ::close(fd_);
		fd_ = std::exchange(other.fd_, -1);
		path_ = std::move(other.path_);
	}
	return *this;
}

RecordingsFolder::~RecordingsFolder() {
	if (fd_ >= 0) ::close(fd_);
}

std::expected<int, std::string> RecordingsFolder::create(const std::string& relative) const {
	open_how how{};
	how.flags = O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC;
	how.mode = 0644;
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
	const long fd = ::syscall(SYS_openat2, fd_, relative.c_str(), &how, sizeof how);
	if (fd < 0) {
		// EXDEV is how RESOLVE_BENEATH says the lookup would have left the folder.
		return std::unexpected(errno == EXDEV ? std::string("it leads outside the recordings folder")
		                                      : std::string(std::strerror(errno)));
	}
	return static_cast<int>(fd);
}

}  // namespace ear::nodes
