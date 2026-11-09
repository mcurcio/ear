// The wedged data loop on the engine side (docs/design/node-lifecycle.md section 5.4, step L-3c's review). NodeHost
// judges the data loop wedged when a barrier's invoke has not run in 5 s and stops calling PipeWire itself; but making
// or destroying a link, an outside node tearing itself down, or a latency update also reach the data loop through a
// blocking invoke (pw_impl_port's mix release, impl-port.c port_set_io), and the main loop would then wait on it for
// good, SIGTERM included. So main.cpp marks the engine once ear.data-loop-wedged is said, and from then on this
// package's adapters refuse or leak instead of calling: Links makes no link (each is refused "the data loop is wedged")
// and leaks every link proxy it would destroy; an outside node leaks its links and body; a port block tells no
// latency. Leaked objects are never freed: main.cpp ends through std::_Exit.
#pragma once
namespace ear::pipewire {
class Engine;
/// Main thread: `engine`'s data loop runs no invoke. It is never unmarked; the mark goes with the engine.
void markDataLoopWedged(const Engine& engine);
/// Main thread: whether markDataLoopWedged was called for `engine`.
bool dataLoopWedged(const Engine& engine) noexcept;
} // namespace ear::pipewire
