// Allocations counted per thread, for the tests that prove a realtime path allocates nothing (ear_pipewire_tests'
// port block's process and invoked functions on the data thread, ear_core_tests' code reporting from process).
// allocation_count.cpp replaces malloc, calloc and realloc for the whole test program that links ear_allocation_count
// (glibc supports that) and forwards to glibc's own; operator new reaches malloc. One definition, so every test file of
// the program shares it.
#pragma once
#include <cstdint>

namespace ear::testing {
/// Allocations made so far by every thread while it counted.
std::uint64_t countedAllocations() noexcept;
/// Whether the calling thread's allocations are counted from now on (each thread starts not counting).
void countAllocationsOnThisThread(bool on) noexcept;
} // namespace ear::testing
