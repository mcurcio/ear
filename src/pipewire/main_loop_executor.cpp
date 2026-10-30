// Work posted from any thread, run on the engine's main loop (docs/design/batch2.md sections 2
// and 4.3, main_loop_executor.hpp): a mutex-guarded queue plus a pw_loop event source. The
// queue owns each posted function, so work left when the executor goes is destroyed unrun.
#include "ear/pipewire/main_loop_executor.hpp"

#include <deque>
#include <mutex>
#include <utility>

#include <pipewire/pipewire.h>

namespace ear::pipewire {

struct MainLoopExecutor::Impl {
  explicit Impl(pw_loop* mainLoop) : loop(mainLoop) {
    event = pw_loop_add_event(loop, &Impl::onEvent, this);
  }

  ~Impl() {
    if (event != nullptr) pw_loop_destroy_source(loop, event);
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
  /// inside itself.
  static void onEvent(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<Impl*>(data);
    std::deque<std::move_only_function<void()>> batch;
    {
      const std::scoped_lock lock(self->mutex);
      batch.swap(self->queue);
    }
    for (auto& work : batch) work();
  }

  pw_loop* loop;
  spa_source* event = nullptr;
  std::mutex mutex;
  std::deque<std::move_only_function<void()>> queue;   // guarded by mutex
  bool stopped = false;                                // guarded by mutex
};

MainLoopExecutor::MainLoopExecutor(Engine& engine)
    : impl_(std::make_unique<Impl>(engine.mainLoop())) {}

MainLoopExecutor::~MainLoopExecutor() = default;

void MainLoopExecutor::post(std::move_only_function<void()> work) {
  {
    const std::scoped_lock lock(impl_->mutex);
    if (impl_->stopped) return;   // destroyed unrun, outside the lock
    impl_->queue.push_back(std::move(work));
  }
  // An eventfd write: safe from any thread, and several posts before the loop wakes coalesce
  // into one event that runs them all.
  pw_loop_signal_event(impl_->loop, impl_->event);
}

}  // namespace ear::pipewire
