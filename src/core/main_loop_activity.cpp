// MainLoopActivity (main_loop_activity.hpp): one seqlock over the call's literal, its length and a fixed copy of the
// node's name. Every field is an atomic, so a reader that races the writer sees a changed sequence and retries; it is
// never a data race.
#include "ear/core/main_loop_activity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ear {
namespace {

constexpr std::size_t wordCount = MainLoopActivity::nameBytes / sizeof(std::uint64_t);
static_assert(MainLoopActivity::nameBytes % sizeof(std::uint64_t) == 0);

struct Shared {
	std::atomic<std::uint64_t> sequence{0};
	std::atomic<const char*> call{nullptr};
	std::atomic<std::size_t> callSize{0};
	std::atomic<std::size_t> nameSize{0};
	std::array<std::atomic<std::uint64_t>, wordCount> name{};
};

Shared shared;

void write(const char* call, std::size_t callSize, std::string_view node) noexcept {
	std::array<std::uint64_t, wordCount> plain{};
	const std::size_t size = std::min(node.size(), MainLoopActivity::nameBytes);
	if (size > 0) std::memcpy(plain.data(), node.data(), size);
	const std::uint64_t sequence = shared.sequence.load(std::memory_order_relaxed);
	shared.sequence.store(sequence + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	shared.call.store(call, std::memory_order_relaxed);
	shared.callSize.store(callSize, std::memory_order_relaxed);
	shared.nameSize.store(size, std::memory_order_relaxed);
	for (std::size_t i = 0; i < wordCount; ++i) shared.name[i].store(plain[i], std::memory_order_relaxed);
	shared.sequence.store(sequence + 2, std::memory_order_release);
}

} // namespace

void MainLoopActivity::set(std::string_view call, std::string_view node) noexcept {
	write(call.data(), call.size(), node);
}

void MainLoopActivity::clear() noexcept { write(nullptr, 0, {}); }

MainLoopActivity::Saved MainLoopActivity::save() noexcept {
	// The main thread is the one writer, so it reads its own last write without the sequence.
	Saved saved;
	saved.call = shared.call.load(std::memory_order_relaxed);
	saved.callSize = shared.callSize.load(std::memory_order_relaxed);
	saved.nameSize = shared.nameSize.load(std::memory_order_relaxed);
	std::array<std::uint64_t, wordCount> plain{};
	for (std::size_t i = 0; i < wordCount; ++i) plain[i] = shared.name[i].load(std::memory_order_relaxed);
	std::memcpy(saved.name.data(), plain.data(), saved.name.size());
	return saved;
}

void MainLoopActivity::restore(const Saved& saved) noexcept {
	write(saved.call, saved.callSize, std::string_view(saved.name.data(), saved.nameSize));
}

std::string MainLoopActivity::current() {
	for (;;) {
		const std::uint64_t before = shared.sequence.load(std::memory_order_acquire);
		if ((before & 1U) != 0) continue;
		const char* call = shared.call.load(std::memory_order_relaxed);
		const std::size_t callSize = shared.callSize.load(std::memory_order_relaxed);
		const std::size_t nameSize = shared.nameSize.load(std::memory_order_relaxed);
		std::array<std::uint64_t, wordCount> plain{};
		for (std::size_t i = 0; i < wordCount; ++i) plain[i] = shared.name[i].load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (shared.sequence.load(std::memory_order_relaxed) != before) continue;
		if (call == nullptr) return {};
		std::string where(call, callSize); // a literal: it outlives every write
		if (nameSize > 0) {
			std::array<char, MainLoopActivity::nameBytes> bytes{};
			std::memcpy(bytes.data(), plain.data(), bytes.size());
			where += ' ';
			where.append(bytes.data(), nameSize);
		}
		return where;
	}
}

} // namespace ear
