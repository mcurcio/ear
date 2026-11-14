// One configuration's way back to the main loop from its other threads (docs/architecture.md#teardown-by-dependency):
// its handle's notes and losses, its consumers' throws, its run object's close, and (from slice 3c's stores and
// recordings) its I/O jobs' completions. Work posted here lands on the main loop in post order and runs only while the
// configuration is attached; once it ended, what lands is destroyed unrun, so nothing of an old configuration reaches a
// newer one. An I/O job's completion carries what the job filled as a small handle (postHolding): dropped, the handle
// goes back to the I/O executor and is released there, never on the main loop, since closing a file after writing can
// block. Private to ear_core: each configuration's Channel (node_host.cpp) owns one and detaches it on every path that
// ends the configuration.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "ear/core/executor.hpp"
#include "ear/core/io_executor.hpp"

namespace ear::detail {

class Completions final : public Executor, public std::enable_shared_from_this<Completions> {
public:
	/// Made shared (std::make_shared): what is queued holds it. `mainLoop` outlives it.
	explicit Completions(Executor& mainLoop) noexcept : mainLoop_(mainLoop) {}
	/// Any thread, never waits. `work` runs on the main loop, after what was posted before it, if the configuration is
	/// still attached when it lands; otherwise it is destroyed unrun there. Never runs inside this call.
	void post(std::move_only_function<void()> work) override;
	/// An I/O job's completion (any thread, the job's own as a rule): `landed(*held)` runs on the main loop as post's
	/// work does, and the handle is let go there after it (`landed` takes from it what it keeps). Landing detached,
	/// `landed` is destroyed unrun and the handle is posted to `io` as the job `release`, so its last reference, and
	/// what it closes, goes on the I/O thread. `landed` must not capture `held`. `io` outlives what is queued here.
	template <class Held> void postHolding(std::shared_ptr<Held> held, IoExecutor& io, std::string release,
	                                       std::move_only_function<void(Held&)> landed) {
		postErased(std::move(held), io, std::move(release),
		           [landed = std::move(landed)](void* handle) mutable { landed(*static_cast<Held*>(handle)); });
	}
	/// Any thread: false once detached. A thread may read it to skip work no one will hear; the main loop's check
	/// when work lands is the one that counts.
	bool attached() const noexcept { return attached_.load(std::memory_order_acquire); }
	/// Main thread, once the configuration ended: nothing posted, before or after, runs.
	void detach() noexcept { attached_.store(false, std::memory_order_release); }

private:
	void postErased(std::shared_ptr<void> held, IoExecutor& io, std::string release,
	                std::move_only_function<void(void*)> landed);
	Executor& mainLoop_;
	std::atomic<bool> attached_{true};
};

} // namespace ear::detail
