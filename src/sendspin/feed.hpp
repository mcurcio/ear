// The feed (docs/design/sendspin.md sections 3, 7 and 14): audio from an ordinary thread to the realtime thread, the
// handoff's reverse, on the same ring. JACK's disk-reader pattern.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include "ear/core/frame_ring.hpp"
namespace ear::sendspin {
/// A FrameRing and an eventfd, written only while the writer says it waits.
class Feed {
public:
  /// Throws std::system_error when it cannot make the eventfd.
  Feed(std::uint32_t channels, std::uint32_t capacityFrames);
  ~Feed();
  Feed(const Feed&) = delete;
  Feed& operator=(const Feed&) = delete;
  std::uint32_t capacityFrames() const noexcept { return ring_.capacityFrames(); }
  /// Realtime; no lock, allocation or waiting. Planar, zeros after what it holds. Returns frames read.
  std::uint32_t read(std::span<const std::span<float>> channels, std::uint32_t frames) noexcept;
  std::uint32_t skip(std::uint32_t frames) noexcept;   // realtime; discards; returns how many
  /// One writing thread. With no room waits up to `patience`. Returns frames accepted.
  std::uint32_t write(std::span<const float> interleaved, std::uint32_t frames, std::chrono::milliseconds patience);
  /// Any ordinary thread. What was written so far is gone at the next read, not counted played.
  void clear() noexcept;
private:
  bool applyClear() noexcept;
  void wakeWriter() noexcept;
  FrameRing ring_;
  int room_;                                  // the eventfd the writer waits on
  std::atomic<bool> writerWaits_{false};
  std::atomic<std::uint64_t> clearTo_{0};     // pending bit, and the ring's write position when it was asked
};
}
