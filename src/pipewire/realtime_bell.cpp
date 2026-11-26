// The realtime bell (docs/engine.md#the-realtime-bell, realtime_bell.hpp): a pw_loop event source on the main loop.
// pw_loop_signal_event is one write(2) on the source's eventfd, safe from the data loop (a block's fade completion
// rings its own the same way), and several before the main loop wakes are one dispatch.
#include "ear/pipewire/realtime_bell.hpp"

#include <cstdint>
#include <utility>

#include <pipewire/pipewire.h>

#include "callback_boundary.hpp"

namespace ear::pipewire {

struct MainLoopRealtimeBell::Impl {
	Impl(pw_loop* mainLoop, std::function<void()> told) : loop(mainLoop), rung(std::move(told)) {
		addEvent(event, loop, guarded<&Impl::onEvent>, this);
	}
	Impl(const Impl&) = delete;
	Impl& operator=(const Impl&) = delete;
	void onEvent(std::uint64_t /*count*/) { rung(); }
	pw_loop* loop;
	std::function<void()> rung;
	LoopSource event; // last: destroyed first, before what it calls
};

MainLoopRealtimeBell::MainLoopRealtimeBell(Engine& engine, std::function<void()> rung)
	: impl_(std::make_unique<Impl>(engine.mainLoop(), std::move(rung))) {}

MainLoopRealtimeBell::~MainLoopRealtimeBell() = default;

void MainLoopRealtimeBell::ring() noexcept { pw_loop_signal_event(impl_->loop, impl_->event.get()); }

} // namespace ear::pipewire
