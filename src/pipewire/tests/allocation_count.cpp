// See allocation_count.hpp.
#include "allocation_count.hpp"

#include <atomic>
#include <cstddef>

extern "C" {
void* __libc_malloc(std::size_t size) noexcept;
void* __libc_calloc(std::size_t count, std::size_t size) noexcept;
void* __libc_realloc(void* memory, std::size_t size) noexcept;
}

namespace {
std::atomic<std::uint64_t> allocations{0};
thread_local bool counting = false;
void counted() noexcept {
	if (counting) allocations.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

extern "C" {
void* malloc(std::size_t size) noexcept {
	counted();
	return __libc_malloc(size);
}
void* calloc(std::size_t count, std::size_t size) noexcept {
	counted();
	return __libc_calloc(count, size);
}
void* realloc(void* memory, std::size_t size) noexcept {
	counted();
	return __libc_realloc(memory, size);
}
}

namespace ear::pipewire::testing {
std::uint64_t countedAllocations() noexcept { return allocations.load(std::memory_order_relaxed); }
void countAllocationsOnThisThread(bool on) noexcept { counting = on; }
} // namespace ear::pipewire::testing
