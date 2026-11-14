#include "ear/runtime/stores.hpp"

#include <condition_variable>
#include <mutex>
#include <utility>
#include <variant>

#include <sys/types.h>

#include "ear/folder/confined_folder.hpp"

namespace ear::runtime {
namespace {

/// One waited job's end, told by the job's own destruction: it ran, or was destroyed unrun (a stopped I/O thread).
struct JobEnd {
	std::mutex mutex;
	std::condition_variable told;
	std::optional<JobWait> end; // mutex
};
class EndSignal {
public:
	explicit EndSignal(std::shared_ptr<JobEnd> end) noexcept : end_(std::move(end)) {}
	EndSignal(const EndSignal&) = delete;
	EndSignal& operator=(const EndSignal&) = delete;
	~EndSignal() {
		{
			const std::scoped_lock lock(end_->mutex);
			end_->end = ran ? JobWait::ran : JobWait::dropped;
		}
		end_->told.notify_all();
	}
	bool ran = false;

private:
	std::shared_ptr<JobEnd> end_;
};

/// Whether `name` is a file of the store `suffix` of `type`, or one's staging file.
bool storeFile(std::string_view name, std::string_view type, std::string_view suffix) {
	const auto matches = [&](std::string_view file) {
		const std::string prefix = std::string(type) + "-";
		const std::string ending = "." + std::string(suffix);
		return file.size() > prefix.size() + ending.size() && file.starts_with(prefix) && file.ends_with(ending);
	};
	constexpr std::string_view staged = ".ear-staging";
	if (name.starts_with('.') && name.ends_with(staged))
		return matches(name.substr(1, name.size() - 1 - staged.size()));
	return matches(name);
}

} // namespace

ConfinedStores::ConfinedStores(std::shared_ptr<const folder::ConfinedFolder> folder, IoExecutor& io) noexcept
	: folder_(std::move(folder)), io_(io) {}

IoExecutor& ConfinedStores::io() noexcept { return io_; }

JobWait ConfinedStores::await(std::string job, std::move_only_function<void()> work, std::chrono::milliseconds bound) {
	auto end = std::make_shared<JobEnd>();
	auto signal = std::make_shared<EndSignal>(end);
	io_.post(std::move(job), [signal = std::move(signal), work = std::move(work)]() mutable {
		work();
		signal->ran = true;
	});
	std::unique_lock lock(end->mutex);
	if (!end->told.wait_for(lock, bound, [&] { return end->end.has_value(); })) return JobWait::overdue;
	return *end->end;
}

std::expected<std::optional<std::string>, std::string> ConfinedStores::read(const std::string& file,
                                                                            std::uint32_t maxBytes) {
	auto read = folder_->read(file, maxBytes);
	if (!read) return std::unexpected(std::move(read.error().detail));
	return std::move(*read);
}

std::expected<void, std::string> ConfinedStores::narrow(const std::string& file, std::uint32_t mode) {
	auto narrowed = folder_->restrictMode(file, static_cast<mode_t>(mode));
	if (!narrowed) return std::unexpected(std::move(narrowed.error().detail));
	return {};
}

std::expected<StoreWrite, std::string> ConfinedStores::replaceIf(const std::string& file,
                                                                 const std::optional<std::string>& expected,
                                                                 std::string_view text, std::uint32_t maxBytes,
                                                                 std::uint32_t mode) {
	auto replaced = folder_->replaceIf(file, expected, text, maxBytes, static_cast<mode_t>(mode));
	if (!replaced) return std::unexpected(std::move(replaced.error().detail));
	return std::holds_alternative<folder::Replaced>(*replaced) ? StoreWrite::replaced : StoreWrite::changed;
}

std::vector<StoreSweepFinding> sweepStores(const folder::ConfinedFolder& folder,
                                           std::span<const std::unique_ptr<const NodeType>> types) {
	std::vector<StoreSweepFinding> findings;
	bool anyStore = false;
	for (const auto& type : types) anyStore = anyStore || !type->stores().empty();
	if (!anyStore) return findings; // nothing declared: the folder is not even listed
	auto names = folder.fileNames();
	if (!names) {
		for (const auto& type : types)
			if (!type->stores().empty()) findings.push_back({std::string(type->name()), names.error().detail});
		return findings;
	}
	for (const auto& type : types)
		for (const StoreDeclaration& store : type->stores())
			for (const std::string& name : *names) {
				if (!storeFile(name, type->name(), store.suffix)) continue;
				if (auto narrowed = folder.restrictMode(name, static_cast<mode_t>(store.rule.mode)); !narrowed)
					findings.push_back({std::string(type->name()), narrowed.error().detail});
			}
	return findings;
}

} // namespace ear::runtime
