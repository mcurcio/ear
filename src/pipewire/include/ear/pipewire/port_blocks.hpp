// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <memory>
#include <vector>
#include "ear/core/levels.hpp"
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
namespace ear::pipewire {
/// One pw_filter per block, node.always-process=true, node.group = graphGroup, one DSP port per
/// name (public and inside). Meters the public ports every cycle; in the cycle whose clock
/// position crosses a window boundary (position / levelWindowFrames changes) it publishes into
/// the block's mailbox and rings `wake`. frames > maxBlock: outputs silenced, process() skipped.
/// First STREAMING -> ready(); ERROR -> lost().
class FilterPortBlocks final : public PortBlockFactory, public LevelReader {
public:
  FilterPortBlocks(Engine& engine, LevelWake& wake);   // both outlive it
  ~FilterPortBlocks();
  std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, PortBlockEvents& events) override;
  std::vector<NodeLevels> latestLevels() override;     // any thread
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
