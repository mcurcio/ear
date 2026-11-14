// Work posted from any thread, run on the engine's main loop (docs/architecture.md#threads, main_loop_executor.hpp): a
// mutex-guarded queue plus a pw_loop event source. The queue owns each posted function, so work left when the executor
// goes is destroyed unrun.
#include "ear/pipewire/main_loop_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <utility>

#include <pipewire/pipewire.h>

#include "callback_boundary.hpp"

namespace ear::pipewire {

struct MainLoopExecutor::Impl {
	explicit Impl(pw_loop* mainLoop) : loop(mainLoop) { addEvent(event, loop, guarded<&Impl::onEvent>, this); }

	~Impl() {
		event.reset();
		// Destroyed outside the lock: a function's destructor may post again (it is dropped).
		std::deque<std::move_only_function<void()>> left;
		{
			const std::scoped_lock lock(mutex);
			left.swap(queue);
			stopped = true;
		}
	}

	Impl(const Impl&) = delete;
	Impl& operator=(const Impl&) = delete;

	/// Main loop: runs what was queued when the event fired, in post order. Work posted while it
	/// runs (from here or another thread) waits for the next event, so post() never runs work
	/// inside itself. Each item runs through the callback boundary on its own, so one that throws is reported as
	/// itself and never drops the items behind it (docs/architecture.md#the-callback-boundary).
	void onEvent(std::uint64_t /*count*/) {
		std::deque<std::move_only_function<void()>> batch;
		{
			const std::scoped_lock lock(mutex);
			batch.swap(queue);
		}
		for (auto& work : batch) shield("MainLoopExecutor's posted work", work);
	}

	pw_loop* loop;
	LoopSource event;
	std::mutex mutex;
	std::deque<std::move_only_function<void()>> queue; // guarded by mutex
	bool stopped = false;                              // guarded by mutex
};

namespace {

/// One pw_loop timer source, armed once. `due` is moved out before it runs, so the timer may be destroyed inside it: a
/// source destroyed during its own dispatch is freed by the loop after the dispatch.
class LoopTimer final : public Timer {
public:
	LoopTimer(pw_loop* loop, std::chrono::milliseconds delay, std::move_only_function<void()> due)
		: loop_(loop), due_(std::move(due)) {
		addTimer(source_, loop_, guarded<&LoopTimer::onTimeout>, this);
		// A zero value would disarm the timer: at least a nanosecond.
		const auto ns = std::max<std::int64_t>(std::chrono::nanoseconds(delay).count(), 1);
		timespec value{static_cast<time_t>(ns / 1'000'000'000), static_cast<long>(ns % 1'000'000'000)};
		pw_loop_update_timer(loop_, source_.get(), &value, nullptr, false);
	}
	LoopTimer(const LoopTimer&) = delete;
	LoopTimer& operator=(const LoopTimer&) = delete;
	~LoopTimer() override = default;

private:
	void onTimeout(std::uint64_t /*expirations*/) {
		auto due = std::move(due_);
		if (due) due();
	}
	pw_loop* loop_;
	std::move_only_function<void()> due_;
	LoopSource source_; // last: destroyed first, before what it calls
};

} // namespace

MainLoopExecutor::MainLoopExecutor(Engine& engine) : impl_(std::make_unique<Impl>(engine.mainLoop())) {}

std::unique_ptr<Timer> MainLoopExecutor::after(std::chrono::milliseconds delay, std::move_only_function<void()> due) {
	return std::make_unique<LoopTimer>(impl_->loop, delay, std::move(due));
}

std::chrono::steady_clock::time_point MainLoopExecutor::now() const {
	return std::chrono::steady_clock::now(); // CLOCK_MONOTONIC on Linux, as the loop's timerfds
}

MainLoopExecutor::~MainLoopExecutor() = default;

void MainLoopExecutor::post(std::move_only_function<void()> work) {
	{
		const std::scoped_lock lock(impl_->mutex);
		if (impl_->stopped) return; // destroyed unrun, outside the lock
		impl_->queue.push_back(std::move(work));
	}
	// An eventfd write: safe from any thread, and several posts before the loop wakes coalesce
	// into one event that runs them all.
	pw_loop_signal_event(impl_->loop, impl_->event.get());
}

} // namespace ear::pipewire
