// A node's notes, at a rate no peer can raise (docs/design/sendspin.md section 11): each note goes on the main loop's queue
// and into the log, and a peer can drive them as fast as it runs pairing attempts. A burst passes, as a whole normal session
// does; then one a second, and what was held back is counted and said once it can be. A loss is never held back.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "ear/core/node_type.hpp"
namespace ear::sendspin {
class NoteGate final : public InsidesEvents {
public:
  using Clock = std::chrono::steady_clock;
  /// `next` outlives this. `now`: the clock, a test's own.
  explicit NoteGate(InsidesEvents& next, std::function<Clock::time_point()> now = Clock::now);
  void say(std::string text) override;
  void lost(std::string detail) override;   // never held back
private:
  InsidesEvents& next_;
  std::function<Clock::time_point()> now_;
  std::mutex mutex_;
  double tokens_;
  Clock::time_point refilled_;
  std::uint64_t dropped_ = 0;
};
}
