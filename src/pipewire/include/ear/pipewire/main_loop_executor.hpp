// Frozen interface (docs/design/batch2.md section 4; Timers by docs/design/node-framework.md section 6, I5a, slice 1).
// Change it only through a design page and a re-cut, never a local edit.
#pragma once
#include <functional>
#include <memory>
#include "ear/core/executor.hpp"
#include "ear/core/timers.hpp"
#include "ear/pipewire/engine.hpp"
// A mutex-guarded queue plus a pw_loop event source: posted work runs on the engine's main loop. Timers are pw_loop
// timer sources on the same loop.
namespace ear::pipewire {
class MainLoopExecutor final : public Executor, public Timers {
public:
	explicit MainLoopExecutor(Engine& engine);                // engine outlives it
	~MainLoopExecutor();                                      // main thread; queued work is destroyed unrun
	void post(std::move_only_function<void()> work) override; // any thread
	/// Main thread; the timer must be destroyed before the executor.
	std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::pipewire
