// The node lifecycle's two pure machines and the node they make (docs/architecture.md#the-node-lifecycle). Each
// machine takes an event and returns the actions NodeHost carries out: neither calls a hook, PipeWire or a timer, so
// both are tested by table. The aggregate (NodeAggregate) holds both and the node's facts, applies the aggregate's
// three rules after every input, and tells its derived health after every machine transition that changes it.
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "ear/core/node_type.hpp"
namespace ear {

// ---- Names on the wire and on standard output (docs/control-port.md#node-health) ----

/// The exact state. Slice 8 adds recovering. A removed node has no phase: it is gone (NodeAggregate::removed()).
enum class NodePhase {
	added,
	configuring,
	configured,
	starting,
	running,
	stopping,
	stopped,
	disabled,
	failed,
	reconfiguring
};
/// The phase's colour: added, configuring and configured are starting; every other phase is its own name.
enum class NodeState { starting, running, stopping, stopped, disabled, failed, reconfiguring };
/// What the graph wants of the node: disabled from ear.conf, else stopped while its stop is held, else running.
enum class Wanted { running, stopped, disabled };
/// docs/architecture.md#phases-states-and-what-is-wanted failure codes and a failure: FailureReason's (node_type.hpp),
/// named for the machines.
using FailureCode = FailureReason::Code;
using Failure = FailureReason;
/// Why a node is stopping, or configuring after a restart, a retry or an enable (docs/control-port.md#node-health
/// cause.kind).
struct NodeCause {
	/// fellBack is a restart's own kind only: a change in place that could not finish
	/// (docs/architecture.md#changes-in-place). reshape is a settings change that changes the ports
	/// (docs/architecture.md#reshapes).
	enum class Kind { stop, removing, disabling, restarting, settings, action, retry, enabling, fellBack, reshape };
	/// Why a change in place fell back to a restart.
	enum class FellBack { restartAsked, deadline, resourceRefused, resourceLost, failed, restartRequested };
	Kind kind;
	/// restarting only: the restart's own kind (settings, reshape, action, enabling or fellBack), as configuring's.
	std::optional<Kind> restart;
	/// settings, reshape or fellBack, or restarting by one: the changed keys.
	std::vector<std::string> keys;
	/// fellBack, or restarting by it: why, and in words.
	std::optional<FellBack> reason{};
	std::string detail{};
	/// reshape, or restarting by it: the public ports the change adds and removes against the configuration's.
	PortsMoved ports{};
	bool operator==(const NodeCause&) const = default;
};

NodeState stateOf(NodePhase phase) noexcept;
std::string_view phaseName(NodePhase phase) noexcept;
std::string_view stateName(NodeState state) noexcept;
std::string_view wantedName(Wanted wanted) noexcept;
std::string_view causeName(NodeCause::Kind kind) noexcept;
std::string_view fellBackName(NodeCause::FellBack reason) noexcept;
inline std::string_view failureCodeName(FailureCode code) noexcept { return failureCode(code); }

// ---- The block machine ----

/// absent is "—": before the add. lost is final and keeps its detail.
enum class BlockState { absent, waiting, made, ready, lost, gone };
enum class BlockEvent {
	addedWedged,      // node added while the data loop is wedged (checked first)
	added,            // node added, no older block of its name
	addedBehindOlder, // node added, an older block of its name exists
	olderDestroyed,   // the older block of the name was destroyed
	ready,            // PortBlockEvents::ready
	lost,             // PortBlockEvents::lost, with its detail
	removed,          // removal: its silence completed, or no code driven since the last silence
};
/// make and destroy are queued into the turn's flush, never made inside a handler.
enum class BlockAction { make, destroy };
std::string_view blockStateName(BlockState state) noexcept;
std::string_view blockEventName(BlockEvent event) noexcept;

class BlockMachine {
public:
	BlockState state() const noexcept { return state_; }
	/// The loss's detail, set once lost.
	const std::string& lossDetail() const noexcept { return lossDetail_; }
	/// An event that does not apply changes nothing and returns no action.
	std::vector<BlockAction> on(BlockEvent event, std::string detail = {});

private:
	BlockState state_ = BlockState::absent;
	std::string lossDetail_;
};
/// The block's loss when a node is added after the data loop was judged wedged.
inline constexpr std::string_view wedgedDetail = "the data loop is wedged";

// ---- The configuration machine ----

/// changing and committing are a change in place (docs/architecture.md#changes-in-place): changing until
/// the change is ready to commit, committing while the commit's invoke is on the data loop.
enum class ConfigState { none, readying, prepared, arming, driving, fading, silencing, tearing, changing, committing };
enum class ConfigEvent {
	configure,       // the aggregate configures, with the outputs' fade fact
	resourcesReady,  // every resource ready (at once when none is pending)
	stopAsked,       // a stop
	startAsked,      // a start (the aggregate sends it in arming only while the intent is stop)
	teardownAsked,   // a restart, disable, removal or fault
	rest,            // the aggregate: wanted stopped, not rested
	drive,           // the aggregate: wanted running, block ready, no change pending
	driven,          // the drive's completion, with its barrier number
	faded,           // the fade's acknowledgement, with its barrier number
	fadeBound,       // 100 ms, after settle() found no acknowledgement
	silencedForStop, // the silence's completion, the intent a stop
	silenced,        // the silence's completion, any other intent
	barrierBound,    // 5 s, after settle() found the silence's invoke had not run
	closed,          // every sink closed, then every loop
	closeBound,      // 10 s
	blockLost,       // the block lost (T13)
	startThrew,      // start() threw: it never set started, so no stop() pairs with it
	changeAsked,     // a change in place, from driving, or from prepared once rested
	changeAgain,     // a newer change in place before the commit point (supersession)
	changeReady,     // the change version answered and every replacement is ready: the commit point
	committed,       // the commit's completion on the data loop, with its barrier number
};
struct ConfigInput {
	ConfigEvent event;
	std::uint64_t barrier = 0; // driven, faded, silenced, committed: the request's number
	bool fades = true;         // configure: some output the code writes is faded (fades)
};
struct ConfigAction {
	enum class Kind {
		configure,          // new generation; make the instance; creation's configure; ask the resources
		prepare,            // prepare the code
		drive,              // carry the latency plan; post drive(code, faded, sinks)
		start,              // call start()
		stop,               // call stop()
		fade,               // post a fade (100 ms timer)
		silence,            // post a silence (5 s timer)
		fadeUnacknowledged, // ear.fade-unacknowledged
		release,            // clear the latency plan; release the code
		teardown,           // queue the engine companions' destruction; close sinks; 10 s timer
		free,               // on the main loop: the code, the instance, the recordings, in that order
		barrierOverdue,     // ear.barrier-overdue
		markWedged,         // the data loop is wedged
		leak,               // leak the configuration
		closeOverdue,       // ear.close-overdue
		reconfigure,        // call the change version; start (or restart) the change's deadline
		commit,             // the commit point: with a barrier when driving (one invoke), none from prepared
		dropChange,         // end the change uncommitted: its replacements released, its staged posts destroyed
	};
	Kind kind;
	std::uint64_t barrier = 0; // drive, fade, silence, a driving commit: the number the request carries
	bool operator==(const ConfigAction&) const = default;
};
std::string_view configStateName(ConfigState state) noexcept;
std::string_view configEventName(ConfigEvent event) noexcept;
std::string_view configActionName(ConfigAction::Kind kind) noexcept;

class ConfigurationMachine {
public:
	ConfigState state() const noexcept { return state_; }
	bool started() const noexcept { return started_; } // start() returned and stop() not called since
	bool driven() const noexcept { return driven_; }   // a code handed to the block since the last silence
	bool rested() const noexcept { return rested_; }   // reached running or stopped
	bool stopAsked() const noexcept { return stopAsked_; }
	bool fades() const noexcept { return fades_; }
	std::uint64_t barrier() const noexcept { return barrier_; } // the newest request's number
	/// changing or committing: a change in place is in flight.
	bool changing() const noexcept { return state_ == ConfigState::changing || state_ == ConfigState::committing; }
	/// Where the change in flight began (driving or prepared), and where its commit returns; none otherwise.
	ConfigState changeFrom() const noexcept { return changeFrom_; }
	/// configure's return, while readying: whether some output the code writes is faded, now that the companions
	/// whose banks are never faded (a sink's) are known. Narrows what `configure` said from the ports alone.
	void outputsFaded(bool fades) noexcept {
		if (state_ == ConfigState::readying) fades_ = fades;
	}
	/// An event that does not apply (or a stale completion) changes nothing and returns no action.
	std::vector<ConfigAction> on(const ConfigInput& input);

private:
	std::vector<ConfigAction> stopDriving();
	ConfigAction fadeOrSilence();
	void throwStart();
	std::vector<ConfigAction> beginTeardown();
	std::vector<ConfigAction> onChanging(const ConfigInput& input);
	ConfigAction post(ConfigAction::Kind kind);
	void reset() noexcept;
	ConfigState state_ = ConfigState::none;
	bool started_ = false;
	bool driven_ = false;
	bool rested_ = false;
	bool stopAsked_ = false;
	bool fades_ = true;
	std::uint64_t barrier_ = 0;
	ConfigState changeFrom_ = ConfigState::none;
};

// ---- The failure's code, by fact ----

enum class Fact {
	engine,             // a sound card or RTP receiver refused or lost; the block refused or lost; the loop wedged
	configureFailed,    // configure answered failed(d)
	consumerNeverBegan, // a sink's consumer did not begin within its readiness deadline
	nodeLost,           // lost(detail) from the node's handle
	hookThrew,          // a type's code threw: a hook, its code's prepare, or a consumer
	adapterFaulted,     // ear's own bug in one of the node's adapters: lost whatever the phase
						// (docs/architecture.md#faults-by-owner)
};
FailureCode failureCodeFor(Fact fact, bool rested) noexcept;

// ---- The aggregate ----

/// What a stop or teardown in flight is for, in rising priority (rule 3): a newer one replaces it only if higher.
enum class Intent { none, stop, restart, fail, disable, remove };
std::string_view intentName(Intent intent) noexcept;

/// The health the aggregate derives (derive() below). waitingFor and since are NodeHost's.
struct Health {
	NodePhase phase;
	NodeState state;
	Wanted wanted;
	std::optional<NodeCause> cause;
	std::optional<Failure> reason;
	bool blockLost;
	/// reconfiguring only: the keys the change in flight applies, and those changed since its commit point, which the
	/// node takes once it ends.
	std::vector<std::string> keys{};
	std::vector<std::string> next{};
	bool operator==(const Health&) const = default;
};
/// What the derivation reads: the two machines' states and the aggregate's facts.
struct DeriveFacts {
	BlockState block = BlockState::made;
	std::string blockLossDetail{};
	ConfigState config = ConfigState::none;
	bool rested = false;
	bool stopAsked = false;
	bool disabled = false;
	bool stopHeld = false;
	Intent intent = Intent::none;
	std::optional<NodeCause> why{}; // a restart's, a retry's or an enable's own kind
	std::optional<Failure> failure{};
	bool configuredTold = false;
	FailureCode blockLossCode = FailureCode::engineRefused; // a block lost by its own bug: lost
	std::vector<std::string> changeKeys{};                  // changing or committing: the change's keys
	std::vector<std::string> nextKeys{};                    // committing: the keys that wait for its end
};
Health derive(const DeriveFacts& facts);
/// The derivation's first row: block gone, configuration none, intent remove.
bool removed(const DeriveFacts& facts) noexcept;

/// The newest committed settings: a version NodeHost names, and whether the code's outputs are faded with them.
struct Queued {
	std::uint64_t version = 0;
	bool fades = true;
};
struct AddFacts {
	bool disabled = false;
	bool olderBlock = false; // an older block of the name still exists (T1)
	bool olderNode = false;  // an older node of the name is not yet gone: no configure before it is (rule 1)
	bool wedged = false;     // the data loop was judged wedged
	Queued queued{};
};
struct Tell {
	Health health;
};
struct Gone {};
/// A fault the aggregate neither applied nor held (a teardown or a failure already outranks it, a second fault during a
/// stop, or no configuration to fail): what was dropped, so NodeHost can record it.
struct Dropped {
	Fact fact;
	std::string detail;
};
using NodeAction = std::variant<BlockAction, ConfigAction, Tell, Gone, Dropped>;
using Actions = std::vector<NodeAction>;

/// What the plan said a change of settings can be: a restart, or a change in place (every changed key's row marked).
enum class ChangeKind { restart, inPlace };

/// core's own replies to start, stop and restart, NodeOperations' (node_host.hpp); MainLoopSide maps them
/// to the wire.
enum class LifecycleApplied { start, stop, restart, retry, unchanged };
struct LifecycleRefusal {
	enum class Code { unknownNode, nodeDisabled, notRestartable, stopping } code;
	enum class Reason { none, notStarted, blockLost } reason = Reason::none; // notRestartable only
	bool operator==(const LifecycleRefusal&) const = default;
};
using LifecycleReply = std::expected<LifecycleApplied, LifecycleRefusal>;
std::string_view appliedName(LifecycleApplied applied) noexcept;
struct Operated {
	LifecycleReply reply;
	Actions actions;
};

/// One node: the two machines and the aggregate's facts. Every input returns the actions it caused, in order, with a
/// Tell after each machine transition that changed the derived health and a Gone once the node is removed. Inputs after
/// Gone change nothing.
class NodeAggregate {
public:
	Actions add(const AddFacts& facts);
	// The block's events.
	Actions olderDestroyed();
	/// Every older node of the name is gone (its block destroyed and its configuration none): rule 1 may configure.
	Actions olderNodeGone();
	Actions blockReady();
	/// The block refused or dropped (engine-refused), or, by `code` lost, failed by its own bug
	/// (docs/architecture.md#faults-by-owner); either way final.
	Actions blockLost(std::string detail, FailureCode code = FailureCode::engineRefused);
	// The plan's outcomes: a change (restart-only, or in place), a change on a disabled node, a disable, an enable, a
	// removal.
	/// A change in place is taken only by a node that has rested and is neither failed nor disabled, from running or
	/// stopped (docs/architecture.md#changes-in-place); any other takes it as a restart, as a restart-only change.
	/// `ports`: the newest settings' ports against the configuration's; not empty, a restart's cause is `reshape`.
	Actions change(Queued queued, std::vector<std::string> keys, ChangeKind kind = ChangeKind::restart,
	               PortsMoved ports = {});
	Actions disable();
	Actions enable();
	Actions remove();
	// The operations; NodeHost refuses every operation stopping during stopAll before asking.
	Operated stop();
	Operated start();
	Operated restart();
	// A fact against the configuration, with the fact's detail.
	/// A fault: applied (a failure and its teardown), held during a stop, or, on every other path, dropped and said
	/// (Dropped in the actions).
	Actions fault(Fact fact, std::string detail);
	Actions startThrew(std::string detail);
	// The configuration's completions and bounds; a stale barrier number changes nothing.
	Actions resourcesReady();
	Actions driven(std::uint64_t barrier);
	Actions faded(std::uint64_t barrier);
	Actions silenced(std::uint64_t barrier);
	Actions closed();
	Actions fadeBound();
	Actions barrierBound();
	Actions closeBound();
	// A change in place's completions: the change version answered and every replacement is ready (the commit point);
	// the commit's invoke completed; the change could not finish and restarts with the newest settings.
	Actions changeReady();
	/// The change's continuation was posted to its loop (docs/writing-a-node-type.md#whenready-and-configuredone): from
	/// here a newer change waits for the change's end, as during the commit's invoke; a stop, a teardown, the block's
	/// loss and a fallback still end it.
	Actions continued();
	Actions committed(std::uint64_t barrier);
	Actions fellBack(NodeCause::FellBack reason, std::string detail);
	/// Another node's barrier marked the data loop wedged: this node is never configured again.
	Actions dataLoopWedged();
	/// configure's return: whether some output the code writes is faded (ConfigurationMachine::outputsFaded).
	void outputsFaded(bool fades) noexcept { config_.outputsFaded(fades); }

