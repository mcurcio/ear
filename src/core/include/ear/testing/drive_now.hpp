// Test support: a port block driven and waited for on the calling thread, which must be the block's main thread with
// its loop not running (an engine test between Waiter::until calls). PortBlock has no blocking drive
// (docs/engine.md#drive-and-fade); engine tests that only need a code running, or silenced, before they go
// on use this instead: the asynchronous drive, then settle() until its completion ran. When it returns true, the data
// loop applied the request between two cycles, so no process() of the code before it runs again.
//
// A caller REQUIREs driveNow's result. A destructor, which must not free a code the data loop may still run, calls
// silenceOrAbort: it waits as long as it takes on a loaded host, and ends the process with a message if the silence
// never completes (a data loop that runs no invoke), rather than go on to a use after free.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "ear/core/node_host.hpp"
#include "sleeps.hpp"
namespace ear::testing {
/// Drives `block` with `code` (null: silence), unfaded, and waits until the drive completed; false past `deadline`.
/// It polls settle() every 100 microseconds, sleeping between, so a loaded host is not loaded further.
[[nodiscard]] inline bool driveNow(PortBlock& block, AudioCode* code,
                                   std::chrono::milliseconds deadline = std::chrono::milliseconds(5000)) {
	auto done = std::make_shared<bool>(false); // outlives a deadline that gave up on it
	block.drive(DriveRequest{code, {}}, [done](std::uint64_t) { *done = true; });
	const auto until = std::chrono::steady_clock::now() + deadline;
	for (;;) {
		block.settle();
		if (*done) return true;
		if (std::chrono::steady_clock::now() > until) return false;
		ear::sleeps::poll(std::chrono::microseconds(100), "between looks at settle(): the drive's completion decides");
	}
}
/// For destructors: silences `block` and waits for it (up to a minute); past that, ends the process with a message,
/// since freeing the code next could be a use after free on the data loop.
inline void silenceOrAbort(PortBlock& block) {
	if (driveNow(block, nullptr, std::chrono::milliseconds(60'000))) return;
	std::fputs("silenceOrAbort: a block's silence did not complete within 60 s; ending before its code is freed\n",
	           stderr);
	std::abort();
}
} // namespace ear::testing
