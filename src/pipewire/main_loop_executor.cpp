// Work posted from any thread, run on the engine's main loop (docs/design/batch2.md section 4.3,
// main_loop_executor.hpp). Slice s0 stub: slice E1 builds the queue and its loop event.
#include "ear/pipewire/main_loop_executor.hpp"

#include <stdexcept>

namespace ear::pipewire {

struct MainLoopExecutor::Impl {};

MainLoopExecutor::MainLoopExecutor(Engine&) {
  throw std::logic_error("MainLoopExecutor is not built yet (slice E1)");
}

MainLoopExecutor::~MainLoopExecutor() = default;

void MainLoopExecutor::post(std::move_only_function<void()>) {
  throw std::logic_error("MainLoopExecutor::post is not built yet (slice E1)");
}

}  // namespace ear::pipewire
