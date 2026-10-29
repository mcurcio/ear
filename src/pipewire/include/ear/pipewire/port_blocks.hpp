// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/node_host.hpp"
#include "ear/pipewire/engine.hpp"
// one pw_filter per node; one "32 bit float mono audio" DSP port per declared port;
// node.always-process=true; PW_FILTER_FLAG_RT_PROCESS. frames > maxBlock: outputs silenced,
// process() skipped. First STREAMING -> ready(); ERROR -> lost().
namespace ear::pipewire {
class FilterPortBlocks final : public PortBlockFactory {
public:
  explicit FilterPortBlocks(Engine& engine);
  std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, AudioCode& code, PortBlockEvents& events) override;
private:
  Engine& engine_;
};
}
