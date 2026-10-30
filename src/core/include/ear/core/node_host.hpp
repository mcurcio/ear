// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_lifecycle.hpp"
namespace ear {
struct NodeStatus { NodeName name; NodeState state; std::optional<FailureReason> reason; };
class NodeStatusListener {
public:
  virtual void nodeChanged(const NodeStatus& status) = 0;
  /// Everything the node made is gone. droppedFrames: frames its handoffs refused (ring full).
  virtual void nodeGone(const NodeName& node, std::uint64_t droppedFrames) = 0;
protected:
  ~NodeStatusListener() = default;
};
/// Engine side of a port block. Destroying it removes its ports and their links; after the
/// destructor returns, no process() of any code is running or will run.
class PortBlock {
public:
  virtual ~PortBlock() = default;
  /// Main thread. The realtime thread runs `code` from its next cycle; nullptr silences every
  /// output. Returns only when the realtime thread can no longer be inside a process() of the
  /// code it ran before: a blocking barrier with the block's data loop. On a block the engine
  /// refused or lost that has no data loop, nothing can run, so it returns at once; the barrier
  /// holds either way. `code` must outlive the next drive() or the block.
  virtual void drive(AudioCode* code) = 0;
};
class PortBlockEvents {           // main thread
public:
  virtual void ready() = 0;                    // processing in the engine; at most once
  virtual void lost(std::string detail) = 0;   // engine refused or dropped the block
protected:
  ~PortBlockEvents() = default;
};
class PortBlockFactory {
public:
  /// Main thread. One engine node named `node` with one 32-bit float mono port per name in
  /// `ports` (public and inside), in EAR's graph group, always processing, silent until drive().
  /// Its public ports' levels are published through the factory's LevelReader. Refusal is told
  /// via events.lost later, never thrown or told inside this call. events outlives the block.
  virtual std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, PortBlockEvents& events) = 0;
protected:
  ~PortBlockFactory() = default;
};
class Handoff {
public:
  virtual ~Handoff() = default;   // main thread; only after `closed` ran, or before any write
  virtual RtWriter& writer() noexcept = 0;
  /// Main thread, once, after the writing port block is gone: drains the rest to the consumer,
  /// calls consumer.finish() on the handoff thread, then `closed` on the main thread.
  virtual void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) = 0;
};
class HandoffFactory {
public:
  /// Main thread; starts the handoff's own ordinary thread.
  virtual std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels,
      std::uint32_t capacityFrames, HandoffConsumer& consumer) = 0;
protected:
  ~HandoffFactory() = default;
};
/// A sound card opened through the engine: its engine node plus the links joining its channels
/// to its port block. Destroying it (main thread) removes both.
class SoundCard {
public:
  virtual ~SoundCard() = default;
};
class SoundCardEvents {   // main thread
public:
  /// The card's PCM runs and every channel is linked to its block port; at most once.
  virtual void ready() = 0;
  /// At most once, never inside make(). When it is told, the card's engine node and links are
  /// already destroyed, so the card no longer clocks or holds any part of the graph.
  virtual void lost(std::string detail) = 0;
protected:
  ~SoundCardEvents() = default;
};
class SoundCardFactory {
public:
  /// Main thread; only once port block `blockNode` is ready (its ports exist). One engine node
  /// named `engineNode` for request.card with request.blockPorts.size() channels at format.rate.
  /// It joins the graph's clock only through its links to `blockNode`, made once its PCM runs.
  virtual std::unique_ptr<SoundCard> make(const NodeName& engineNode, const NodeName& blockNode,
      const SoundCardRequest& request, const AudioFormat& format, SoundCardEvents& events) = 0;
protected:
  ~SoundCardFactory() = default;
};
class NodeHost final : public DesiredNodeListener {
public:
  NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs,
           SoundCardFactory& cards);   // all outlive it
  ~NodeHost();   // precondition: stopAll completed (or no node was ever added)
  void subscribe(NodeStatusListener& listener);
  /// starting; port block made; config.start(grant); code.prepare; block.drive(&code); the cards
  /// the type asked for are made once the block is ready; insides ready when every card is
  /// ready (at once with none). A failure fails the node; its port block stays.
  void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
  /// Restart: drive(nullptr), release(), cards destroyed, handoffs closed, insides destroyed and
  /// then their config let go, then started from `config` as in nodeAdded. The port block and
  /// routes stay. An update arriving during a restart replaces the config to start. If the
  /// block was lost: the old insides are torn down, nothing starts, the node stays failed.
  void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
  /// stopping; port block destroyed; then as the restart's teardown; nodeGone. Others untouched.
  void nodeRemoved(const NodeName& node) override;
  /// `closed` proves the graph takes no more edits, so no nodeAdded, nodeUpdated or nodeRemoved
  /// can follow (one would throw std::logic_error: a wiring bug). Every node: stopping; torn
  /// down; gone. `allGone` runs once on the main thread after the last (at once if none).
  void stopAll(GraphClosed closed, std::move_only_function<void()> allGone);
  /// Nodes not gone, in arrival order (a removed node still stopping comes before a newer node
  /// of the same name).
  std::vector<NodeStatus> status() const;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
