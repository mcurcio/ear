// The node lifecycle's two pure machines and the aggregate (docs/design/node-lifecycle.md section 4: 4.1's and 4.2's
// tables, 4.3's derivation and rules, 4.4's operations). node_machines.hpp says what each part is.
#include "ear/core/node_machines.hpp"

#include <utility>

namespace ear::lifecycle {

// ---- Names ----

NodeState stateOf(NodePhase phase) noexcept {
	switch (phase) {
	case NodePhase::added:
	case NodePhase::configuring:
	case NodePhase::configured:
	case NodePhase::starting:
		return NodeState::starting;
	case NodePhase::running:
		return NodeState::running;
	case NodePhase::stopping:
		return NodeState::stopping;
	case NodePhase::stopped:
		return NodeState::stopped;
	case NodePhase::disabled:
		return NodeState::disabled;
	case NodePhase::failed:
		return NodeState::failed;
	}
	return NodeState::failed;
}

std::string_view phaseName(NodePhase phase) noexcept {
	switch (phase) {
	case NodePhase::added:
		return "added";
	case NodePhase::configuring:
		return "configuring";
	case NodePhase::configured:
		return "configured";
	case NodePhase::starting:
		return "starting";
	case NodePhase::running:
		return "running";
	case NodePhase::stopping:
		return "stopping";
	case NodePhase::stopped:
		return "stopped";
	case NodePhase::disabled:
		return "disabled";
	case NodePhase::failed:
		return "failed";
	}
	return "unknown";
}

std::string_view stateName(NodeState state) noexcept {
	switch (state) {
	case NodeState::starting:
		return "starting";
	case NodeState::running:
		return "running";
	case NodeState::stopping:
		return "stopping";
	case NodeState::stopped:
		return "stopped";
	case NodeState::disabled:
		return "disabled";
	case NodeState::failed:
		return "failed";
	}
	return "unknown";
}

std::string_view wantedName(Wanted wanted) noexcept {
	switch (wanted) {
	case Wanted::running:
		return "running";
	case Wanted::stopped:
		return "stopped";
	case Wanted::disabled:
		return "disabled";
	}
	return "unknown";
}

std::string_view causeName(NodeCause::Kind kind) noexcept {
	switch (kind) {
	case NodeCause::Kind::stop:
		return "stop";
	case NodeCause::Kind::removing:
		return "removing";
	case NodeCause::Kind::disabling:
		return "disabling";
	case NodeCause::Kind::restarting:
		return "restarting";
	case NodeCause::Kind::settings:
		return "settings";
	case NodeCause::Kind::action:
		return "action";
	case NodeCause::Kind::retry:
		return "retry";
	case NodeCause::Kind::enabling:
		return "enabling";
	}
	return "unknown";
}

std::string_view failureCodeName(FailureCode code) noexcept {
	switch (code) {
	case FailureCode::startFailed:
		return "start-failed";
	case FailureCode::engineRefused:
		return "engine-refused";
	case FailureCode::lost:
		return "lost";
	}
	return "unknown";
}

std::string_view blockStateName(BlockState state) noexcept {
	switch (state) {
	case BlockState::absent:
		return "absent";
	case BlockState::waiting:
		return "waiting";
	case BlockState::made:
		return "made";
	case BlockState::ready:
		return "ready";
	case BlockState::lost:
		return "lost";
	case BlockState::gone:
		return "gone";
	}
	return "unknown";
}

std::string_view blockEventName(BlockEvent event) noexcept {
	switch (event) {
	case BlockEvent::addedWedged:
		return "added while wedged";
	case BlockEvent::added:
		return "added";
	case BlockEvent::addedBehindOlder:
		return "added behind an older block";
	case BlockEvent::olderDestroyed:
		return "older destroyed";
	case BlockEvent::ready:
		return "ready";
	case BlockEvent::lost:
		return "lost";
	case BlockEvent::removed:
		return "removed";
	}
	return "unknown";
}

std::string_view configStateName(ConfigState state) noexcept {
	switch (state) {
	case ConfigState::none:
		return "none";
	case ConfigState::readying:
		return "readying";
	case ConfigState::prepared:
		return "prepared";
	case ConfigState::arming:
		return "arming";
	case ConfigState::driving:
		return "driving";
	case ConfigState::fading:
		return "fading";
	case ConfigState::silencing:
		return "silencing";
	case ConfigState::tearing:
		return "tearing";
	}
	return "unknown";
}

std::string_view configEventName(ConfigEvent event) noexcept {
	switch (event) {
	case ConfigEvent::configure:
		return "configure";
	case ConfigEvent::resourcesReady:
		return "resources ready";
	case ConfigEvent::stopAsked:
		return "stop asked";
	case ConfigEvent::startAsked:
		return "start asked";
	case ConfigEvent::teardownAsked:
		return "teardown asked";
	case ConfigEvent::rest:
		return "rest";
	case ConfigEvent::drive:
		return "drive";
	case ConfigEvent::driven:
		return "driven";
	case ConfigEvent::faded:
		return "faded";
	case ConfigEvent::fadeBound:
		return "fade bound";
	case ConfigEvent::silencedForStop:
		return "silenced for a stop";
	case ConfigEvent::silenced:
		return "silenced";
	case ConfigEvent::barrierBound:
		return "barrier bound";
	case ConfigEvent::closed:
		return "closed";
	case ConfigEvent::closeBound:
		return "close bound";
	case ConfigEvent::blockLost:
		return "block lost";
	case ConfigEvent::startThrew:
		return "start threw";
	}
	return "unknown";
}

std::string_view configActionName(ConfigAction::Kind kind) noexcept {
	using enum ConfigAction::Kind;
	switch (kind) {
	case configure:
		return "configure";
	case runRecipe:
		return "run recipe";
	case prepare:
		return "prepare";
	case drive:
		return "drive";
	case start:
		return "start";
	case stop:
		return "stop";
	case fade:
		return "fade";
	case silence:
		return "silence";
	case fadeUnacknowledged:
		return "fade unacknowledged";
	case release:
		return "release";
	case teardown:
		return "teardown";
	case free:
		return "free";
	case barrierOverdue:
		return "barrier overdue";
	case markWedged:
		return "mark wedged";
	case leak:
		return "leak";
	case closeOverdue:
		return "close overdue";
	}
	return "unknown";
}

std::string_view intentName(Intent intent) noexcept {
	switch (intent) {
	case Intent::none:
		return "none";
	case Intent::stop:
		return "stop";
	case Intent::restart:
		return "restart";
	case Intent::fail:
		return "fail";
	case Intent::disable:
		return "disable";
	case Intent::remove:
		return "remove";
	}
	return "unknown";
}

std::string_view appliedName(LifecycleApplied applied) noexcept {
	switch (applied) {
	case LifecycleApplied::start:
		return "start";
	case LifecycleApplied::stop:
		return "stop";
	case LifecycleApplied::restart:
		return "restart";
	case LifecycleApplied::retry:
		return "retry";
	case LifecycleApplied::unchanged:
		return "unchanged";
	}
	return "unknown";
}

// ---- The block machine (4.1) ----

std::vector<BlockAction> BlockMachine::on(BlockEvent event, std::string detail) {
	using enum BlockState;
	switch (state_) {
	case absent:
		if (event == BlockEvent::addedWedged) {
			// Making a block would call into the wedged loop (D-L10): nothing is queued.
			state_ = lost;
			lossDetail_ = std::string(wedgedDetail);
			return {};
		}
		if (event == BlockEvent::added) {
			state_ = made;
			return {BlockAction::make};
		}
		if (event == BlockEvent::addedBehindOlder) state_ = waiting;
		return {};
	case waiting:
		if (event == BlockEvent::olderDestroyed) {
			state_ = made;
			return {BlockAction::make};
		}
		if (event == BlockEvent::removed) state_ = gone; // nothing to destroy
		return {};
	case made:
	case ready:
		if (event == BlockEvent::ready && state_ == made) {
			state_ = ready;
			return {};
		}
		if (event == BlockEvent::lost) {
			state_ = lost;
			lossDetail_ = std::move(detail);
			return {};
		}
		[[fallthrough]];
	case lost: // a lost block is never remade (I1): only its removal moves it
		if (event == BlockEvent::removed) {
			state_ = gone;
			return {BlockAction::destroy};
		}
		return {};
	case gone:
		return {};
	}
	return {};
}

// ---- The configuration machine (4.2) ----

void ConfigurationMachine::reset() noexcept {
	started_ = false;
	driven_ = false;
	rested_ = false;
	stopAsked_ = false;
}

ConfigAction ConfigurationMachine::post(ConfigAction::Kind kind) { return {kind, ++barrier_}; }

/// driving, stop or teardown asked: stop() if started, then a fade, or a silence when no output is faded.
std::vector<ConfigAction> ConfigurationMachine::stopDriving() {
	std::vector<ConfigAction> actions;
	if (started_) {
		actions.push_back({ConfigAction::Kind::stop});
		started_ = false;
	}
	actions.push_back(fadeOrSilence());
	return actions;
}

/// A driven code goes silent: a fade first, or a silence at once when no output is faded.
ConfigAction ConfigurationMachine::fadeOrSilence() {
	state_ = fades_ ? ConfigState::fading : ConfigState::silencing;
	return post(fades_ ? ConfigAction::Kind::fade : ConfigAction::Kind::silence);
}

/// readying or prepared, never driven: no barrier; release if prepared; then the teardown (I5, "before ready").
std::vector<ConfigAction> ConfigurationMachine::beginTeardown() {
	std::vector<ConfigAction> actions;
	if (state_ == ConfigState::prepared) actions.push_back({ConfigAction::Kind::release});
	actions.push_back({ConfigAction::Kind::teardown});
	state_ = ConfigState::tearing;
	return actions;
}

std::vector<ConfigAction> ConfigurationMachine::on(const ConfigInput& input) {
	using enum ConfigState;
	using Kind = ConfigAction::Kind;
	const ConfigEvent event = input.event;
	const bool current = input.barrier == barrier_;
	switch (state_) {
	case none:
		if (event != ConfigEvent::configure) return {};
		reset();
		fades_ = input.fades;
		state_ = readying;
		return {{Kind::configure}};
	case readying:
		if (event == ConfigEvent::resourcesReady) {
			state_ = prepared;
			return {{Kind::runRecipe}, {Kind::prepare}};
		}
		if (event == ConfigEvent::teardownAsked || event == ConfigEvent::blockLost) return beginTeardown();
		return {};
	case prepared:
		if (event == ConfigEvent::rest) {
			rested_ = true;
			return {};
		}
		if (event == ConfigEvent::drive) {
			driven_ = true;
			state_ = arming;
			return {post(Kind::drive)};
		}
		// A prepared configuration is never driven (T3, or T7 cleared it): a lost block needs no silence.
		if (event == ConfigEvent::teardownAsked || event == ConfigEvent::blockLost) return beginTeardown();
		return {};
	case arming:
		if (event == ConfigEvent::driven && current) {
			if (stopAsked_) { // 6.3's "stop in starting": no start(); fade what was just driven
				stopAsked_ = false;
				return {fadeOrSilence()};
			}
			started_ = true;
			rested_ = true;
			state_ = driving;
			return {{Kind::start}};
		}
		if (event == ConfigEvent::stopAsked || event == ConfigEvent::teardownAsked) stopAsked_ = true;
		if (event == ConfigEvent::startAsked) stopAsked_ = false;
		if (event == ConfigEvent::blockLost) {
			stopAsked_ = false;
			state_ = silencing;
			return {post(Kind::silence)}; // completes by signal on a block with no data loop
		}
		return {};
	case driving:
		if (event == ConfigEvent::stopAsked || event == ConfigEvent::teardownAsked) return stopDriving();
		if (event == ConfigEvent::startThrew) {
			started_ = false;
			rested_ = false;
		}
		if (event == ConfigEvent::blockLost) {
			started_ = false; // no stop(), no fade
			state_ = silencing;
			return {post(Kind::silence)};
		}
		return {};
	case fading:
		if (event == ConfigEvent::faded && current) {
			state_ = silencing;
			return {post(Kind::silence)};
		}
		if (event == ConfigEvent::fadeBound) {
			state_ = silencing;
			return {{Kind::fadeUnacknowledged}, post(Kind::silence)};
		}
		if (event == ConfigEvent::blockLost) {
			state_ = silencing;
			return {post(Kind::silence)};
		}
		return {};
	case silencing:
		if (event == ConfigEvent::silencedForStop && current) {
			driven_ = false;
			state_ = prepared;
			return {{Kind::release}, {Kind::prepare}}; // the next drive starts it afresh (D-L33)
		}
		if (event == ConfigEvent::silenced && current) {
			driven_ = false;
			state_ = tearing;
			return {{Kind::release}, {Kind::teardown}};
		}
		if (event == ConfigEvent::barrierBound) {
			reset();
			state_ = none;
			return {{Kind::barrierOverdue}, {Kind::markWedged}, {Kind::leak}};
		}
		return {};
	case tearing:
		if (event == ConfigEvent::closed) {
			reset();
			state_ = none;
			return {{Kind::free}};
		}
		if (event == ConfigEvent::closeBound) {
			reset();
			state_ = none;
			return {{Kind::closeOverdue}, {Kind::leak}};
		}
		return {};
	}
	return {};
}

// ---- The failure's code, by fact (4.2) ----

FailureCode failureCodeFor(Fact fact, bool rested) noexcept {
	switch (fact) {
	case Fact::engine:
		return FailureCode::engineRefused;
	case Fact::configureFailed:
	case Fact::consumerLost:
	case Fact::recipeFailed:
		return FailureCode::startFailed;
	case Fact::nodeLost:
	case Fact::hookThrew:
		return rested ? FailureCode::lost : FailureCode::startFailed;
	}
	return FailureCode::startFailed;
}

// ---- The derivation (4.3) ----

namespace {

bool stoppingConfig(ConfigState state) noexcept {
	return state == ConfigState::fading || state == ConfigState::silencing || state == ConfigState::tearing;
}

/// restarting, with the restart's own kind and keys.
NodeCause restarting(const std::optional<NodeCause>& why) {
	NodeCause cause{NodeCause::Kind::restarting, std::nullopt, {}};
	if (why) {
		cause.restart = why->kind;
		cause.keys = why->keys;
	}
	return cause;
}

std::optional<NodeCause> causeOf(Intent intent, const std::optional<NodeCause>& why) {
	switch (intent) {
	case Intent::stop:
		return NodeCause{NodeCause::Kind::stop, std::nullopt, {}};
	case Intent::restart:
		return restarting(why);
	case Intent::disable:
		return NodeCause{NodeCause::Kind::disabling, std::nullopt, {}};
	case Intent::remove:
		return NodeCause{NodeCause::Kind::removing, std::nullopt, {}};
	case Intent::none:
	case Intent::fail:
		return std::nullopt;
	}
	return std::nullopt;
}

int rank(Intent intent) noexcept { return static_cast<int>(intent); }

} // namespace

bool removed(const DeriveFacts& facts) noexcept {
	return facts.block == BlockState::gone && facts.config == ConfigState::none && facts.intent == Intent::remove;
}

Health derive(const DeriveFacts& f) {
	Health h{NodePhase::added, NodeState::starting, Wanted::running, std::nullopt, std::nullopt, false};
	h.wanted = f.disabled ? Wanted::disabled : f.stopHeld ? Wanted::stopped : Wanted::running;
	h.blockLost = f.block == BlockState::lost;
	if (f.intent == Intent::remove) {
		h.phase = NodePhase::stopping;
		h.cause = causeOf(Intent::remove, f.why);
	} else if (f.predecessor) {
		h.phase = NodePhase::stopping;
		h.cause = restarting(f.why);
	} else if (f.failure || (h.blockLost && !f.disabled)) {
		h.phase = NodePhase::failed;
		h.reason = f.failure ? *f.failure : Failure{FailureCode::engineRefused, f.blockLossDetail};
	} else if (f.config == ConfigState::none && f.disabled) {
		h.phase = NodePhase::disabled;
	} else if (stoppingConfig(f.config) || (f.config == ConfigState::arming && f.stopAsked) ||
	           (f.config == ConfigState::none && f.intent == Intent::restart)) {
		h.phase = NodePhase::stopping;
		h.cause = causeOf(f.intent, f.why);
	} else if (f.config == ConfigState::none) {
		h.phase = NodePhase::added;
	} else if (f.config == ConfigState::readying) {
		h.phase = NodePhase::configuring;
		h.cause = f.why;
	} else if (f.config == ConfigState::prepared) {
		h.phase = f.stopHeld                         ? NodePhase::stopped
		          : (!f.configuredTold && !f.rested) ? NodePhase::configured
		                                             : NodePhase::starting;
	} else if (f.config == ConfigState::arming) {
		h.phase = NodePhase::starting;
	} else {
		h.phase = NodePhase::running;
	}
	h.state = stateOf(h.phase);
	return h;
}

// ---- The aggregate (4.3, 4.4) ----

DeriveFacts NodeAggregate::facts() const {
	return {block_.state(),
	        block_.lossDetail(),
	        config_.state(),
	        config_.rested(),
	        config_.stopAsked(),
	        disabled_,
	        stopHeld_,
	        intent_,
	        why_,
	        failure_,
	        predecessor_,
	        configuredTold_};
}

Health NodeAggregate::health() const { return derive(facts()); }

bool NodeAggregate::removed() const noexcept {
	return block_.state() == BlockState::gone && config_.state() == ConfigState::none && intent_ == Intent::remove;
}

Wanted NodeAggregate::wanted() const noexcept {
	return disabled_ ? Wanted::disabled : stopHeld_ ? Wanted::stopped : Wanted::running;
}

bool NodeAggregate::changePending() const noexcept {
	return config_.state() != ConfigState::none && queued_.version != configuredVersion_;
}

bool NodeAggregate::waitingForBlock() const {
	return health().phase == NodePhase::starting && block_.state() != BlockState::ready;
}

bool NodeAggregate::failed() const { return health().phase == NodePhase::failed; }

/// Rule 3: a newer intent replaces the one in flight only if it ranks higher (remove over disable over fail over
/// restart over stop); with none in flight any intent is set.
bool NodeAggregate::raise(Intent next, std::optional<NodeCause> why) {
	if (intent_ != Intent::none && rank(next) <= rank(intent_)) return false;
	intent_ = next;
	if (why) why_ = std::move(why);
	return true;
}

void NodeAggregate::tell(Actions& out) {
	if (goneTold_) return;
	if (removed()) {
		goneTold_ = true;
		out.emplace_back(Gone{});
		return;
	}
	for (;;) {
		const Health now = health();
		if (told_ && *told_ == now) return;
		out.emplace_back(Tell{now});
		told_ = now;
		// Row 10: configured is told once, then derived again (starting).
		if (now.phase != NodePhase::configured) return;
		configuredTold_ = true;
	}
}

void NodeAggregate::onBlock(BlockEvent event, std::string detail, Actions& out) {
	for (BlockAction action : block_.on(event, std::move(detail))) out.emplace_back(action);
	tell(out);
}

/// The configuration machine moves; the facts its moves end are ended here (rule 3's lifetimes), then health is told.
void NodeAggregate::onConfig(ConfigInput input, Actions& out) {
	const ConfigState before = config_.state();
	for (const ConfigAction& action : config_.on(input)) out.emplace_back(action);
	const ConfigState after = config_.state();
	if (after != before) {
		if (after != ConfigState::prepared) configuredTold_ = false;
		if (before == ConfigState::readying && after == ConfigState::prepared) why_.reset();
		if (before == ConfigState::silencing && after == ConfigState::prepared && intent_ == Intent::stop) {
			intent_ = Intent::none; // the stop rested (T7)
			if (heldFault_) {       // and a fault held during it fails the node before its rest is told (D-L32)
				intent_ = Intent::fail;
				failure_ = std::move(heldFault_);
				heldFault_.reset();
			}
		}
		if (after == ConfigState::none && (intent_ == Intent::disable || intent_ == Intent::fail))
			intent_ = Intent::none;
	}
	destroyIfUndriven(out);
	tell(out);
}

/// T17: the block is destroyed once the removal's silence completed, or at once when no code was driven (D-L7).
void NodeAggregate::destroyIfUndriven(Actions& out) {
	if (intent_ != Intent::remove || config_.driven()) return;
	const BlockState state = block_.state();
	if (state == BlockState::absent || state == BlockState::gone) return;
	for (BlockAction action : block_.on(BlockEvent::removed)) out.emplace_back(action);
}

void NodeAggregate::askConfig(ConfigEvent event, Actions& out) { onConfig({event}, out); }

/// A restart (intent restart): the configuration's teardown, then configure from empty with queued.
bool NodeAggregate::beginRestart(NodeCause why, Actions& out) {
	if (!raise(Intent::restart, std::move(why))) return false; // a higher intent is in flight
	askConfig(ConfigEvent::teardownAsked, out);
	return true;
}

/// Rule 1, in its order, until nothing more applies.
void NodeAggregate::converge(Actions& out) {
	using enum ConfigState;
	for (;;) {
		if (goneTold_) return;
		const ConfigState state = config_.state();
		const BlockState block = block_.state();
		const bool blockUsable = block == BlockState::made || block == BlockState::ready;
		if (state == none && !disabled_ && (!failure_ || retryAsked_) && blockUsable && !predecessor_ && !olderNode_ &&
		    intent_ != Intent::remove) {
			retryAsked_ = false;
			heldFault_.reset();
			if (intent_ == Intent::restart) intent_ = Intent::none; // its cause stays for configuring
			if (wedged_) {
				// Nothing can be driven on a wedged loop (D-L10): the node fails instead of configuring.
				failure_ = Failure{FailureCode::engineRefused, std::string(wedgedDetail)};
				tell(out);
				return;
			}
			failure_.reset();
			configuredVersion_ = queued_.version;
			onConfig({ConfigEvent::configure, 0, queued_.fades}, out);
			continue;
		}
		if (state == prepared && intent_ == Intent::fail) { // T14 for the fault held during the stop (D-L32)
			askConfig(ConfigEvent::teardownAsked, out);
			continue;
		}
		if (state == prepared && changePending()) { // before any drive (D-L41)
			if (!beginRestart({NodeCause::Kind::settings, std::nullopt, changedKeys_}, out)) return;
			continue;
		}
		if (state == prepared && wanted() == Wanted::running && block == BlockState::ready) {
			askConfig(ConfigEvent::drive, out);
			continue;
		}
		if (state == prepared && wanted() == Wanted::stopped && !config_.rested()) {
			askConfig(ConfigEvent::rest, out);
			continue;
		}
		if ((state == arming && !config_.stopAsked()) || state == driving) {
			if (wanted() == Wanted::stopped) {
				raise(Intent::stop);
				askConfig(ConfigEvent::stopAsked, out);
				continue;
			}
		}
		return;
	}
}

Actions NodeAggregate::add(const AddFacts& add) {
	Actions out;
	if (block_.state() != BlockState::absent) return out;
	disabled_ = add.disabled;
	stopHeld_ = add.stopHeld;
	predecessor_ = add.predecessor;
	olderNode_ = add.olderNode;
	wedged_ = add.wedged;
	queued_ = add.queued;
	changedKeys_ = add.keys;
	if (add.predecessor) why_ = NodeCause{NodeCause::Kind::settings, std::nullopt, add.keys};
	onBlock(add.wedged       ? BlockEvent::addedWedged
	        : add.olderBlock ? BlockEvent::addedBehindOlder
	                         : BlockEvent::added,
	        {}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::olderDestroyed() {
	Actions out;
	if (goneTold_) return out;
	if (predecessorClosed_) predecessor_ = false;
	// Told once converged: a successor goes from row 3 straight to configuring.
	for (BlockAction action : block_.on(BlockEvent::olderDestroyed)) out.emplace_back(action);
	converge(out);
	tell(out);
	return out;
}

Actions NodeAggregate::predecessorGone() {
	Actions out;
	if (goneTold_ || !predecessor_) return out;
	// Its block's destruction may still be queued: until it ran, this node waits in row 3, never added.
	predecessorClosed_ = true;
	if (block_.state() == BlockState::waiting) return out;
	predecessor_ = false;
	converge(out);
	tell(out);
	return out;
}

Actions NodeAggregate::olderNodeGone() {
	Actions out;
	if (goneTold_ || !olderNode_) return out;
	olderNode_ = false;
	converge(out);
	tell(out);
	return out;
}

Actions NodeAggregate::blockReady() {
	Actions out;
	if (goneTold_) return out;
	onBlock(BlockEvent::ready, {}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::blockLost(std::string detail) {
	Actions out;
	if (goneTold_) return out;
	const BlockState before = block_.state();
	if (before != BlockState::made && before != BlockState::ready) return out;
	// The block's loss replaces any failure (4.4's "reason replaced"), and nothing is configured again (I1).
	failure_.reset();
	heldFault_.reset();
	retryAsked_ = false;
	if (config_.state() != ConfigState::none) raise(Intent::fail);
	onBlock(BlockEvent::lost, std::move(detail), out);
	askConfig(ConfigEvent::blockLost, out);
	converge(out);
	return out;
}

Actions NodeAggregate::change(Queued queued, std::vector<std::string> keys) {
	Actions out;
	if (goneTold_ || intent_ == Intent::remove) return out; // not wanted any more
	queued_ = queued;
	changedKeys_ = keys;
	if (disabled_) return out; // stored for the enable
	if (failed()) {
		if (block_.state() != BlockState::lost) { // T15 once the failed configuration is gone (D-L31)
			retryAsked_ = true;
			why_ = NodeCause{NodeCause::Kind::settings, std::nullopt, std::move(keys)};
		}
		converge(out);
		tell(out);
		return out;
	}
	const ConfigState state = config_.state();
	if (state == ConfigState::readying || state == ConfigState::prepared || state == ConfigState::arming ||
	    state == ConfigState::driving) // rule 2: a settled configuration restarts at once (D-L3)
		beginRestart({NodeCause::Kind::settings, std::nullopt, std::move(keys)}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::disable() {
	Actions out;
	if (goneTold_ || intent_ == Intent::remove || disabled_) return out;
	disabled_ = true;
	stopHeld_ = false; // the node's stop, if held, is dropped (T20)
	failure_.reset();  // T21
	retryAsked_ = false;
	heldFault_.reset();
	if (config_.state() != ConfigState::none) {
		raise(Intent::disable);
		askConfig(ConfigEvent::teardownAsked, out);
	}
	converge(out);
	tell(out);
	return out;
}

Actions NodeAggregate::enable() {
	Actions out;
	if (goneTold_ || intent_ == Intent::remove || !disabled_) return out;
	disabled_ = false;
	why_ = NodeCause{NodeCause::Kind::enabling, std::nullopt, {}};
	if (intent_ == Intent::disable) intent_ = Intent::restart; // configured afresh once the old one is gone
	converge(out);
	tell(out);
	return out;
}

Actions NodeAggregate::remove() {
	Actions out;
	if (goneTold_ || intent_ == Intent::remove) return out;
	intent_ = Intent::remove;
	if (config_.state() != ConfigState::none) askConfig(ConfigEvent::teardownAsked, out);
	destroyIfUndriven(out);
	tell(out);
	return out;
}

Operated NodeAggregate::stop() {
	using Code = LifecycleRefusal::Code;
	if (goneTold_ || intent_ == Intent::remove) return {std::unexpected(LifecycleRefusal{Code::unknownNode}), {}};
	if (disabled_) return {std::unexpected(LifecycleRefusal{Code::nodeDisabled}), {}};
	if (stopHeld_) return {LifecycleApplied::unchanged, {}};
	Actions out;
	stopHeld_ = true;
	converge(out);
	tell(out);
	return {LifecycleApplied::stop, std::move(out)};
}

Operated NodeAggregate::start() {
	using Code = LifecycleRefusal::Code;
	if (goneTold_ || intent_ == Intent::remove) return {std::unexpected(LifecycleRefusal{Code::unknownNode}), {}};
	if (disabled_) return {std::unexpected(LifecycleRefusal{Code::nodeDisabled}), {}};
	Actions out;
	if (failed()) {
		if (block_.state() == BlockState::lost)
			return {std::unexpected(LifecycleRefusal{Code::notRestartable, LifecycleRefusal::Reason::blockLost}), {}};
		retryAsked_ = true; // a retry, even when nothing holds it stopped (T15)
		stopHeld_ = false;
		why_ = NodeCause{NodeCause::Kind::retry, std::nullopt, {}};
		converge(out);
		tell(out);
		return {LifecycleApplied::retry, std::move(out)};
	}
	if (!stopHeld_) return {LifecycleApplied::unchanged, {}};
	stopHeld_ = false;
	if (config_.state() == ConfigState::arming && intent_ == Intent::stop) {
		intent_ = Intent::none; // a start cancels the stop while its drive is in flight
		askConfig(ConfigEvent::startAsked, out);
	}
	converge(out);
	tell(out);
	return {LifecycleApplied::start, std::move(out)};
}

Operated NodeAggregate::restart() {
	using Code = LifecycleRefusal::Code;
	using Reason = LifecycleRefusal::Reason;
	if (goneTold_ || intent_ == Intent::remove) return {std::unexpected(LifecycleRefusal{Code::unknownNode}), {}};
	if (disabled_) return {std::unexpected(LifecycleRefusal{Code::nodeDisabled}), {}};
	Actions out;
	if (failed()) {
		if (block_.state() == BlockState::lost)
			return {std::unexpected(LifecycleRefusal{Code::notRestartable, Reason::blockLost}), {}};
		retryAsked_ = true;
		why_ = NodeCause{NodeCause::Kind::retry, std::nullopt, {}};
		converge(out);
		tell(out);
		return {LifecycleApplied::retry, std::move(out)};
	}
	if (intent_ == Intent::restart || predecessor_) return {LifecycleApplied::unchanged, {}};
	if (health().phase == NodePhase::added)
		return {std::unexpected(LifecycleRefusal{Code::notRestartable, Reason::notStarted}), {}};
	beginRestart({NodeCause::Kind::action, std::nullopt, {}}, out);
	converge(out);
	return {LifecycleApplied::restart, std::move(out)};
}

Actions NodeAggregate::fault(Fact fact, std::string detail) {
	Actions out;
	if (goneTold_) return out;
	const ConfigState state = config_.state();
	Failure failure{failureCodeFor(fact, config_.rested()), std::move(detail)};
	switch (state) {
	case ConfigState::none:
		return out;
	case ConfigState::arming:
		// A drive in flight whose completion will fade and silence it (a stop or teardown asked, 4.3's row 6) is a
		// stop or teardown already begun: as in fading, held during a stop and dropped during a teardown (I6), so a
		// fault cannot turn a restart, disable or removal into a failure.
		if (config_.stopAsked()) {
			if (intent_ == Intent::stop && !heldFault_) heldFault_ = std::move(failure);
			return out;
		}
		[[fallthrough]];
	case ConfigState::readying:
	case ConfigState::prepared:
	case ConfigState::driving:
		if (!raise(Intent::fail)) return out; // a disable or removal in flight drops it
		failure_ = std::move(failure);
		askConfig(ConfigEvent::teardownAsked, out);
		break;
	case ConfigState::fading:
	case ConfigState::silencing:
		// Held during a stop and applied once rested (D-L32); dropped during a teardown (I6).
		if (intent_ == Intent::stop && !heldFault_) heldFault_ = std::move(failure);
		return out;
	case ConfigState::tearing:
		return out;
	}
	converge(out);
	return out;
}

Actions NodeAggregate::startThrew(std::string detail) {
	if (goneTold_ || config_.state() != ConfigState::driving) return {};
	Actions out;
	askConfig(ConfigEvent::startThrew, out);
	Actions failed = fault(Fact::hookThrew, std::move(detail));
	out.insert(out.end(), failed.begin(), failed.end());
	return out;
}

Actions NodeAggregate::resourcesReady() {
	Actions out;
	if (goneTold_) return out;
	askConfig(ConfigEvent::resourcesReady, out);
	converge(out);
	return out;
}

Actions NodeAggregate::driven(std::uint64_t barrier) {
	Actions out;
	if (goneTold_) return out;
	onConfig({ConfigEvent::driven, barrier}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::faded(std::uint64_t barrier) {
	Actions out;
	if (goneTold_) return out;
	onConfig({ConfigEvent::faded, barrier}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::silenced(std::uint64_t barrier) {
	Actions out;
	if (goneTold_) return out;
	onConfig({intent_ == Intent::stop ? ConfigEvent::silencedForStop : ConfigEvent::silenced, barrier}, out);
	converge(out);
	return out;
}

Actions NodeAggregate::closed() {
	Actions out;
	if (goneTold_) return out;
	askConfig(ConfigEvent::closed, out);
	converge(out);
	return out;
}

Actions NodeAggregate::fadeBound() {
	Actions out;
	if (goneTold_) return out;
	askConfig(ConfigEvent::fadeBound, out);
	converge(out);
	return out;
}

Actions NodeAggregate::barrierBound() {
	Actions out;
	if (goneTold_ || config_.state() != ConfigState::silencing) return out;
	wedged_ = true;
	// The configuration is leaked on a wedged loop: a stop or a restart can never end, so the node fails
	// engine-refused (the code for a wedged loop, 4.2); a disable or removal goes on to its end.
	if (raise(Intent::fail)) failure_ = Failure{FailureCode::engineRefused, std::string(wedgedDetail)};
	askConfig(ConfigEvent::barrierBound, out);
	converge(out);
	return out;
}

Actions NodeAggregate::closeBound() {
	Actions out;
	if (goneTold_) return out;
	askConfig(ConfigEvent::closeBound, out);
	converge(out);
	return out;
}

Actions NodeAggregate::dataLoopWedged() {
	Actions out;
	if (goneTold_ || wedged_) return out;
	wedged_ = true;
	converge(out);
	return out;
}

} // namespace ear::lifecycle
