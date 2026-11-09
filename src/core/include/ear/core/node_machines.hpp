// The node lifecycle's two pure machines and the node they make (docs/design/node-lifecycle.md section 4, the home of
// their tables; the system rules are docs/design/node-framework.md sections 6.2 and 6.3). Step L-1 of slice 3a: pure,
// tested by table, wired into nothing yet. NodeHost still runs on NodeLifecycle (node_lifecycle.hpp) until step L-3a,
// and today's NodeState keeps its name until step L-3d, so these live in ear::lifecycle; L-3d moves them into ear and
// removes NodeLifecycle (node-lifecycle.md section 6).
//
// Each machine takes an event and returns the actions NodeHost carries out (D-L1): neither calls a hook, PipeWire or a
// timer. The aggregate (NodeAggregate) holds both and the node's facts, applies the aggregate's three rules (4.3) after
// every input, and tells its derived health after every machine transition that changes it (D-L34).
#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace ear::lifecycle {

// ---- Names on the wire and on standard output (control-plane.md 4.8, node-framework.md 6.2) ----

/// The exact state (D-L22). 3d adds reconfiguring, slice 8 recovering. A removed node has no phase: it is gone
/// (NodeAggregate::removed()).
enum class NodePhase { added, configuring, configured, starting, running, stopping, stopped, disabled, failed };
/// The phase's colour: added, configuring and configured are starting; every other phase is its own name.
enum class NodeState { starting, running, stopping, stopped, disabled, failed };
/// What the graph wants of the node: disabled from ear.conf, else stopped while its stop is held, else running.
enum class Wanted { running, stopped, disabled };
/// node-framework.md 6.2's failure codes. L-3d moves lost into FailureReason::Code.
enum class FailureCode { startFailed, engineRefused, lost };
struct Failure {
	FailureCode code;
	std::string detail;
	bool operator==(const Failure&) const = default;
};
/// Why a node is stopping, or configuring after a restart, a retry or an enable (control-plane.md 4.8's cause.kind).
struct NodeCause {
	enum class Kind { stop, removing, disabling, restarting, settings, action, retry, enabling };
	Kind kind;
	/// restarting only: the restart's own kind (settings, action or enabling), as configuring's.
	std::optional<Kind> restart;
	/// settings, or restarting by settings: the changed keys.
	std::vector<std::string> keys;
	bool operator==(const NodeCause&) const = default;
};

NodeState stateOf(NodePhase phase) noexcept;
std::string_view phaseName(NodePhase phase) noexcept;
std::string_view stateName(NodeState state) noexcept;
std::string_view wantedName(Wanted wanted) noexcept;
std::string_view causeName(NodeCause::Kind kind) noexcept;
std::string_view failureCodeName(FailureCode code) noexcept;

// ---- The block machine (4.1) ----

/// absent is "—": before the add. lost is final and keeps its detail.
enum class BlockState { absent, waiting, made, ready, lost, gone };
enum class BlockEvent {
	addedWedged,      // node added while the data loop is wedged (checked first)
	added,            // node added, no older block of its name
	addedBehindOlder, // node added, an older block of its name exists
	olderDestroyed,   // the older block of the name was destroyed
	ready,            // PortBlockEvents::ready
	lost,             // PortBlockEvents::lost, with its detail
	removed,          // removal: its silence completed, or no code driven since the last silence (D-L7)
};
/// make and destroy are queued into the turn's flush, never made inside a handler (5.5).
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
/// The block's loss when a node is added after the data loop was judged wedged (D-L10).
inline constexpr std::string_view wedgedDetail = "the data loop is wedged";

// ---- The configuration machine (4.2) ----

enum class ConfigState { none, readying, prepared, arming, driving, fading, silencing, tearing };
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
	closed,          // every sink and handoff closed, then the legacy run object
	closeBound,      // 10 s
	blockLost,       // the block lost (T13)
	startThrew,      // start() threw: it never set started, so no stop() pairs with it
};
struct ConfigInput {
	ConfigEvent event;
	std::uint64_t barrier = 0; // driven, faded, silenced: the request's number
	bool fades = true;         // configure: some output the code writes is faded (5.1's faded)
};
struct ConfigAction {
	enum class Kind {
		configure,          // new generation; make the instance; creation's configure; ask the resources
		runRecipe,          // legacy only (5.10): run the recipe
		prepare,            // prepare the code
		drive,              // carry the latency plan; post drive(code, faded, sinks)
		start,              // call start()
		stop,               // call stop()
		fade,               // post a fade (100 ms timer)
		silence,            // post a silence (5 s timer)
		fadeUnacknowledged, // ear.fade-unacknowledged
		release,            // clear the latency plan; release the code
		teardown,           // queue the outside nodes' destruction; close sinks and legacy handoffs; 10 s timer
		free,               // on the main loop: the code, the instance, the recordings, in that order
		barrierOverdue,     // ear.barrier-overdue
		markWedged,         // the data loop is wedged
		leak,               // leak the configuration (I5a)
		closeOverdue,       // ear.close-overdue
	};
	Kind kind;
	std::uint64_t barrier = 0; // drive, fade, silence: the number the request carries
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
	bool rested() const noexcept { return rested_; }   // reached running or stopped (D-L23)
	bool stopAsked() const noexcept { return stopAsked_; }
	bool fades() const noexcept { return fades_; }
	std::uint64_t barrier() const noexcept { return barrier_; } // the newest request's number
	/// An event that does not apply (or a stale completion) changes nothing and returns no action.
	std::vector<ConfigAction> on(const ConfigInput& input);

private:
	std::vector<ConfigAction> stopDriving();
	ConfigAction fadeOrSilence();
	std::vector<ConfigAction> beginTeardown();
	ConfigAction post(ConfigAction::Kind kind);
	void reset() noexcept;
	ConfigState state_ = ConfigState::none;
	bool started_ = false;
	bool driven_ = false;
	bool rested_ = false;
	bool stopAsked_ = false;
	bool fades_ = true;
	std::uint64_t barrier_ = 0;
};

