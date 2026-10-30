// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_host.hpp"
struct pw_loop;
// spa_ringbuffer + one pw_thread_loop per handoff, woken by pw_loop_signal_event.
namespace ear::pipewire {
class ThreadHandoffs final : public HandoffFactory {
public:
  explicit ThreadHandoffs(pw_loop* mainLoop);   // where `closed` runs; no Engine needed
  std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels,
      std::uint32_t capacityFrames, HandoffConsumer& consumer) override;
private:
  pw_loop* mainLoop_;
};
}
