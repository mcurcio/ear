// One value from one writing thread to one reading thread, the newest only: a seqlock, so the writer never waits and
// the reader retries. The value's bytes travel in atomic words, so a torn read is seen by the sequence and is no data
// race. Realtime-safe on the writing side.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
namespace ear::sendspin {
template <typename Value> class Mailbox {
	static_assert(std::is_trivially_copyable_v<Value>);

public:
	/// One thread; never waits.
	void publish(const Value& value) noexcept {
		std::array<std::uint64_t, wordCount> plain{};
		std::memcpy(plain.data(), &value, sizeof(Value));
		const std::uint64_t sequence = sequence_.load(std::memory_order_relaxed);
		sequence_.store(sequence + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		for (std::size_t i = 0; i < wordCount; ++i) words_[i].store(plain[i], std::memory_order_relaxed);
		sequence_.store(sequence + 2, std::memory_order_release);
	}
	/// One other thread. The newest value, or nothing before the first publish.
	std::optional<Value> read() const noexcept {
		for (;;) {
			const std::uint64_t before = sequence_.load(std::memory_order_acquire);
			if (before == 0) return std::nullopt;
			if ((before & 1U) != 0) continue;
			std::array<std::uint64_t, wordCount> plain{};
			for (std::size_t i = 0; i < wordCount; ++i) plain[i] = words_[i].load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence_.load(std::memory_order_relaxed) != before) continue;
			Value value;
			std::memcpy(&value, plain.data(), sizeof(Value));
			return value;
		}
	}

private:
	static constexpr std::size_t wordCount = (sizeof(Value) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
	std::atomic<std::uint64_t> sequence_{0};
	std::array<std::atomic<std::uint64_t>, wordCount> words_{};
};
} // namespace ear::sendspin
