// Route links through the engine's link-factory (docs/design/m1-white-to-file.md section 2,
// links.hpp).
#include "ear/pipewire/links.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "link_proxy.hpp"

namespace ear::pipewire {

/// Every event ends a link's story or moves it on; a link that is done (failed or gone) is
/// silenced at once and its proxy destroyed later on the main loop, outside the proxy's own
/// event and outside any listener's call into destroyLink.
struct Links::Impl final : LinkEvents {
  explicit Impl(Engine& engine) : engine(engine) {
    later = pw_loop_add_event(engine.mainLoop(), &Impl::onLater, this);
  }
  ~Impl() {
    live.clear();
    done.clear();
    if (later != nullptr) pw_loop_destroy_source(engine.mainLoop(), later);
  }

  void active(LinkId link) override {
    for (LinkListener* listener : listeners) listener->linkActive(link);
  }
  void failed(LinkId link, std::string detail) override {
    retire(link);
    for (LinkListener* listener : listeners) listener->linkFailed(link, detail);
  }
  void gone(LinkId link) override {
    retire(link);
    for (LinkListener* listener : listeners) listener->linkGone(link);
  }

  void retire(LinkId link) {
    auto found = live.find(link);
    if (found == live.end()) return;
    found->second->silence();
    done.push_back(std::move(found->second));
    live.erase(found);
    pw_loop_signal_event(engine.mainLoop(), later);
  }

  static void onLater(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<Impl*>(data);
    self->done.clear();
    auto refused = std::exchange(self->refused, {});
    for (auto& [link, detail] : refused) {
      for (LinkListener* listener : self->listeners) listener->linkFailed(link, detail);
    }
  }

  Engine& engine;
  std::vector<LinkListener*> listeners;
  std::map<LinkId, std::unique_ptr<LinkProxy>> live;
  std::vector<std::unique_ptr<LinkProxy>> done;
  std::vector<std::pair<LinkId, std::string>> refused;   // told later, never inside createLink
  spa_source* later = nullptr;
};

Links::Links(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}

Links::~Links() = default;

void Links::subscribe(LinkListener& listener) { impl_->listeners.push_back(&listener); }

void Links::createLink(LinkId link, EnginePortId output, EnginePortId input) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input);
  auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
      impl_->engine.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
      &props->dict, 0));
  pw_properties_free(props);
  if (proxy == nullptr) {
    impl_->refused.emplace_back(link, std::string("cannot ask for the link: ") +
                                          spa_strerror(-errno));
    pw_loop_signal_event(impl_->engine.mainLoop(), impl_->later);
    return;
  }
  impl_->live[link] = std::make_unique<LinkProxy>(link, proxy, *impl_);
}

void Links::destroyLink(LinkId link) {
  impl_->retire(link);
  std::erase_if(impl_->refused, [link](const auto& entry) { return entry.first == link; });
}

}  // namespace ear::pipewire
