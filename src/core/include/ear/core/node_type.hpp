// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/command_error.hpp"
namespace ear {
/// Invariant: names unique across inputs and outputs (DesiredGraph::addNode throws
/// std::logic_error otherwise: a type bug, not a user error).
struct Ports { std::vector<PortName> inputs; std::vector<PortName> outputs; };
/// Why a node is failed. Part of status; never returned by a control call.
struct FailureReason { enum class Code { engineRefused, startFailed }; Code code; std::string detail; };
std::string_view failureCode(FailureReason::Code code) noexcept;  // "engine-refused" | "start-failed"
struct AudioFormat { std::uint32_t rate; std::uint32_t maxBlock; };   // fixed at boot in M1

class AudioCode {
public:
  virtual ~AudioCode() = default;
  /// Main thread, before the first process(), never concurrently with it. May allocate.
  virtual void prepare(const AudioFormat& format) = 0;
  /// Realtime: no locks, allocation, file/network access or waiting. inputs/outputs are in
  /// declaration order, each span exactly `frames` long, frames <= maxBlock. An unlinked
  /// input reads zeros. Every output sample must be written.
  virtual void process(std::span<const std::span<const float>> inputs,
                       std::span<const std::span<float>> outputs,
                       std::uint32_t frames) noexcept = 0;
  /// Main thread, after the last process() returned.
  virtual void release() noexcept = 0;
};
/// Realtime end of a handoff. Valid until the node's port block is gone.
class RtWriter {
public:
  /// Realtime thread only. Interleaves `frames` frames of planar `channels` (size == the
  /// handoff's channel count, each span >= frames) into the ring. Never blocks, locks or
  /// allocates. Returns frames accepted; the rest are dropped and counted by the framework.
  virtual std::uint32_t write(std::span<const std::span<const float>> channels,
                              std::uint32_t frames) noexcept = 0;
protected:
  ~RtWriter() = default;
};
/// Ordinary-thread end of a handoff, implemented by the node type.
class HandoffConsumer {
public:
  virtual ~HandoffConsumer() = default;
  /// The handoff's own thread (never realtime or main). May block on I/O. In write order;
  /// interleaved.size() == frames * channels.
  virtual void consume(std::span<const float> interleaved, std::uint32_t frames) = 0;
  /// Same thread, exactly once, after the last consume().
  virtual void finish() = 0;
};
/// The type's own objects for one node. Destroyed by the framework last.
class NodeInsides {
public:
  virtual ~NodeInsides() = default;
  /// Drives this node's one port block (the framework makes it; the type cannot make two
  /// or none). Same object every call; lives as long as the insides.
  virtual AudioCode& audioCode() = 0;
};
/// Powers granted to one starting node. Everything made here belongs to the node and is torn
/// down by the framework in order: port block, handoffs, insides.
class NodeGrant {
public:
  virtual AudioFormat format() const = 0;   // the value prepare() will get
  /// `consumer` must live in the returned NodeInsides.
  virtual RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames,
                            HandoffConsumer& consumer) = 0;
protected:
  ~NodeGrant() = default;
};
/// One node's checked settings; made only by its type's parse().
class NodeConfig {
public:
  virtual ~NodeConfig() = default;
  virtual const Ports& ports() const = 0;   // pure; the only place this node's ports are declared
  /// Main thread. Error: the node becomes failed with this reason (startFailed).
  virtual std::expected<std::unique_ptr<NodeInsides>, FailureReason> start(NodeGrant& grant) const = 0;
};
class NodeType {
public:
  virtual ~NodeType() = default;
  virtual std::string_view name() const = 0;          // the NodeSpec `type`
  /// Pure; the only place settings are checked; unknown keys are invalid.
  virtual std::expected<std::unique_ptr<const NodeConfig>, InvalidSettings>
      parse(const NodeName& node, const NodeSettings& settings) const = 0;
};
}
