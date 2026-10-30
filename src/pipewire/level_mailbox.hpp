// One port block's level mailbox (docs/design/batch2.md section 4.3): the realtime thread
// publishes each finished level window into it, an ordinary thread reads the latest. Private to
// ear_pipewire (port_blocks.cpp).
//
// A triple buffer: the writer fills its own slot and swaps it with the middle one in one atomic
// exchange, the reader swaps the middle one into its own slot when it is fresh. Neither side
// waits, locks or allocates; every slot is sized in the constructor. One writer (the block's
// process()) and one reader at a time (FilterPortBlocks holds its registry mutex to read).
//
// Each publish carries the block's window before it as well, so a reader that meets the
// realtime thread half way through one cycle's blocks (some have published window N, some not
// yet) can still take window N - 1 from every block: one snapshot is one window.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "ear/core/levels.hpp"

namespace ear::pipewire {

/// One finished window of one block: a level per metered port.
struct LevelWindow {
  std::uint64_t windowEnd = 0;
  std::vector<Level> levels;
};

/// What the reader sees: the block's latest window and, when it has one, the window before.
struct PublishedLevels {
  LevelWindow latest;
  bool hasPrevious = false;
  LevelWindow previous;
};

class LevelMailbox {
public:
  /// Main thread: sizes every slot for `ports` levels.
  explicit LevelMailbox(std::size_t ports) : last_{0, std::vector<Level>(ports)} {
    for (Slot& slot : slots_) {
      slot.levels.latest.levels.assign(ports, Level{0.0F, 0.0F});
      slot.levels.previous.levels.assign(ports, Level{0.0F, 0.0F});
    }
  }

  LevelMailbox(const LevelMailbox&) = delete;
  LevelMailbox& operator=(const LevelMailbox&) = delete;

  /// Realtime thread: where the next window's levels go (one per port, before publish()).
  std::span<Level> next() noexcept { return slots_[back_].levels.latest.levels; }

  /// Realtime thread: the window filled through next() ended at `windowEnd`.
  void publish(std::uint64_t windowEnd) noexcept {
    Slot& slot = slots_[back_];
    slot.levels.latest.windowEnd = windowEnd;
    slot.levels.hasPrevious = hasLast_;
    slot.levels.previous.windowEnd = last_.windowEnd;
    std::ranges::copy(last_.levels, slot.levels.previous.levels.begin());
    slot.valid = true;
    last_.windowEnd = windowEnd;
    std::ranges::copy(slot.levels.latest.levels, last_.levels.begin());
    hasLast_ = true;
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
  }

  /// Reader (one at a time): the latest publish, or nullptr before the first.
  const PublishedLevels* read() noexcept {
    if ((middle_.load(std::memory_order_acquire) & kFresh) != 0) {
      front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
    }
    const Slot& slot = slots_[front_];
    return slot.valid ? &slot.levels : nullptr;
  }

private:
  static constexpr std::uint8_t kIndex = 0x3;
  static constexpr std::uint8_t kFresh = 0x4;

  struct Slot {
    bool valid = false;
    PublishedLevels levels;
  };

  std::array<Slot, 3> slots_;
  std::atomic<std::uint8_t> middle_{1};
  std::uint8_t back_ = 2;    // realtime thread only
  std::uint8_t front_ = 0;   // reader only
  // Realtime thread only: the window it published last.
  LevelWindow last_;
  bool hasLast_ = false;
};

}  // namespace ear::pipewire
