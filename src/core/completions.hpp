// One configuration's way back to the main loop from its other threads (docs/architecture.md#teardown-by-dependency):
// its handle's notes and losses, its consumers' throws, its run object's close, and (from slice 3c's stores and
// recordings) its I/O jobs' completions. Work posted here lands on the main loop in post order and runs only while the
// configuration is attached; once it ended, what lands is destroyed unrun, so nothing of an old configuration reaches a
// newer one and what a dropped job held (a descriptor it opened) is closed by its own destructor. Private to ear_core:
// each configuration's Channel (node_host.cpp) owns one and detaches it on every path that ends the configuration.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include "ear/core/executor.hpp"

namespace ear::detail {

class Completions final : public Executor, public std::enable_shared_from_this<Completions> {
public:
	/// Made shared (std::make_shared): what is queued holds it. `mainLoop` outlives it.
	explicit Completions(Executor& mainLoop) noexcept : mainLoop_(mainLoop) {}
	/// Any thread, never waits. `work` runs on the main loop, after what was posted before it, if the configuration is
	/// still attached when it lands; otherwise it is destroyed unrun there. Never runs inside this call.
	void post(std::move_only_function<void()> work) override;
	/// Any thread: false once detached. A thread may read it to skip work no one will hear; the main loop's check
	/// when work lands is the one that counts.
	bool attached() const noexcept { return attached_.load(std::memory_order_acquire); }
	/// Main thread, once the configuration ended: nothing posted, before or after, runs.
	void detach() noexcept { attached_.store(false, std::memory_order_release); }

private:
	Executor& mainLoop_;
	std::atomic<bool> attached_{true};
};

} // namespace ear::detail
