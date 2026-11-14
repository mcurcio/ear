#include "ear/runtime/recordings.hpp"

#include <utility>

#include <unistd.h>

#include "ear/folder/confined_folder.hpp"

namespace ear::runtime {
namespace {

/// One descriptor ConfinedFolder::create made; closed when destroyed (on the I/O thread, as NodeHost lets go of it).
class Descriptor final : public RecordingFile {
public:
	explicit Descriptor(int fd) noexcept : fd_(fd) {}
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	~Descriptor() override { ::close(fd_); }
	int descriptor() const noexcept override { return fd_; }

private:
	int fd_;
};

} // namespace

ConfinedRecordings::ConfinedRecordings(std::shared_ptr<const folder::ConfinedFolder> folder, IoExecutor& io) noexcept
	: folder_(std::move(folder)), io_(io) {}

IoExecutor& ConfinedRecordings::io() noexcept { return io_; }

std::expected<std::unique_ptr<RecordingFile>, std::string> ConfinedRecordings::open(const NodeName&,
                                                                                    const std::string& path) {
	auto made = folder_->create(path);
	if (!made) return std::unexpected(std::move(made.error().detail));
	return std::make_unique<Descriptor>(*made);
}

} // namespace ear::runtime
