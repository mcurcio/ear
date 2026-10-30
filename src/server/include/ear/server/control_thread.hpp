// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <functional>
#include <memory>
#include "ear/core/executor.hpp"
// No Boost type in this header: Asio stays private to ear_server.
namespace ear::server {
/// The control thread: one io_context run by one thread. Start it after Engine::create (it
/// inherits the blocked stop signals).
class ControlThread final : public Executor {
public:
  ControlThread();                       // starts the thread
  ~ControlThread();                      // stop()
  void post(std::move_only_function<void()> work) override;   // any thread; post order kept
  /// Any thread but this one; idempotent. Joins; queued and later work is destroyed unrun.
  void stop();
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
