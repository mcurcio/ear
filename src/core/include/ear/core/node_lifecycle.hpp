// Frozen interface (docs/design/batch2.md section 4). Change it only through an erratum in that
// page's Errata section (section 11) and a re-cut, never a local edit.
#pragma once
#include <optional>
#include <string_view>
#include "ear/core/node_type.hpp"
namespace ear {
enum class NodeState { starting, running, failed, stopping };   // "recovering" arrives with programs
std::string_view stateName(NodeState state) noexcept;
/// The one node state machine. Pure. running = the port block is ready AND the insides are
/// ready. A lost port block is final: it is never remade, so the node stays failed with the
/// block's reason until it is removed. Each fact returns true when the state or its reason
/// changed; a fact that does not apply changes nothing and returns false. No transitions after
/// gone().
class NodeLifecycle {
public:
  NodeLifecycle() noexcept;                      // starting; nothing ready
  NodeState state() const noexcept;
  const std::optional<FailureReason>& reason() const noexcept;   // set only while failed
  bool gone() const noexcept;
  bool blockLost() const noexcept;
  bool portBlockReady() noexcept;                // starting -> running if insides ready
  bool portBlockLost(FailureReason reason);      // starting|running|failed -> failed, this
                                                 // reason (it replaces an insides reason)
  bool insidesReady() noexcept;                  // starting -> running if block ready;
                                                 // nothing once the block is lost
  bool insidesFailed(FailureReason reason);      // starting|running -> failed; nothing once
                                                 // the block is lost
  bool restartRequested() noexcept;              // block not lost: starting|running|failed ->
                                                 // starting, insides not ready, reason cleared;
                                                 // block lost: nothing
  bool stopRequested() noexcept;                 // starting|running|failed -> stopping
  bool stopped() noexcept;                       // stopping -> gone
private:
  NodeState state_; std::optional<FailureReason> reason_;
  bool blockReady_; bool blockLost_; bool insidesReady_; bool gone_;
};
}
