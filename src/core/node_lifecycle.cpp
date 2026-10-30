// The one node state machine (docs/design/batch2.md section 4.1, node_lifecycle.hpp),
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

namespace {

bool sameReason(const std::optional<FailureReason>& held, const FailureReason& reason) {
	return held && held->code == reason.code && held->detail == reason.detail;
}

/// starting -> running once both readiness facts hold.
bool runIfReady(NodeState& state, bool blockReady, bool insidesReady) noexcept {
	if (state != NodeState::starting || !blockReady || !insidesReady) return false;
	state = NodeState::running;
	return true;
}

} // namespace

NodeLifecycle::NodeLifecycle() noexcept
	: state_(NodeState::starting), blockReady_(false), blockLost_(false), insidesReady_(false), gone_(false) {}

NodeState NodeLifecycle::state() const noexcept { return state_; }

const std::optional<FailureReason>& NodeLifecycle::reason() const noexcept { return reason_; }

bool NodeLifecycle::gone() const noexcept { return gone_; }

bool NodeLifecycle::blockLost() const noexcept { return blockLost_; }

bool NodeLifecycle::portBlockReady() noexcept {
	if (gone_ || blockLost_) return false;
	blockReady_ = true;
	return runIfReady(state_, blockReady_, insidesReady_);
}

bool NodeLifecycle::portBlockLost(FailureReason reason) {
	if (gone_ || state_ == NodeState::stopping) return false;
	blockLost_ = true;
	blockReady_ = false;
	const bool changed = state_ != NodeState::failed || !sameReason(reason_, reason);
	state_ = NodeState::failed;
	reason_ = std::move(reason);
	return changed;
}

bool NodeLifecycle::insidesReady() noexcept {
	if (gone_ || blockLost_) return false;
	insidesReady_ = true;
	return runIfReady(state_, blockReady_, insidesReady_);
}

bool NodeLifecycle::insidesFailed(FailureReason reason) {
	if (gone_ || blockLost_ || (state_ != NodeState::starting && state_ != NodeState::running)) return false;
	insidesReady_ = false;
	state_ = NodeState::failed;
	reason_ = std::move(reason);
	return true;
}

bool NodeLifecycle::restartRequested() noexcept {
	if (gone_ || blockLost_ || state_ == NodeState::stopping) return false;
	insidesReady_ = false;
	const bool changed = state_ != NodeState::starting || reason_.has_value();
	state_ = NodeState::starting;
	reason_.reset();
	return changed;
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
