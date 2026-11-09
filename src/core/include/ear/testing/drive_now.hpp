// Test support: a port block driven and waited for on the calling thread, which must be the block's main thread with
// its loop not running (an engine test between Waiter::until calls). Step L-3a removed PortBlock's blocking drive
// (docs/design/node-lifecycle.md section 5.3); engine tests that only need a code running, or silenced, before they go
// on use this instead: the asynchronous drive, then settle() until its completion ran. When it returns true, the data
// loop applied the request between two cycles, so no process() of the code before it runs again.
#pragma once
#include <chrono>
#include <memory>
#include <thread>
#include "ear/core/node_host.hpp"
namespace ear::testing {
/// Drives `block` with `code` (null: silence), unfaded, and waits until the drive completed; false past `deadline`.
inline bool driveNow(PortBlock& block, AudioCode* code,
                     std::chrono::milliseconds deadline = std::chrono::milliseconds(5000)) {
	auto done = std::make_shared<bool>(false); // outlives a deadline that gave up on it
	block.drive(DriveRequest{code, {}}, [done](std::uint64_t) { *done = true; });
	const auto until = std::chrono::steady_clock::now() + deadline;
	while (!*done) {
		block.settle();
		if (*done) break;
		if (std::chrono::steady_clock::now() > until) return false;
		std::this_thread::yield();
	}
	return true;
}
} // namespace ear::testing
