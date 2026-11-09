// Main-loop timers (docs/architecture.md#the-node-lifecycle): the node framework's close deadline; and
// their clock, now(), so core reads no clock of its own. The engine's
// main loop implements them (ear_pipewire's MainLoopExecutor); tests turn a fake clock.
#pragma once
#include <chrono>
#include <functional>
#include <memory>
namespace ear {
/// One armed timer. Destroying it (main thread) cancels it: `due` never runs after that.
class Timer {
public:
	virtual ~Timer() = default;
};
class Timers {
public:
	/// Main thread. Runs `due` once on the main thread, `delay` from now, unless the returned timer is destroyed
	/// first. Never runs it inside this call. `due` is moved out of the timer before it runs, so destroying the timer
	/// inside `due` is allowed.
	virtual std::unique_ptr<Timer> after(std::chrono::milliseconds delay, std::move_only_function<void()> due) = 0;
	/// Main thread. The clock `after` counts on: monotonic, never stepped.
	virtual std::chrono::steady_clock::time_point now() const = 0;

protected:
	~Timers() = default;
};
} // namespace ear
