// The nodes' recordings on disk (docs/writing-a-node-type.md#recordings): core's RecordingAdapter over the recordings
// folder and the I/O thread. The disk's part only: when a recording is claimed, opened and closed is NodeHost's.
#pragma once
#include <expected>
#include <memory>
#include <string>
#include "ear/core/io_executor.hpp"
#include "ear/core/recording.hpp"
namespace ear::folder {
class ConfinedFolder;
}
namespace ear::runtime {

/// Every recording beneath one folder (the recordings folder), created or truncated through ConfinedFolder::create
/// (RESOLVE_BENEATH, no ".." out, no subfolder made). Its opens and closes run on `io`.
class ConfinedRecordings final : public RecordingAdapter {
public:
	/// Both outlive it, and `io` outlives every job posted to it.
	ConfinedRecordings(std::shared_ptr<const folder::ConfinedFolder> folder, IoExecutor& io) noexcept;
	IoExecutor& io() noexcept override;
	std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName& node,
	                                                                const std::string& path) override;

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;
	IoExecutor& io_;
};

} // namespace ear::runtime