// ---- The failure's code, by fact (4.2) ----

enum class Fact {
	engine,          // a sound card or RTP receiver refused or lost; the block refused or lost; the loop wedged
	configureFailed, // configure answered failed(d)
	consumerLost,    // a sink consumer's begin said lost
	recipeFailed,    // a legacy recipe failed
	nodeLost,        // lost(detail) from the node's handle
	hookThrew,       // configure, start() or stop() threw
};
FailureCode failureCodeFor(Fact fact, bool rested) noexcept;

// ---- The aggregate (4.3) ----

/// What a stop or teardown in flight is for, in rising priority (rule 3): a newer one replaces it only if higher.
enum class Intent { none, stop, restart, fail, disable, remove };
std::string_view intentName(Intent intent) noexcept;

/// The health the aggregate derives (4.3's table). waitingFor and since are NodeHost's (4.5).
struct Health {
	NodePhase phase;
	NodeState state;
	Wanted wanted;
	std::optional<NodeCause> cause;
	std::optional<Failure> reason;
	bool blockLost;
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
	bool predecessor = false;
	bool configuredTold = false;
};
Health derive(const DeriveFacts& facts);
/// 4.3's row 1: block gone, configuration none, intent remove.
bool removed(const DeriveFacts& facts) noexcept;

/// The newest committed settings: a version NodeHost names, and whether the code's outputs are faded with them.
struct Queued {
	std::uint64_t version = 0;
	bool fades = true;
};
struct AddFacts {
	bool disabled = false;
	bool stopHeld = false;           // a port-caused replace carries its predecessor's stop (D-L36)
	bool olderBlock = false;         // an older block of the name still exists (T1)
	bool predecessor = false;        // this node is a port-caused replace of an older node still tearing down
	bool olderNode = false;          // an older node of the name is not yet gone: no configure before it is (rule 1)
	std::vector<std::string> keys{}; // the predecessor's changed keys, for its cause
	bool wedged = false;             // the data loop was judged wedged
	Queued queued{};
};
struct Tell {
	Health health;
};
struct Gone {};
using NodeAction = std::variant<BlockAction, ConfigAction, Tell, Gone>;
using Actions = std::vector<NodeAction>;

/// core's own replies to start, stop and restart (5.2, D-L35); MainLoopSide maps them to the wire. L-3d moves them
/// beside NodeOperations in node_host.hpp.
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

/// One node: the two machines and 4.3's facts. Every input returns the actions it caused, in order, with a Tell after
/// each machine transition that changed the derived health and a Gone once the node is removed. Inputs after Gone
/// change nothing.
class NodeAggregate {
public:
	Actions add(const AddFacts& facts);
	// The block's events.
	Actions olderDestroyed();
	/// The predecessor of a port-caused replace closed (its configuration none); it is gone once its block, too, is
	/// destroyed (olderDestroyed), so the successor never shows added.
	Actions predecessorGone();
	/// Every older node of the name is gone (its block destroyed and its configuration none): rule 1 may configure.
	Actions olderNodeGone();
	Actions blockReady();
	Actions blockLost(std::string detail);
	// The plan's outcomes (D-L4): a restart-only change, a change on a disabled node, a disable, an enable, a removal.
	Actions change(Queued queued, std::vector<std::string> keys);
	Actions disable();
	Actions enable();
	Actions remove();
	// The operations (4.4); NodeHost refuses every operation stopping during stopAll before asking.
	Operated stop();
	Operated start();
	Operated restart();
	// A fact against the configuration, with the fact's detail.
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
	/// Another node's barrier marked the data loop wedged: this node is never configured again.
	Actions dataLoopWedged();

	Health health() const;
	bool removed() const noexcept;
	/// 4.5's waits the aggregate knows: an older node of the name, and the block.
	bool waitingForOlder() const noexcept { return block_.state() == BlockState::waiting || olderNode_; }
	bool waitingForBlock() const;
	const BlockMachine& block() const noexcept { return block_; }
	const ConfigurationMachine& configuration() const noexcept { return config_; }
	Intent intent() const noexcept { return intent_; }
	Wanted wanted() const noexcept;
	bool stopHeld() const noexcept { return stopHeld_; }
	bool retryAsked() const noexcept { return retryAsked_; }
	bool changePending() const noexcept;
	const std::optional<Failure>& failure() const noexcept { return failure_; }
	const std::optional<Failure>& heldFault() const noexcept { return heldFault_; }

private:
	DeriveFacts facts() const;
	bool raise(Intent next, std::optional<NodeCause> why = std::nullopt);
	void onBlock(BlockEvent event, std::string detail, Actions& out);
	void onConfig(ConfigInput input, Actions& out);
	void tell(Actions& out);
	void askConfig(ConfigEvent event, Actions& out);
	bool beginRestart(NodeCause why, Actions& out);
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
	std::optional<Failure> failure_;
	bool retryAsked_ = false;
	std::optional<Failure> heldFault_;
	bool predecessor_ = false;
	bool predecessorClosed_ = false;
	bool olderNode_ = false;
	bool configuredTold_ = false;
	bool wedged_ = false;
	bool goneTold_ = false;
	std::optional<Health> told_;
};

} // namespace ear::lifecycle
