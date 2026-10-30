// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
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
/// destructor returns, process() is never called again.
class PortBlock { public: virtual ~PortBlock() = default; };
class PortBlockEvents {           // main thread
public:
  virtual void ready() = 0;                    // processing in the engine; at most once
  virtual void lost(std::string detail) = 0;   // engine refused or dropped the block
protected:
  ~PortBlockEvents() = default;
};
class PortBlockFactory {
public:
  /// Main thread; code.prepare(format) already called. One engine node named `node` with
  /// exactly `ports` (32-bit float mono each), always processing, driven by `code` on the
  /// realtime thread. Refusal is reported via events.lost, never thrown. code/events outlive it.
  virtual std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports,
      const AudioFormat& format, AudioCode& code, PortBlockEvents& events) = 0;
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
class NodeHost final : public DesiredNodeListener {
public:
  NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs);
  ~NodeHost();   // precondition: stopAll completed (or no node was ever added)
  void subscribe(NodeStatusListener& listener);
  /// starting -> config.start(grant) -> prepare -> port block; failures -> failed(reason).
  void nodeAdded(const NodeName& node, const NodeConfig& config) override;
  /// Every node: stopping; block destroyed, release(), handoffs closed, insides destroyed;
  /// gone. `allGone` runs once on the main thread after the last (at once if none).
  void stopAll(std::move_only_function<void()> allGone);
  std::vector<NodeStatus> status() const;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
