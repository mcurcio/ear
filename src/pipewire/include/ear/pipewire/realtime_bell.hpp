#pragma once
#include <functional>
#include <memory>
#include "ear/core/realtime_channels.hpp"
#include "ear/pipewire/engine.hpp"
// The realtime bell (docs/engine.md#the-realtime-bell): a pw_loop event on the engine's main loop that a node's code
// rings from the data loop after a cycle it wrote in, and whose dispatch tells its owner on the main loop.
namespace ear::pipewire {
class MainLoopRealtimeBell final : public RealtimeBell {
public:
	/// `rung` runs on the main loop, once for every rings before the loop woke (NodeHost::realtimeRung). The engine
	/// outlives it; main thread.
	MainLoopRealtimeBell(Engine& engine, std::function<void()> rung);
	~MainLoopRealtimeBell(); // main thread; after every code that may ring it is gone
	MainLoopRealtimeBell(const MainLoopRealtimeBell&) = delete;
	MainLoopRealtimeBell& operator=(const MainLoopRealtimeBell&) = delete;
	/// Any thread, the data loop's included: one eventfd write, never a wait or an allocation.
	void ring() noexcept override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear::pipewire
