// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <memory>
#include "ear/core/reconciler.hpp"
#include "ear/pipewire/engine.hpp"
// link-factory by port global id, owned by EAR's connection (no linger); link proxy info state
// ACTIVE -> linkActive, ERROR/proxy error -> linkFailed, removed -> linkGone.
namespace ear::pipewire {
class Links final : public LinkMaker {
public:
  explicit Links(Engine& engine);
  ~Links();                                   // destroys every link it made
  void subscribe(LinkListener& listener);
  void createLink(LinkId link, EnginePortId output, EnginePortId input) override;
  void destroyLink(LinkId link) override;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
