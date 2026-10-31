// The one outside-node lifecycle (docs/design/rtp.md sections 6 and 7.2, outside_nodes.hpp):
// batch 2's card state machine (docs/design/batch2.md section 5), moved out of the card so every
// kind of outside node is joined, made ready, lost and torn down by this code. A kind's body
// says only how its engine node is made and heard from; this owns "is this outside node in the
// graph": it runs alone until its body says it runs, then links its channels to its port block,
// and on any failure destroys its links and its body before it reports the loss.
#include "outside_nodes.hpp"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "link_proxy.hpp"

namespace ear::pipewire {
namespace {

class JoinedOutsideNode final : public OutsideNode, public OutsideBodyEvents, public LinkEvents {
public:
  JoinedOutsideNode(Engine& engine, OutsideJoin join, OutsideNodeEvents& events, OutsideBodyMaker makeBody)
      : engine_(engine), join_(std::move(join)), events_(events) {
    later_ = pw_loop_add_event(engine_.mainLoop(), &JoinedOutsideNode::onLater, this);
    pw_core_add_listener(engine_.core(), &coreHook_, &coreEvents(), this);
    coreListening_ = true;
    body_ = makeBody(*this);
  }

  ~JoinedOutsideNode() override {
    silence();
    links_.clear();
    body_.reset();
    if (later_ != nullptr) pw_loop_destroy_source(engine_.mainLoop(), later_);
  }

  JoinedOutsideNode(const JoinedOutsideNode&) = delete;
  JoinedOutsideNode& operator=(const JoinedOutsideNode&) = delete;

private:
  enum class State { opening, linking, ready, failing, lost };

  static const pw_core_events& coreEvents() {
    static const pw_core_events table = [] {
      pw_core_events events{};
      events.version = PW_VERSION_CORE_EVENTS;
      events.done = &JoinedOutsideNode::onCoreDone;
      return events;
    }();
    return table;
  }

  // OutsideBodyEvents. opening --running--> linking; any state --failed--> failing.
  void running(std::uint32_t nodeId) override {
    if (state_ == State::opening) link(nodeId);
  }
  void failed(std::string detail) override { fail(std::move(detail)); }

  /// One link per channel, the outside node by its global id and the block by its node.name,
  /// each port by name, owned by EAR's connection.
  void link(std::uint32_t nodeId) {
    state_ = State::linking;
    const bool toNode = join_.flow == ChannelFlow::blockToNode;
    const std::string node = std::to_string(nodeId);
    for (std::size_t channel = 0; channel < join_.blockPorts.size(); ++channel) {
      const std::string& nodePort = join_.nodePorts[channel];
      const std::string& blockPort = join_.blockPorts[channel];
      pw_properties* props = pw_properties_new(
          PW_KEY_LINK_OUTPUT_NODE, toNode ? join_.blockNode.c_str() : node.c_str(),
          PW_KEY_LINK_OUTPUT_PORT, toNode ? blockPort.c_str() : nodePort.c_str(),
          PW_KEY_LINK_INPUT_NODE, toNode ? node.c_str() : join_.blockNode.c_str(),
          PW_KEY_LINK_INPUT_PORT, toNode ? nodePort.c_str() : blockPort.c_str(),
          nullptr);
      auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
          engine_.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict,
          0));
      pw_properties_free(props);
      if (proxy == nullptr) {
        fail("cannot ask for the " + join_.kind + "'s link: " + spa_strerror(-errno));
        return;
      }
      links_.push_back(std::make_unique<LinkProxy>(channel, proxy, *this));
    }
  }

  // LinkEvents. linking --every link ACTIVE--> ready; any link failed or gone --> failing.
  void active(LinkId /*link*/) override {
    if (state_ != State::linking || ++activeLinks_ < links_.size()) return;
    state_ = State::ready;
    events_.ready();
  }
  void failed(LinkId link, std::string detail) override {
    fail("the " + join_.kind + "'s link for channel " + std::to_string(link + 1) + " failed: " + detail);
  }
  void gone(LinkId link) override {
    fail("the " + join_.kind + "'s link for channel " + std::to_string(link + 1) + " is gone");
  }

  /// The loss path, from any state: nothing more is heard from the links, and the teardown runs
  /// later on the main loop (never inside a proxy's own event, never inside make()). The body's
  /// own events are ignored from here on.
  void fail(std::string detail) {
    if (state_ == State::failing || state_ == State::lost) return;
    state_ = State::failing;
    detail_ = std::move(detail);
    for (const auto& link : links_) link->silence();
    if (later_ != nullptr) pw_loop_signal_event(engine_.mainLoop(), later_);
  }

  /// Destroys the links and the body (its engine node goes with it), then syncs with the
  /// engine, so the removal is done before the loss is told.
  static void onLater(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<JoinedOutsideNode*>(data);
    if (self->state_ != State::failing || self->syncing_) return;
    self->syncing_ = true;
    self->links_.clear();
    self->body_.reset();
    self->syncSeq_ = pw_core_sync(self->engine_.core(), PW_ID_CORE, 0);
    if (self->syncSeq_ < 0) self->tellLost();   // nothing to wait for
  }

  static void onCoreDone(void* data, std::uint32_t id, int seq) {
    auto* self = static_cast<JoinedOutsideNode*>(data);
    if (id != PW_ID_CORE || !self->syncing_ || self->state_ != State::failing ||
        seq != self->syncSeq_) {
      return;
    }
    self->tellLost();
  }

  void tellLost() {
    state_ = State::lost;
    silence();
    events_.lost(detail_);
  }

  void silence() {
    for (const auto& link : links_) link->silence();
    if (coreListening_) {
      coreListening_ = false;
      spa_hook_remove(&coreHook_);
    }
  }

  Engine& engine_;
  OutsideJoin join_;
  OutsideNodeEvents& events_;
  State state_ = State::opening;
  std::string detail_;
  std::unique_ptr<OutsideBody> body_;
  spa_hook coreHook_{};
  bool coreListening_ = false;
  std::vector<std::unique_ptr<LinkProxy>> links_;   // link id = channel
  std::size_t activeLinks_ = 0;
  spa_source* later_ = nullptr;
  bool syncing_ = false;
  int syncSeq_ = 0;
};

}  // namespace

std::unique_ptr<OutsideNode> joinOutsideNode(Engine& engine, OutsideJoin join, OutsideNodeEvents& events,
                                             OutsideBodyMaker makeBody) {
  return std::make_unique<JoinedOutsideNode>(engine, std::move(join), events, std::move(makeBody));
}

}  // namespace ear::pipewire