	Health health() const;
	bool removed() const noexcept;
	/// The waits the aggregate knows: an older node of the name, and the block.
	bool waitingForOlder() const noexcept { return block_.state() == BlockState::waiting || olderNode_; }
	bool waitingForBlock() const;
	const BlockMachine& block() const noexcept { return block_; }
	const ConfigurationMachine& configuration() const noexcept { return config_; }
	Intent intent() const noexcept { return intent_; }
	Wanted wanted() const noexcept;
	bool stopHeld() const noexcept { return stopHeld_; }
	bool retryAsked() const noexcept { return retryAsked_; }
	bool changePending() const noexcept;
	/// A change in place is in flight (reconfiguring), and the settings version it applies.
	bool changing() const noexcept { return config_.changing(); }
	/// The change in flight is past its continuation's post: a newer change waits for it (continued()).
	bool continuing() const noexcept;
	std::uint64_t changeVersion() const noexcept { return changeVersion_; }
	const std::optional<Failure>& failure() const noexcept { return failure_; }
	const std::optional<Failure>& heldFault() const noexcept { return heldFault_; }

private:
	DeriveFacts facts() const;
	bool raise(Intent next, std::optional<NodeCause> why = std::nullopt);
	Actions holdOrDrop(Fact fact, std::string detail);
	void onBlock(BlockEvent event, std::string detail, Actions& out);
	void onConfig(ConfigInput input, Actions& out);
	void tell(Actions& out);
	void askConfig(ConfigEvent event, Actions& out);
	bool beginRestart(NodeCause why, Actions& out);
	NodeCause settingsCause(std::vector<std::string> keys) const; // settings, or reshape when the ports moved
	void beginChange(Actions& out);
	bool mayChangeInPlace() const;
	void configNone(Actions& out);
	void destroyIfUndriven(Actions& out);
	void converge(Actions& out);
	bool failed() const;
	BlockMachine block_;
	ConfigurationMachine config_;
	bool disabled_ = false;
	bool stopHeld_ = false;
	Intent intent_ = Intent::none;
	std::optional<NodeCause> why_;
	Queued queued_;
	std::uint64_t configuredVersion_ = 0;
	std::vector<std::string> changedKeys_;
	PortsMoved movedPorts_; // the newest settings' ports against the configuration's
	// A change in place: the version and keys in flight; the keys of the changes since the applied settings (or, while
	// committing, since the change in flight) not yet taken, and whether every one of them is in place.
	std::uint64_t changeVersion_ = 0;
	bool continued_ = false; // the change in flight posted its continuation
	std::vector<std::string> changeKeys_;
	std::vector<std::string> pendingKeys_;
	bool pendingInPlace_ = false;
	std::optional<Failure> failure_;
	bool retryAsked_ = false;
	std::optional<Failure> heldFault_;
	FailureCode blockLossCode_ = FailureCode::engineRefused;
	bool olderNode_ = false;
	bool configuredTold_ = false;
	bool wedged_ = false;
	bool goneTold_ = false;
	std::optional<Health> told_;
};

} // namespace ear
