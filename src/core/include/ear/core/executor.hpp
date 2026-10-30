// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <functional>
namespace ear {
/// Runs work on one thread, one item at a time, in post order.
class Executor {
public:
  /// Any thread. Never runs `work` inside this call, never waits for it. Work posted after the
  /// executor stopped is destroyed unrun.
  virtual void post(std::move_only_function<void()> work) = 0;
protected:
  ~Executor() = default;
};
}
