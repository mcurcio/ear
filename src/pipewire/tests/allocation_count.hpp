// Allocations counted per thread, for the tests that prove a realtime path allocates nothing (port_block_test.cpp's
// process, spikes_test.cpp's invoked functions on the data thread). allocation_count.cpp replaces malloc, calloc and
// realloc for the whole ear_pipewire_tests binary (glibc supports that) and forwards to glibc's own; operator new
// reaches malloc either way, including handoff_test.cpp's replacement of it. One definition, so every test file of the
// binary shares it.
#pragma once
#include <cstdint>

namespace ear::pipewire::testing {
/// Allocations made so far by every thread while it counted.
std::uint64_t countedAllocations() noexcept;
/// Whether the calling thread's allocations are counted from now on (each thread starts not counting).
void countAllocationsOnThisThread(bool on) noexcept;
} // namespace ear::pipewire::testing
