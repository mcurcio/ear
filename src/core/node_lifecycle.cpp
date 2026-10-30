// The one node state machine (docs/design/m1-white-to-file.md section 2, node_lifecycle.hpp),
// and the names status lines give its states and its failure reasons. failureCode is declared
// in node_type.hpp, which has no source file of its own; it lives here beside stateName
// because a failure reason is part of a node's state.
#include "ear/core/node_lifecycle.hpp"

#include <utility>

namespace ear {

std::string_view stateName(NodeState state) noexcept {
	switch (state) {
	case NodeState::starting: return "starting";
	case NodeState::running: return "running";
	case NodeState::failed: return "failed";
	case NodeState::stopping: return "stopping";
	}
	return "unknown";
}

std::string_view failureCode(FailureReason::Code code) noexcept {
	switch (code) {
	case FailureReason::Code::engineRefused: return "engine-refused";
	case FailureReason::Code::startFailed: return "start-failed";
	}
	return "unknown";
}

NodeLifecycle::NodeLifecycle() noexcept : state_(NodeState::starting), gone_(false) {}

NodeState NodeLifecycle::state() const noexcept { return state_; }

const std::optional<FailureReason>& NodeLifecycle::reason() const noexcept { return reason_; }

bool NodeLifecycle::gone() const noexcept { return gone_; }

bool NodeLifecycle::portBlockReady() noexcept {
	if (gone_ || state_ != NodeState::starting) return false;
	state_ = NodeState::running;
	return true;
}

bool NodeLifecycle::refused(FailureReason reason) {
	if (gone_ || (state_ != NodeState::starting && state_ != NodeState::running)) return false;
	state_ = NodeState::failed;
	reason_ = std::move(reason);
	return true;
}

bool NodeLifecycle::stopRequested() noexcept {
	if (gone_ || state_ == NodeState::stopping) return false;
	state_ = NodeState::stopping;
	reason_.reset();   // a reason is set only while failed
	return true;
}

bool NodeLifecycle::stopped() noexcept {
	if (gone_ || state_ != NodeState::stopping) return false;
	gone_ = true;
	return true;
}

} // namespace ear
