// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "ear/core/graph_spec.hpp"
namespace ear {
/// One port over one window, linear (1.0 = full scale): sample peak and RMS.
struct Level { float peak; float rms; };
struct PortLevel { PortName port; Level level; };
/// One port block's latest window: its public ports, inputs then outputs, declaration order.
/// windowEnd: the graph clock position (frames) where the window ended. Every block's windows
/// end at the same positions (multiples of levelWindowFrames), so one snapshot is one window.
struct NodeLevels { NodeName node; std::uint64_t windowEnd; std::vector<PortLevel> ports; };
/// Windows per second (the one home). A stated display rate: meters are read by eye, browsers
/// paint at 60 Hz, and 30 is the common meter refresh; at 48 kHz a window is 1600 frames
/// (6.25 cycles of 256: a block publishes in the cycle that crosses a boundary). About 20 bytes
/// per port per window per client.
inline constexpr std::uint32_t levelWindowsPerSecond = 30;
/// ceil(rate / levelWindowsPerSecond).
std::uint32_t levelWindowFrames(std::uint32_t rate) noexcept;
/// One port's accumulator. Realtime-safe: no locks, allocation or system calls.
class LevelMeter {
public:
  void add(std::span<const float> samples) noexcept;
  /// The level since the last take() ({0, 0} if no samples); starts a new window.
  Level take() noexcept;
private:
  float peak_ = 0.0F; double sumSquares_ = 0.0; std::uint64_t frames_ = 0;
};
/// Realtime end: a port block published a window. Realtime-safe; never blocks.
class LevelWake {
public:
  virtual void levelsPublished() noexcept = 0;
protected:
  ~LevelWake() = default;
};
/// Ordinary-thread end. Thread-safe; never blocks the realtime thread.
class LevelReader {
public:
  /// A complete snapshot: the latest window of every port block that exists and has published
  /// one. A block that has gone is absent (levels are a latest-value signal).
  virtual std::vector<NodeLevels> latestLevels() = 0;
protected:
  ~LevelReader() = default;
};
}
