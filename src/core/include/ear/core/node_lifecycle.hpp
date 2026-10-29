// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <optional>
#include <string_view>
#include "ear/core/node_type.hpp"
namespace ear {
enum class NodeState { starting, running, failed, stopping };   // "recovering" arrives with programs
std::string_view stateName(NodeState state) noexcept;
/// The one node state machine. Pure. Each fact returns true when the state changed; a fact
/// that does not apply changes nothing and returns false. No transitions after gone().
class NodeLifecycle {
public:
  NodeLifecycle() noexcept;                       // starting
  NodeState state() const noexcept;
  const std::optional<FailureReason>& reason() const noexcept;   // set only while failed
  bool gone() const noexcept;
  bool portBlockReady() noexcept;                 // starting -> running
  bool refused(FailureReason reason);             // starting|running -> failed
  bool stopRequested() noexcept;                  // starting|running|failed -> stopping
  bool stopped() noexcept;                        // stopping -> gone
private:
  NodeState state_; std::optional<FailureReason> reason_; bool gone_;
};
}
