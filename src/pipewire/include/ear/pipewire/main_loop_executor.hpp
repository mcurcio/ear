// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <functional>
#include <memory>
#include "ear/core/executor.hpp"
#include "ear/pipewire/engine.hpp"
// A mutex-guarded queue plus a pw_loop event source: posted work runs on the engine's main loop.
namespace ear::pipewire {
class MainLoopExecutor final : public Executor {
public:
  explicit MainLoopExecutor(Engine& engine);   // engine outlives it
  ~MainLoopExecutor();   // main thread; queued work is destroyed unrun
  void post(std::move_only_function<void()> work) override;   // any thread
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
