// The nodes' stores on disk (docs/writing-a-node-type.md#stores): core's StoreAdapter over the graph file folder and
// the I/O thread, and the boot sweep that narrows every declared store's files before any node exists. The disk's part
// only: what is read, judged and written, and when, is NodeHost's.
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/io_executor.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/store.hpp"
namespace ear::folder {
class ConfinedFolder;
}
namespace ear::runtime {

/// Every store's file in one folder (the graph file folder), each name resolved beneath it with no symlink anywhere on
/// the path, and replaced through a staging file renamed over it (ConfinedFolder's rules). Its jobs run on `io`.
class ConfinedStores final : public StoreAdapter {
public:
	/// Both outlive it, and `io` outlives every job posted to it.
	ConfinedStores(std::shared_ptr<const folder::ConfinedFolder> folder, IoExecutor& io) noexcept;
	IoExecutor& io() noexcept override;
	/// Waits on a condition variable for the job's end (it ran, or `io` destroyed it unrun) or `bound`.
	JobWait await(std::string job, std::move_only_function<void()> work, std::chrono::milliseconds bound) override;
	std::expected<std::optional<std::string>, std::string> read(const std::string& file,
	                                                            std::uint32_t maxBytes) override;
	std::expected<std::optional<std::string>, std::string> readNarrowed(const std::string& file, std::uint32_t maxBytes,
	                                                                    std::uint32_t mode) override;
	std::expected<void, std::string> narrow(const std::string& file, std::uint32_t mode) override;
	std::expected<StoreWrite, std::string> replaceIf(const std::string& file,
	                                                 const std::optional<std::string>& expected, std::string_view text,
	                                                 std::uint32_t maxBytes, std::uint32_t mode) override;

private:
	std::shared_ptr<const folder::ConfinedFolder> folder_;
	IoExecutor& io_;
};

/// What the boot sweep could not narrow: the type whose store it is (a log record's source) and why (it names the
/// file).
struct StoreSweepFinding {
	std::string type;
	std::string detail;
};
/// Boot, before any node: every file in `folder` that is one of `types`' declared stores ("<type>-*.<suffix>"),
/// whatever its node, and each one's staging file a crash left (".<type>-*.<suffix>.ear-staging"), narrowed to its
/// store's mode: a mount (an fsGroup) may widen them each time, and nothing else opens the file of a node no longer in
/// the graph. Blocking, on the calling thread.
std::vector<StoreSweepFinding> sweepStores(const folder::ConfinedFolder& folder,
                                           std::span<const std::unique_ptr<const NodeType>> types);

} // namespace ear::runtime
