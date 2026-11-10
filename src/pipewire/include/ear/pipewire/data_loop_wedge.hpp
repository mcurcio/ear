// The wedged data loop on the engine side (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog). NodeHost
// judges the data loop wedged when a barrier's invoke has not run in 5 s and stops calling PipeWire itself; but making
// or destroying a link, an engine companion tearing itself down, or a latency update also reach the data loop through a
// blocking invoke (pw_impl_port's mix release, impl-port.c port_set_io), and the main loop would then wait on it for
// good, SIGTERM included. So main.cpp marks the engine once ear.data-loop-wedged is said, and from then on this
// package's adapters refuse or leak instead of calling: Links makes no link (each is refused "the data loop is wedged")
// and leaks every link proxy it would destroy; an engine companion leaks its links and body; a port block tells no
// latency. Every step that would wait on that loop (a blocking invoke, removing a filter's listener, destroying a
// filter or a proxy) is taken in the callback boundary, which skips it and leaks once the engine is marked, so no
// other path (an adapter's fault among them) can wait either. Leaked objects are never freed: main.cpp ends through
// std::_Exit.
#pragma once
namespace ear::pipewire {
class Engine;
/// Main thread: `engine`'s data loop runs no invoke. It is never unmarked; the mark goes with the engine.
void markDataLoopWedged(const Engine& engine);
/// Main thread: whether markDataLoopWedged was called for `engine`.
bool dataLoopWedged(const Engine& engine) noexcept;
} // namespace ear::pipewire
