// The node framework (docs/architecture.md#the-node-lifecycle; node_host.hpp), on the node lifecycle's
// two pure machines (docs/architecture.md#the-node-lifecycle, node_lifecycle.hpp). Each node is a
// NodeAggregate: NodeHost feeds it every input (a commit, a block's or an engine companion's event, a completion, a
// post from the node's own threads, a timer) and carries out the actions it returns, never waiting for another thread.
//
// The port block is made when the node is added and goes only once it is removed; configurations come and go
// behind it, one at a time. Each configuration has its instance (docs/writing-a-node-type.md),
// made by its config: the type's class (lifecycle.hpp's SpecInstance). Its configure acquires only, through
// CreateContext, implemented here, and gives the code; a rule it breaks at a call or at
// the return, and a configure that throws, fail the node start-failed. Its claims (docs/writing-a-node-type.md#claims)
// are judged first against every other configuration's in the claim registry (claims.hpp), a granted port then probed
// behind the turn's flush, and released last at teardown step 5. Its stores (docs/writing-a-node-type.md#stores) and
// recordings (docs/writing-a-node-type.md#recordings) are opened next, each one job on the I/O executor completing
// through the configuration's Completions; its
// loops (docs/writing-a-node-type.md#loops) are started next, each with its state; the engine companions it asked for are made once those are running and the
// block is ready; each claim, each store, each loop, each engine companion and each sink's consumer is an item of the configuration's
// readiness list (readiness.hpp), a companion's and a consumer's with its kind's deadline from its asking, past which
// the node fails
// (docs/engine.md#readiness-deadlines), and every companion is in the node's list a client
// sees (companionsOf; docs/engine.md#companions-in-a-nodes-list); once every resource is ready (T3) the code is
// prepared, and once the block is ready it is driven by an asynchronous
// drive (T5), whose completion the block
// posts and NodeHost checks against the node's serial, its configuration's generation and the barrier number
// before the machine hears it; then the instance's start() runs. Its stop() runs once before each silence that follows
// a start() that returned. Each hook is timed (ear.hook-slow, once per node and hook). Every call into a type's code
// (its configure, start() and stop(), its code's prepare, and a sink's consumer on that one's own thread) goes
// through one catch (callType, ConsumerCalls), so a type's throw is a fact about its
// node (docs/writing-a-node-type.md#rules-the-framework-enforces). ear's own bugs are split by owner
// (docs/architecture.md#faults-by-owner): an adapter's that NodeHost caught where it called it (inside a context call,
// or a factory in the flush) is told as ear.internal-error and fails only its node, lost; what NodeHost throws itself
// leaves for the callback boundary, which ends the process. A configuration is silenced the
// same way, then torn down by dependency: release(); engine companions; sinks closed, each on its own thread; its
// loops finished (their states destroyed on their own threads) and closed; then, on the main loop, the code, the sinks, the instance, the loops, the stores, the recordings
// (closed on the I/O thread) and the config. What
// is sent to a loop before the node is configured is staged on the main loop, released at configured, and destroyed
// unrun by a teardown before it. Steps 3 and 4 have the close deadline: past it the configuration is leaked, never
// freed.
// Everything a configuration's threads tell crosses to the main loop and lands only on that configuration.
//
// PipeWire's own object changes (a block made or destroyed, an engine companion made or destroyed) are never made
// inside an input: they are queued into the turn's flush, posted to the main loop once per turn and run there in
// one go, destroys first (engine companions, then blocks), then makes (blocks, then engine companions, which need their
// block's ports). The flush works on a snapshot: an item an input causes while it runs (a waiting block freed by a
// destroy) runs behind the snapshot in the same flush, and an event an adapter tells inside an item (an engine
// companion ready inside its making) is an input of its own, handled once that item returned. Each item, and each
// type's hook, names itself in MainLoopActivity while it runs. A name added back (T1) gets its block once the older
// block of the name was destroyed, in the flush that destroyed it, and its configuration once the older node is gone;
// an engine companion joins its block by the block's global id (PortBlock::blockId).
//
// Fades and bounds: every output the code writes (public outputs and inside outputs, which feed a card; never a sink's
// bank) fades in with each drive and out before each planned silence, a node with none silenced at once; a fade unacknowledged within 100 ms is said (ear.fade-unacknowledged)
// and the silence posted unfaded, and a drive or silence not completed within 5 s is a data loop that runs no invoke
// (ear.barrier-overdue, then ear.data-loop-wedged once, through EngineDiagnosticListener). Every bound first settles
// the block, as an input of its own, and judges in the input behind what that settle ran, so a completion a stalled
// main loop has not dispatched yet is heard first and nothing is judged. Once wedged, NodeHost makes no PipeWire call
// again: the flush still runs its items in order, but a destruction leaks what it would destroy (a block, an engine
// companion), a making makes nothing (a block is lost "the data loop is wedged"), latency is no longer carried, and
// every drive, fade or silence a configuration would post is judged overdue at once, its configuration leaked; a node
// added afterwards fails engine-refused "the data loop is wedged" before T1. So every removal still ends, stopAll's
// allGone still runs, and main.cpp leaves through std::_Exit (leaked() is above 0).
//
// Health: after every machine transition that changes a member of the derived health, every
// listener is told, in subscribe order, with what the node still waits for added once its phase has lasted
// 2 s, then changed at most once a second. The plan's outcomes are honoured (nodeChanged): a disable tears
// the configuration down (its stop, if held, dropped) and keeps the block and wires, an enable configures afresh, a
// change on a disabled node is stored, and a node added disabled is never configured. A change of ports keeps the node
// and its block, reshaped (docs/architecture.md#reshapes); the older node of a name added back (a replace by another
// type, or a removal and an add) tells only the listeners that hear a superseded node (standard output, not the wire). NodeOperations' stop, start and restart are
// operations of their own, answered in the call.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <deque>
#include <map>
#include <exception>
#include <expected>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "ear/core/lifecycle.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/pass_through.hpp"
#include "caught.hpp"
#include "claims.hpp"
#include "completions.hpp"
#include "io_opens.hpp"
#include "note_gate.hpp"
#include "readiness.hpp"
#include "status_book.hpp"
#include "stores.hpp"

namespace ear {
namespace {

namespace lc = ::ear; // the lifecycle's machines (node_lifecycle.hpp)

template <typename... Kinds> struct Overloaded : Kinds... {
	using Kinds::operator()...;
};

using detail::caughtFrom;
using detail::whatOf;

/// A hook of a type's code NodeHost calls on the main loop: its name in a fault's detail ("<name> threw: <what>"), the
/// call MainLoopActivity names while it runs (a string literal: only its address is kept), and whether it is timed
/// (ear.hook-slow, docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog).
struct TypeHook {
	std::string_view name;
	std::string_view activity;
	bool timed;
};

namespace hooks {
inline constexpr TypeHook configure{"configure", "configure", true}; // the instance made, then its configure
inline constexpr TypeHook start{"start", "start", true};
inline constexpr TypeHook stop{"stop", "stop", true};
inline constexpr TypeHook prepare{"prepare", "prepare", false}; // the code's
} // namespace hooks

/// What a call into a type's code ended with instead of returning (callType): its own throw, the first rule a context
/// call found broken while it ran, which fails it even when the type caught the throw, or a bug of an adapter a context
/// call made (adapterFaulted: the node fails lost, docs/architecture.md#faults-by-owner).
struct TypeFault {
	std::string detail;          // "<hook> threw: <what>", or "internal error in <adapter call>: <what>"
	std::string what;            // the exception's what(), or the rule's text
	bool adapterFaulted = false; // ear's adapter's bug, not the type's: told as ear.internal-error first
};

/// Who owns a bug of ear's own caught inside a type's call (docs/architecture.md#faults-by-owner): an adapter, whose
/// state is its node's alone, or NodeHost itself, whose state is every node's.
enum class FaultOwner { adapter, host };

/// ear's own fault inside a context call a type made: thrown on through the type's frames in place of what was thrown
/// (the type may catch it, as it may any throw) and recorded by the context, so callType routes it by its owner rather
/// than fail the node as the type's throw. Never seen outside NodeHost.
class EarFault final : public std::exception {
public:
	EarFault(FaultOwner owner, std::string callback, std::exception_ptr thrown)
		: owner_(owner), callback_(std::move(callback)), thrown_(std::move(thrown)), what_(whatOf(thrown_)) {}
	FaultOwner owner() const noexcept { return owner_; }
	/// The adapter call that threw ("SinkAdapter::make"); empty for NodeHost's own.
	const std::string& callback() const noexcept { return callback_; }
	/// What was thrown, rethrown as itself when the owner is NodeHost.
	const std::exception_ptr& thrown() const noexcept { return thrown_; }
	const char* what() const noexcept override { return what_.c_str(); }

private:
	FaultOwner owner_;
	std::string callback_;
	std::exception_ptr thrown_;
	std::string what_;
};

/// An exception a drain or flush keeps to rethrow once it has run everything else, with the place it left the main
/// loop (MainLoopActivity::takeThrown), which the rethrow carries to the callback boundary that catches it
/// (docs/architecture.md#the-callback-boundary).
struct KeptThrow {
	std::exception_ptr error;
	MainLoopActivity::Saved at;

	explicit operator bool() const noexcept { return error != nullptr; }
	/// Inside a catch handler: keeps the exception handled, and its place, when none is kept yet; a later one's place
	/// is forgotten.
	void keepCaught() noexcept {
		const std::optional<MainLoopActivity::Saved> left = MainLoopActivity::takeThrown();
		if (error) return;
		error = std::current_exception();
		at = left.value_or(MainLoopActivity::save());
	}
	[[noreturn]] void rethrow() const {
		MainLoopActivity::putThrown(at);
		std::rethrow_exception(error);
	}
};

std::int64_t millisecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

/// The suffix of a configuration's engine companion: "playback" or "capture" (a sound card, by its bank's direction),
/// "rtp" (an RTP receiver), so its engine node is "<node>.<suffix>" (a node name never holds '.', so they cannot
/// collide with a node).
std::string_view engineSuffix(const SoundCardShape&, Direction bank) noexcept {
	return bank == Direction::output ? "playback" : "capture";
}
std::string_view engineSuffix(const RtpReceiverShape&, Direction) noexcept { return "rtp"; }
std::string_view engineSuffix(const PipeReceiverShape&, Direction) noexcept { return "pipe"; }

/// An engine companion's kind: a card's by its bank's direction.
CompanionKind engineKind(const SoundCardShape&, Direction bank) noexcept {
	return bank == Direction::output ? CompanionKind::cardPlayback : CompanionKind::cardCapture;
}
CompanionKind engineKind(const RtpReceiverShape&, Direction) noexcept { return CompanionKind::rtpReceiver; }
CompanionKind engineKind(const PipeReceiverShape&, Direction) noexcept { return CompanionKind::pipeReceiver; }

/// What a host with no adapter of the kind is said to have none of: "no <kind> in this host".
std::string_view engineKindWords(const SoundCardShape&) noexcept { return "sound cards"; }
std::string_view engineKindWords(const RtpReceiverShape&) noexcept { return "RTP receivers"; }
std::string_view engineKindWords(const PipeReceiverShape&) noexcept { return "pipe receivers"; }

/// The adapter call that made an engine companion, as an adapter's bug names it.
std::string_view engineMaker(const SoundCardShape&) noexcept { return "SoundCardCompanions::make"; }
std::string_view engineMaker(const RtpReceiverShape&) noexcept { return "RtpReceiverCompanions::make"; }
std::string_view engineMaker(const PipeReceiverShape&) noexcept { return "PipeReceiverCompanions::make"; }

/// Why an engine companion that missed its readiness deadline failed its node (engine-refused,
/// docs/engine.md#readiness-deadlines).
std::string neverReady(const SoundCardShape& shape) { return "the card never started: " + shape.card.text(); }
std::string neverReady(const RtpReceiverShape& shape) {
	return "the RTP receiver never ran (UDP port " + std::to_string(shape.port) + " held?)";
}
std::string neverReady(const PipeReceiverShape&) { return "the pipe receiver never ran"; }

/// How many times an AnyTcp claim asks the probe for a free port before it is refused: the kernel may offer a port
/// another configuration holds (docs/writing-a-node-type.md#claims).
constexpr int anyTcpTries = 8;

/// The most runtime files one program may have (ProgramShape::files).
constexpr std::size_t maxProgramFiles = 8;

/// A program's file name: 1 to 32 of [a-z0-9.-], never "feed" (the feed's own runtime path).
bool plainFileName(std::string_view name) noexcept {
	if (name.empty() || name.size() > 32 || name == "feed") return false;
	return std::ranges::all_of(
		name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'; });
}

/// The inside bank of this direction a companion joins; std::logic_error (a type bug) otherwise.
const Ports::Bank& insideBank(const Ports& ports, const std::string& name, Direction direction, std::string_view what) {
	const Ports::Bank& bank = ports.bank(name);
	if (bank.reach != PortBank::Reach::inside || bank.direction != direction)
		throw std::logic_error(std::string(what) + " joins an inside " +
		                       (direction == Direction::input ? "input" : "output") + " bank, not \"" + name + "\"");
	return bank;
}

/// The inside bank a sound card joins, of either direction (its direction is the card's); std::logic_error (a type
/// bug) for a routable bank.
const Ports::Bank& cardBank(const Ports& ports, const std::string& name) {
	const Ports::Bank& bank = ports.bank(name);
	if (bank.reach != PortBank::Reach::inside)
		throw std::logic_error("a sound card joins an inside bank, not \"" + name + "\"");
	return bank;
}

/// The phases whose waits reach health once they lasted 2 s (docs/control-port.md#node-health).
bool waitingPhase(NodePhase phase) noexcept {
	return phase == NodePhase::added || phase == NodePhase::configuring || phase == NodePhase::configured ||
	       phase == NodePhase::starting;
}

} // namespace

std::string_view waitingKindName(WaitingFor::Kind kind) noexcept {
	switch (kind) {
	case WaitingFor::Kind::olderNode:
		return "older-node";
	case WaitingFor::Kind::block:
		return "block";
	case WaitingFor::Kind::companion:
		return "companion";
	case WaitingFor::Kind::consumer:
		return "consumer";
	case WaitingFor::Kind::loop:
		return "loop";
	case WaitingFor::Kind::store:
		return "store";
	case WaitingFor::Kind::recording:
		return "recording";
	case WaitingFor::Kind::claim:
		return "claim";
	case WaitingFor::Kind::program:
		return "program";
	}
	return "unknown";
}

struct NodeHost::Impl {
	/// PipeWire's object changes a turn queues, in the order a flush runs them: destroys (engine companions, then
	/// blocks), then makes (blocks, then engine companions).
	enum class FlushKind { destroyCompanion, destroyBlock, makeBlock, makeCompanion };
	struct FlushItem {
		FlushKind kind;
		NodeName node;
		std::move_only_function<void()> run;
	};

	/// One wanted node: its aggregate, its port block and its configuration. Members are declared in reverse teardown
	/// order, so destroying a node that never stopped still removes its port block first.
	class Node final : public PortBlockEvents, public CreateContext {
	public:
		Node(Impl& host, NodeName name, std::string type, std::uint64_t serial)
			: host_(host), name_(std::move(name)), type_(std::move(type)), serial_(serial), span_(serial) {}
		/// On a wedged data loop nothing that loop may still reach is destroyed: destroying a block waits on it.
		~Node() {
			if (!host_.wedged) return;
			static_cast<void>(block_.release());
			static_cast<void>(config_.release());
		}
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		const NodeName& name() const noexcept { return name_; }
		std::uint64_t serial() const noexcept { return serial_; }
		/// The span of counts it belongs to (docs/engine.md#reporting-the-counters): its own serial.
		std::uint64_t span() const noexcept { return span_; }
		/// Gone: its block destroyed (the flush ran its destruction) and its configuration gone (removed() in
		/// node_lifecycle.hpp), and so told.
		bool removed() const noexcept { return gone_; }
		/// Its block exists in the engine, or its making or destruction is still queued (T1: a newer block of its name
		/// waits for that).
		void markGone() noexcept { gone_ = true; }
		bool holdsBlock() const noexcept { return block_ != nullptr || makePending_ || destroyPending_; }
		/// Its block was leaked on a wedged loop: the block still holds this node as its PortBlockEvents, so the node
		/// is never freed either.
		bool blockLeaked() const noexcept { return blockLeaked_; }
		/// Its block waits for an older block of its name (T1).
		bool blockWaiting() const noexcept { return aggregate_.block().state() == lc::BlockState::waiting; }
		/// No longer wanted: removed by an edit or by stopAll, still stopping or gone.
		bool removing() const noexcept { return aggregate_.intent() == lc::Intent::remove; }
		NodeHealth health() const { return composed(aggregate_.health()); }
		/// A newer node of this name was added while this one closes: only standard output hears it. From now on only
		/// the listeners that hear a superseded node hear this one, told its health at once, so they keep it apart from
		/// the newer node told next.
		void supersede() {
			if (std::exchange(superseded_, true)) return;
			tell(aggregate_.health());
		}
		bool superseded() const noexcept { return superseded_; }
		std::uint64_t dropped() const noexcept { return dropped_; }
		/// Main thread. Kept by code and subject (docs/output-records.md#diagnostic), stamped, counted, then told:
		/// a condition said inactive while it is not active is dropped; a condition said active again while active
		/// keeps its count.
		void diagnose(NodeDiagnostic diagnostic) {
			diagnostic.at = host_.timers.now();
			const auto kept = std::ranges::find_if(diagnostics_, [&](const NodeDiagnostic& each) {
				return each.code == diagnostic.code && each.subject == diagnostic.subject;
			});
			const bool known = kept != diagnostics_.end();
			const std::uint64_t before = known ? kept->count : 0;
			if (diagnostic.condition && !diagnostic.active) {
				if (!known || !kept->active) return;
				diagnostic.count = before;
			} else if (diagnostic.condition) diagnostic.count = before + (known && kept->active ? 0 : 1);
			else diagnostic.count = before + 1;
			if (known) *kept = diagnostic;
			else diagnostics_.push_back(diagnostic);
			host_.toldDiagnostic(name_, diagnostic, superseded_);
		}
		/// Every diagnostic this node was told, as last told, in the order each was first said.
		const std::vector<NodeDiagnostic>& diagnostics() const noexcept { return diagnostics_; }

	private:
		class Channel;
		struct Configuration;

	public:
		// ---- Status and events (docs/architecture.md#status-and-events) ----

		/// The status view, every field in declaration order; empty before the node first configured.
		std::vector<FieldValue> status() const { return book_ ? book_->view() : std::vector<FieldValue>{}; }
		/// Records of `channel`, the current configuration's, are waiting: drained at once (a loss behind them), or at
		/// the next tick.
		void reportsDue(Channel& channel, bool now) {
			if (now) return drainReports(channel);
			host_.statusDirty();
		}
		/// The tick (Impl::tickStatus): the current configuration's records drained, then the book's own paces.
		void statusTick(std::chrono::steady_clock::time_point now) {
			if (!book_) return;
			if (config_ && config_->channel) drainReports(*config_->channel);
			BookOut out(*this, nullptr);
			book_->tick(now, out);
		}
		std::optional<std::chrono::steady_clock::time_point> statusDue() const {
			return book_ ? book_->due() : std::nullopt;
		}
		/// The node's counters from its add (docs/engine.md#counters): its block's now, or what it counted before it
		/// went. Main thread.
		NodeCounts counts() const noexcept { return block_ ? block_->counts() : blockCounted_; }

		/// The port block once no older block of this name exists (T1: two blocks never share a node.name), and a
		/// configuration once no older node of this name is left (rule 1), so an old and a new configuration never
		/// share what the name owns (a listening port, a key file). A node added disabled keeps its block and is never
		/// configured (T19).
		void add(const NodeSpec& spec, std::shared_ptr<const NodeConfig> config, bool olderBlock, bool olderNode) {
			ports_ = config->ports();
			queued_ = std::move(config);
			// A node added on a wedged loop fails before T1, with no block.
			feed(aggregate_.add(lc::AddFacts{.disabled = spec.disabled,
			                                 .olderBlock = olderBlock,
			                                 .olderNode = olderNode,
			                                 .wedged = host_.wedged,
			                                 .queued = queued()}));
		}

		/// The plan's outcome for a node kept by name. Its newest config is kept for the next configure
		/// (a lost block is final: nothing is kept). restart: a configuration settled restarts at once, one still going
		/// takes it next (rule 2), a failed node retries; disable: the configuration torn down (T20, T21), its
		/// stop dropped, its block and wires kept; enable: configured afresh (T22); stored: kept for the enable;
		/// reshape: a restart, or in place (docs/architecture.md#reshapes); unchanged: nothing.
		void change(std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
			if (change.outcome == NodeOutcome::unchanged) return; // nothing changes, nothing is told
			const bool lost = aggregate_.block().state() == lc::BlockState::lost;
			queued_ = lost ? nullptr : std::move(config);
			++version_;
			// A restart's cause is a reshape when the newest ports differ from the configuration's
			// (docs/architecture.md#reshapes).
			PortsMoved moved;
			if (config_ && config_->config && queued_) moved = portsMoved(config_->config->ports(), queued_->ports());
			switch (change.outcome) {
			case NodeOutcome::unchanged: // returned above
				break;
			case NodeOutcome::reconfigure: {
				// In place only for a type with a change version: without one the change restarts as any other, with
				// no reconfiguring between (docs/architecture.md#changes-in-place).
				const bool inPlace = config_ && config_->instance && config_->instance->changes();
				feed(aggregate_.change(queued(), change.keys,
				                       inPlace ? lc::ChangeKind::inPlace : lc::ChangeKind::restart));
				break;
			}
			case NodeOutcome::reshape: {
				// In place when every changed key is, for a type with a change version and a configuration with no
				// companion; else a restart, its block reshaped between the two configurations
				// (docs/architecture.md#reshapes).
				const bool inPlace =
					change.inPlace && config_ && config_->instance && config_->instance->changes() && reshapesInPlace();
				feed(aggregate_.change(queued(), change.keys,
				                       inPlace ? lc::ChangeKind::inPlace : lc::ChangeKind::restart, moved));
				break;
			}
			case NodeOutcome::restart:
			case NodeOutcome::stored:
				feed(aggregate_.change(queued(), change.keys, lc::ChangeKind::restart, moved));
				break;
			case NodeOutcome::disable:
				feed(aggregate_.disable());
				feed(aggregate_.change(queued(), change.keys)); // stored for the enable
				break;
			case NodeOutcome::enable:
				feed(aggregate_.change(queued(), change.keys)); // stored, then configured by the enable
				feed(aggregate_.enable());
				break;
			}
			// A node with no configuration (disabled, failed) reshapes its block at once: a stored change of ports
			// reshapes the block alone.
			if (!config_) adoptPorts();
		}

		/// A reshape in place is taken only by a configuration with no companion: one on a resized bank would need its
		/// replacement made before break on the new ports, which is not built (docs/architecture.md#reshapes).
		bool reshapesInPlace() const {
			const Configuration& c = *config_;
			return c.engineAsks.empty() && c.sinks.empty() && c.sources.empty() && c.recordings.empty() && !c.program;
		}

		/// How a removal relates to a node of the same name added in the same commit (a replace, by another type).
		enum class Replaced { no, superseded };

		/// No longer wanted. A replaced node's newer node of the name comes in the same commit: from here on the older
		/// node tells only standard output.
		void remove(Replaced replaced = Replaced::no) {
			queued_.reset(); // never configured again
			superseded_ = superseded_ || replaced == Replaced::superseded;
			stopping(); // stopping (removing) before anything the removal does
			feed(aggregate_.remove());
		}

		/// An operation: the aggregate answers, and what it caused is carried out and told before the reply.
		/// `judged` hears the reply before any of it is carried out.
		lc::LifecycleReply operate(lc::Operated (lc::NodeAggregate::*operation)(),
		                           NodeOperations::OperationJudged& judged) {
			lc::Operated operated = (aggregate_.*operation)();
			if (judged) judged(operated.reply);
			feed(std::move(operated.actions));
			return operated.reply;
		}

		/// Every older node of this name is gone.
		void olderGone() { feed(aggregate_.olderNodeGone()); }
		/// The older block of this name was destroyed (T1): this node's block is made in the same flush.
		void olderDestroyed() { feed(aggregate_.olderDestroyed()); }

		/// Inside the handler of what an adapter NodeHost called in the flush threw: told as ear.internal-error, with
		/// the flush item it was thrown in, then `fail` given the node's detail ("internal error in <callback>: ...").
		template <typename Fail> void adapterFaulted(std::string_view callback, Fail fail) {
			const std::string what = whatOf(std::current_exception());
			host_.internalError(InternalError{.callback = callback,
			                                  .node = name_,
			                                  .where = MainLoopActivity::describe(MainLoopActivity::save()),
			                                  .what = what});
			fail(internalErrorDetail(callback, what));
		}
		static std::string_view companionMaker(const EngineCompanionShape& shape) noexcept {
			return std::visit([](const auto& kind) { return engineMaker(kind); }, shape);
		}
		/// The block lost for good, an input of its own: engine-refused, or lost for its own bug.
		void blockGone(std::string detail, FailureCode code) {
			host_.inputFor(serial_, [detail = std::move(detail), code](Node& node) mutable {
				node.feed(node.aggregate_.blockLost(std::move(detail), code));
				node.queued_.reset(); // a lost block is final: nothing is configured again
			});
		}

		/// Flush items. The block's making, unless its destruction overtook it in the flush.
		void makeBlock() {
			if (!makePending_) return;
			makePending_ = false;
			if (host_.wedged) { // making a block would call into the wedged loop
				lost(std::string(lc::wedgedDetail));
				return;
			}
			try {
				blockPorts_ = ports_;
				block_ = host_.blocks.make(name_, ports_, host_.format, *this);
			} catch (...) {
				// A factory refuses through lost(), never by a throw, so one that throws has a bug of its own; it made
				// nothing and NodeHost changed nothing yet, so the adapter's fault is its node's alone: told, and the
				// block lost for good (docs/architecture.md#faults-by-owner).
				adapterFaulted("PortBlockFactory::make", [this](std::string detail) { faulted(std::move(detail)); });
			}
		}
		/// The block's destruction (after the removal's silence completed, or with no code ever driven); a block
		/// whose making is still queued is never made. Then, as an input of its own: the node is told gone if its
		/// configuration is, and a waiting block of the name is made.
		void destroyBlock() {
			if (!destroyPending_) return;
			destroyPending_ = false;
			if (block_) blockCounted_ = blockCounted_ + block_->counts(); // the counters outlive the block
			if (makePending_) {
				makePending_ = false;
			} else if (host_.wedged && block_) {
				static_cast<void>(block_.release()); // leaked: destroying it waits on the wedged loop
				blockLeaked_ = true;
				++host_.leaked;
			} else {
				block_.reset();
			}
			host_.inputFor(serial_, [](Node& node) {
				node.tellGone();
				node.host_.blockDestroyed(node.name_);
			});
		}
		/// The engine companion `index` of the configuration of `generation`, in request order, once its block is
		/// ready; nothing for a configuration that moved on (torn down, or another one).
		void makeCompanion(std::uint64_t generation, std::size_t index) {
			if (host_.wedged || !current(generation) || config_->startFailed || config_->tearing || !block_ ||
			    aggregate_.configuration().state() != lc::ConfigState::readying ||
			    index != config_->engineCompanions.size() || index >= config_->engineAsks.size())
				return;
			Configuration& c = *config_;
			auto& companion = *c.engineCompanions.emplace_back(
				std::make_unique<Companion>(host_, serial_, c.generation, index, ++host_.companionIds));
			EngineAsk& asked = c.engineAsks[index];
			CompanionTarget& target = asked.target;
			target.blockId = block_->blockId(); // ready, so read
			try {
				companion.made = std::visit(
					// Asked only where the host has the kind's adapter (askEngineCompanion).
					[&](const auto& shape) {
						return host_.resources.adapterFor(shape)->make(target, shape, companion);
					},
					asked.shape);
			} catch (...) {
				// An adapter refuses through lost(), never by a throw, so one that throws has a bug of its own; the
				// half-made entry goes and nothing is stranded, so the fault is its node's alone: told, and the node
				// lost (docs/architecture.md#faults-by-owner).
				c.engineCompanions.pop_back();
				adapterFaulted(companionMaker(asked.shape), [this, generation](std::string detail) {
					host_.inputFor(serial_, [generation, detail = std::move(detail)](Node& node) mutable {
						if (node.current(generation))
							node.feed(node.aggregate_.fault(lc::Fact::adapterFaulted, std::move(detail)));
					});
				});
			}
		}

		// PortBlockEvents: inputs of their own, whatever calls them.
		void ready() override {
			host_.inputFor(serial_, [](Node& node) {
				node.feed(node.aggregate_.blockReady());
				node.askResources();
			});
		}
		void lost(std::string detail) override { blockGone(std::move(detail), FailureCode::engineRefused); }
		void faulted(std::string detail) override { blockGone(std::move(detail), FailureCode::lost); }

		// CreateContext (docs/writing-a-node-type.md#configure-and-createof). Each call is valid only inside its
		// configure (else a std::logic_error naming the call). Every rule a call finds broken is also recorded
		// (guarded), so the node fails with it even when the type catches the throw.
		AudioFormat format() const override { return host_.format; }
		const NodeName& node() const override { return name_; }
		const Ports& ports() const override { return ports_; }
		/// An engine companion (docs/writing-a-node-type.md#engine-companions), its bank's rule its shape's: a sound
		/// card on an inside bank of either direction, an RTP receiver or a pipe receiver on an inside input bank. A
		/// pipe receiver's FIFO is the configuration's runtime path "<bank>.fifo", named in what this returns; a host
		/// with no adapter of the kind, or no runtime folder for a pipe receiver, refuses it.
		std::optional<part::Fifo> attachEngine(std::string_view bank, EngineCompanionShape shape) override {
			return guarded([&]() -> std::optional<part::Fifo> {
				requireConfiguring("attach");
				const std::string name(bank);
				return std::visit(
					Overloaded{
						[&](SoundCardShape& card) -> std::optional<part::Fifo> {
							askEngineCompanion(std::move(card), cardBank(ports_, name));
							return std::nullopt;
						},
						[&](RtpReceiverShape& receiver) -> std::optional<part::Fifo> {
							askEngineCompanion(std::move(receiver),
					                           insideBank(ports_, name, Direction::input, "an RTP receiver"));
							return std::nullopt;
						},
						[&](PipeReceiverShape& pipe) -> std::optional<part::Fifo> {
							const Ports::Bank& found = insideBank(ports_, name, Direction::input, "a pipe receiver");
							askEngineCompanion(std::move(pipe), found);
							return fifoOf(found.name);
						},
					},
					shape);
			});
		}
		/// An AnyTcp claim (docs/writing-a-node-type.md#claims): recorded now, its port picked at readiness step
		/// `claims`; its handle is its index among the configuration's claims.
		GrantedPort claimAnyTcp() override {
			return guarded([&] {
				requireConfiguring("claim");
				const GrantedPort granted(config_->claims.size());
				config_->claims.push_back(ClaimSlot{Claim{Claim::Kind::tcp, {}}, detail::noReadiness, {}, true});
				return granted;
			});
		}
		/// The configuration's program (docs/writing-a-node-type.md#programs): every rule its shape can break here is a
		/// std::logic_error; a FIFO it names is checked at configure's return, once every pipe receiver was asked for.
		void program(ProgramShape shape, std::optional<ProgramFeedTo> feed) override {
			guarded([&] {
				requireConfiguring("program");
				if (host_.resources.programs() == nullptr) throw std::logic_error("no programs in this host");
				if (host_.resources.runtimeFolder().empty()) throw std::logic_error("no runtime folder in this host");
				Configuration& c = *config_;
				if (c.program) throw std::logic_error("a second program: one per configuration");
				const std::string name(shape.executable.name());
				if (shape.files.size() > maxProgramFiles)
					throw std::logic_error("program " + name + ": more than " + std::to_string(maxProgramFiles) +
					                       " files");
				for (std::size_t i = 0; i < shape.files.size(); ++i) {
					const std::string& file = shape.files[i].name;
					const bool twice =
						std::ranges::any_of(shape.files.begin(), shape.files.begin() + i,
					                        [&](const ProgramFile& other) { return other.name == file; });
					if (!plainFileName(file) || twice)
						throw std::logic_error("program " + name + ": bad file name " + file);
				}
				if (shape.needs.size() > maxProgramNeeds)
					throw std::logic_error("program " + name + ": more than " + std::to_string(maxProgramNeeds) +
					                       " needs");
				for (std::size_t i = 0; i < shape.needs.size(); ++i)
					if (std::ranges::find(shape.needs.begin(), shape.needs.begin() + i, shape.needs[i]) !=
					    shape.needs.begin() + i)
						throw std::logic_error("program " + name + " needs " + shape.needs[i].text() + " twice");
				const auto check = [&](const ProgramPart& piece) {
					std::visit(Overloaded{
								   [](const part::Text&) {},
								   [](const part::Fifo&) {}, // checked at configure's return
								   [&](const part::Feed&) {
									   if (!feed)
										   throw std::logic_error("program " + name + " names its feed but has none");
								   },
								   [&](const part::File& file) {
									   if (std::ranges::none_of(shape.files, [&](const ProgramFile& own) {
											   return own.name == file.name;
										   }))
										   throw std::logic_error("program " + name + " names no file " + file.name);
								   },
								   [&](const part::Port& port) {
									   const std::size_t index = port.port.claim();
									   if (index >= c.claims.size() || !c.claims[index].anyTcp)
										   throw std::logic_error("program " + name +
							                                      " names a port this configuration was not granted");
								   },
							   },
					           piece);
				};
				for (const ProgramArg& arg : shape.args)
					for (const ProgramPart& piece : arg.parts) check(piece);
				for (const ProgramFile& file : shape.files)
					for (const ProgramPart& piece : file.text) check(piece);
				if (feed) static_cast<void>(loopSlot("program", feed->loop)); // a loop the configuration declares
				c.program.emplace(ProgramSlot{std::move(shape), std::move(feed)});
			});
		}
		/// A recording (docs/writing-a-node-type.md#recordings): nothing opened here. Its path is claimed at readiness
		/// step `claims` and the file opened at step `opens`. A path the recording path rule refuses (it reaches a
		/// type from its settings, so it is the node's failure, not the type's bug) and a host with no recordings
		/// adapter fail the node at configure's return, "cannot open <path>: <why>".
		Recording& recording(const RecordingRequest& request) override {
			return guarded([&]() -> Recording& {
				requireConfiguring("recording");
				if (auto refused = recordingPathRefused(request.path))
					return *config_->recordings.emplace_back(
						std::make_unique<RecordingSlot>(request.path, nullptr, name_, std::move(*refused)));
				const std::string claimed = recordingClaimPath(request.path);
				for (const auto& slot : config_->recordings)
					if (slot->claimPath() == claimed)
						throw std::logic_error(claimKey(Claim{Claim::Kind::recording, claimed}) + " claimed twice");
				return *config_->recordings.emplace_back(
					std::make_unique<RecordingSlot>(request.path, host_.resources.recordings(), name_));
			});
		}
		/// A sink (docs/writing-a-node-type.md#sinks): made at once on the inside output bank with its one consumer, its
		/// thread started; its bank is never faded. A consumer it cannot take (null, or in the state of a loop declared
		/// without one) is the type's broken rule, found before the sink is made. An adapter that cannot make it (no
		/// thread) fails the node through configure, recorded so a type that catches it still fails; an adapter's own
		/// bug fails it lost (adapter()).
		SinkControl& sink(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) override {
			return guarded([&]() -> SinkControl& {
				requireConfiguring("attach");
				const std::string name(bank);
				const Ports::Bank& found = insideBank(ports_, name, Direction::output, "a sink");
				requireOneName(name_ + "." + found.name);
				requireFreeBank(found);
				if (auto* own = std::get_if<std::unique_ptr<SinkConsumer>>(&consumer)) {
					if (!*own) throw std::logic_error("sink " + found.name + " given no consumer");
				} else {
					const LoopSlot& slot = loopSlot("sink", std::get<StateSinkConsumer>(consumer).loop);
					if (!slot.declared.stateful)
						throw std::logic_error("sink " + found.name + " is consumed by loop " +
						                       std::string(slot.declared.name) + ", which has no state");
				}
				Configuration& c = *config_;
				SinkAdapter& sinks = given(host_.resources.sinks(), "sinks");
				std::unique_ptr<Sink> made = adapter("SinkAdapter::make", [&] {
					auto sink = sinks.make(name_, found.name, static_cast<std::uint32_t>(found.ports.size()),
					                       shape.frames, host_.format.rate);
					if (!sink) throw std::logic_error("the sink adapter made no sink for bank " + found.name);
					return sink;
				});
				auto& slot = *c.sinks.emplace_back(std::make_unique<SinkSlot>(found.name, std::move(consumer)));
				slot.sink = std::move(made);
				slot.binding = SinkBinding{&slot.sink->tap(), found.first, found.ports.size()};
				c.requestOrder.push_back({Asked::Family::sink, c.sinks.size() - 1});
				for (std::size_t i = found.first; i < found.first + found.ports.size() && i < c.fadedCount; ++i)
					c.faded[i] = false;
				return slot.sink->control();
			});
		}
		/// A source (docs/writing-a-node-type.md#sources): made at once on the inside input bank, ready at once, bound
		/// by every drive of the configuration; its capacity declared beyond the bank; its clock, if it has one, kept
		/// with it (the source owns it, freed with it at teardown step 5). An adapter that cannot make it
		/// (no eventfd) fails the node through configure, recorded so a type that catches it still fails; an adapter's
		/// own bug fails it lost (adapter()).
		SourceWriter& source(std::string_view bank, SourceShape shape, std::unique_ptr<SourceClock> clock) override {
			return guarded([&]() -> SourceWriter& {
				requireConfiguring("attach");
				const std::string name(bank);
				const Ports::Bank& found = insideBank(ports_, name, Direction::input, "a source");
				if (shape.frames == 0) throw std::logic_error("a source on bank " + found.name + " holds no frames");
				if (shape.frames > maxSourceFrames)
					throw std::logic_error("a source on bank " + found.name + " holds more than " +
					                       std::to_string(maxSourceFrames) + " frames");
				requireOneName(name_ + "." + found.name);
				requireFreeBank(found);
				Configuration& c = *config_;
				SourceAdapter& sources = given(host_.resources.sources(), "sources");
				std::unique_ptr<Source> made = adapter("SourceAdapter::make", [&] {
					auto source = sources.make(name_, found.name, static_cast<std::uint32_t>(found.ports.size()),
					                           shape.frames, host_.format.rate, std::move(clock));
					if (!source) throw std::logic_error("the source adapter made no source for bank " + found.name);
					return source;
				});
				auto& slot = c.sources.emplace_back(SourceSlot{found.name, std::move(made), {}});
				slot.binding = SourceBinding{&slot.source->tap(), found.first, found.ports.size()};
				c.requestOrder.push_back({Asked::Family::source, c.sources.size() - 1});
				return slot.source->writer();
			});
		}
		void declareLatency(std::string_view from, std::string_view to, Latency latency) override {
			guarded([&] {
				requireConfiguring("declareLatency");
				const Ports::Bank& input = ports_.bank(from);
				const Ports::Bank& output = ports_.bank(to);
				if (input.direction != Direction::input || output.direction != Direction::output)
					throw std::logic_error("declareLatency names a pair: an input bank, then an output bank");
				recordLatency(output.name, latency, input.name);
			});
		}
		Loop& loopExecutor(std::size_t loop) override {
			return guarded([&]() -> Loop& { return loopSlot("executor", loop); });
		}
		void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) override {
			guarded([&] { loopSlot("post", loop).postState(std::move(work)); });
		}
		/// A declared store (docs/writing-a-node-type.md#stores): opened at readiness step `opens` whether or not it is
		/// asked for here; a host with no stores adapter refuses it.
		Store& store(std::size_t store) override {
			return guarded([&]() -> Store& {
				requireConfiguring("store");
				given(host_.resources.stores(), "stores");
				return *config_->stores.at(store);
			});
		}

		/// A port the type's own thread binds (docs/writing-a-node-type.md#claims), judged with the configuration's other
		/// claims at readiness step `claims`; a store's or a recording's is the framework's own.
		void claim(Claim claim) override {
			guarded([&] {
				requireConfiguring("claim");
				if (claim.kind != Claim::Kind::tcp && claim.kind != Claim::Kind::udp)
					throw std::logic_error("claim takes a port: " + claimKey(claim) + " is the framework's to claim");
				addPortClaim(std::move(claim));
			});
		}

		/// What the book tells, fanned out to the host's status listeners (a superseded node's only to those that hear
		/// one), its diagnostics kept as the node's own, and a loss to the node while its configuration's teardown has
		/// not begun.
		class BookOut final : public detail::StatusBookEvents {
		public:
			BookOut(Node& node, Channel* channel) : node_(node), channel_(channel) {}
			void written(const StatusWrite& write) override {
				for (auto* listener : node_.heard()) listener->statusWritten(node_.name_, write);
			}
			void changed(std::span<const FieldValue> fields) override {
				for (auto* listener : node_.heard()) listener->statusChanged(node_.name_, fields);
			}
			void reset(std::span<const FieldValue> fields) override {
				for (auto* listener : node_.heard()) listener->statusReset(node_.name_, fields);
			}
			void raised(const RaisedEvent& event) override {
				for (auto* listener : node_.heard()) listener->eventRaised(node_.name_, event);
			}
			void diagnosed(NodeDiagnostic diagnostic) override { node_.diagnose(std::move(diagnostic)); }
			void lost(std::string detail) override {
				if (channel_ != nullptr && !channel_->isClosing()) node_.insidesLost(std::move(detail));
			}

		private:
			Node& node_;
			Channel* channel_;
		};
		/// The status listeners that hear this node.
		std::vector<NodeStatusListener*> heard() const {
			std::vector<NodeStatusListener*> heard;
			for (auto* listener : host_.statusListeners)
				if (!superseded_ || listener->hearsSuperseded()) heard.push_back(listener);
			return heard;
		}
		/// Every record `channel` holds, in order, into the book.
		void drainReports(Channel& channel) {
			if (!book_) return;
			BookOut out(*this, &channel);
			std::deque<detail::ReportRecord> records = channel.queue().take();
			const std::uint64_t dropped = channel.queue().takeDropped();
			if (records.empty() && dropped == 0) return;
			book_->apply(std::move(records), dropped, host_.timers.now(), out);
		}
		/// The configuration closed (or failed to start, or is leaked): its last records drained, nothing more of it
		/// reaches the node, then the status view returns to every initial value (docs/architecture.md#status-and-events).
		/// `repointed`: its channel is detached by its LeakedRecord's repoint instead.
		void closeStatus(Configuration& c, bool repointed = false) {
			drainReports(*c.channel);
			if (!repointed) c.channel->detach();
			if (!book_) return;
			BookOut out(*this, nullptr);
			book_->reset(host_.timers.now(), out);
		}

		/// ear.claim-wait, a condition (docs/output-records.md#diagnostic): the claim of `key` waits for its holder.
		void claimWaits(const std::string& key, const std::string& why) {
			diagnose({.code = "ear.claim-wait",
			          .subject = key,
			          .condition = true,
			          .active = true,
			          .detail = key + " " + why});
		}
		/// The claim of `key` no longer waits: judged otherwise. Said only when it waited.
		void claimWaitEnded(const std::string& key) {
			diagnose({.code = "ear.claim-wait",
			          .subject = key,
			          .condition = true,
			          .active = false,
			          .detail = key + " no longer waits for its holder"});
		}
		/// Every claim that waits no longer does: its configuration's teardown began (or its change was dropped).
		void claimWaitsEnded() {
			std::vector<std::string> waiting;
			for (const NodeDiagnostic& each : diagnostics_)
				if (each.code == "ear.claim-wait" && each.active) waiting.push_back(each.subject);
			for (const std::string& key : waiting) claimWaitEnded(key);
		}

		/// A claim of the configuration of `generation` judged (an input of its own): granted, to probe, refused, or
		/// waiting for its holder, which is closing.
		void claimJudged(std::uint64_t generation, const Claim& claim, detail::ClaimJudged judged) {
			if (current(generation) && changeClaimJudged(claim, judged)) return;
			if (!stillReadying(generation)) return;
			const auto found = std::ranges::find(config_->claims, claim, &ClaimSlot::claim);
			if (found == config_->claims.end()) return;
			const auto index = static_cast<std::size_t>(found - config_->claims.begin());
			if (judged.outcome != detail::ClaimJudged::Outcome::waits) claimWaitEnded(claimKey(found->claim));
			switch (judged.outcome) {
			case detail::ClaimJudged::Outcome::granted:
				itemReady(generation, found->item);
				break;
			case detail::ClaimJudged::Outcome::probe:
				// An AnyTcp port was picked free by the probe, or by the host's own count: never probed again.
				if (found->anyTcp) itemReady(generation, found->item);
				else probeClaim(generation, index, judged.leaked);
				break;
			case detail::ClaimJudged::Outcome::refused:
				refuseClaim(generation, index, judged.why, judged.leaked);
				break;
			case detail::ClaimJudged::Outcome::waits:
				found->waitingOn = judged.why;
				claimWaits(claimKey(found->claim), "held by node " + judged.why + ", which is closing");
				break;
			}
		}

		/// A leaked configuration that refused this node's configuration of `generation` closed late: the node is
		/// started again, once, as node.restart would, if that refusal is still what it rests failed on.
		void restartRefused(std::uint64_t generation) {
			if (refusedGeneration_ != generation || aggregate_.health().state != NodeState::failed) return;
			refusedGeneration_ = 0;
			NodeOperations::OperationJudged none;
			static_cast<void>(operate(&lc::NodeAggregate::restart, none));
		}

		/// Runs a context call, telling apart by owner what it throws (docs/architecture.md#faults-by-owner), each
		/// recorded, the first only, before it is thrown on to the type: a std::logic_error NodeHost's checks threw is a
		/// rule the type broke; std::bad_alloc is resource exhaustion, not a bug, and fails the node as a refusal does;
		/// an adapter's refusal or bug was recorded by adapter() and passes as it is: the refusal is known by its
		/// identity, which nothing clears until the type's call ends; anything else thrown by NodeHost's own code is NodeHost's bug, thrown on as an EarFault
		/// NodeHost owns, which callType rethrows as itself for the callback boundary.
		template <typename Call> std::invoke_result_t<Call> guarded(Call&& call) {
			try {
				return std::forward<Call>(call)();
			} catch (const EarFault&) {
				throw;
			} catch (const std::logic_error& broken) {
				if (!brokenRule_) brokenRule_ = broken.what();
				throw;
			} catch (const std::bad_alloc& exhausted) {
				if (!brokenRule_) brokenRule_ = exhausted.what();
				throw;
			} catch (...) {
				if (refused_ != nullptr && std::current_exception() == refused_) throw;
				throw recordFault(FaultOwner::host, {});
			}
		}

		/// The host's adapter of a kind, from inside a context call; for a kind the host has none of (a test's host), the
		/// node's refusal "no <kind> in this host", recorded and thrown as an adapter's refusal is (adapter()).
		template <class Adapter> Adapter& given(Adapter* found, std::string_view kind) {
			if (found != nullptr) return *found;
			return adapter("ResourceRegistry", [&]() -> Adapter& {
				throw std::runtime_error("no " + std::string(kind) + " in this host");
			});
		}

		/// Calls an adapter from inside a context call. What it throws is told apart by its class, as only an adapter's
		/// own code can throw here: a std::logic_error is the adapter's bug, recorded and thrown on as an EarFault the
		/// adapter owns, so its node fails lost and the rest of the graph runs on; anything else is its refusal (no
		/// thread, no eventfd), not a bug, recorded as the node's failure so a type that catches it still fails, and
		/// thrown on as itself.
		template <typename Call> std::invoke_result_t<Call> adapter(std::string_view callback, Call&& call) {
			try {
				return std::forward<Call>(call)();
			} catch (const std::logic_error&) {
				throw recordFault(FaultOwner::adapter, std::string(callback));
			} catch (...) {
				if (!brokenRule_) brokenRule_ = whatOf(std::current_exception());
				refused_ = std::current_exception();
				throw;
			}
		}

		/// Inside a catch handler: the exception handled, as ear's own fault of `owner`, recorded when it is the first
		/// in the type's call (callType reads it even when the type caught what it threw).
		EarFault recordFault(FaultOwner owner, std::string callback) {
			EarFault fault(owner, std::move(callback), std::current_exception());
			if (!earFault_) earFault_ = fault;
			return fault;
		}

	private:
		/// One engine companion the configuration asked for (a sound card or an RTP receiver), and its events, which
		/// land only on the configuration that made it.
		class Companion final : public EngineCompanionEvents {
		public:
			/// `id`: unique across the host, so an event of a companion a change replaced (retiring) is told apart from
			/// its replacement's on the same index.
			Companion(Impl& host, std::uint64_t serial, std::uint64_t generation, std::size_t index, std::uint64_t id)
				: host_(host), serial_(serial), generation_(generation), index_(index), id_(id) {}
			std::unique_ptr<EngineCompanion> made;
			std::uint64_t id() const noexcept { return id_; }
			// Told inside its making (during the flush) or later: an input of its own either way.
			void ready() override {
				host_.inputFor(serial_, [generation = generation_, index = index_, id = id_](Node& node) {
					// Still current while it tears down, when its engine companions are already gone: nothing to ready.
					if (!node.current(generation) || node.config_->tearing ||
					    index >= node.config_->engineCompanions.size() || !node.config_->engineCompanions[index] ||
					    node.config_->engineCompanions[index]->id() != id)
						return;
					// Once the configuration was ready, a companion made is a change's replacement.
					if (node.config_->readyTold) return node.replacementReady(id);
					node.itemReady(generation, node.config_->engineAsks[index].item);
				});
			}
			void lost(std::string detail) override { tell(lc::Fact::engine, std::move(detail)); }
			void faulted(std::string detail) override { tell(lc::Fact::adapterFaulted, std::move(detail)); }

		private:
			void tell(lc::Fact fact, std::string detail) {
				host_.inputFor(serial_, [generation = generation_, index = index_, id = id_, fact,
				                         detail = std::move(detail)](Node& node) mutable {
					if (!node.current(generation)) return;
					// A companion a change replaces is retiring: its loss is said, never a fault of the node; its
					// replacement's before the commit point falls the change back
					// (docs/architecture.md#changes-in-place).
					if (node.companionLostInChange(index, id, detail)) return;
					node.feed(node.aggregate_.fault(fact, std::move(detail)));
				});
			}
			Impl& host_; // never the node: a queued destruction may outlive it
			std::uint64_t serial_;
			std::uint64_t generation_;
			std::size_t index_;
			std::uint64_t id_;
		};

		class LeakedRecord;

		/// What a change version works through (ChangeContext; docs/writing-a-node-type.md#the-change-version): the
		/// node's own, valid only inside the change version's call; every call after it is a broken rule, as a
		/// CreateContext's is. Its checks run through the node's guarded, so a broken rule fails the node even when the
		/// type catches it.
		class ChangeScope final : public ChangeContext {
		public:
			explicit ChangeScope(Node& node) noexcept : node_(node) {}
			bool valid = false;  // inside the change version's call
			bool broken = false; // a call found a rule broken in this call
			AudioFormat format() const override { return node_.host_.format; }
			const NodeName& node() const override { return node_.name_; }
			const Ports& ports() const override { return node_.ports_; }
			void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) override {
				checked("post", [&] {
					if (loop >= node_.config_->loops.size()) throw std::logic_error("a post to a loop not declared");
					node_.config_->change->posts.emplace_back(loop, std::move(work));
				});
			}
			void attachEngine(std::string_view bank, EngineCompanionShape shape, Order order) override {
				checked("attach", [&] { node_.attachInChange(bank, std::move(shape), order); });
			}
			SinkControl& sink(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) override {
				return checked("attach",
				               [&]() -> SinkControl& { return node_.sinkInChange(bank, shape, std::move(consumer)); });
			}
			SourceWriter& source(std::string_view bank, SourceShape shape,
			                     std::unique_ptr<SourceClock> clock) override {
				return checked("attach",
				               [&]() -> SourceWriter& { return node_.sourceInChange(bank, shape, std::move(clock)); });
			}
			Recording& recording(const Recording& replaced, const RecordingRequest& request) override {
				return checked("recording", [&]() -> Recording& { return node_.recordingInChange(replaced, request); });
			}
			void hold(std::span<const std::string_view> banks) override {
				checked("hold", [&] { node_.holdBanks(banks, true); });
			}
			void holdAll() override {
				checked("hold", [&] {
					std::vector<std::string_view> all;
					for (const Ports::Bank& bank : node_.config_->ports.banks())
						if (bank.direction == Direction::output && bank.reach == PortBank::Reach::routable)
							all.push_back(bank.name);
					node_.holdBanks(all, true);
				});
			}
			void release(std::span<const std::string_view> banks) override {
				checked("release", [&] { node_.holdBanks(banks, false); });
			}
			void declareLatency(std::string_view from, std::string_view to, Latency latency) override {
				checked("declareLatency", [&] {
					const Ports::Bank& input = node_.ports_.bank(from);
					const Ports::Bank& output = node_.ports_.bank(to);
					if (input.direction != Direction::input || output.direction != Direction::output)
						throw std::logic_error("declareLatency names a pair: an input bank, then an output bank");
					auto& declared = node_.config_->change->latency;
					if (!declared) declared.emplace();
					if (std::ranges::any_of(*declared, [&](const DeclaredLatency& one) {
							return one.bank == output.name && one.from == input.name;
						}))
						throw std::logic_error("latency declared twice for the path " + input.name + " to " +
						                       output.name);
					declared->push_back(DeclaredLatency{input.name, output.name, latency});
				});
			}
			void claim(Claim claim) override {
				checked("claim", [&] { node_.claimInChange(std::move(claim)); });
			}
			void unclaim(Claim claim) override {
				checked("unclaim", [&] { node_.unclaimInChange(claim); });
			}
			void whenReady(std::size_t loop, std::move_only_function<void(void*, ConfigureDone)> work) override {
				checked("whenReady", [&] {
					Configuration::Change& ch = *node_.config_->change;
					if (ch.continuation || ch.continuationPosted)
						throw std::logic_error("whenReady called twice in one change");
					if (loop >= node_.config_->loops.size()) throw std::logic_error("whenReady on a loop not declared");
					ch.continuation = std::move(work);
					ch.continuationLoop = loop;
				});
			}

		private:
			template <typename Call> std::invoke_result_t<Call> checked(std::string_view call, Call&& run) {
				return node_.guarded([&]() -> std::invoke_result_t<Call> {
					try {
						if (!valid) throw std::logic_error(std::string(call) + " outside configure");
						return run();
					} catch (const std::logic_error&) {
						broken = true;
						throw;
					}
				});
			}
			Node& node_;
		};

		/// One configuration as its own threads reach the node, and its handle's implementation
		/// (docs/writing-a-node-type.md#the-node-handle): everything they tell crosses to the main loop, in order, and
		/// lands only while the configuration is attached. Each configuration has its own Channel, detached on every path
		/// that ends it (it closed, it failed to start, or it was leaked), before the next one can start: that is the
		/// configuration's generation, so no write of an old configuration lands on a new one. A configuration leaked at
		/// the close deadline is re-pointed instead (docs/architecture.md#teardown-by-dependency): its handle's notes,
		/// losses and log entries and its I/O completions are dropped as after a detach, while its closes (each sink's
		/// and loop's) and its consumers' throws land on its LeakedRecord, never on the
		/// node. Whatever is still queued holds this object, not the node, so it may outlive the node. Its note rate is
		/// its own, so it is per node and lasts as long as the configuration: a stop and a start keep it, a restart
		/// begins a fresh one.
		class Channel final : public NodeHandle, public std::enable_shared_from_this<Channel> {
		public:
			/// `sinks` are NodeHost's log sinks, all subscribed before the first node (subscribeLog); `timers` is the
			/// clock the note rate counts on.
			Channel(Node& node, Executor& mainLoop, Timers& timers, NodeName name, std::string type,
			        std::vector<LogSink*> sinks)
				: node_(&node), mainLoop_(mainLoop), timers_(timers),
				  completions_(std::make_shared<detail::Completions>(mainLoop)), name_(std::move(name)),
				  type_(std::move(type)), sinks_(std::move(sinks)), gate_([&timers] { return timers.now(); }) {}
			const NodeName& node() const noexcept override { return name_; }
			/// Judged on the saying thread, so a note held back is never queued; posted under the gate's lock, so the
			/// order notes pass the gate is the order they land in, `notes-dropped` just before the note it precedes.
			void say(std::string text) override {
				if (!completions_->attached()) return;
				const std::scoped_lock lock(sayMutex_);
				const std::optional<std::uint64_t> held = gate_.pass();
				if (!held) return;
				completions_->post([self = shared_from_this(), held = *held, text = std::move(text)] {
					Node* node = self->attachedNode();
					if (node == nullptr) return;
					if (held > 0) node->host_.noted(node->name_, detail::notesDropped(held));
					node->host_.noted(node->name_, text);
				});
			}
			/// Through the queue, behind the status and events written before it (docs/architecture.md#status-and-events),
			/// and drained at once rather than at the next tick: a loss is never paced. Never dropped by the queue's
			/// bound; dropped on the main loop once the teardown began.
			void lost(std::string detail) override {
				queue_.push(
					{.kind = detail::ReportRecord::Kind::lost, .detail = std::move(detail), .at = timers_.now()});
				completions_->post([self = shared_from_this()] {
					if (Node* node = self->attachedNode()) node->reportsDue(*self, true);
				});
			}
			/// Report's writes (report.hpp): into the queue on the writer's thread, the first since the last drain
			/// waking the main loop once. Dropped once the configuration has ended.
			void reportStatus(std::size_t field, StatusValue value) override {
				if (!completions_->attached()) return;
				woken(queue_.push({.kind = detail::ReportRecord::Kind::status,
				                   .index = field,
				                   .value = std::move(value),
				                   .at = timers_.now()}));
			}
			void reportEvent(std::size_t event, std::vector<StatusValue> args) override {
				if (!completions_->attached()) return;
				woken(queue_.push({.kind = detail::ReportRecord::Kind::event,
				                   .index = event,
				                   .args = std::move(args),
				                   .at = timers_.now()}));
			}
			/// Main thread: the records written since the last take, and how many the bound dropped.
			detail::ReportQueue& queue() noexcept { return queue_; }
			bool isClosing() const noexcept { return closing_; }
			/// Straight to the sinks on the calling thread, as captured output goes; the attached flag is atomic, so a
			/// configuration that ended logs nothing, but for one entry that read the flag just before the detach
			/// (the detach does not wait for a log in progress).
			void log(LogEntry entry) override {
				if (!completions_->attached()) return;
				entry.node = name_;
				if (entry.source.empty()) entry.source = type_;
				for (LogSink* sink : sinks_) sink->log(entry);
			}
			/// Any thread: a consumer (or a loop's handler) of this configuration threw (ConsumerCalls). A fact, as a
			/// hook's throw is, while facts reach the node; once its teardown began a log record, as a stop() that throws
			/// then is, a leaked configuration's included (never a fact: it reaches no node).
			void threw(std::string detail) {
				mainLoop_.post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					if (Node* node = self->attachedNode()) {
						if (self->closing_)
							node->host_.logged(LogEntry{LogLevel::error, "ear", std::move(detail), {}, node->name_});
						else node->consumerThrew(std::move(detail));
					} else if (LeakedRecord* record = self->record_) record->threw(std::move(detail));
				});
			}
			/// Main thread. The configuration is going (its teardown began): a fact no longer reaches the node.
			void closing() noexcept { closing_ = true; }
			/// The configuration's way back to the main loop, for its I/O jobs' completions (its stores').
			const std::shared_ptr<detail::Completions>& completions() const noexcept { return completions_; }
			/// Main thread. Nothing more of this configuration reaches the node, or its leaked record.
			void detach() noexcept {
				node_ = nullptr;
				record_ = nullptr;
				completions_->detach();
			}
			/// Main thread, as the configuration is leaked at the close deadline: nothing more reaches the node, and its
			/// closes and consumers' throws reach `record` from now on.
			void repoint(LeakedRecord& record) noexcept {
				detach();
				record_ = &record;
			}
			/// The `begun` sink `index` of this configuration gets: on the main thread, later, behind any lost() its
			/// consumer said inside begin() (the same FIFO).
			std::move_only_function<void()> sinkBegunCall(std::size_t index) {
				return [self = shared_from_this(), index] {
					if (Node* node = self->attachedNode()) node->sinkBegun(node->config_->generation, index);
				};
			}
			/// The `closed` sink `index` of this configuration gets: on the main thread, later.
			std::move_only_function<void(std::uint64_t)> sinkClosedCall(std::size_t index) {
				return [self = shared_from_this(), index](std::uint64_t droppedFrames) {
					if (Node* node = self->attachedNode())
						node->sinkClosed(node->config_->generation, index, droppedFrames);
					else if (LeakedRecord* record = self->record_) record->sinkClosed(index);
				};
			}
			/// The `begun` a sink a change made as a replacement gets, by its id.
			std::move_only_function<void()> replacementBegunCall(std::uint64_t id) {
				return [self = shared_from_this(), id] {
					if (Node* node = self->attachedNode()) node->replacementSinkBegun(node->config_->generation, id);
				};
			}
			/// The `closed` a sink a change replaced (or a replacement it dropped) gets, by its id.
			std::move_only_function<void(std::uint64_t)> retiredSinkClosedCall(std::uint64_t id) {
				return [self = shared_from_this(), id](std::uint64_t droppedFrames) {
					if (Node* node = self->attachedNode())
						node->retiredSinkClosed(node->config_->generation, id, droppedFrames);
					else if (LeakedRecord* record = self->record_) record->retiredSinkClosed(id);
				};
			}
			/// The `finished` loop `index` of this configuration gets at teardown step 4: on the main thread, later.
			std::move_only_function<void()> loopFinishedCall(std::size_t index) {
				return [self = shared_from_this(), index] {
					if (Node* node = self->attachedNode()) node->loopFinishedHeard(node->config_->generation, index);
					else if (LeakedRecord* record = self->record_) record->loopFinished(index);
				};
			}
			/// The `closed` loop `index` of this configuration gets at teardown step 4: on the main thread, later.
			std::move_only_function<void()> loopClosedCall(std::size_t index) {
				return [self = shared_from_this(), index] {
					if (Node* node = self->attachedNode()) node->loopClosedHeard(node->config_->generation, index);
					else if (LeakedRecord* record = self->record_) record->loopClosed(index);
				};
			}
			/// What the configuration's program tells (ProgramEvents, on the adapter's thread), crossing to the main loop as
			/// a close does: started, ready, refused and exited land on the node while attached, refused and exited on the
			/// leaked record once re-pointed (a program reaped is one of the closes its late close waits for).
			void programStarted() {
				mainLoop_.post([self = shared_from_this()] {
					if (Node* node = self->attachedNode()) node->programStarted(node->config_->generation);
				});
			}
			void programReady() {
				mainLoop_.post([self = shared_from_this()] {
					if (Node* node = self->attachedNode()) node->programReady(node->config_->generation);
				});
			}
			void programRefused(std::string why) {
				mainLoop_.post([self = shared_from_this(), why = std::move(why)]() mutable {
					if (Node* node = self->attachedNode())
						node->programRefused(node->config_->generation, std::move(why));
					else if (LeakedRecord* record = self->record_) record->programEnded();
				});
			}
			void programExited(ProgramExit exit) {
				mainLoop_.post([self = shared_from_this(), exit = std::move(exit)]() mutable {
					if (Node* node = self->attachedNode())
						node->programExited(node->config_->generation, std::move(exit));
					else if (LeakedRecord* record = self->record_) record->programEnded();
				});
			}
			/// A change's continuation answered (ConfigureDone, any thread): heard on the main loop while the configuration
			/// is attached and not going, by the change of `epoch` alone.
			void changeAnswered(std::uint64_t epoch, std::optional<std::string> failure) {
				completions_->post([self = shared_from_this(), epoch, failure = std::move(failure)]() mutable {
					if (Node* node = self->attachedNode(); node != nullptr && !self->closing_)
						node->continuationAnswered(node->config_->generation, epoch, std::move(failure));
				});
			}
			/// Its report feed's bytes: dropped once the configuration ended, as every report of its threads is.
			void programFed(std::string bytes) {
				completions_->post([self = shared_from_this(), bytes = std::move(bytes)]() mutable {
					if (Node* node = self->attachedNode())
						node->programFed(node->config_->generation, std::move(bytes));
				});
			}

		private:
			void woken(bool first) {
				if (!first) return;
				completions_->post([self = shared_from_this()] {
					if (Node* node = self->attachedNode()) node->reportsDue(*self, false);
				});
			}
			/// Main thread: the node, while this configuration is attached.
			Node* attachedNode() const noexcept { return node_; }
			Node* node_;                     // main thread only
			LeakedRecord* record_ = nullptr; // main thread only: set once leaked at the close deadline
			/// Where the reports a leaked configuration may still make are posted (its closes, its consumers' throws),
			/// each judged where it lands: the node's while attached, else the record's if re-pointed, else dropped.
			Executor& mainLoop_;
			Timers& timers_; // what a report's record is stamped with
			/// The way back to the main loop for everything else this configuration's threads tell (its handle's notes
			/// and losses, its I/O jobs' completions), detached with node_ and never re-pointed; its attached() is node_
			/// for the threads that cannot read it.
			const std::shared_ptr<detail::Completions> completions_;
			const NodeName name_;    // a copy: a leaked configuration may outlive its node
			const std::string type_; // a log entry's source when the type gave none
			const std::vector<LogSink*> sinks_;
			std::mutex sayMutex_;
			detail::NoteGate gate_; // sayMutex_
			detail::ReportQueue queue_;
			bool closing_ = false; // main thread only
		};

		/// Where a change's ConfigureDone answers: its configuration's Channel, so an answer that outlives the change or
		/// the configuration lands nowhere.
		class ContinuationAnswer final : public detail::ChangeAnswer {
		public:
			ContinuationAnswer(std::shared_ptr<Channel> channel, std::uint64_t epoch) noexcept
				: channel_(std::move(channel)), epoch_(epoch) {}
			void answer(std::optional<std::string_view> failure) noexcept override {
				try {
					channel_->changeAnswered(epoch_, failure ? std::optional<std::string>(*failure) : std::nullopt);
				} catch (...) { // no memory to say it: the change's deadline ends it
				}
			}

		private:
			std::shared_ptr<Channel> channel_;
			std::uint64_t epoch_;
		};

		class LoopSlot;

		/// What the instance's NodeOf wraps (lifecycle.hpp): the node's own for one configuration, declared before the
		/// instance, so it outlives it.
		class Context final : public NodeContext {
		public:
			Context(AudioFormat format, NodeName name, Channel& channel,
			        std::vector<std::unique_ptr<LoopSlot>>& loops) noexcept
				: format_(format), name_(std::move(name)), channel_(channel), loops_(loops) {}
			AudioFormat format() const noexcept override { return format_; }
			const NodeName& node() const noexcept override { return name_; }
			NodeHandle& handle() noexcept override { return channel_; }
			void postToLoop(std::size_t loop, std::move_only_function<void(void*)> work) override {
				loops_.at(loop)->postState(std::move(work));
			}

		private:
			AudioFormat format_;
			NodeName name_; // a copy: a leaked configuration may outlive its node
			Channel& channel_;
			std::vector<std::unique_ptr<LoopSlot>>& loops_; // the configuration's, which outlive the instance
		};

		/// The catch a type's consumer is called through, on the one thread that calls it (its sink's): each call through caughtFrom, so what it throws is told as its configuration's fact
		/// (Channel::threw, "<consumer> threw in <call>: <what>") and never reaches that thread's callback.
		class ConsumerCalls {
		public:
			ConsumerCalls(std::shared_ptr<Channel> channel, std::string consumer)
				: channel_(std::move(channel)), consumer_(std::move(consumer)) {}
			/// Whether `run` returned.
			template <typename Call> bool operator()(std::string_view call, Call&& run) {
				auto done = caughtFrom(std::forward<Call>(run));
				if (done) return true;
				threw_ = true;
				channel_->threw(consumer_ + " threw in " + std::string(call) + ": " + done.error());
				return false;
			}
			/// A call threw: the consumer's state is the type's bug's, so no more chunks are handed to it.
			bool threw() const noexcept { return threw_; }

		private:
			std::shared_ptr<Channel> channel_;
			std::string consumer_;
			bool threw_ = false; // its thread's only
		};

		/// A sink's consumer as its sink calls it (docs/writing-a-node-type.md#sinks). After a begin that threw nothing
		/// more is called; after a consume that threw no more consume, but finish still pairs with the begin that
		/// returned, so what begin opened is closed.
		class GuardedSinkConsumer final : public SinkConsumer {
		public:
			GuardedSinkConsumer(std::unique_ptr<SinkConsumer> type, std::shared_ptr<Channel> channel,
			                    const std::string& bank)
				: type_(std::move(type)), calls_(std::move(channel), "the sink " + bank + "'s consumer") {}
			void begin() override {
				began_ = calls_("begin", [&] { type_->begin(); });
			}
			void consume(const Chunk& chunk) override {
				if (began_ && !calls_.threw()) calls_("consume", [&] { type_->consume(chunk); });
			}
			void finish() override {
				if (began_) calls_("finish", [&] { type_->finish(); });
			}

		private:
			std::unique_ptr<SinkConsumer> type_;
			ConsumerCalls calls_;
			bool began_ = false; // its sink's thread's only
		};

		/// One attached sink (docs/writing-a-node-type.md#sinks): the sink, its one consumer until Sink::begin takes it,
		/// and what a drive binds.
		class SinkSlot {
		public:
			SinkSlot(std::string name, SinkConsumerFrom consumer) : bank(std::move(name)) {
				if (auto* own = std::get_if<std::unique_ptr<SinkConsumer>>(&consumer)) {
					consumer_ = std::move(*own);
				} else {
					auto& state = std::get<StateSinkConsumer>(consumer);
					stateLoop = state.loop;
					member_ = std::move(state.member);
				}
			}
			/// Main thread, at configure's applied return: the consumer handed to the sink, called through its catch.
			/// `state`: the state of loop `stateLoop` when the consumer is part of it (it lives until teardown step 4,
			/// after the sink's last call at step 3), else null.
			void begin(std::shared_ptr<Channel> channel, std::move_only_function<void()> begun, void* state) {
				resolved = stateLoop ? &member_(state) : consumer_.get();
				std::unique_ptr<SinkConsumer> consumer =
					stateLoop ? std::make_unique<StateConsumer>(member_(state)) : std::move(consumer_);
				sink->begin(std::make_unique<GuardedSinkConsumer>(std::move(consumer), std::move(channel), bank),
				            std::move(begun));
			}

			std::string bank;
			std::unique_ptr<Sink> sink;
			SinkBinding binding{};
			bool closed = false;                    // its close reported
			std::size_t item = detail::noReadiness; // its consumer's readiness item: ready once it began
			std::optional<std::size_t> stateLoop;   // the loop whose state holds the consumer
			std::uint64_t id = 0;                   // unique across the host: a replaced sink's reports find it
			bool closeAsked = false;                // a replaced sink's close was asked (once)
			bool kept = false; // a replacement dropped after its change's continuation was posted: freed at step 5
			const SinkConsumer* resolved = nullptr; // the consumer its sink calls, once begun
			/// The consumer `state` holds for this sink, by the member it names: an address alone, nothing read.
			const SinkConsumer* stateConsumer(void* state) { return stateLoop ? &member_(state) : nullptr; }

		private:
			/// A consumer a loop's state owns, called through: the sink owns only this.
			class StateConsumer final : public SinkConsumer {
			public:
				explicit StateConsumer(SinkConsumer& owned) noexcept : owned_(owned) {}
				void begin() override { owned_.begin(); }
				void consume(const Chunk& chunk) override { owned_.consume(chunk); }
				void finish() override { owned_.finish(); }

			private:
				SinkConsumer& owned_;
			};

			std::unique_ptr<SinkConsumer> consumer_;               // until Sink::begin takes it
			std::move_only_function<SinkConsumer&(void*)> member_; // finds a state's consumer
		};

		/// One attached source (docs/writing-a-node-type.md#sources): the source, its bank, and what a drive binds.
		struct SourceSlot {
			std::string bank;
			std::unique_ptr<Source> source;
			SourceBinding binding{};
		};

		/// One declared loop of a configuration (docs/writing-a-node-type.md#loops): the type's view of it (Loop, which
		/// stages what is sent to it until the node is configured), its state, and the adapter's events about it (main
		/// thread, landing only on the configuration that made it). Every source armed through it is counted while its
		/// handle lives, so the close can say the ones a state left armed.
		class LoopSlot final : public Loop, public LoopEvents {
		public:
			LoopSlot(Impl& host, std::uint64_t serial, std::uint64_t generation, std::size_t index,
			         LoopDeclaration declared, std::shared_ptr<Channel> channel)
				: declared(declared), host_(host), serial_(serial), generation_(generation), index_(index),
				  channel_(std::move(channel)) {}
			/// Main thread, at teardown step 5 (or a configure that failed): a state never handed to a loop goes here,
			/// while the slot is whole (its destructor may ask the loop, inLoop()), after the adapter's loop.
			~LoopSlot() {
				made.reset();
				state.reset();
			}

			// Loop, as the type sees it: staged until released, then the adapter's loop.
			void post(std::move_only_function<void()> work) override {
				std::move_only_function<void()> refused;
				{
					const std::scoped_lock lock(mutex_);
					if (!released_) {
						staged_.push_back(Staged{.work = std::move(work)});
						return;
					}
					if (real_ != nullptr) {
						real_->post(std::move(work));
						return;
					}
					refused = std::move(work); // a loop never made, its staging dropped: destroyed unrun
				}
			}
			std::unique_ptr<Timer> after(std::chrono::milliseconds delay,
			                             std::move_only_function<void()> due) override {
				return arm([delay, due = whileStateLives(std::move(due))](NodeLoop& loop) mutable {
					return loop.after(delay, std::move(due));
				});
			}
			std::unique_ptr<Timer> every(std::chrono::milliseconds period,
			                             std::move_only_function<void()> due) override {
				return arm([period, due = whileStateLives(std::move(due))](NodeLoop& loop) mutable {
					return loop.every(period, std::move(due));
				});
			}
			std::unique_ptr<Timer> onWake(Wake& wake, std::move_only_function<void()> rung) override {
				return arm([&wake, rung = whileStateLives(std::move(rung))](NodeLoop& loop) mutable {
					return loop.onWake(wake, std::move(rung));
				});
			}
			std::unique_ptr<Timer> onReadable(int fd, std::move_only_function<void()> readable) override {
				return arm([fd, readable = whileStateLives(std::move(readable))](NodeLoop& loop) mutable {
					return loop.onReadable(fd, std::move(readable));
				});
			}
			LoopKind kind() const noexcept override { return declared.kind; }
			bool inLoop() const noexcept override {
				const std::scoped_lock lock(mutex_);
				return real_ != nullptr && real_->inLoop();
			}

			/// A timer's or watch's handler, run only while the loop's state lives: between teardown step 4's finish (the
			/// state destroyed) and its close (the sources left cancelled), a source the state left armed may still fire,
			/// and its handler may reach the state. Both run on the loop's thread.
			std::move_only_function<void()> whileStateLives(std::move_only_function<void()> handler) {
				return [this, handler = std::move(handler)]() mutable {
					if (!stateGone_) handler();
				};
			}
			/// A post with the loop's state (null for a loop with none), run only while the state lives: once its loop
			/// destroyed it, a post still queued is destroyed unrun (both on the loop's thread).
			void postState(std::move_only_function<void(void*)> work) {
				post([this, work = std::move(work)]() mutable {
					if (!stateGone_) work(state.get());
				});
			}

			// LoopEvents, told by the adapter on the main loop: inputs of their own.
			void ready() override {
				host_.inputFor(serial_, [generation = generation_, index = index_](Node& node) {
					node.loopReady(generation, index);
				});
			}
			void failed(std::string detail) override {
				host_.inputFor(serial_,
				               [generation = generation_, index = index_, detail = std::move(detail)](
								   Node& node) mutable { node.loopFailed(generation, index, std::move(detail)); });
			}
			void slow(std::int64_t ms) override {
				host_.inputFor(serial_, [generation = generation_, ms, name = std::string(declared.name)](Node& node) {
					if (!node.current(generation)) return;
					const std::array fields{DiagnosticField{"ms", ms}};
					node.diagnose({.code = "ear.loop-slow",
					               .subject = name,
					               .detail = "loop " + name + " ran one handler for " + std::to_string(ms) + " ms",
					               .fields = {fields.begin(), fields.end()}});
				});
			}
			/// Through the configuration's Channel, so a leaked configuration's is logged on its record.
			void threw(std::string what) override {
				channel_->threw("loop " + std::string(declared.name) + "'s handler threw: " + std::move(what));
			}

			/// Main thread: work from outside the node with the loop's state (a program's report feed), staged as the node's
			/// own until the node is configured, then the adapter's outside work, which teardown step 1 refuses
			/// (NodeLoop::closeToOthers).
			void postOutsideState(std::move_only_function<void(void*)> work) {
				std::move_only_function<void()> wrapped = [this, work = std::move(work)]() mutable {
					if (!stateGone_) work(state.get());
				};
				const std::scoped_lock lock(mutex_);
				if (!released_) {
					staged_.push_back(Staged{.work = std::move(wrapped)});
					return;
				}
				if (made) made->postFromOutside(std::move(wrapped));
			}
			/// Main thread, at readiness step `loops`: the adapter makes the loop; it tells ready or failed later.
			void start(LoopAdapter& adapter, const NodeName& node) {
				auto loop = adapter.make(node, declared.name, declared.kind, *this);
				if (!loop) throw std::logic_error("the loop adapter made no loop " + std::string(declared.name));
				const std::scoped_lock lock(mutex_);
				made = std::move(loop);
				real_ = made.get();
			}
			/// Main thread, once the node is configured: what was staged runs on the loop, in the order it was sent,
			/// ahead of anything sent from now on. Arming staged timers and watches happens there, on the loop's thread.
			void release() {
				const std::scoped_lock lock(mutex_);
				if (released_) return;
				released_ = true;
				if (real_ == nullptr || staged_.empty()) {
					staged_.clear();
					return;
				}
				real_->post([loop = real_, batch = std::exchange(staged_, {})]() mutable {
					for (Staged& item : batch) {
						if (item.work) {
							item.work();
							continue;
						}
						{
							const std::scoped_lock armLock(item.proxy->mutex);
							if (item.proxy->cancelled) continue;
						}
						std::unique_ptr<Timer> armed = item.arm(*loop);
						const std::scoped_lock armLock(item.proxy->mutex);
						if (!item.proxy->cancelled) item.proxy->real = std::move(armed);
						else
							armed.swap(item.proxy->real); // destroyed below, outside nothing: its handle went meanwhile
					}
				});
			}
			/// Main thread, as a teardown begins before the node was configured: what was staged is destroyed unrun here.
			void dropStaged() noexcept {
				std::deque<Staged> dropped;
				{
					const std::scoped_lock lock(mutex_);
					if (released_) return;
					released_ = true;
					dropped.swap(staged_);
				}
			}
			/// Sources armed through this loop whose handles still live.
			std::size_t armedLeft() const noexcept { return live_->load(); }
			/// Main thread, at teardown step 4: the state is destroyed on the loop's thread.
			std::move_only_function<void()> lastCall() {
				return [this] {
					stateGone_ = true;
					state.reset();
				};
			}

			const LoopDeclaration declared;
			std::unique_ptr<void, void (*)(void*)> state{nullptr,
			                                             [](void*) {}}; // the type's, until its loop destroys it
			std::size_t item = detail::noReadiness; // its readiness item: ready once the adapter says it runs
			bool failedHeard = false;               // the adapter said it cannot start: nothing to finish or close
			bool finished = false;
			bool closed = false;
			std::unique_ptr<NodeLoop> made; // last: destroyed first, so its adapter tells nothing after the slot

		private:
			/// A timer or watch armed while staged: armed for real at the release, on the loop's thread, unless its
			/// handle went first.
			struct Proxied {
				std::mutex mutex;
				bool cancelled = false;
				std::unique_ptr<Timer> real;
			};
			struct Staged {
				std::move_only_function<void()> work{}; // a post
				std::shared_ptr<Proxied> proxy{};       // or an arming
				std::move_only_function<std::unique_ptr<Timer>(NodeLoop&)> arm{};
			};
			/// A handle the type holds: counted while it lives; its destruction cancels the source.
			class Counted final : public Timer {
			public:
				Counted(std::shared_ptr<std::atomic<std::size_t>> live, std::unique_ptr<Timer> real,
				        std::shared_ptr<Proxied> proxy)
					: live_(std::move(live)), real_(std::move(real)), proxy_(std::move(proxy)) {
					++*live_;
				}
				~Counted() override {
					real_.reset();
					if (proxy_) {
						std::unique_ptr<Timer> armed;
						{
							const std::scoped_lock lock(proxy_->mutex);
							proxy_->cancelled = true;
							armed.swap(proxy_->real);
						}
					}
					--*live_;
				}

			private:
				std::shared_ptr<std::atomic<std::size_t>> live_;
				std::unique_ptr<Timer> real_;
				std::shared_ptr<Proxied> proxy_;
			};
			std::unique_ptr<Timer> arm(std::move_only_function<std::unique_ptr<Timer>(NodeLoop&)> arming) {
				NodeLoop* loop = nullptr;
				{
					const std::scoped_lock lock(mutex_);
					if (!released_) {
						auto proxy = std::make_shared<Proxied>();
						staged_.push_back(Staged{.proxy = proxy, .arm = std::move(arming)});
						return std::make_unique<Counted>(live_, nullptr, std::move(proxy));
					}
					loop = real_;
				}
				if (loop == nullptr) return std::make_unique<Counted>(live_, nullptr, nullptr); // never armed
				return std::make_unique<Counted>(live_, arming(*loop), nullptr);
			}

			Impl& host_; // never the node: an adapter's event may land after it
			std::uint64_t serial_;
			std::uint64_t generation_;
			std::size_t index_;
			std::shared_ptr<Channel> channel_; // the configuration's: what its adapter says lands where it says
			mutable std::mutex mutex_;
			bool released_ = false;     // mutex_: staging ended (released, or dropped by a teardown)
			std::deque<Staged> staged_; // mutex_
			NodeLoop* real_ = nullptr;  // mutex_: made's, once made
			bool stateGone_ = false;    // the loop's thread only, once started: its state was destroyed
			std::shared_ptr<std::atomic<std::size_t>> live_ = std::make_shared<std::atomic<std::size_t>>(0);
		};

		/// The ProgramEvents a configuration's program tells, on the adapter's thread: each crosses through the
		/// configuration's Channel (docs/architecture.md#teardown-by-dependency), so nothing reaches a newer
		/// configuration. A line it says goes to the log sinks at once, as NodeHandle::log does. Owned by its
		/// ProgramSlot, so it outlives the RunningProgram the adapter tells it about.
		class ProgramLink final : public ProgramEvents {
		public:
			explicit ProgramLink(std::shared_ptr<Channel> channel) noexcept : channel_(std::move(channel)) {}
			void started() override { channel_->programStarted(); }
			void ready() override { channel_->programReady(); }
			void refused(std::string why) override { channel_->programRefused(std::move(why)); }
			void fed(std::string bytes) override { channel_->programFed(std::move(bytes)); }
			void said(LogEntry entry) override { channel_->log(std::move(entry)); }
			void exited(ProgramExit exit) override { channel_->programExited(std::move(exit)); }

		private:
			std::shared_ptr<Channel> channel_;
		};

		/// The configuration's one program (docs/writing-a-node-type.md#programs): what configure asked, its readiness
		/// item, and once launched its process and what it told.
		struct ProgramSlot {
			ProgramShape shape;
			std::optional<ProgramFeedTo> feed;
			std::size_t item = detail::noReadiness; // ready once it is (ProgramEvents::ready)
			std::unique_ptr<ProgramLink> events{};  // made at its launch; outlives `running`
			std::unique_ptr<RunningProgram> running{};
			bool started = false;
			bool ended = false; // told refused or exited: nothing of it runs
			bool stopped = false;
			/// Launched and not yet reaped: teardown step 3 waits for it.
			bool open() const noexcept { return running && !ended; }
			std::string name() const { return std::string(shape.executable.name()); }
		};

		/// What a store's jobs carry back to the main loop: the configuration's Completions, and where the node is
		/// found once they land (only while the configuration is attached, so the node is alive then).
		struct StoreLink {
			Impl* host;
			std::uint64_t serial;
			std::uint64_t generation;
			std::size_t index;
			std::shared_ptr<detail::Completions> completions;
			/// The configuration's stores may still be written: false once it was leaked (any thread reads it).
			std::shared_ptr<const std::atomic<bool>> writable;
			/// Any thread (the I/O thread's job): the store adapter threw in `job`'s call `callback`. On the main loop,
			/// while attached: the record, then the node fails lost (docs/architecture.md#faults-by-owner).
			void faulted(std::string job, std::string callback, std::string what) const {
				completions->post([host = host, serial = serial, generation = generation, job = std::move(job),
				                   callback = std::move(callback), what = std::move(what)]() mutable {
					host->inputFor(serial, [generation, job = std::move(job), callback = std::move(callback),
					                        what = std::move(what)](Node& node) mutable {
						node.ioJobFaulted(generation, job, callback, what);
					});
				});
			}
		};
		/// One declared store of a configuration (docs/writing-a-node-type.md#stores): the type's view of it, its state
		/// (shared with its jobs), its readiness item, and the ReadyEvents its open's completion tells on the main loop.
		/// Its open and writes run on the store adapter's I/O executor, each job holding only the state and the
		/// configuration's Completions, never this slot, and complete through those Completions, so nothing of them
		/// lands once the configuration ended.
		class StoreSlot final : public Store, public ReadyEvents {
		public:
			StoreSlot(Node& node, std::shared_ptr<detail::StoreState> state, StoreLink link)
				: node_(node), state_(std::move(state)), link_(std::move(link)) {}
			const std::string& fileName() const noexcept override { return state_->file; }
			std::string text() const override { return state_->text(); }
			StoreWritten replace(std::string text) override {
				assert(!onStrand() &&
				       "a store's replace waits: never from a strand (docs/writing-a-node-type.md#stores)");
				// The main loop never waits on the I/O thread: a hook writes with replaceLater.
				if (onMainLoop())
					throw std::logic_error("store " + state_->file +
					                       ": replace waits, so never on the main loop: a hook uses replaceLater");
				requireOpened("written");
				// 0 pending, 1 running, 2 given up: a job its waiter gave up on before it began never writes.
				auto ticket = std::make_shared<std::atomic<int>>(0);
				auto outcome = std::make_shared<StoreWritten>();
				const JobWait waited = state_->adapter.await(
					"write store " + state_->file,
					[state = state_, link = link_, ticket, outcome, text = std::move(text)]() mutable {
						int pending = 0;
						if (!ticket->compare_exchange_strong(pending, 1)) return;
						*outcome = written(*state, link, std::move(text));
					},
					storeBound);
				switch (waited) {
				case JobWait::ran:
					return std::move(*outcome);
				case JobWait::overdue: {
					const auto seconds =
						std::to_string(std::chrono::duration_cast<std::chrono::seconds>(storeBound).count());
					int pending = 0;
					// Given up before it began: it never runs, so the file is as it was.
					if (ticket->compare_exchange_strong(pending, 2))
						return refused(state_->file + ": not written: not begun within " + seconds + " s");
					return std::unexpected(
						StoreWriteError{StoreWriteError::Kind::unconfirmed,
					                    state_->file + ": not confirmed within " + seconds +
					                        " s: the write was under way and may still land (compare text())"});
				}
				case JobWait::dropped:
					break;
				}
				return refused(state_->file + ": not written: the I/O thread has stopped");
			}
			void replaceLater(std::string text, Loop& loop, std::move_only_function<void(StoreWritten)> done) override {
				requireOpened("written");
				// The loop is reached from the main loop, through the configuration's Completions, never from the I/O
				// thread: a job may outlive the loop, and the Completions drop what lands once the configuration ended.
				state_->adapter.io().post("write store " + state_->file, [state = state_, link = link_, loop = &loop,
				                                                          done = std::move(done),
				                                                          text = std::move(text)]() mutable {
					auto outcome = written(*state, link, std::move(text));
					link.completions->post([loop, done = std::move(done), outcome = std::move(outcome)]() mutable {
						loop->post([done = std::move(done), outcome = std::move(outcome)]() mutable {
							done(std::move(outcome));
						});
					});
				});
			}

			// ReadyEvents, main thread: told by the open's completion.
			void ready() override { node_.storeReady(link_.generation, link_.index); }
			void failed(std::string detail) override {
				node_.storeFailed(link_.generation, link_.index, std::move(detail));
			}

			/// Main thread, at readiness step `opens`: the open, one job on the I/O executor (detail::postIoOpen); any
			/// throw of the adapter's there is its bug.
			void open() {
				detail::postIoOpen(
					state_->adapter.io(), "open store " + state_->file, "release store " + state_->file, state_,
					link_.completions,
					[](detail::StoreState& state) -> detail::IoOpened {
						try {
							return {state.open(), std::nullopt};
						} catch (...) {
							return {
								{},
								detail::IoFault{std::string(state.calling), detail::whatOf(std::current_exception())}};
						}
					},
					[host = link_.host, serial = link_.serial, generation = link_.generation, index = link_.index](
						std::expected<void, std::string> outcome, std::optional<detail::IoFault> fault) {
						host->inputFor(serial, [generation, index, outcome = std::move(outcome),
					                            fault = std::move(fault)](Node& node) mutable {
							node.storeOpened(generation, index, std::move(outcome), std::move(fault));
						});
					});
			}

			std::size_t item = detail::noReadiness; // its readiness item: ready once its open trusted it

		private:
			/// A write on the I/O thread: the adapter's throw is its bug, reported (the node fails lost) and answered as
			/// the failure it gives the node.
			/// The I/O thread's: a write of a configuration leaked since it was asked is dropped, the file left as it is
			/// (docs/architecture.md#teardown-by-dependency); its outcome reaches no one, as its `done` is dropped too.
			static StoreWritten written(detail::StoreState& state, const StoreLink& link, std::string text) {
				if (!link.writable->load(std::memory_order_acquire))
					return refused(state.file + ": not written: its configuration was leaked");
				try {
					auto wrote = state.write(std::move(text));
					if (!wrote) return refused(std::move(wrote.error()));
					return {};
				} catch (...) {
					const std::string what = detail::whatOf(std::current_exception());
					const std::string callback(state.calling);
					link.faulted("write store " + state.file, callback, what);
					return refused(internalErrorDetail(callback, what));
				}
			}
			static StoreWritten refused(std::string detail) {
				return std::unexpected(StoreWriteError{StoreWriteError::Kind::refused, std::move(detail)});
			}
			void requireOpened(std::string_view done) const {
				if (!state_->opened())
					throw std::logic_error("store " + state_->file + " was " + std::string(done) +
					                       " before it was opened");
			}
			Node& node_; // the configuration's slots go before the node
			std::shared_ptr<detail::StoreState> state_;
			StoreLink link_;
		};

		/// One recording's file and what opens it, shared by its slot and its I/O jobs, so the last of them to let go
		/// closes it, on the I/O thread.
		struct RecordingState {
			RecordingAdapter& adapter;
			NodeName node;
			std::string path;
			std::unique_ptr<RecordingFile> file; // the open job's, then read-only
		};

		/// One recording of a configuration (docs/writing-a-node-type.md#recordings): the type's view of it, its claim's
		/// path, its readiness item, and its state (shared with its jobs). Opened at readiness step `opens` as one job
		/// on the recordings adapter's I/O executor (detail::postIoOpen), so nothing of it lands once the configuration
		/// ended; closed on the I/O thread when the slot goes (teardown
		/// step 5, after the sinks that write it), so a later open of the path, posted after, runs after the close.
		class RecordingSlot final : public Recording {
		public:
			/// `refused`: why the path rule refuses it (then `adapter` is null, and nothing is claimed or opened).
			RecordingSlot(std::string path, RecordingAdapter* adapter, const NodeName& node,
			              std::optional<std::string> refused = std::nullopt)
				: path_(std::move(path)), claimPath_(recordingClaimPath(path_)), refused_(std::move(refused)),
				  state_(adapter == nullptr
			                 ? nullptr
			                 : std::make_shared<RecordingState>(RecordingState{*adapter, node, path_, {}})) {}
			RecordingSlot(const RecordingSlot&) = delete;
			RecordingSlot& operator=(const RecordingSlot&) = delete;
			~RecordingSlot() { close(); }
			/// Main thread, once nothing writes it any more (teardown step 5, or a leaked configuration's late close
			/// before it releases its claims): its file's last reference goes to the I/O executor, closed there. Once.
			void close() noexcept {
				if (!state_) return;
				opened_.store(false, std::memory_order_release);
				IoExecutor& io = state_->adapter.io();
				try {
					io.post("close recording " + path_, [state = std::move(state_)]() mutable { state.reset(); });
				} catch (...) { // no memory to post: closed here, the last resort
				}
				state_.reset();
			}
			int descriptor() const override {
				if (!opened_.load(std::memory_order_acquire))
					throw std::logic_error("recording " + path_ + "'s descriptor was read while it was not open");
				return state_->file->descriptor();
			}
			const std::string& path() const noexcept override { return path_; }
			const std::string& claimPath() const noexcept { return claimPath_; }
			const std::optional<std::string>& refused() const noexcept { return refused_; }

			/// Main thread, at readiness step `opens`: the open, one job on the I/O executor (detail::postIoOpen). The
			/// adapter's std::logic_error, or no file made, is its bug; any other throw its refusal, as an error is.
			void open(Impl* host, std::uint64_t serial, std::uint64_t generation, std::size_t index,
			          std::shared_ptr<detail::Completions> completions) {
				detail::postIoOpen(
					state_->adapter.io(), "open recording " + path_, "release recording " + path_, state_,
					std::move(completions),
					[](RecordingState& state) -> detail::IoOpened {
						constexpr const char* callback = "RecordingAdapter::open";
						try {
							auto file = state.adapter.open(state.node, state.path);
							if (!file) return {std::unexpected(std::move(file.error())), std::nullopt};
							if (*file == nullptr)
								return {{}, detail::IoFault{callback, "the recording adapter opened no file"}};
							state.file = std::move(*file);
							return {};
						} catch (const std::logic_error&) {
							return {{}, detail::IoFault{callback, detail::whatOf(std::current_exception())}};
						} catch (...) {
							return {std::unexpected(detail::whatOf(std::current_exception())), std::nullopt};
						}
					},
					[host, serial, generation, index, id = id](std::expected<void, std::string> outcome,
				                                               std::optional<detail::IoFault> fault) {
						host->inputFor(serial, [generation, index, id, outcome = std::move(outcome),
					                            fault = std::move(fault)](Node& node) mutable {
							node.recordingOpened(generation, index, id, std::move(outcome), std::move(fault));
						});
					});
			}
			/// Main thread: its open landed and made the file; the descriptor may be read from now on.
			void opened() noexcept { opened_.store(true, std::memory_order_release); }

			std::size_t item = detail::noReadiness; // its readiness item: ready once its open made the file
			std::uint64_t id = 0;  // a change's replacement's, by which its open finds it; 0 for creation's
			bool awaiting = false; // a recording a change replaced, until its commit completed
			bool due = false;      // then: it closes once no sink the change replaced writes it

		private:
			const std::string path_;      // as asked
			const std::string claimPath_; // its claim's value
			const std::optional<std::string> refused_;
			std::shared_ptr<RecordingState> state_;
			std::atomic<bool> opened_{false};
		};

		/// One claim of a configuration (docs/writing-a-node-type.md#claims): its own (cx.claim), a store's file, a
		/// recording's path, or an engine companion's (claimsOf), once each.
		struct ClaimSlot {
			Claim claim;
			std::size_t item = detail::noReadiness; // its readiness item: ready once granted (and probed, for a port)
			std::string waitingOn{};                // the node whose closing configuration it waits for
			bool anyTcp = false;                    // an AnyTcp claim: its port (claim.value) picked at step claims
		};

		/// One engine companion a configuration asked for, until its block is ready: its shape, and where it goes, the
		/// inside bank it joins named in it (one companion per bank).
		struct EngineAsk {
			EngineCompanionShape shape;
			CompanionTarget target;
			std::size_t item = detail::noReadiness; // its readiness item: ready once the companion is
		};

		/// One companion a configuration asked for, in request order: which of its lists holds it, and where.
		struct Asked {
			enum class Family { engine, sink, source };
			Family family;
			std::size_t index;
		};

		/// One declareLatency: the input bank its path starts at and the output bank it ends at.
		struct DeclaredLatency {
			std::string from;
			std::string bank;
			Latency latency;
		};

		/// One configuration (docs/architecture.md#the-node-lifecycle,
		/// docs/writing-a-node-type.md#configure-and-createof), from configure to the end of its close. Members in
		/// reverse teardown order.
		struct Configuration {
			std::uint64_t generation = 0;
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			std::unique_ptr<Context> context;
			// Each declared store: a loop's state, a sink's consumer or the instance may hold it, so it goes after all
			// of them (teardown step 5); its jobs hold only its state.
			std::vector<std::unique_ptr<StoreSlot>> stores;
			// Cleared when the configuration is leaked: a write of its stores that has not begun is dropped, so a leaked
			// configuration's thread never replaces what a newer configuration of the node keeps in the file.
			std::shared_ptr<std::atomic<bool>> storesWritable = std::make_shared<std::atomic<bool>>(true);
			// Each recording (cx.recording): outlives the sinks whose consumers write it and the instance that may hold
			// it (file's WavWriter closes its SNDFILE on the descriptor when freed); closed on the I/O thread when freed.
			std::vector<std::unique_ptr<RecordingSlot>> recordings;
			// Each recording a change replaced (closed once its commit completed and every sink it replaced finished) or
			// a replacement it dropped (closed at once): kept, closed, until step 5, so a Recording& the type holds stays
			// valid (its descriptor refused once closed).
			std::vector<std::unique_ptr<RecordingSlot>> retiredRecordings;
			// Each sink's consumer may write a recording above, and may hold a reference into the instance: freed after
			// the code and before the instance (teardown step 5).
			std::vector<std::unique_ptr<SinkSlot>> sinks;
			std::vector<SinkBinding> bindings; // every sink's, built at the return checks; every drive points at it
			// Each sink a change replaced, from its commit point (or a replacement a change ending uncommitted dropped):
			// drained to its own consumer and finished, then freed; teardown step 4 waits for each as for a sink, but
			// none counts in sinksClosed (docs/writing-a-node-type.md#sinks-and-sources-in-a-change).
			std::vector<std::unique_ptr<SinkSlot>> retiredSinks;
			// Each source's writer may be held by the instance's threads; freed after the instance (teardown step 5).
			std::vector<SourceSlot> sources;
			std::vector<SourceBinding> sourceBindings; // every source's, built at the return checks, likewise
			// Each source a change replaced, its writer closed: kept until step 5 with the sources, since a thread of the
			// instance may still call its writer.
			std::vector<SourceSlot> retiredSources;
			// Each declared loop, its state and its adapter's loop: a state may hold a source's writer, so they go first;
			// the instance only posts to them, so they go after it.
			std::vector<std::unique_ptr<LoopSlot>> loops;
			std::unique_ptr<ErasedInstance> instance;
			std::unique_ptr<AudioCode> code;   // may read the instance, so goes first
			std::vector<EngineAsk> engineAsks; // in request order
			std::vector<std::unique_ptr<Companion>> engineCompanions;
			/// An engine companion a make-first replacement was made beside: still running, listed retiring, until the
			/// commit point destroys it (or, the change ending uncommitted, it is the bank's companion again).
			struct RetiringCompanion {
				std::size_t index;
				EngineAsk ask;
				std::unique_ptr<Companion> companion;
			};
			std::vector<RetiringCompanion> retiring;
			// Its one program: destroyed after the instance and before the claims are released (teardown step 5), so a
			// port it held is released only once it was reaped.
			std::optional<ProgramSlot> program;
			std::vector<ClaimSlot> claims;   // every claim, judged at readiness step `claims`; released at step 5
			detail::ReadinessList readiness; // every engine ask's item, then every sink's, from configure's return
			std::vector<Asked> requestOrder; // every companion, as asked: the companions' list's order
			/// Whose each readiness item is, by item: a claim's, a store's, a recording's, a loop's, an engine companion's or a sink
			/// consumer's, and its index.
			struct ItemOwner {
				enum class Family { claim, store, recording, loop, engine, consumer, program } family;
				std::size_t index;
			};
			std::vector<ItemOwner> itemOwners;
			std::unique_ptr<Timer> deadline;
			std::unique_ptr<Timer> bound; // the request in flight's bound: the fade's or the barrier's
			std::chrono::steady_clock::time_point postedAt;   // when that request was posted
			std::chrono::steady_clock::time_point closeBegan; // when teardown step 3 began, for ear.close-late
			// One flag per output in process()'s order (outputs, then inside outputs): every one the code writes is
			// faded but a sink's bank. Read by the block until the drive's invoke ran.
			std::unique_ptr<bool[]> faded;
			std::size_t fadedCount = 0;
			/// The ports its code is laid out for, whose order the block hands its spans in: the node's at configure, a
			/// reshape in place's from its commit point (docs/architecture.md#reshapes).
			Ports ports;
			std::vector<BankPair> pairs;          // the pass-through's pairs, for the latency plan
			std::vector<DeclaredLatency> latency; // what configure declared, one per path
			LatencyPlan plan;                     // worked out at configure's return; carried with each drive
			bool startFailed = false;     // configure failed, threw or broke a rule: nothing to make, prepare or drive
			bool companionsAsked = false; // its engine companions' making is queued
			bool readyTold = false;       // T3: the machine was told every resource is ready
			bool prepared = false;
			bool tearing = false;       // teardown step 3 began
			bool ending = false;        // teardown step 1 began: its companions' list is empty from here
			bool loopsReleased = false; // configured: what was staged on its loops was released
			/// Teardown step 4's loops: every loop made is finished, then closed.
			enum class LoopsStep { open, finishing, closing, closed } loopsStep = LoopsStep::open;
			bool closedTold = false; // step 4 is done: the machine was told closed
			std::size_t sinksClosed = 0;
			/// A change in place in flight (docs/architecture.md#changes-in-place): what its change version asked, until
			/// its commit point or its end. Its holds are the configuration's (held), kept across a supersession.
			struct Change {
				std::shared_ptr<const NodeConfig> target; // the settings it applies, kept alive until the commit
				std::uint64_t epoch = 0;                  // this change's, so a late timer or answer finds it gone
				bool reshape = false;          // its ports differ from the configuration's: a reshape in place
				std::vector<std::string> keys; // the keys it applies, for its diagnostics
				std::chrono::steady_clock::time_point began;
				std::vector<std::pair<std::size_t, std::move_only_function<void(void*)>>> posts; // staged, in order
				std::unique_ptr<AudioCode> code; // a new code, prepared when the old one is (swapped at the commit)
				std::vector<BankPair> pairs;     // the new code's pass-through pairs, if it is one
				bool codeGiven = false;
				bool codePrepared = false;
				std::optional<std::vector<DeclaredLatency>> latency; // declared in the change: the new paths
				LatencyPlan plan;                                    // the plan from the commit point
				bool answered = false;                               // the change version answered applied
				std::unique_ptr<Timer> deadline;
				std::unique_ptr<Timer> holdBound; // the holds' acknowledgement's 100 ms
				bool holdGaveUp = false;          // past it: the change went on
				/// An engine companion the change replaces, break-first behind a hold: its claims judged first, then
				/// (once held) the old one destroyed and the new one made in one flush.
				struct Replacement {
					std::size_t index;            // the engine ask, and companion, it replaces
					EngineCompanionShape shape;   // the new one's
					std::vector<Claim> claims;    // the new one's, by its shape's rule
					std::uint64_t retiringId = 0; // the companion it replaces
					std::uint64_t id = 0;         // its own, once made
					std::vector<Claim> oldClaims; // the replaced one's, by its shape's rule (once broken or made)
					/// Made before break (docs/writing-a-node-type.md#make-before-break): the new one made beside the old
					/// one, which goes at the commit point; else break-first.
					bool makeFirst = false;
					bool broken = false; // break-first: the old one destroyed, the new one asked for
					bool made = false;   // make-first: the new one asked for beside the old one, which is retiring
					bool ready = false;
				};
				std::vector<Replacement> replacements;
				/// A sink the change replaces (docs/writing-a-node-type.md#sinks-and-sources-in-a-change): made beside the
				/// old one, its consumer begun once the change's claims and recordings are ready; the tap moves to it in
				/// the commit's invoke.
				struct SinkReplacement {
					std::size_t index; // the sink it replaces
					std::unique_ptr<SinkSlot> slot;
					bool asked = false; // its consumer asked to begin
					bool ready = false; // its consumer began
				};
				std::vector<SinkReplacement> sinks;
				/// A source the change replaces: made beside the old one, ready at once, played from the commit's invoke.
				struct SourceReplacement {
					std::size_t index; // the source it replaces
					SourceSlot slot;
				};
				std::vector<SourceReplacement> sources;
				/// A recording the change replaces (docs/writing-a-node-type.md#recordings-in-a-change): claimed with the
				/// change's claims, then opened beside the old one, before any companion of the change.
				struct RecordingReplacement {
					std::size_t index; // the recording it replaces
					std::unique_ptr<RecordingSlot> slot;
					bool asked = false;  // its open posted
					bool opened = false; // its open made the file
				};
				std::vector<RecordingReplacement> recordings;
				/// docs/writing-a-node-type.md#claims-in-a-change: the ports cx.claim asked (each once), and those it
				/// unclaimed (keys the configuration holds, released at the commit point, last).
				std::vector<Claim> claimed;
				std::vector<std::string> unclaimed;
				/// Every claim of the change the configuration does not hold yet (cx.claim's and the replacements'),
				/// judged when the change version returned, before anything else is acquired; granted once probed.
				struct Added {
					Claim claim;
					bool granted = false;
				};
				std::vector<Added> added;
				std::size_t claimsPending = 0;
				/// whenReady's continuation (docs/writing-a-node-type.md#whenready-and-configuredone): held here until
				/// every acquisition is ready, then posted to its loop; the commit point waits for its done().
				std::optional<std::move_only_function<void(void*, ConfigureDone)>> continuation;
				std::size_t continuationLoop = 0;
				bool continuationPosted = false;
				bool continuationDone = false;
			};
			std::optional<Change> change;
			std::uint64_t changeEpoch = 0;
			std::vector<std::uint8_t> held; // per output (outputs then inside outputs): held by a change, until it ends
			std::uint64_t holdsAsked = 0;   // holds posted to the block, and those it acknowledged
			std::uint64_t holdsAcked = 0;
			std::unique_ptr<AudioCode> retiringCode; // the code a change's swap replaces, until the swap completed
			std::vector<std::uint64_t> retiredIds;   // the engine companions a change replaced: their losses are said
			// Per engine companion: the id of the replacement a break last asked for, which its flush item makes. A newer
			// change that breaks before an older break's flush ran asks for its own; the older item then makes nothing.
			std::vector<std::uint64_t> replacing;
			bool retiringPrepared = false; // and whether it was prepared

			Configuration() = default;
			Configuration(const Configuration&) = delete;
			Configuration& operator=(const Configuration&) = delete;
			~Configuration() { freeInOrder(); }
			/// Teardown step 5's order, its one statement (docs/architecture.md#teardown-by-dependency): the code
			/// (released and unreachable since the barrier; it may hold references into the instance, never the
			/// reverse), then the sinks (each closed, so its consumer, which may hold a reference into the instance or
			/// be part of a loop's state, is idle), then the states of loops that never started (which may read the
			/// instance too), every state before any loop, since one may hold another's executor, then the instance,
			/// the loops, the sources (after the instance, whose threads may hold a writer), the stores (after every
			/// state and consumer that may hold one; a job still queued holds its state), the recordings and the
			/// program (reaped at step 3, and before its claims go). The rest goes with the config. Again is nothing.
			///
			/// Every thread of it has closed by then, but for a configuration freed with its node: a host destroyed
			/// while the node was still configured, which only a test does (~NodeHost wants stopAll completed). That
			/// one was never torn down, so each loop still up is destroyed, which ends its thread, before any state
			/// that thread may be in; its sinks' threads end where the sinks stand in the order, before the states.
			void freeInOrder() noexcept {
				change.reset();
				retiringCode.reset();
				code.reset();
				sinks.clear();
				retiredSinks.clear();
				if (loopsStep != LoopsStep::closed)
					for (const auto& slot : loops) slot->made.reset();
				for (const auto& slot : loops) slot->state.reset();
				instance.reset();
				loops.clear();
				sources.clear();
				retiredSources.clear();
				stores.clear();
				recordings.clear();
				retiredRecordings.clear();
				program.reset();
			}
		};

		/// A configuration leaked at the close deadline (docs/architecture.md#teardown-by-dependency): not freed while its
		/// threads may still touch any of it, and still listening. Its Channel is re-pointed here, so every close it
		/// still reports (a sink's, a loop's finish and close) lands here in order on the
		/// main loop, and the teardown carries on from where it stopped: once the sinks closed, the loops
		/// finish and close. Once everything reported, ear.close-late is said, the
		/// Channel is detached, the configuration no longer counts in leaked(), and a later turn frees it. A consumer's throw is an `ear` log
		/// record naming the node, never a fact: nothing here reaches the node or a newer configuration. Until then
		/// NodeHost reaches it by the configuration's Channel alone (the Impl outlives it but on an exit through _Exit).
		class LeakedRecord {
		public:
			LeakedRecord(Impl& host, NodeName name, std::uint64_t serial, std::unique_ptr<Configuration> config)
				: host_(host), name_(std::move(name)), serial_(serial), generation_(config->generation),
				  config_(std::move(config)) {}
			/// Main thread, once, as it is leaked: its closes land here from now on.
			void repoint() noexcept { config_->channel->repoint(*this); }
			void sinkClosed(std::size_t index) {
				host_.input([this, index] {
					Configuration& c = *config_;
					if (index >= c.sinks.size() || c.sinks[index]->closed) return;
					c.sinks[index]->closed = true;
					++c.sinksClosed;
					carryOn();
				});
			}
			void retiredSinkClosed(std::uint64_t id) {
				host_.input([this, id] {
					for (auto& slot : config_->retiredSinks)
						if (slot->id == id) slot->closed = true;
					carryOn();
				});
			}
			void loopFinished(std::size_t index) {
				host_.input([this, index] { loopFinishedIn(*config_, index); });
			}
			void loopClosed(std::size_t index) {
				host_.input([this, index] {
					if (loopClosedIn(host_, serial_, name_, *config_, index)) carryOn();
				});
			}
			/// Its program was reaped (or refused): one of the closes its late close waits for.
			void programEnded() {
				host_.input([this] {
					if (!config_->program || config_->program->ended) return;
					config_->program->ended = true;
					carryOn();
				});
			}
			void threw(std::string detail) {
				host_.logged(LogEntry{LogLevel::error, "ear", std::move(detail), {}, name_});
			}

		private:
			/// As closeLoops: once every sink closed and the program was reaped, the loops; once they closed, it has
			/// closed.
			void carryOn() {
				Configuration& c = *config_;
				if (closed_ || c.sinksClosed != c.sinks.size() || !retiredSinksClosed(c) ||
				    (c.program && c.program->open()))
					return;
				if (c.loopsStep == Configuration::LoopsStep::open) finishLoopsOf(c);
				if (c.loopsStep != Configuration::LoopsStep::closed) return;
				closedLate();
			}
			/// Everything it held reported: its recordings' closes posted to the I/O thread first, so they come
			/// before any open of their paths that releasing its claims allows; its claims released and said, no
			/// longer counted, and each node its claims refused started again, once; then freed in a turn of its
			/// own (never inside the close that called this), as teardown step 5 frees a configuration that closed in
			/// time.
			void closedLate() {
				if (closed_) return;
				closed_ = true;
				config_->channel->detach();
				--host_.leaked;
				for (const auto& recording : config_->recordings) recording->close();
				for (const auto& recording : config_->retiredRecordings) recording->close();
				const bool held = host_.claims.holds(generation_);
				host_.claimsDecided(host_.claims.release(generation_));
				const auto refused = host_.refusedByLeaked.extract(generation_);
				const std::int64_t ms = millisecondsBetween(config_->closeBegan, host_.timers.now());
				const std::array fields{DiagnosticField{"ms", ms}};
				host_.diagnosed(serial_, name_,
				                {.code = "ear.close-late",
				                 .detail = "the configuration leaked at the close deadline closed " +
				                           std::to_string(ms) + " ms after its close began" +
				                           (held ? "; its claims are released" : ""),
				                 .fields = {fields.begin(), fields.end()}});
				if (refused)
					for (const auto& [serial, generation] : refused.mapped())
						host_.inputFor(serial, [generation](Node& node) { node.restartRefused(generation); });
				host_.mainLoop.post([record = this, alive = std::weak_ptr<char>(host_.alive)] {
					if (!alive.lock()) return; // the host is gone: so is what the record would free into
					record->free();
					delete record;
				});
			}
			/// Teardown step 5 (Configuration::freeInOrder): every thread of it has reported closed, so nothing reaches
			/// what goes here (a dedicated loop's thread has returned; its program was reaped, its claims released
			/// when it closed late). Its Channel may outlive it, held by a type's thread, detached.
			void free() {
				const MainLoopActivity::Scope activity("free configuration", name_);
				config_.reset();
			}

			Impl& host_;
			const NodeName name_;            // a copy: the node may be gone
			const std::uint64_t serial_;     // its node's, which keeps its diagnostics while it is not gone
			const std::uint64_t generation_; // its claims' holder
			std::unique_ptr<Configuration> config_;
			bool closed_ = false;
		};

		/// The newest committed settings, and whether a drive of them may fade: some output the code writes exists
		/// (narrowed at configure's return, once its sinks' banks are known: outputsFaded).
		lc::Queued queued() const noexcept {
			const Ports& ports = queued_ ? queued_->ports() : ports_;
			return {version_, !ports.outputs().empty() || !ports.insideOutputs().empty()};
		}

		/// docs/architecture.md#reshapes: with no configuration, or at a configuration's start (the block silent), the
		/// node takes the ports of its newest settings, and its block is reshaped to them (its links on the ports both
		/// name kept). A block not made yet is made with them; none is reshaped once the data loop is wedged.
		void adoptPorts() {
			// A configuration of other settings still holds the block's ports: it adopts them at its own start.
			if (!queued_ || (config_ && config_->config != queued_)) return;
			ports_ = queued_->ports();
			reshapeBlock();
		}

		/// The block takes the node's ports, when it was made or last reshaped with others.
		void reshapeBlock() {
			if (!block_ || host_.wedged || blockPorts_ == ports_) return;
			blockPorts_ = ports_;
			block_->reshape(ports_);
		}

		/// The configuration of this generation is the current one.
		bool current(std::uint64_t generation) const noexcept {
			return config_ != nullptr && config_->generation == generation;
		}

		/// The health the aggregate derived, with what the node waits for as last shown.
		NodeHealth composed(const lc::Health& health) const {
			return NodeHealth{name_,
			                  health.state,
			                  health.reason,
			                  health.phase,
			                  health.wanted,
			                  health.cause,
			                  health.phase == wantedPhase_ ? waitingShown_ : std::vector<WaitingFor>{},
			                  health.blockLost,
			                  superseded_,
			                  health.keys,
			                  health.next};
		}

		/// What the node waits for now, unpaced: an older node of its name; its engine companions
		/// while configuring (the block first, since they are made only once it is ready); the block while starting.
		std::vector<WaitingFor> waitingNow() const {
			std::vector<WaitingFor> waits;
			switch (aggregate_.health().phase) {
			case lc::NodePhase::added:
				if (aggregate_.waitingForOlder()) waits.push_back({WaitingFor::Kind::olderNode, name_});
				break;
			case lc::NodePhase::configuring:
				if (!config_ || config_->startFailed) break;
				for (WaitingFor wait : config_->readiness.waiting()) {
					// An engine companion is made only once the block is ready, so until then it waits for the block.
					if (wait.kind == WaitingFor::Kind::companion && aggregate_.block().state() != lc::BlockState::ready)
						wait = {WaitingFor::Kind::block};
					if (std::ranges::find(waits, wait) == waits.end()) waits.push_back(std::move(wait));
				}
				break;
			case lc::NodePhase::starting:
				if (aggregate_.block().state() != lc::BlockState::ready) waits.push_back({WaitingFor::Kind::block});
				break;
			default:
				break;
			}
			return waits;
		}

		/// After an input: once the phase has lasted 2 s and no 1 s pace runs, a changed list is shown at once and the
		/// pace begins; while it runs, the newest list waits for it.
		void refreshWaiting() {
			if (!waitingAllowed_ || paceTimer_ || removed()) return;
			std::vector<WaitingFor> now = waitingNow();
			if (now == waitingShown_) return;
			waitingShown_ = std::move(now);
			pace();
			tell(aggregate_.health());
		}

		/// The 1 s pace: when it ends the newest list is shown if it changed, and the pace begins again.
		void pace() {
			paceTimer_ = host_.timers.after(waitingForPace, [&host = host_, serial = serial_] {
				host.inputFor(serial, [](Node& node) {
					node.paceTimer_.reset();
					node.refreshWaiting();
				});
			});
		}

		/// A phase began (or the node was first told): its waits are hidden for 2 s.
		void phaseBegan(NodePhase phase) {
			wantedPhase_ = phase;
			waitingShown_.clear();
			waitingAllowed_ = false;
			paceTimer_.reset();
			phaseTimer_.reset();
			if (!waitingPhase(phase) || host_.wedged) return;
			phaseTimer_ = host_.timers.after(waitingForDelay, [&host = host_, serial = serial_, phase] {
				host.inputFor(serial, [phase](Node& node) {
					node.phaseTimer_.reset();
					if (node.wantedPhase_ != phase) return;
					node.waitingAllowed_ = true;
					node.refreshWaiting();
				});
			});
		}

		/// Carries out what the aggregate returned, in order. An input an action causes (a
		/// resource ready at once) is queued behind them, never handled inside.
		void feed(lc::Actions actions) {
			// A removal that ends in its own input (nothing was configured, or its configuration was already gone) is
			// told gone without a stopping before it; every removal says stopping first.
			if (std::ranges::any_of(actions,
			                        [](const auto& action) { return std::holds_alternative<lc::Gone>(action); }))
				stopping();
			for (const lc::NodeAction& action : actions) {
				// A start() that threw never reached running: the running its batch would tell is not told.
				if (startThrew_ && std::holds_alternative<lc::Tell>(action) &&
				    std::get<lc::Tell>(action).health.phase == NodePhase::running)
					continue;
				std::visit(Overloaded{
							   [&](lc::BlockAction block) { act(block); },
							   [&](const lc::ConfigAction& config) { act(config); },
							   [&](const lc::Tell& told) { tell(told.health); },
							   [&](lc::Gone) {
								   goneHeld_ = true;
								   tellGone();
							   },
							   [&](const lc::Dropped& dropped) { recordDropped(dropped); },
						   },
				           action);
			}
			// A teardown asked while the drive is in flight (arming, stopAsked) has begun: from here on nothing the
			// configuration's threads tell is a fact, as from its silence on.
			if (config_ && aggregate_.configuration().state() == lc::ConfigState::arming &&
			    aggregate_.configuration().stopAsked() && aggregate_.intent() != lc::Intent::stop &&
			    aggregate_.intent() != lc::Intent::none) {
				config_->channel->closing();
				claimWaitsEnded();
			}
			// A hook that threw is a fact once its batch was carried out
			// (docs/writing-a-node-type.md#rules-the-framework-enforces).
			if (auto threw = std::exchange(prepareThrew_, std::nullopt))
				feed(aggregate_.fault(lc::Fact::hookThrew, std::move(*threw)));
			if (auto threw = std::exchange(startThrew_, std::nullopt)) feed(aggregate_.startThrew(std::move(*threw)));
			if (auto threw = std::exchange(stopThrew_, std::nullopt))
				feed(aggregate_.fault(lc::Fact::hookThrew, std::move(*threw)));
			refreshWaiting();
			// After the batch's health: a configure's list follows its configuring, a teardown's empty list the state
			// that began it (docs/engine.md#companions-in-a-nodes-list).
			refreshCompanions();
		}

		/// Removed (the aggregate's Gone): told gone once its block's destruction ran in the flush, not when queued.
		void tellGone() {
			if (!goneHeld_ || gone_ || destroyPending_) return;
			host_.gone(*this, dropped_);
		}

		void tell(const lc::Health& health) {
			if (health.phase != wantedPhase_ || !told_) phaseBegan(health.phase);
			tell(composed(health));
		}

		void tell(NodeHealth now) {
			if (told_ && *told_ == now) return;
			told_ = now;
			host_.changed(*told_, superseded_);
		}

		/// A removal's stopping (removing), told before anything it does.
		void stopping() {
			lc::Health health = aggregate_.health();
			health.phase = NodePhase::stopping;
			health.state = NodeState::stopping;
			health.cause = lc::NodeCause{lc::NodeCause::Kind::removing, std::nullopt, {}};
			health.reason.reset();
			tell(health);
		}

		void act(lc::BlockAction action) {
			switch (action) {
			case lc::BlockAction::make:
				makePending_ = true;
				host_.queue(FlushKind::makeBlock, name_, [&host = host_, serial = serial_] {
					if (Node* node = host.find(serial)) node->makeBlock();
				});
				break;
			case lc::BlockAction::destroy:
				destroyPending_ = true;
				host_.queue(FlushKind::destroyBlock, name_, [&host = host_, serial = serial_] {
					if (Node* node = host.find(serial)) node->destroyBlock();
				});
				break;
			}
		}

		void act(const lc::ConfigAction& action) {
			using Kind = lc::ConfigAction::Kind;
			switch (action.kind) {
			case Kind::configure:
				configure();
				break;
			case Kind::prepare:
				prepare();
				break;
			case Kind::drive:
				drive(action.barrier);
				break;
			case Kind::start:
				startHook();
				break;
			case Kind::stop:
				stopHook();
				break;
			case Kind::fade:
				fade(action.barrier);
				break;
			case Kind::silence:
				silence(action.barrier);
				break;
			case Kind::release:
				release();
				break;
			case Kind::teardown:
				teardown();
				break;
			case Kind::free:
				freeConfiguration();
				break;
			case Kind::closeOverdue:
				closeOverdue();
				break;
			case Kind::leak:
				leak();
				break;
			case Kind::fadeUnacknowledged:
				if (!synthetic_) {
					const std::array fields{DiagnosticField{"ms", judgedMs_}};
					diagnose({.code = "ear.fade-unacknowledged",
					          .detail = "the fade before its silence was not acknowledged within " +
					                    std::to_string(judgedMs_) + " ms, so the silence was taken unfaded",
					          .fields = {fields.begin(), fields.end()}});
				}
				break;
			case Kind::barrierOverdue:
				if (!synthetic_) {
					const std::array fields{DiagnosticField{"ms", judgedMs_}};
					diagnose({.code = "ear.barrier-overdue",
					          .detail = "a barrier posted to the data loop did not complete within " +
					                    std::to_string(judgedMs_) + " ms, so its configuration and block are leaked",
					          .fields = {fields.begin(), fields.end()}});
				}
				break;
			case Kind::markWedged:
				host_.markWedged(name_, judgedMs_);
				break;
			case Kind::reconfigure:
				reconfigure();
				break;
			case Kind::commit:
				commit(action.barrier);
				break;
			case Kind::dropChange:
				dropChange();
				break;
			}
		}

		/// Records an engine companion to make once the block is ready, under its one name, on a bank of its own.
		template <class Shape> void askEngineCompanion(Shape shape, const Ports::Bank& bank) {
			given(host_.resources.adapterFor(shape), engineKindWords(shape));
			// A pipe receiver's FIFO is a runtime file of the configuration's (runtimePath), so it needs the folder.
			std::string fifo;
			if constexpr (std::is_same_v<Shape, PipeReceiverShape>) {
				if (host_.resources.runtimeFolder().empty())
					adapter("ResourceRegistry",
					        []() -> void { throw std::runtime_error("no runtime folder in this host"); });
				fifo = runtimePath(host_.resources.runtimeFolder(), name_, config_->generation, bank.name + ".fifo");
			}
			const NodeName engineNode = name_ + "." + std::string(engineSuffix(shape, bank.direction));
			requireOneName(engineNode);
			requireFreeBank(bank);
			// What it holds ear-wide, by its shape's rule, so no shape's field is named here.
			for (Claim& claim : claimsOf(shape)) addPortClaim(std::move(claim));
			config_->engineAsks.push_back(EngineAsk{std::move(shape), CompanionTarget{.engineNode = engineNode,
			                                                                          .blockNode = name_,
			                                                                          .bank = bank.direction,
			                                                                          .blockPorts = bank.ports,
			                                                                          .format = host_.format,
			                                                                          .insideBank = bank.name,
			                                                                          .fifo = std::move(fifo)}});
			config_->requestOrder.push_back({Asked::Family::engine, config_->engineAsks.size() - 1});
		}

		/// One engine name per resource of a configuration: an engine companion's "<node>.<suffix>" and a sink's or
		/// source's "<node>.<bank>" (a sink's thread's name) are never two of one name.
		void requireOneName(const NodeName& engineNode) const {
			const bool taken =
				std::ranges::any_of(config_->engineAsks,
			                        [&](const EngineAsk& asked) { return asked.target.engineNode == engineNode; }) ||
				std::ranges::any_of(config_->sinks,
			                        [&](const auto& slot) { return name_ + "." + slot->bank == engineNode; }) ||
				std::ranges::any_of(config_->sources,
			                        [&](const auto& slot) { return name_ + "." + slot.bank == engineNode; });
			if (taken) throw std::logic_error("a second resource named " + engineNode + " in one configuration");
		}

		/// One companion per inside bank, whatever its kind (an engine companion, a sink, a source), so the block's
		/// storage for bindings, one per inside bank, always holds them all.
		void requireFreeBank(const Ports::Bank& bank) const {
			const bool taken =
				std::ranges::any_of(config_->engineAsks,
			                        [&](const EngineAsk& asked) { return asked.target.insideBank == bank.name; }) ||
				std::ranges::any_of(config_->sinks, [&](const auto& slot) { return slot->bank == bank.name; }) ||
				std::ranges::any_of(config_->sources, [&](const auto& slot) { return slot.bank == bank.name; });
			if (taken) throw std::logic_error("a second companion on bank " + bank.name);
		}

		/// A declaration is keyed by its path, so a code with several inputs into one output (a mixer) declares each
		/// path once.
		void recordLatency(const std::string& bank, Latency latency, std::string from) {
			if (std::ranges::any_of(config_->latency, [&](const DeclaredLatency& declared) {
					return declared.bank == bank && declared.from == from;
				}))
				throw std::logic_error("latency declared twice for the path " + from + " to " + bank);
			config_->latency.push_back(DeclaredLatency{std::move(from), bank, latency});
		}

		/// The configuration's latency plan, by role (docs/engine.md#latency-per-bank-pair). What each source's ring
		/// may hold is latency beyond its bank: the most its frames wait before they play; and a module-run
		/// converter's buffer (moduleRunDelay), which its ports report as zero. A declaration (from, to) is a
		/// path of its own: its pair is carried (added for a code of the type's own; a pass-through must already copy
		/// it, checked at configure's return) with the delay on it, both ways.
		LatencyPlan latencyPlan(const Configuration& c) const {
			return latencyPlan(c, c.pairs, c.latency, c.engineAsks);
		}
		/// The same, with these pairs, declarations and engine companions (a change's, from its commit point).
		LatencyPlan latencyPlan(const Configuration& c, const std::vector<BankPair>& pairs,
		                        const std::vector<DeclaredLatency>& latency, const std::vector<EngineAsk>& asks) const {
			LatencyPlan plan{.pairs = pairs};
			for (const SourceSlot& slot : c.sources)
				plan.beyond.emplace_back(slot.bank, Delay{Latency{0, slot.source->writer().capacityFrames()}});
			for (const EngineAsk& asked : asks)
				std::visit(
					[&]<class Shape>(const Shape& shape) {
						if constexpr (ModuleRunShape<Shape>)
							plan.beyond.emplace_back(asked.target.insideBank,
						                             moduleRunDelay(shape, FrameClock(host_.format.rate)));
					},
					asked.shape);
			for (const DeclaredLatency& declared : latency) {
				const BankPair pair{declared.from, declared.bank};
				if (std::ranges::find(plan.pairs, pair) == plan.pairs.end()) plan.pairs.push_back(pair);
				plan.onPair.emplace_back(pair, Delay{declared.latency});
			}
			return plan;
		}

		/// Every context call is valid only inside its configure.
		void requireConfiguring(std::string_view call) const {
			if (!configuring_) throw std::logic_error(std::string(call) + " outside configure");
		}

		/// Declared loop `loop`, from inside a context call; a host with no loops adapter refuses it.
		LoopSlot& loopSlot(std::string_view call, std::size_t loop) {
			requireConfiguring(call);
			given(host_.resources.loops(), "loops");
			return *config_->loops.at(loop);
		}

		/// Every call NodeHost makes into a type's code on the main loop: named in MainLoopActivity while it runs, timed
		/// when the hook is (one past hookSlowBound is ear.hook-slow, once per node and hook for the node's life,
		/// whether or not it threw), and caught. What it returned, or what ended it instead, by owner
		/// (docs/architecture.md#faults-by-owner): a fault of ear's own a context call recorded first, even one the
		/// type caught: an adapter's is told as ear.internal-error and returned as adapterFaulted, and NodeHost's own
		/// is rethrown as what was thrown, from inside this call's scope, so the callback boundary names it and where;
		/// then a rule a context call found broken while it ran (first), even one the type caught; then the type's
		/// throw (docs/writing-a-node-type.md#rules-the-framework-enforces).
		template <typename Call> auto callType(const TypeHook& hook, Call&& call)
			-> std::expected<std::invoke_result_t<Call>, TypeFault> {
			const MainLoopActivity::Scope activity(hook.activity, name_);
			brokenRule_.reset();
			earFault_.reset();
			refused_ = nullptr;
			const auto began = host_.timers.now();
			auto result = caughtFrom(std::forward<Call>(call));
			if (hook.timed) {
				const std::int64_t ms = millisecondsBetween(began, host_.timers.now());
				if (std::chrono::milliseconds(ms) > hookSlowBound && hooksReportedSlow_.insert(hook.name).second) {
					const std::array fields{DiagnosticField{"ms", ms}};
					diagnose({.code = "ear.hook-slow",
					          .subject = std::string(hook.name),
					          .detail = std::string(hook.name) + " ran " + std::to_string(ms) + " ms",
					          .fields = {fields.begin(), fields.end()}});
				}
			}
			refused_ = nullptr;
			if (std::optional<EarFault> fault = std::exchange(earFault_, std::nullopt)) {
				brokenRule_.reset();
				if (fault->owner() == FaultOwner::host) std::rethrow_exception(fault->thrown());
				host_.internalError(InternalError{.callback = fault->callback(),
				                                  .node = name_,
				                                  .where = MainLoopActivity::describe(MainLoopActivity::save()),
				                                  .what = fault->what()});
				return std::unexpected(
					TypeFault{internalErrorDetail(fault->callback(), fault->what()), fault->what(), true});
			}
			std::optional<std::string> what = std::exchange(brokenRule_, std::nullopt);
			if (!what && !result) what = std::move(result.error());
			if (what) return std::unexpected(TypeFault{std::string(hook.name) + " threw: " + *what, std::move(*what)});
			if constexpr (std::is_void_v<std::invoke_result_t<Call>>) return {};
			else return std::move(*result);
		}

		/// What is wrong with an applied answer, if anything: no code, or states that are not exactly those of the
		/// loops declared with one (docs/writing-a-node-type.md#rules-the-framework-enforces).
		static std::optional<std::string> wrongAnswer(const Configuration& c, Configured& answer) {
			if (!answer.code().own() && answer.code().pairs().empty())
				return "the answer holds no code: a null code or a pass-through of no pair";
			std::vector<bool> given(c.loops.size(), false);
			for (const ErasedLoopState& state : answer.states()) {
				if (state.loop >= c.loops.size())
					return "the answer holds a state for loop " + std::to_string(state.loop) +
					       ", which is not declared";
				const std::string name(c.loops[state.loop]->declared.name);
				if (!c.loops[state.loop]->declared.stateful)
					return "the answer holds a state for loop " + name + ", which takes none";
				if (given[state.loop]) return "the answer holds two states for loop " + name;
				if (!state.state) return "the answer holds a null state for loop " + name;
				given[state.loop] = true;
			}
			for (std::size_t loop = 0; loop < c.loops.size(); ++loop)
				if (c.loops[loop]->declared.stateful && !given[loop])
					return "the answer holds no state for loop " + std::string(c.loops[loop]->declared.name);
			return std::nullopt;
		}

		/// T2 (docs/writing-a-node-type.md#configure-and-createof): a new generation and its context; the instance made
		/// and its configure called, timed together as the hook `configure`; then its answer and the return checks. A
		/// throw, a failed answer or a broken rule is a fault in readying (start-failed); else the code is kept,
		/// unprepared, and the resources are asked for.
		void configure() {
			if (config_) throw std::logic_error("node " + name_ + ": a configuration began while another had not gone");
			if (!book_)
				book_ = std::make_unique<detail::StatusBook>(queued_->statusDeclared(), queued_->eventsDeclared());
			config_ = std::make_unique<Configuration>();
			Configuration& c = *config_;
			c.generation = ++host_.generations;
			refusedGeneration_ = 0;
			c.config = queued_;
			adoptPorts(); // a restart's reshape: the block, silent, takes the new ports before configure sees them
			c.ports = ports_;
			c.channel = std::make_shared<Channel>(*this, host_.mainLoop, host_.timers, name_, type_, host_.logSinks);
			c.context = std::make_unique<Context>(host_.format, name_, *c.channel, c.loops);
			c.fadedCount = ports_.outputs().size() + ports_.insideOutputs().size();
			c.faded = std::make_unique<bool[]>(c.fadedCount);
			std::fill_n(c.faded.get(), c.fadedCount, true);
			// Until configure returned a code that holds, the configuration asks for nothing more.
			c.startFailed = true;
			configuring_ = true;
			std::expected<Configured, TypeFault> answer = callType(hooks::configure, [&] {
				c.instance = c.config->make(*c.context);
				const std::span<const LoopDeclaration> declared = c.instance->loops();
				for (std::size_t i = 0; i < declared.size(); ++i)
					c.loops.push_back(
						std::make_unique<LoopSlot>(host_, serial_, c.generation, i, declared[i], c.channel));
				// Each declared store, on a host that has stores (else refused below, and at a store call).
				if (StoreAdapter* stores = host_.resources.stores()) {
					const std::span<const StoreDeclaration> kept = c.instance->stores();
					for (std::size_t i = 0; i < kept.size(); ++i)
						c.stores.push_back(std::make_unique<StoreSlot>(
							*this,
							std::make_shared<detail::StoreState>(*stores, storeFileName(type_, name_, kept[i].suffix),
						                                         kept[i].rule),
							StoreLink{&host_, serial_, c.generation, i, c.channel->completions(), c.storesWritable}));
				}
				return c.instance->configure(*this);
			});
			configuring_ = false;
			// A rule a call found broken fails the node whatever configure did next: caught, answered or thrown. Only
			// configure's context calls reach an adapter, so only here can an adapter's bug end a type's call.
			if (!answer)
				return failConfigure(answer.error().adapterFaulted ? lc::Fact::adapterFaulted : lc::Fact::hookThrew,
				                     std::move(answer.error().detail));
			if (answer->isFailed()) {
				if (answer->detail().empty())
					return failConfigure(lc::Fact::configureFailed, "configure answered failed with no detail");
				return failConfigure(lc::Fact::configureFailed, answer->detail());
			}
			// An applied answer holds the code and exactly its loops' states: a type's class cannot answer otherwise
			// (CreateOf::applied), so one that does is its instance's maker's bug, told as ear's own.
			if (std::optional<std::string> wrong = wrongAnswer(c, *answer)) {
				const MainLoopActivity::Scope activity(hooks::configure.activity, name_);
				host_.internalError(InternalError{.callback = "ErasedInstance::configure",
				                                  .node = name_,
				                                  .where = MainLoopActivity::describe(MainLoopActivity::save()),
				                                  .what = *wrong});
				return failConfigure(lc::Fact::adapterFaulted,
				                     internalErrorDetail("ErasedInstance::configure", *wrong));
			}
			c.pairs = answer->code().pairs();
			c.code = std::move(answer->code().own());
			for (ErasedLoopState& given : answer->states()) c.loops[given.loop]->state = std::move(given.state);
			// The return checks, on an applied answer only.
			if (!c.code) {
				try {
					c.code = makePassThrough(ports_, c.pairs);
				} catch (const std::logic_error& error) {
					return failConfigure(lc::Fact::configureFailed,
					                     std::string("configure gave a pass-through: ") + error.what());
				}
			}
			// Each loop declared with a state type was given one, on a host that runs loops; stores on a host that has them.
			if (!c.loops.empty() && host_.resources.loops() == nullptr)
				return failConfigure(lc::Fact::configureFailed, "no loops in this host");
			if (!c.instance->stores().empty() && host_.resources.stores() == nullptr)
				return failConfigure(lc::Fact::configureFailed, "no stores in this host");
			for (const auto& slot : c.recordings)
				if (slot->refused())
					return failConfigure(lc::Fact::configureFailed,
					                     "cannot open " + slot->path() + ": " + *slot->refused());
			if (!c.recordings.empty() && host_.resources.recordings() == nullptr)
				return failConfigure(lc::Fact::configureFailed,
				                     "cannot open " + c.recordings.front()->path() + ": no recordings folder");
			// A FIFO the program names is a pipe receiver's of this configuration: attaching one is the only way to its
			// FIFO, so only one kept from another configuration fails here.
			if (c.program)
				if (std::optional<std::string> bank = fifoWithoutReceiver(*c.program))
					return failConfigure(lc::Fact::configureFailed, "configure gave program " + c.program->name() +
					                                                    " the FIFO of bank " + *bank +
					                                                    ", which has no pipe receiver");
			// A declared path must exist: a pass-through copies only its pairs, so a delay declared on any other path
			// would lie on audio that never flows there.
			if (!c.pairs.empty())
				for (const DeclaredLatency& declared : c.latency)
					if (std::ranges::find(c.pairs, BankPair{declared.from, declared.bank}) == c.pairs.end())
						return failConfigure(lc::Fact::configureFailed, "configure declared latency on " +
						                                                    declared.from + " to " + declared.bank +
						                                                    ", a path its pass-through does not copy");
			// The latency plan fits the ports, worked out here as the block will (with no peer yet), so a misfit fails
			// the node now with its reason and every drive carries a plan that fits.
			try {
				c.plan = latencyPlan(c);
				const std::vector<std::optional<Latency>> inputs(ports_.inputs().size() + ports_.insideInputs().size());
				const std::vector<std::optional<Latency>> outputs(ports_.outputs().size() +
				                                                  ports_.insideOutputs().size());
				static_cast<void>(workOut(ports_, c.plan, inputs, outputs));
			} catch (const std::logic_error& error) {
				return failConfigure(lc::Fact::configureFailed,
				                     std::string("configure gave a latency plan that does not fit: ") + error.what());
			}
			c.startFailed = false;
			// Every claim it holds while it lives (docs/writing-a-node-type.md#claims): its ports, listed at the call that
			// asked for each (cx.claim, an engine companion's by its shape), then each store's file, then each
			// recording's path.
			for (const auto& slot : c.stores) addClaim(Claim{Claim::Kind::store, slot->fileName()});
			for (const auto& slot : c.recordings) addClaim(Claim{Claim::Kind::recording, slot->claimPath()});
			// A drive fades exactly the outputs flagged (a sink's bank never is), so a teardown fades first only when
			// one is: a node whose only outputs are sinks' banks is silenced at once.
			aggregate_.outputsFaded(std::any_of(c.faded.get(), c.faded.get() + c.fadedCount, [](bool f) { return f; }));
			// Every drive of this configuration binds these taps; then each consumer begins on its sink's thread.
			c.bindings.reserve(c.sinks.size());
			for (const auto& slot : c.sinks) c.bindings.push_back(slot->binding);
			c.sourceBindings.reserve(c.sources.size());
			for (const auto& slot : c.sources) c.sourceBindings.push_back(slot.binding);
			// Every readiness item exists before any is asked, so a ready told at once still finds its item. Each is started
			// when its step is asked: a loop made, a consumer begun, an engine companion made once the block is ready too.
			addReadiness();
			c.readiness.begin([this, generation = c.generation](std::size_t item) -> std::unique_ptr<Timer> {
				startItem(item);
				const auto bound = config_->readiness.item(item).bound;
				if (!bound) return nullptr;
				return armDeadline(generation, item, *bound);
			});
			askResources();
		}

		/// Each engine companion and each sink's consumer is a readiness item with its kind's deadline from its
		/// request (docs/engine.md#readiness-deadlines), engine companions first, each kind in request order; a source
		/// is ready when made and has none. Each deadline is an input of its own when it fires and judges readiness
		/// then, so a ready heard first wins.
		void addReadiness() {
			Configuration& c = *config_;
			using Family = Configuration::ItemOwner::Family;
			// Each claim at step `claims`, before anything is opened or started, bounded by the wait for a closing holder.
			const auto waitSeconds = std::chrono::duration_cast<std::chrono::seconds>(claimWaitBound).count();
			for (std::size_t i = 0; i < c.claims.size(); ++i) {
				ClaimSlot& slot = c.claims[i];
				slot.item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::claim,
					.name = slot.anyTcp ? std::string("tcp:any") : claimKey(slot.claim),
					.step = detail::ReadinessStep::claims,
					.bound = claimWaitBound,
					.fact = lc::Fact::configureFailed,
					.overdue =
						[this, i, waitSeconds] {
							const ClaimSlot& late = config_->claims[i];
							const std::string within = std::to_string(waitSeconds) + " s";
							return claimText(late.claim, late.waitingOn.empty()
					                                         ? "not granted within " + within
					                                         : "held by node " + late.waitingOn +
					                                               ", which did not let go within " + within);
						},
				});
				c.itemOwners.push_back({Family::claim, i});
			}
			// Each store at step `opens`, before any loop starts, bounded: its open is one job on the I/O thread.
			const auto storeSeconds = std::chrono::duration_cast<std::chrono::seconds>(storeBound).count();
			for (std::size_t i = 0; i < c.stores.size(); ++i) {
				StoreSlot& slot = *c.stores[i];
				slot.item = c.readiness.add(
					detail::ioOpenItem(WaitingFor::Kind::store, slot.fileName(), storeBound,
				                       slot.fileName() + ": not opened within " + std::to_string(storeSeconds) + " s"));
				c.itemOwners.push_back({Family::store, i});
			}
			// Each recording at step `opens`, after the stores, in request order, bounded: its open is one job on the
			// I/O thread.
			const auto recordingSeconds = std::chrono::duration_cast<std::chrono::seconds>(recordingBound).count();
			for (std::size_t i = 0; i < c.recordings.size(); ++i) {
				RecordingSlot& slot = *c.recordings[i];
				slot.item = c.readiness.add(detail::ioOpenItem(WaitingFor::Kind::recording, slot.path(), recordingBound,
				                                               "cannot open " + slot.path() + ": not opened within " +
				                                                   std::to_string(recordingSeconds) + " s"));
				c.itemOwners.push_back({Family::recording, i});
			}
			// Each loop at step `loops`, with no bound: an adapter tells at once whether its thread started.
			for (std::size_t i = 0; i < c.loops.size(); ++i) {
				LoopSlot& slot = *c.loops[i];
				slot.item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::loop,
					.name = std::string(slot.declared.name),
					.step = detail::ReadinessStep::loops,
					.bound = std::nullopt,
					.fact = lc::Fact::configureFailed,
					.overdue = {},
				});
				c.itemOwners.push_back({Family::loop, i});
			}
			for (std::size_t i = 0; i < c.engineAsks.size(); ++i) {
				EngineAsk& asked = c.engineAsks[i];
				const CompanionKind kind =
					std::visit([&](const auto& shape) { return engineKind(shape, asked.target.bank); }, asked.shape);
				asked.item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::companion,
					.name = asked.target.engineNode,
					.step = detail::ReadinessStep::companions,
					.bound = readinessBound(kind),
					.fact = lc::Fact::engine,
					.overdue = [this, i] { return engineNeverReady(i); },
				});
				c.itemOwners.push_back({Family::engine, i});
			}
			const auto sinkSeconds = std::chrono::duration_cast<std::chrono::seconds>(sinkReadinessBound).count();
			for (std::size_t i = 0; i < c.sinks.size(); ++i) {
				SinkSlot& slot = *c.sinks[i];
				slot.item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::consumer,
					.name = {}, // the wire names no sink for a consumer's wait (docs/control-port.md#node-health)
					.step = detail::ReadinessStep::companions,
					.bound = readinessBound(CompanionKind::sink),
					.fact = lc::Fact::consumerNeverBegan,
					.overdue = [text = "the sink " + slot.bank + "'s consumer did not begin within " +
				                       std::to_string(sinkSeconds) + " s"] { return text; },
				});
				c.itemOwners.push_back({Family::consumer, i});
			}
			// The program at step `programs`, once every engine companion and consumer is ready, bounded from its asking
			// until it is ready.
			if (c.program) {
				c.program->item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::program,
					.name = c.program->name(),
					.step = detail::ReadinessStep::programs,
					.bound = programReadinessBound,
					.fact = lc::Fact::configureFailed,
					.overdue = [text =
				                    programOverdueDetail(c.program->name(), c.program->shape.needs)] { return text; },
				});
				c.itemOwners.push_back({Family::program, 0});
			}
		}

		/// Readiness item `item`'s step was asked: its owner starts it. A loop is made by the adapter (an adapter's own
		/// bug fails the node lost, docs/architecture.md#faults-by-owner); a sink's consumer begins on its sink's thread; an
		/// engine companion is made once the block is ready too (askResources).
		void startItem(std::size_t item) {
			Configuration& c = *config_;
			using Family = Configuration::ItemOwner::Family;
			const Configuration::ItemOwner owner = c.itemOwners.at(item);
			switch (owner.family) {
			case Family::claim: {
				if (c.claims[owner.index].anyTcp && !grantAnyTcp(owner.index)) break;
				// Judged now, so a claimant judged after it in this turn finds it held; the node hears it as an input.
				const Claim& claim = c.claims[owner.index].claim;
				host_.claimsDecided({detail::ClaimDecided{detail::ClaimHolder{c.generation, serial_, name_}, claim,
				                                          host_.claims.claim({c.generation, serial_, name_}, claim)}});
				break;
			}
			case Family::store:
				c.stores[owner.index]->open();
				break;
			case Family::recording:
				c.recordings[owner.index]->open(&host_, serial_, c.generation, owner.index, c.channel->completions());
				break;
			case Family::loop: {
				LoopSlot& slot = *c.loops[owner.index];
				try {
					slot.start(*host_.resources.loops(), name_); // checked at configure's return
				} catch (...) {
					adapterFaulted("LoopAdapter::make", [this, generation = c.generation](std::string detail) {
						host_.inputFor(serial_, [generation, detail = std::move(detail)](Node& node) mutable {
							if (node.current(generation))
								node.feed(node.aggregate_.fault(lc::Fact::adapterFaulted, std::move(detail)));
						});
					});
				}
				break;
			}
			case Family::consumer: {
				SinkSlot& sink = *c.sinks[owner.index];
				// A state's consumer begins only once its loop runs (loops are the step before), so the state is the
				// loop's still, until teardown step 4.
				void* state = sink.stateLoop ? c.loops.at(*sink.stateLoop)->state.get() : nullptr;
				sink.begin(c.channel, c.channel->sinkBegunCall(owner.index), state);
				break;
			}
			case Family::engine:
				// Queued in the flush once the block is ready; a ready step behind a later input asks it then.
				host_.inputFor(serial_, [](Node& node) { node.askResources(); });
				break;
			case Family::program:
				launchProgram();
				break;
			}
		}

		/// AnyTcp claim `index`, at step claims: a port no claim in the host holds, the kernel's free one through the
		/// probe (tried up to anyTcpTries times), else the lowest from anyTcpFirstPort up. Picked, it is claimed as a
		/// TcpListen of that port and not probed again (freeTcp was its probe); refused, the node fails start-failed
		/// "cannot find a free TCP port: <why>" as an input of its own. False when refused.
		bool grantAnyTcp(std::size_t index) {
			Configuration& c = *config_;
			std::optional<std::uint16_t> picked;
			std::string why;
			if (PortProbe* probe = host_.resources.ports()) {
				why = "every port the kernel offered is held by a node";
				for (int tries = 0; tries < anyTcpTries && !picked; ++tries) {
					const std::expected<std::uint16_t, std::string> offered = probe->freeTcp();
					if (!offered) {
						why = offered.error();
						break;
					}
					if (!host_.claims.held(claimKey(claim::of(claim::TcpListen(*offered))))) picked = *offered;
				}
			} else {
				for (std::uint32_t port = anyTcpFirstPort; port <= 65535 && !picked; ++port)
					if (!host_.claims.held("tcp:" + std::to_string(port))) picked = static_cast<std::uint16_t>(port);
				why = "every port from " + std::to_string(anyTcpFirstPort) + " up is held by a node";
			}
			if (picked) {
				c.claims[index].claim = claim::of(claim::TcpListen(*picked));
				return true;
			}
			host_.inputFor(serial_, [generation = c.generation, index,
			                         text = "cannot find a free TCP port: " + why](Node& node) mutable {
				if (!node.stillReadying(generation)) return;
				if (auto failure = node.config_->readiness.failed(node.config_->claims[index].item, std::move(text)))
					node.feed(node.aggregate_.fault(failure->fact, std::move(failure->detail)));
			});
			return false;
		}

		/// The bank of the first FIFO the program names that has no pipe receiver in this configuration, or none.
		std::optional<std::string> fifoWithoutReceiver(const ProgramSlot& program) const {
			const auto unjoined = [&](const ProgramPart& piece) -> std::optional<std::string> {
				const auto* fifo = std::get_if<part::Fifo>(&piece);
				if (fifo == nullptr || pipeReceiverOn(fifo->bank()) != nullptr) return std::nullopt;
				return fifo->bank();
			};
			for (const ProgramArg& arg : program.shape.args)
				for (const ProgramPart& piece : arg.parts)
					if (auto bank = unjoined(piece)) return bank;
			for (const ProgramFile& file : program.shape.files)
				for (const ProgramPart& piece : file.text)
					if (auto bank = unjoined(piece)) return bank;
			return std::nullopt;
		}
		/// The pipe receiver this configuration asked for on inside bank `bank`, or null.
		const EngineAsk* pipeReceiverOn(const std::string& bank) const {
			for (const EngineAsk& asked : config_->engineAsks)
				if (std::holds_alternative<PipeReceiverShape>(asked.shape) && asked.target.insideBank == bank)
					return &asked;
			return nullptr;
		}

		/// At step programs: every part resolved to text (docs/writing-a-node-type.md#programs), then the adapter
		/// launches it; it tells started or refused later. An adapter's own bug fails the node lost
		/// (docs/architecture.md#faults-by-owner).
		void launchProgram() {
			Configuration& c = *config_;
			ProgramSlot& program = *c.program;
			const std::string& folder = host_.resources.runtimeFolder();
			const auto pathOf = [&](std::string_view what) { return runtimePath(folder, name_, c.generation, what); };
			const auto text = [&](const std::vector<ProgramPart>& parts) {
				std::string joined;
				for (const ProgramPart& piece : parts)
					joined += std::visit(
						Overloaded{
							[](const part::Text& given) { return given.text; },
							[&](const part::Port& port) { return c.claims.at(port.port.claim()).claim.value; },
							[&](const part::Fifo& fifo) { return pipeReceiverOn(fifo.bank())->target.fifo; },
							[&](const part::Feed&) { return pathOf("feed"); },
							[&](const part::File& file) { return pathOf(file.name); },
						},
						piece);
				return joined;
			};
			ProgramLaunch launch{.node = name_,
			                     .executable = std::string(program.shape.executable.path()),
			                     .argv = {program.name()},
			                     .folder = folder};
			for (const ProgramArg& arg : program.shape.args) launch.argv.push_back(text(arg.parts));
			for (const ProgramFile& file : program.shape.files)
				launch.files.emplace_back(pathOf(file.name), text(file.text));
			if (program.feed) launch.feed = pathOf("feed");
			for (const SharedMemoryName& need : program.shape.needs) launch.needs.push_back(need.text());
			program.events = std::make_unique<ProgramLink>(c.channel);
			try {
				program.running = host_.resources.programs()->launch(std::move(launch), *program.events);
				if (!program.running)
					throw std::logic_error("the program adapter launched no program " + program.name());
			} catch (...) {
				program.ended = true; // nothing of it runs: nothing to wait for
				adapterFaulted("ProgramAdapter::launch", [this, generation = c.generation](std::string detail) {
					host_.inputFor(serial_, [generation, detail = std::move(detail)](Node& node) mutable {
						if (node.current(generation))
							node.feed(node.aggregate_.fault(lc::Fact::adapterFaulted, std::move(detail)));
					});
				});
			}
		}

		/// The program started. Its readiness waits for ready: a program that needs shared memory runs only once that
		/// exists (docs/writing-a-node-type.md#programs).
		void programStarted(std::uint64_t generation) {
			host_.inputFor(serial_, [generation](Node& node) {
				if (!node.current(generation) || !node.config_->program) return;
				node.config_->program->started = true;
			});
		}
		/// The program is ready (started, and every shared-memory name it needs exists): its readiness item is ready.
		void programReady(std::uint64_t generation) {
			host_.inputFor(serial_, [generation](Node& node) {
				if (!node.current(generation) || !node.config_->program) return;
				if (node.stillReadying(generation)) node.itemReady(generation, node.config_->program->item);
			});
		}
		/// The program cannot run: the node fails start-failed "cannot start <name>: <why>"; nothing of it is left to wait
		/// for.
		void programRefused(std::uint64_t generation, std::string why) {
			host_.inputFor(serial_, [generation, why = std::move(why)](Node& node) mutable {
				if (!node.current(generation) || !node.config_->program) return;
				ProgramSlot& program = *node.config_->program;
				program.ended = true;
				if (node.stillReadying(generation)) {
					if (auto failure =
					        node.config_->readiness.failed(program.item, "cannot start " + program.name() + ": " + why))
						node.feed(node.aggregate_.fault(failure->fact, std::move(failure->detail)));
				}
				node.closeLoops();
			});
		}
		/// The program was reaped. During a teardown (from its step 1) that is the end the teardown waits for; otherwise
		/// the node fails with its words (start-failed before it first rested, lost after: Fact::nodeLost).
		void programExited(std::uint64_t generation, ProgramExit exit) {
			host_.inputFor(serial_, [generation, exit = std::move(exit)](Node& node) mutable {
				if (!node.current(generation) || !node.config_->program) return;
				ProgramSlot& program = *node.config_->program;
				program.ended = true;
				if (node.config_->ending) return node.closeLoops();
				node.feed(node.aggregate_.fault(lc::Fact::nodeLost, programExitDetail(program.name(), exit)));
			});
		}
		/// Bytes of the program's report feed: to its loop's state, staged until the node is configured; none from
		/// teardown step 1, when the loops refuse outside work.
		void programFed(std::uint64_t generation, std::string bytes) {
			host_.inputFor(serial_, [generation, bytes = std::move(bytes)](Node& node) mutable {
				if (!node.current(generation) || !node.config_->program || node.config_->ending) return;
				ProgramFeedTo* feed = node.config_->program->feed ? &*node.config_->program->feed : nullptr;
				if (feed == nullptr) return;
				node.config_->loops.at(feed->loop)
					->postOutsideState([deliver = &feed->deliver, bytes = std::move(bytes)](void* state) {
						(*deliver)(state, bytes);
					});
			});
		}

		/// One claim, once each in a configuration (a store's file, named by the framework, is held once).
		void addClaim(Claim claim) {
			if (std::ranges::find(config_->claims, claim, &ClaimSlot::claim) == config_->claims.end())
				config_->claims.push_back(ClaimSlot{std::move(claim)});
		}

		/// A port, from inside a context call: one the configuration already claimed (by cx.claim or an engine
		/// companion's shape) is the type's bug, a std::logic_error the call records, so the node fails start-failed.
		void addPortClaim(Claim claim) {
			const std::string key = claimKey(claim);
			if (std::ranges::any_of(config_->claims,
			                        [&](const ClaimSlot& slot) { return claimKey(slot.claim) == key; }))
				throw std::logic_error(key + " claimed twice");
			config_->claims.push_back(ClaimSlot{std::move(claim)});
		}

		/// Claim `index` is a port the registry granted: probed against the kernel (a host with no probe grants it). The
		/// probe runs behind the turn's flush, so an engine companion a teardown just queued for destruction (a restarted
		/// rtp node's own receiver) has let go of its port first (docs/writing-a-node-type.md#claims). `leaked`: the
		/// leaked configuration whose abandoned port it is, to which a refusal is owed.
		void probeClaim(std::uint64_t generation, std::size_t index, std::uint64_t leaked) {
			PortProbe* probe = host_.resources.ports();
			if (probe == nullptr) return itemReady(generation, config_->claims[index].item);
			host_.mainLoop.post([&host = host_, alive = std::weak_ptr<char>(host_.alive), probe, serial = serial_,
			                     generation, index, leaked, claim = config_->claims[index].claim] {
				if (!alive.lock()) return;
				const MainLoopActivity::Scope activity("probe port", claimKey(claim));
				const auto port = static_cast<std::uint16_t>(std::stoul(claim.value));
				std::expected<void, std::string> outcome;
				std::optional<std::string> fault;
				try {
					outcome = claim.kind == Claim::Kind::tcp ? probe->tcpListen(port) : probe->udp(port, claim.group);
				} catch (...) {
					fault = detail::whatOf(std::current_exception());
				}
				host.inputFor(serial, [generation, index, leaked, outcome = std::move(outcome),
				                       fault = std::move(fault)](Node& node) mutable {
					node.probed(generation, index, std::move(outcome), std::move(fault), leaked);
				});
			});
		}

		/// Claim `index`'s probe came back: ready, or refused with the kernel's reason; a probe that threw is ear's own
		/// bug, told as ear.internal-error, and the node fails lost (docs/architecture.md#faults-by-owner).
		void probed(std::uint64_t generation, std::size_t index, std::expected<void, std::string> outcome,
		            std::optional<std::string> fault, std::uint64_t leaked) {
			if (!stillReadying(generation) || index >= config_->claims.size()) return;
			if (fault) {
				const std::string callback = "PortProbe";
				host_.internalError(InternalError{.callback = callback,
				                                  .node = name_,
				                                  .where = "the probe of " + claimKey(config_->claims[index].claim),
				                                  .what = *fault});
				return feed(aggregate_.fault(lc::Fact::adapterFaulted, internalErrorDetail(callback, *fault)));
			}
			if (outcome) return itemReady(generation, config_->claims[index].item);
			refuseClaim(generation, index, outcome.error(), leaked);
		}

		/// Claim `index` refused (`why` after its words): the node fails start-failed. A refusal on a leaked
		/// configuration's account is listed on it, so its late close starts this node again.
		void refuseClaim(std::uint64_t generation, std::size_t index, const std::string& why, std::uint64_t leaked) {
			if (leaked != 0)
				if (const auto record = host_.refusedByLeaked.find(leaked); record != host_.refusedByLeaked.end()) {
					record->second[serial_] = generation;
					refusedGeneration_ = generation;
				}
			const ClaimSlot& slot = config_->claims[index];
			if (auto failure = config_->readiness.failed(slot.item, claimText(slot.claim, why)))
				feed(aggregate_.fault(failure->fact, std::move(failure->detail)));
		}

		/// Store `index`'s open landed (attached, so this configuration's): a fault of the adapter's fails the node lost;
		/// otherwise its ReadyEvents hear it.
		void storeOpened(std::uint64_t generation, std::size_t index, std::expected<void, std::string> outcome,
		                 std::optional<detail::IoFault> fault) {
			if (!current(generation) || index >= config_->stores.size()) return;
			StoreSlot& slot = *config_->stores[index];
			if (fault) return ioJobFaulted(generation, "open store " + slot.fileName(), fault->callback, fault->what);
			if (outcome) slot.ready();
			else slot.failed(std::move(outcome.error()));
		}

		/// Recording `index`'s open landed (attached, so this configuration's): a fault of the adapter's fails the node
		/// lost (ear.internal-error); a refusal fails it start-failed "cannot open <path>: <why>"; a file made is ready.
		void recordingOpened(std::uint64_t generation, std::size_t index, std::uint64_t id,
		                     std::expected<void, std::string> outcome, std::optional<detail::IoFault> fault) {
			if (id != 0) return replacementRecordingOpened(generation, id, std::move(outcome), std::move(fault));
			if (!current(generation) || index >= config_->recordings.size()) return;
			RecordingSlot& slot = *config_->recordings[index];
			if (fault) return ioJobFaulted(generation, "open recording " + slot.path(), fault->callback, fault->what);
			if (outcome) slot.opened();
			if (!stillReadying(generation)) return;
			if (outcome) return itemReady(generation, slot.item);
			if (auto failure =
			        config_->readiness.failed(slot.item, "cannot open " + slot.path() + ": " + outcome.error()))
				feed(aggregate_.fault(failure->fact, std::move(failure->detail)));
		}

		/// Store `index` was opened and trusted: its readiness item is ready.
		void storeReady(std::uint64_t generation, std::size_t index) {
			if (!current(generation) || config_->startFailed || config_->tearing || index >= config_->stores.size())
				return;
			itemReady(generation, config_->stores[index]->item);
		}

		/// Store `index` cannot be opened, or is not trusted: the node fails start-failed with "<file>: <why>", unless it
		/// moved on.
		void storeFailed(std::uint64_t generation, std::size_t index, std::string detail) {
			if (!stillReadying(generation) || index >= config_->stores.size()) return;
			if (auto failure = config_->readiness.failed(config_->stores[index]->item, std::move(detail)))
				feed(aggregate_.fault(failure->fact, std::move(failure->detail)));
		}

		/// A store or recording adapter's bug in one of this configuration's I/O jobs (`job`, its call `callback`): told
		/// as ear.internal-error, and the node fails lost (docs/architecture.md#faults-by-owner).
		void ioJobFaulted(std::uint64_t generation, const std::string& job, const std::string& callback,
		                  const std::string& what) {
			if (!current(generation)) return;
			const std::string where = "the I/O job " + job;
			host_.internalError(InternalError{.callback = callback, .node = name_, .where = where, .what = what});
			feed(aggregate_.fault(lc::Fact::adapterFaulted, internalErrorDetail(callback, what)));
		}

		/// The adapter says loop `index` runs: its readiness item is ready.
		void loopReady(std::uint64_t generation, std::size_t index) {
			if (!current(generation) || config_->startFailed || config_->tearing || index >= config_->loops.size())
				return;
			itemReady(generation, config_->loops[index]->item);
		}

		/// The adapter says loop `index` cannot start: the node fails start-failed, unless it was ready or moved on.
		void loopFailed(std::uint64_t generation, std::size_t index, std::string detail) {
			if (!current(generation) || index >= config_->loops.size()) return;
			LoopSlot& slot = *config_->loops[index];
			slot.failedHeard = true;
			if (!stillReadying(generation)) return;
			if (auto failure = config_->readiness.failed(
					slot.item, "cannot start loop " + std::string(slot.declared.name) + ": " + std::move(detail)))
				feed(aggregate_.fault(failure->fact, std::move(failure->detail)));
		}

		/// One readiness deadline's timer. When it fires late (the main loop stalled), the judgement is posted to the
		/// main loop once more, so that a ready the stall held back, dispatched behind the timer, is heard first.
		std::unique_ptr<Timer> armDeadline(std::uint64_t generation, std::size_t item,
		                                   std::chrono::milliseconds bound) {
			const auto due = host_.timers.now() + bound;
			return host_.timers.after(bound, [&host = host_, serial = serial_, generation, item, due] {
				host.inputFor(serial, [generation, item, due](Node& node) {
					if (node.host_.timers.now() - due <= lateSlack) return node.deadlinePassed(generation, item);
					node.host_.mainLoop.post([&host = node.host_, alive = std::weak_ptr<char>(node.host_.alive),
					                          serial = node.serial_, generation, item] {
						if (!alive.lock()) return;
						host.inputFor(serial,
						              [generation, item](Node& later) { later.deadlinePassed(generation, item); });
					});
				});
			});
		}

		/// The configuration of `generation` is the current one and still waits for its resources.
		bool stillReadying(std::uint64_t generation) const {
			return current(generation) && !config_->startFailed && !config_->tearing && !config_->readyTold &&
			       aggregate_.configuration().state() == lc::ConfigState::readying;
		}

		/// Readiness item `item` missed its deadline: unless it became ready meanwhile, the node fails with its kind's
		/// fact and text. The teardown destroys an engine companion in the flush, made or not (destroying it tells
		/// nothing, so its lost() never comes), and closes a sink, which lets a begin still running finish first.
		void deadlinePassed(std::uint64_t generation, std::size_t item) {
			if (!stillReadying(generation)) return;
			if (auto overdue = config_->readiness.passed(item))
				feed(aggregate_.fault(overdue->fact, std::move(overdue->detail)));
		}

		/// Why engine companion `index` missed its deadline (engine-refused): one made and not ready is blamed with its
		/// kind's text; one never made, because the block was not ready, blames the block.
		std::string engineNeverReady(std::size_t index) const {
			const Configuration& c = *config_;
			const EngineAsk& asked = c.engineAsks[index];
			if (index < c.engineCompanions.size())
				return std::visit([](const auto& shape) { return neverReady(shape); }, asked.shape);
			const CompanionKind kind =
				std::visit([&](const auto& shape) { return engineKind(shape, asked.target.bank); }, asked.shape);
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(*readinessBound(kind)).count();
			return asked.target.engineNode + " was never made: the node's block was not ready within " +
			       std::to_string(seconds) + " s";
		}

		/// Readiness item `item` is ready: the companions' list shows it ready before the health its readiness allows.
		void itemReady(std::uint64_t generation, std::size_t item) {
			config_->readiness.ready(item);
			refreshCompanions();
			checkReady(generation);
			refreshWaiting();
		}

		/// A sink's consumer began: its readiness item is ready.
		void sinkBegun(std::uint64_t generation, std::size_t index) {
			host_.inputFor(serial_, [generation, index](Node& node) {
				if (!node.current(generation) || node.config_->startFailed || node.config_->tearing ||
				    index >= node.config_->sinks.size())
					return;
				node.itemReady(generation, node.config_->sinks[index]->item);
			});
		}

		/// A sink closed (teardown step 3): its drops are the node's.
		void sinkClosed(std::uint64_t generation, std::size_t index, std::uint64_t droppedFrames) {
			host_.inputFor(serial_, [generation, index, droppedFrames](Node& node) {
				if (!node.current(generation) || index >= node.config_->sinks.size() ||
				    node.config_->sinks[index]->closed)
					return;
				node.config_->sinks[index]->closed = true;
				node.dropped_ += droppedFrames;
				++node.config_->sinksClosed;
				node.closeLoops();
			});
		}

		/// configure failed, threw or broke a rule: nothing was driven by its code, so the sinks may go now, none
		/// begun (a consumer given is freed unbegun, with nothing to finish); no engine companion was made. The recordings and the instance stay
		/// until step 5 frees them, the instance first. The fault is an input of its own.
		void failConfigure(lc::Fact fact, std::string detail) {
			Configuration& c = *config_;
			c.sinks.clear();
			closeSources(); // freed at step 5, after the instance that may hold their writers
			c.engineAsks.clear();
			closeStatus(c);
			host_.inputFor(serial_, [generation = c.generation, fact, detail = std::move(detail)](Node& node) mutable {
				if (!node.current(generation)) return;
				node.feed(node.aggregate_.fault(fact, std::move(detail)));
			});
		}

		// ---- A change in place (docs/architecture.md#changes-in-place) ----

		bool changeFromDriving() const noexcept {
			return aggregate_.configuration().changeFrom() == lc::ConfigState::driving;
		}

		/// The change version: a new change of the configuration, its deadline armed, and the type's change version
		/// called through the change's context, timed as `configure`. Its answer, as an input of its own: a rule it broke
		/// (or a fault of ear's adapter) fails the node; a throw, or a restart it asks for, falls back to a restart with
		/// the new settings; an applied answer's new code is prepared now, while the old one runs, and the commit point
		/// comes once every hold it asked was acknowledged.
		void reconfigure() {
			Configuration& c = *config_;
			Configuration::Change& ch = c.change.emplace();
			ch.target = queued_;
			ch.epoch = ++c.changeEpoch;
			// A reshape in place (docs/architecture.md#reshapes): the new ports are in the engine before the change
			// version runs, the old code keeping its view until the commit's swap; a newer change that supersedes one
			// takes the block to its own ports (the old ones again, for a change back).
			ch.reshape = ch.target->ports() != c.ports;
			ports_ = ch.target->ports();
			reshapeBlock();
			ch.keys = aggregate_.health().keys;
			ch.began = host_.timers.now();
			const std::chrono::milliseconds deadline = c.instance->reconfigureDeadline();
			ch.deadline = host_.timers.after(
				deadline, [&host = host_, serial = serial_, generation = c.generation, epoch = ch.epoch, deadline] {
					host.inputFor(serial, [generation, epoch, deadline](Node& node) {
						if (!node.inChange(generation, epoch)) return;
						const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(deadline).count();
						node.fallBack(lc::NodeCause::FellBack::deadline,
					                  "deadline " + std::to_string(seconds) + "s passed");
					});
				});
			if (c.held.size() != c.fadedCount) c.held.assign(c.fadedCount, 0);
			changeScope_.valid = true;
			changeScope_.broken = false;
			std::expected<Changed, TypeFault> answer =
				callType(hooks::configure, [&] { return c.instance->change(changeScope_, *ch.target); });
			changeScope_.valid = false;
			const std::uint64_t epoch = ch.epoch;
			if (!answer) {
				if (answer.error().adapterFaulted) return faultLater(lc::Fact::adapterFaulted, answer.error().detail);
				// A rule broken in a change is the type's bug: the node fails lost, as a rested node's hook that throws.
				if (changeScope_.broken) return faultLater(lc::Fact::hookThrew, answer.error().detail);
				return fallBackLater(epoch, lc::NodeCause::FellBack::failed, answer.error().detail);
			}
			if (answer->restarts())
				return fallBackLater(epoch, lc::NodeCause::FellBack::restartAsked, answer->reason());
			// The code running is laid out for the old ports, so a change of them must bring the code for the new ones.
			if (ch.reshape && !answer->code())
				return faultLater(lc::Fact::hookThrew,
				                  "configure changed the ports in place and gave no code for them");
			// whenReady's Changed is the answer when it was called, and only then.
			if (ch.continuation.has_value() != answer->continues())
				return faultLater(lc::Fact::hookThrew, "configure called whenReady and answered with another Changed");
			if (std::optional<NodeCode>& given = answer->code()) {
				ch.codeGiven = true;
				ch.pairs = given->pairs();
				ch.code = std::move(given->own());
				if (!ch.code) {
					try {
						ch.code = makePassThrough(ports_, ch.pairs);
					} catch (const std::logic_error& error) {
						return faultLater(lc::Fact::hookThrew,
						                  std::string("configure gave a pass-through: ") + error.what());
					}
				}
			}
			// The plan from the commit point, worked out now as configure's return does, so a misfit fails here.
			const std::vector<BankPair>& pairs = ch.codeGiven ? ch.pairs : c.pairs;
			const std::vector<DeclaredLatency>& latency = ch.latency ? *ch.latency : c.latency;
			if (!pairs.empty())
				for (const DeclaredLatency& declared : latency)
					if (std::ranges::find(pairs, BankPair{declared.from, declared.bank}) == pairs.end())
						return faultLater(lc::Fact::hookThrew, "configure declared latency on " + declared.from +
						                                           " to " + declared.bank +
						                                           ", a path its pass-through does not copy");
			try {
				std::vector<EngineAsk> asks = c.engineAsks;
				for (const Configuration::Change::Replacement& r : ch.replacements) asks[r.index].shape = r.shape;
				ch.plan = latencyPlan(c, pairs, latency, asks);
				const std::vector<std::optional<Latency>> inputs(ports_.inputs().size() + ports_.insideInputs().size());
				const std::vector<std::optional<Latency>> outputs(ports_.outputs().size() +
				                                                  ports_.insideOutputs().size());
				static_cast<void>(workOut(ports_, ch.plan, inputs, outputs));
			} catch (const std::logic_error& error) {
				return faultLater(lc::Fact::hookThrew,
				                  std::string("configure gave a latency plan that does not fit: ") + error.what());
			}
			// The new code is prepared now, while the old one runs, when the old one is (a stopped node's after its
			// stop's silence is not: its start prepares the code it then has).
			if (ch.code && (changeFromDriving() || c.prepared)) {
				if (auto prepared = callType(hooks::prepare, [&] { ch.code->prepare(host_.format); }); !prepared)
					return faultLater(lc::Fact::hookThrew, std::move(prepared.error().detail));
				ch.codePrepared = true;
			}
			for (const auto& r : ch.recordings)
				if (r.slot->refused())
					return fallBackLater(epoch, lc::NodeCause::FellBack::resourceRefused,
					                     "cannot open " + r.slot->path() + ": " + *r.slot->refused());
			ch.answered = true;
			refreshCompanions(); // a replacement sink or source is listed beside the one it replaces from here
			judgeChangeClaims();
			checkChangeReady();
		}

		/// The configuration of `generation` is current and its change of `epoch` is the one in flight, before its commit
		/// point.
		bool inChange(std::uint64_t generation, std::uint64_t epoch) const {
			return current(generation) && config_->change && config_->change->epoch == epoch &&
			       aggregate_.configuration().state() == lc::ConfigState::changing;
		}

		/// A hold asked of the block and not yet acknowledged, while the change has not given up on it.
		bool holdsWaiting() const {
			return config_->holdsAcked < config_->holdsAsked && !(config_->change && config_->change->holdGaveUp);
		}

		/// The change's next steps, as an input of its own: once every claim of the change is granted, each replacement
		/// is held, then made once its hold is acknowledged; the commit point comes once the change version answered
		/// applied, every claim is granted, every hold it asked was acknowledged and every replacement is ready.
		void checkChangeReady() {
			if (!config_ || !config_->change) return;
			host_.inputFor(serial_, [generation = config_->generation, epoch = config_->change->epoch](Node& node) {
				if (!node.inChange(generation, epoch) || !node.config_->change->answered) return;
				node.stepReplacements();
				if (node.config_->change->claimsPending > 0 || node.holdsWaiting()) return;
				if (!std::ranges::all_of(node.config_->change->replacements, [](const auto& r) { return r.ready; }) ||
				    !std::ranges::all_of(node.config_->change->sinks, [](const auto& r) { return r.ready; }) ||
				    !std::ranges::all_of(node.config_->change->recordings, [](const auto& r) { return r.opened; }))
					return;
				Configuration::Change& ch = *node.config_->change;
				if (ch.continuation) return node.postContinuation();
				if (ch.continuationPosted && !ch.continuationDone) return;
				node.feed(node.aggregate_.changeReady());
			});
		}

		/// Every acquisition ready: the continuation posted to its loop as the node's own work, with the change's token
		/// (destroyed unrun with the work if its loop's state is gone). From here a newer change waits for the change.
		void postContinuation() {
			Configuration& c = *config_;
			Configuration::Change& ch = *c.change;
			auto work = std::move(*ch.continuation);
			ch.continuation.reset();
			ch.continuationPosted = true;
			c.loops.at(ch.continuationLoop)
				->postState([work = std::move(work),
			                 done = ConfigureDone(std::make_unique<ContinuationAnswer>(c.channel, ch.epoch))](
								void* state) mutable { work(state, std::move(done)); });
			feed(aggregate_.continued());
		}

		/// The continuation answered: done, the commit point comes once nothing else is pending; failed (or its token
		/// dropped), the change falls back. Ignored once the change ended any other way.
		void continuationAnswered(std::uint64_t generation, std::uint64_t epoch, std::optional<std::string> failure) {
			if (!inChange(generation, epoch) || !config_->change->continuationPosted ||
			    config_->change->continuationDone)
				return;
			if (failure) return fallBack(lc::NodeCause::FellBack::failed, std::move(*failure));
			config_->change->continuationDone = true;
			checkChangeReady();
		}

		/// A fault found in a change, as an input of its own.
		void faultLater(lc::Fact fact, std::string detail) {
			host_.inputFor(serial_,
			               [generation = config_->generation, fact, detail = std::move(detail)](Node& node) mutable {
							   if (node.current(generation)) node.feed(node.aggregate_.fault(fact, std::move(detail)));
						   });
		}

		/// A fallback the change's answer asked, as an input of its own.
		void fallBackLater(std::uint64_t epoch, lc::NodeCause::FellBack reason, std::string detail) {
			host_.inputFor(serial_, [generation = config_->generation, epoch, reason,
			                         detail = std::move(detail)](Node& node) mutable {
				if (node.inChange(generation, epoch)) node.fallBack(reason, std::move(detail));
			});
		}

		/// The fallback (docs/architecture.md#changes-in-place): said (ear.reconfigure-fell-back), then a restart with the
		/// newest settings.
		void fallBack(lc::NodeCause::FellBack reason, std::string detail) {
			const Configuration::Change& ch = *config_->change;
			const std::int64_t ms = millisecondsBetween(ch.began, host_.timers.now());
			const std::array fields{DiagnosticField{"elapsedMs", ms}};
			std::string keys;
			for (const std::string& key : ch.keys) keys += (keys.empty() ? "" : ", ") + key;
			diagnose({.code = "ear.reconfigure-fell-back",
			          .detail = "could not apply " + keys + " in place (" + std::string(lc::fellBackName(reason)) +
			                    "): " + detail,
			          .fields = {fields.begin(), fields.end()}});
			feed(aggregate_.fellBack(reason, std::move(detail)));
		}

		/// A mask over `from`'s outputs (outputs then inside outputs) as one over `to`'s: each output by its name.
		static std::vector<std::uint8_t> outputsIn(const Ports& to, const Ports& from,
		                                           std::span<const std::uint8_t> mask) {
			std::vector<PortName> was = from.outputs();
			was.insert(was.end(), from.insideOutputs().begin(), from.insideOutputs().end());
			std::vector<PortName> now = to.outputs();
			now.insert(now.end(), to.insideOutputs().begin(), to.insideOutputs().end());
			std::vector<std::uint8_t> moved(now.size(), 0);
			for (std::size_t i = 0; i < was.size() && i < mask.size(); ++i) {
				if (mask[i] == 0) continue;
				const auto found = std::ranges::find(now, was[i]);
				if (found != now.end()) moved[static_cast<std::size_t>(found - now.begin())] = 1;
			}
			return moved;
		}

		/// The routable output banks named, as a mask over the block's outputs (outputs then inside outputs).
		std::vector<std::uint8_t> outputMask(std::span<const std::string_view> banks, bool inside) const {
			std::vector<std::uint8_t> mask(config_->fadedCount, 0);
			for (const std::string_view name : banks) {
				// The banks of the code running, which a reshape in place keeps until its commit point.
				const Ports::Bank& bank = config_->ports.bank(name);
				if (bank.direction != Direction::output || (bank.reach != PortBank::Reach::routable && !inside))
					throw std::logic_error("hold names a routable output bank, not \"" + bank.name + "\"");
				for (std::size_t i = 0; i < bank.ports.size() && bank.first + i < mask.size(); ++i)
					mask[bank.first + i] = 1;
			}
			return mask;
		}

		/// A change's hold or release of these banks: told to the block only while it is driven (a stopped node is
		/// silent already), each hold's acknowledgement bounded by 100 ms. `inside`: the framework's own hold, which
		/// may take an inside output (a playback card's bank).
		void holdBanks(std::span<const std::string_view> banks, bool hold, bool inside = false) {
			Configuration& c = *config_;
			const std::vector<std::uint8_t> marked = outputMask(banks, inside);
			std::vector<std::uint8_t> changed(marked.size(), 0);
			bool any = false;
			for (std::size_t i = 0; i < marked.size() && i < c.held.size(); ++i)
				if (marked[i] != 0 && (c.held[i] != 0) != hold) {
					changed[i] = 1;
					c.held[i] = hold ? 1 : 0;
					any = true;
				}
			if (!any || !changeFromDriving() || host_.wedged) return;
			if (!hold) return block_->release(std::move(changed));
			const std::uint64_t asked = ++c.holdsAsked;
			Configuration::Change& ch = *c.change;
			if (!ch.holdBound)
				ch.holdBound = host_.timers.after(fadeAcknowledgeBound, [&host = host_, serial = serial_,
				                                                         generation = c.generation, epoch = ch.epoch] {
					host.inputFor(serial, [generation, epoch](Node& node) {
						if (!node.inChange(generation, epoch) || !node.holdsWaiting()) return;
						node.config_->change->holdGaveUp = true;
						const std::array fields{DiagnosticField{"ms", fadeAcknowledgeBound.count()}};
						node.diagnose({.code = "ear.fade-unacknowledged",
						               .detail = "the hold before a change in place was not acknowledged within " +
						                         std::to_string(fadeAcknowledgeBound.count()) +
						                         " ms, so the change went on",
						               .fields = {fields.begin(), fields.end()}});
						node.checkChangeReady();
					});
				});
			block_->hold(std::move(changed), [&host = host_, serial = serial_, generation = c.generation, asked] {
				host.inputFor(serial, [generation, asked](Node& node) {
					if (!node.current(generation)) return;
					node.config_->holdsAcked = std::max(node.config_->holdsAcked, asked);
					if (node.config_->change && !node.holdsWaiting()) {
						node.config_->change->holdBound.reset();
						node.checkChangeReady();
					}
				});
			});
		}

		/// A replacement in a change (docs/writing-a-node-type.md#make-before-break): an RTP receiver or a sound card for
		/// the one on the same inside bank, recorded now and judged once the change version answered.
		void attachInChange(std::string_view bank, EngineCompanionShape shape, Order order) {
			Configuration& c = *config_;
			const std::string name(bank);
			const auto* receiver = std::get_if<RtpReceiverShape>(&shape);
			const bool card = std::holds_alternative<SoundCardShape>(shape);
			if (receiver == nullptr && !card)
				throw std::logic_error("a change replaces an RTP receiver or a sound card, never a pipe receiver");
			const Ports::Bank& found =
				card ? cardBank(ports_, name) : insideBank(ports_, name, Direction::input, "an RTP receiver");
			const auto at = std::ranges::find_if(
				c.engineAsks, [&](const EngineAsk& asked) { return asked.target.insideBank == found.name; });
			if (at == c.engineAsks.end() || at->shape.index() != shape.index())
				throw std::logic_error("a change attaches only a replacement: bank " + found.name + " has no " +
				                       (card ? "sound card" : "RTP receiver"));
			const auto index = static_cast<std::size_t>(at - c.engineAsks.begin());
			if (std::ranges::any_of(c.change->replacements, [&](const auto& r) { return r.index == index; }))
				throw std::logic_error("a second replacement on bank " + found.name + " in one change");
			std::vector<Claim> claims = std::visit([](const auto& one) { return claimsOf(one); }, shape);
			// Two receivers bound to one port would split or duplicate its datagrams, so a receiver on the same port is
			// replaced break-first, whatever the type asks; on a new port, made before break. A card goes as the type
			// orders, since only it knows whether the new card is the same ALSA device
			// (docs/writing-a-node-type.md#make-before-break).
			const bool makeFirst =
				card ? order == Order::makeFirst : std::get<RtpReceiverShape>(at->shape).port != receiver->port;
			c.change->replacements.push_back(Configuration::Change::Replacement{
				.index = index, .shape = std::move(shape), .claims = std::move(claims), .makeFirst = makeFirst});
		}

		/// The ring companion of `family` on bank `name`, which a change replaces: its index among the configuration's
		/// sinks or sources. A bank with none, or one replaced already in this change, is a broken rule.
		template <class Slots, class Replacements>
		std::size_t ringReplaced(const Slots& slots, const Replacements& replacements, const std::string& name,
		                         std::string_view what) {
			const auto at = std::ranges::find_if(slots, [&](const auto& slot) {
				if constexpr (requires { slot->bank; }) return slot->bank == name;
				else return slot.bank == name;
			});
			if (at == slots.end())
				throw std::logic_error("a change attaches only a replacement: bank " + name + " has no " +
				                       std::string(what));
			const auto index = static_cast<std::size_t>(at - slots.begin());
			if (std::ranges::any_of(replacements, [&](const auto& r) { return r.index == index; }))
				throw std::logic_error("a second replacement on bank " + name + " in one change");
			return index;
		}

		/// A sink's replacement in a change (docs/writing-a-node-type.md#sinks-and-sources-in-a-change): made now beside
		/// the old one, by creation's rules, its consumer begun once the change's claims and recordings are ready. A
		/// consumer in a loop's state may not be the one the old sink calls: two sinks would call it at once.
		SinkControl& sinkInChange(std::string_view bank, SinkShape shape, SinkConsumerFrom consumer) {
			Configuration& c = *config_;
			Configuration::Change& ch = *c.change;
			const Ports::Bank& found = insideBank(ports_, std::string(bank), Direction::output, "a sink");
			const std::size_t index = ringReplaced(c.sinks, ch.sinks, found.name, "sink");
			if (auto* own = std::get_if<std::unique_ptr<SinkConsumer>>(&consumer)) {
				if (!*own) throw std::logic_error("sink " + found.name + " given no consumer");
			} else {
				const LoopSlot& slot = *c.loops.at(std::get<StateSinkConsumer>(consumer).loop);
				if (!slot.declared.stateful)
					throw std::logic_error("sink " + found.name + " is consumed by loop " +
					                       std::string(slot.declared.name) + ", which has no state");
			}
			auto made = std::make_unique<SinkSlot>(found.name, std::move(consumer));
			if (made->stateLoop) {
				const SinkSlot& old = *c.sinks[index];
				if (old.resolved != nullptr &&
				    made->stateConsumer(c.loops.at(*made->stateLoop)->state.get()) == old.resolved)
					throw std::logic_error("the replacement for sink " + found.name +
					                       " is consumed by the consumer it replaces");
			}
			SinkAdapter& sinks = given(host_.resources.sinks(), "sinks");
			made->sink = adapter("SinkAdapter::make", [&] {
				auto sink = sinks.make(name_, found.name, static_cast<std::uint32_t>(found.ports.size()), shape.frames,
				                       host_.format.rate);
				if (!sink) throw std::logic_error("the sink adapter made no sink for bank " + found.name);
				return sink;
			});
			made->binding = SinkBinding{&made->sink->tap(), found.first, found.ports.size()};
			made->id = ++host_.companionIds;
			SinkControl& control = made->sink->control();
			ch.sinks.push_back(Configuration::Change::SinkReplacement{index, std::move(made)});
			return control;
		}

		/// A source's replacement in a change: made now beside the old one and ready at once, by creation's rules.
		SourceWriter& sourceInChange(std::string_view bank, SourceShape shape, std::unique_ptr<SourceClock> clock) {
			Configuration& c = *config_;
			Configuration::Change& ch = *c.change;
			const Ports::Bank& found = insideBank(ports_, std::string(bank), Direction::input, "a source");
			if (shape.frames == 0) throw std::logic_error("a source on bank " + found.name + " holds no frames");
			if (shape.frames > maxSourceFrames)
				throw std::logic_error("a source on bank " + found.name + " holds more than " +
				                       std::to_string(maxSourceFrames) + " frames");
			const std::size_t index = ringReplaced(c.sources, ch.sources, found.name, "source");
			SourceAdapter& sources = given(host_.resources.sources(), "sources");
			std::unique_ptr<Source> made = adapter("SourceAdapter::make", [&] {
				auto source = sources.make(name_, found.name, static_cast<std::uint32_t>(found.ports.size()),
				                           shape.frames, host_.format.rate, std::move(clock));
				if (!source) throw std::logic_error("the source adapter made no source for bank " + found.name);
				return source;
			});
			SourceSlot slot{found.name, std::move(made), {}};
			slot.binding = SourceBinding{&slot.source->tap(), found.first, found.ports.size()};
			SourceWriter& writer = slot.source->writer();
			ch.sources.push_back(Configuration::Change::SourceReplacement{index, std::move(slot)});
			return writer;
		}

		/// A recording's replacement in a change (docs/writing-a-node-type.md#recordings-in-a-change): claimed with the
		/// change's claims, opened beside the old one once they are granted. `replaced` is one the configuration holds;
		/// its own path, or one another recording holds, is a broken rule.
		Recording& recordingInChange(const Recording& replaced, const RecordingRequest& request) {
			Configuration& c = *config_;
			Configuration::Change& ch = *c.change;
			const auto at = std::ranges::find_if(
				c.recordings, [&](const auto& slot) { return static_cast<const Recording*>(slot.get()) == &replaced; });
			if (at == c.recordings.end())
				throw std::logic_error("a change replaces a recording its configuration holds, not " + replaced.path());
			const auto index = static_cast<std::size_t>(at - c.recordings.begin());
			if (std::ranges::any_of(ch.recordings, [&](const auto& r) { return r.index == index; }))
				throw std::logic_error("a second replacement for recording " + replaced.path() + " in one change");
			std::unique_ptr<RecordingSlot> made;
			if (auto refused = recordingPathRefused(request.path)) {
				made = std::make_unique<RecordingSlot>(request.path, nullptr, name_, std::move(refused));
			} else {
				const std::string claimed = recordingClaimPath(request.path);
				const auto holds = [&](const auto& slot) { return slot->claimPath() == claimed; };
				if (std::ranges::any_of(c.recordings, holds) ||
				    std::ranges::any_of(ch.recordings, [&](const auto& r) { return holds(r.slot); }))
					throw std::logic_error(claimKey(Claim{Claim::Kind::recording, claimed}) + " claimed twice");
				made = std::make_unique<RecordingSlot>(request.path, &given(host_.resources.recordings(), "recordings"),
				                                       name_);
			}
			made->id = ++host_.companionIds;
			Recording& recording = *made;
			ch.recordings.push_back(Configuration::Change::RecordingReplacement{index, std::move(made)});
			return recording;
		}

		/// A replacement recording's open landed: made, the change may go on; refused, it falls back (resource-refused),
		/// as the restart would fail.
		void replacementRecordingOpened(std::uint64_t generation, std::uint64_t id,
		                                std::expected<void, std::string> outcome,
		                                std::optional<detail::IoFault> fault) {
			if (!current(generation) || !config_->change ||
			    aggregate_.configuration().state() != lc::ConfigState::changing)
				return;
			for (auto& r : config_->change->recordings) {
				if (r.slot->id != id || r.opened) continue;
				if (fault)
					return ioJobFaulted(generation, "open recording " + r.slot->path(), fault->callback, fault->what);
				if (!outcome)
					return fallBack(lc::NodeCause::FellBack::resourceRefused,
					                "cannot open " + r.slot->path() + ": " + outcome.error());
				r.slot->opened();
				r.opened = true;
				return checkChangeReady();
			}
		}

		/// Each recording a change retired whose commit completed is closed (on the I/O thread) once no sink the change
		/// replaced still writes it, and then, last, its claim is released, so a next open of its path comes after the
		/// close.
		void closeRetiredRecordings() {
			Configuration& c = *config_;
			if (!retiredSinksClosed(c)) return;
			for (auto& slot : c.retiredRecordings) {
				if (!std::exchange(slot->due, false)) continue;
				slot->close();
				const std::string key = claimKey(Claim{Claim::Kind::recording, slot->claimPath()});
				if (!holdsClaim(key)) continue;
				std::erase_if(c.claims, [&](const ClaimSlot& claim) { return claimKey(claim.claim) == key; });
				host_.claimsDecided(host_.claims.release(c.generation, key));
			}
		}

		/// A replacement sink's consumer began: the change may go on.
		void replacementSinkBegun(std::uint64_t generation, std::uint64_t id) {
			host_.inputFor(serial_, [generation, id](Node& node) {
				if (!node.current(generation) || !node.config_->change) return;
				for (auto& r : node.config_->change->sinks)
					if (r.slot->id == id && !r.ready) {
						r.ready = true;
						node.refreshCompanions();
						node.checkChangeReady();
					}
			});
		}

		/// Every sink a change retired whose close is not yet asked: drained to its own consumer and finished, each on
		/// its own thread, once nothing taps it.
		void closeRetiredSinks() {
			Configuration& c = *config_;
			for (auto& slot : c.retiredSinks)
				if (!std::exchange(slot->closeAsked, true))
					slot->sink->close(c.channel->retiredSinkClosedCall(slot->id));
		}

		/// A retired sink closed: its drops are the node's, it is freed, and a teardown waiting for it goes on.
		void retiredSinkClosed(std::uint64_t generation, std::uint64_t id, std::uint64_t droppedFrames) {
			host_.inputFor(serial_, [generation, id, droppedFrames](Node& node) {
				if (!node.current(generation)) return;
				auto& retired = node.config_->retiredSinks;
				const auto at = std::ranges::find_if(retired, [&](const auto& slot) { return slot->id == id; });
				if (at == retired.end() || (*at)->closed) return;
				node.dropped_ += droppedFrames;
				(*at)->closed = true;
				// Kept while a continuation that may hold its control can still run (Change::SinkReplacement).
				if (!(*at)->kept) retired.erase(at);
				node.closeRetiredRecordings();
				node.closeLoops();
			});
		}

		/// cx.claim in a change (docs/writing-a-node-type.md#claims-in-a-change): a port, recorded now and judged when
		/// the change version returned. Asked twice is the type's bug, as on creation.
		void claimInChange(Claim claim) {
			if (claim.kind != Claim::Kind::tcp && claim.kind != Claim::Kind::udp)
				throw std::logic_error("claim takes a port: " + claimKey(claim) + " is the framework's to claim");
			Configuration::Change& ch = *config_->change;
			const std::string key = claimKey(claim);
			if (std::ranges::any_of(ch.claimed, [&](const Claim& one) { return claimKey(one) == key; }))
				throw std::logic_error(key + " claimed twice");
			ch.claimed.push_back(std::move(claim));
		}

		/// cx.unclaim: a port the configuration holds, released at the commit point, last.
		void unclaimInChange(const Claim& claim) {
			const std::string key = claimKey(claim);
			if (!holdsClaim(key)) throw std::logic_error("unclaim names a port the configuration holds, not " + key);
			std::vector<std::string>& unclaimed = config_->change->unclaimed;
			if (std::ranges::find(unclaimed, key) == unclaimed.end()) unclaimed.push_back(key);
		}

		/// Whether the configuration holds the claim of `key`.
		bool holdsClaim(const std::string& key) const {
			return std::ranges::any_of(config_->claims,
			                           [&](const ClaimSlot& slot) { return claimKey(slot.claim) == key; });
		}

		/// Once the change version answered: the companions it replaces are retiring from now on, and every claim
		/// of the change the configuration does not hold yet (a key it holds is its own: granted, never probed) is
		/// judged at once, before anything else is acquired. The registry decides now, so two nodes whose changes
		/// in one edit swap ports each find the other's port held by a configuration that runs, and both fall back
		/// (docs/writing-a-node-type.md#claims-in-a-change); the decisions are heard in a later turn, each as an
		/// input of its own.
		void judgeChangeClaims() {
			Configuration& c = *config_;
			Configuration::Change& ch = *c.change;
			for (auto& r : ch.replacements)
				if (r.index < c.engineCompanions.size() && c.engineCompanions[r.index])
					r.retiringId = c.engineCompanions[r.index]->id();
			const auto ask = [&](const Claim& claim) {
				const std::string key = claimKey(claim);
				if (holdsClaim(key) ||
				    std::ranges::any_of(ch.added, [&](const auto& one) { return claimKey(one.claim) == key; }))
					return;
				ch.added.push_back(Configuration::Change::Added{claim});
			};
			for (const Claim& claim : ch.claimed) ask(claim);
			for (const auto& r : ch.recordings) ask(Claim{Claim::Kind::recording, r.slot->claimPath()});
			for (const auto& r : ch.replacements)
				for (const Claim& claim : r.claims) ask(claim);
			ch.claimsPending = ch.added.size();
			if (ch.added.empty()) return;
			std::vector<detail::ClaimDecided> decided;
			for (const auto& added : ch.added) {
				const detail::ClaimHolder holder{c.generation, serial_, name_};
				decided.push_back(detail::ClaimDecided{holder, added.claim, host_.claims.claim(holder, added.claim)});
			}
			host_.mainLoop.post(
				[&host = host_, alive = std::weak_ptr<char>(host_.alive), decided = std::move(decided)]() mutable {
					if (alive.lock()) host.claimsDecided(std::move(decided));
				});
		}

		/// The change's claim of `claim`'s key, if any.
		Configuration::Change::Added* addedAsking(const Claim& claim) {
			if (!config_->change) return nullptr;
			const std::string key = claimKey(claim);
			for (auto& added : config_->change->added)
				if (claimKey(added.claim) == key) return &added;
			return nullptr;
		}

		/// A claim the change asked, judged: granted, probed, refused (the change falls back, resource-refused,
		/// before anything was broken) or waiting for its holder, which is closing (against the change's deadline).
		/// False for a claim no change in flight asked.
		bool changeClaimJudged(const Claim& claim, const detail::ClaimJudged& judged) {
			Configuration::Change::Added* added = addedAsking(claim);
			if (added == nullptr || added->granted || aggregate_.configuration().state() != lc::ConfigState::changing)
				return false;
			if (judged.outcome != detail::ClaimJudged::Outcome::waits) claimWaitEnded(claimKey(claim));
			switch (judged.outcome) {
			case detail::ClaimJudged::Outcome::granted:
				changeClaimReady(claim);
				break;
			case detail::ClaimJudged::Outcome::probe:
				probeChangeClaim(claim);
				break;
			case detail::ClaimJudged::Outcome::refused:
				fallBack(lc::NodeCause::FellBack::resourceRefused, claimText(claim, judged.why));
				break;
			case detail::ClaimJudged::Outcome::waits:
				claimWaits(claimKey(claim), "held by node " + judged.why + ", which is closing");
				break;
			}
			return true;
		}

		void changeClaimReady(const Claim& claim) {
			Configuration::Change::Added* added = addedAsking(claim);
			if (added == nullptr || added->granted) return;
			added->granted = true;
			--config_->change->claimsPending;
			checkChangeReady();
		}

		/// A change's port granted by the registry, probed behind the turn's flush as a readiness claim is.
		void probeChangeClaim(const Claim& claim) {
			PortProbe* probe = host_.resources.ports();
			if (probe == nullptr) return changeClaimReady(claim);
			host_.mainLoop.post([&host = host_, alive = std::weak_ptr<char>(host_.alive), probe, serial = serial_,
			                     generation = config_->generation, epoch = config_->change->epoch, claim] {
				if (!alive.lock()) return;
				const MainLoopActivity::Scope activity("probe port", claimKey(claim));
				const auto port = static_cast<std::uint16_t>(std::stoul(claim.value));
				std::expected<void, std::string> outcome;
				std::optional<std::string> fault;
				try {
					outcome = claim.kind == Claim::Kind::tcp ? probe->tcpListen(port) : probe->udp(port, claim.group);
				} catch (...) {
					fault = detail::whatOf(std::current_exception());
				}
				host.inputFor(serial, [generation, epoch, claim, outcome = std::move(outcome),
				                       fault = std::move(fault)](Node& node) mutable {
					if (!node.inChange(generation, epoch)) return;
					if (fault) {
						const std::string callback = "PortProbe";
						node.host_.internalError(InternalError{.callback = callback,
						                                       .node = node.name_,
						                                       .where = "the probe of " + claimKey(claim),
						                                       .what = *fault});
						return node.feed(
							node.aggregate_.fault(lc::Fact::adapterFaulted, internalErrorDetail(callback, *fault)));
					}
					if (!outcome)
						return node.fallBack(lc::NodeCause::FellBack::resourceRefused,
						                     claimText(claim, outcome.error()));
					node.changeClaimReady(claim);
				});
			});
		}

		/// The public output banks the inside bank `index`'s companion feeds: through the pass-through, the banks its
		/// pairs copy that bank to; a code of the type's own, every routable output.
		std::vector<std::string_view> fedBy(std::size_t index) const {
			const std::string& bank = config_->engineAsks[index].target.insideBank;
			// A playback card's bank is an inside output: what it carries is the bank itself.
			if (config_->engineAsks[index].target.bank == Direction::output) return {std::string_view(bank)};
			std::vector<std::string_view> fed;
			for (const Ports::Bank& one : ports_.banks()) {
				if (one.direction != Direction::output || one.reach != PortBank::Reach::routable) continue;
				const bool copied = std::ranges::any_of(
					config_->pairs, [&](const BankPair& pair) { return pair.from == bank && pair.to == one.name; });
				if (config_->pairs.empty() || copied) fed.push_back(one.name);
			}
			return fed;
		}

		/// Break-first (docs/architecture.md#changes-in-place): a replacement whose claims are all granted holds what its
		/// bank feeds; once that is acknowledged, the old companion is destroyed and the new one made, in one flush, the
		/// bank's companion the new one's from here on.
		void stepReplacements() {
			Configuration& c = *config_;
			if (c.change->claimsPending > 0) return; // claims first, before anything is broken or made
			// Then files: each replacement recording opened beside the old one, before any companion, so a consumer
			// that writes it finds it open at its begin.
			for (auto& r : c.change->recordings)
				if (!std::exchange(r.asked, true))
					r.slot->open(&host_, serial_, c.generation, 0, c.channel->completions());
			if (!std::ranges::all_of(c.change->recordings, [](const auto& r) { return r.opened; })) return;
			// Each replacement sink's consumer begins beside the old one's, which goes on consuming until the commit.
			for (auto& r : c.change->sinks) {
				if (std::exchange(r.asked, true)) continue;
				SinkSlot& slot = *r.slot;
				void* state = slot.stateLoop ? c.loops.at(*slot.stateLoop)->state.get() : nullptr;
				slot.begin(c.channel, c.channel->replacementBegunCall(slot.id), state);
			}
			bool listed = false;
			for (auto& r : c.change->replacements) {
				if (r.broken || r.made) continue;
				// What the bank feeds is held either way: two sources into one inside input would mix for a moment.
				const std::vector<std::string_view> fed = fedBy(r.index);
				holdBanks(fed, true, true);
				if (holdsWaiting()) continue;
				r.oldClaims =
					std::visit([](const auto& shape) { return claimsOf(shape); }, c.engineAsks[r.index].shape);
				if (r.retiringId != 0) c.retiredIds.push_back(r.retiringId);
				if (r.makeFirst) {
					r.made = true;
					listed = true;
					if (r.index < c.engineCompanions.size() && c.engineCompanions[r.index])
						c.retiring.push_back(Configuration::RetiringCompanion{r.index, c.engineAsks[r.index],
						                                                      std::move(c.engineCompanions[r.index])});
				} else {
					r.broken = true;
					if (r.index < c.engineCompanions.size() && c.engineCompanions[r.index])
						destroyCompanionLater(std::move(c.engineCompanions[r.index]));
				}
				c.engineAsks[r.index].shape = r.shape;
				r.id = ++host_.companionIds;
				if (c.replacing.size() <= r.index) c.replacing.resize(r.index + 1, 0);
				c.replacing[r.index] = r.id;
				host_.queue(FlushKind::makeCompanion, name_,
				            [&host = host_, serial = serial_, generation = c.generation, index = r.index, id = r.id] {
								if (Node* node = host.find(serial)) node->makeReplacement(generation, index, id);
							});
			}
			if (listed) refreshCompanions();
		}

		/// An engine companion destroyed in the turn's flush; leaked once the data loop is wedged, since destroying it
		/// calls into that loop.
		void destroyCompanionLater(std::unique_ptr<Companion> companion) {
			host_.queue(FlushKind::destroyCompanion, name_,
			            [&host = host_, companion = std::move(companion)]() mutable {
							if (!host.wedged) {
								companion.reset();
								return;
							}
							static_cast<void>(companion.release());
							++host.leaked;
						});
		}

		/// At the commit point: each companion a make-first replacement was made beside is destroyed, and the bank is
		/// its replacement's (so its claims settle as a break's do).
		void retireMadeFirst(Configuration::Change& ch) {
			Configuration& c = *config_;
			for (auto& r : ch.replacements) {
				if (!r.made) continue;
				r.broken = true;
				for (auto& retiring : c.retiring)
					if (retiring.index == r.index && retiring.companion)
						destroyCompanionLater(std::move(retiring.companion));
				std::erase_if(c.retiring, [&](const auto& retiring) { return retiring.index == r.index; });
			}
		}

		/// A change ending uncommitted: each make-first replacement made (or asked for) is destroyed, and the companion
		/// it was made beside is the bank's again, no longer retiring.
		void restoreMadeFirst(Configuration::Change& ch) {
			Configuration& c = *config_;
			for (auto& r : ch.replacements) {
				if (!r.made) continue;
				r.made = false;
				if (r.index < c.engineCompanions.size() && c.engineCompanions[r.index])
					destroyCompanionLater(std::move(c.engineCompanions[r.index]));
				if (r.index < c.replacing.size()) c.replacing[r.index] = 0; // a make still queued makes nothing
				const auto found =
					std::ranges::find_if(c.retiring, [&](const auto& retiring) { return retiring.index == r.index; });
				if (found == c.retiring.end()) continue;
				c.engineAsks[r.index] = found->ask;
				if (found->companion) std::erase(c.retiredIds, found->companion->id());
				c.engineCompanions[r.index] = std::move(found->companion);
				c.retiring.erase(found);
			}
		}

		/// A flush item: the replacement on engine companion `index` made, unless its configuration moved on or a newer
		/// break asked for another in its place (that one's own item makes it, with the shape the ask now has).
		void makeReplacement(std::uint64_t generation, std::size_t index, std::uint64_t id) {
			if (host_.wedged || !current(generation) || config_->tearing || !block_ ||
			    index >= config_->engineCompanions.size() || config_->engineCompanions[index] ||
			    index >= config_->replacing.size() || config_->replacing[index] != id)
				return;
			Configuration& c = *config_;
			auto& companion =
				*(c.engineCompanions[index] = std::make_unique<Companion>(host_, serial_, c.generation, index, id));
			EngineAsk& asked = c.engineAsks[index];
			asked.target.blockId = block_->blockId();
			try {
				companion.made = std::visit(
					[&](const auto& shape) {
						return host_.resources.adapterFor(shape)->make(asked.target, shape, companion);
					},
					asked.shape);
			} catch (...) {
				c.engineCompanions[index].reset();
				adapterFaulted(companionMaker(asked.shape), [this, generation](std::string detail) {
					host_.inputFor(serial_, [generation, detail = std::move(detail)](Node& node) mutable {
						if (node.current(generation))
							node.feed(node.aggregate_.fault(lc::Fact::adapterFaulted, std::move(detail)));
					});
				});
			}
		}

		/// A replacement companion is ready (a configuration's companion ready after its readiness ended is one).
		void replacementReady(std::uint64_t id) {
			if (!config_->change) return;
			for (auto& r : config_->change->replacements)
				if (r.id == id && !r.ready) {
					r.ready = true;
					refreshCompanions();
					checkChangeReady();
				}
		}

		/// An engine companion's loss while a change is in flight or after one replaced it: a retiring companion's is
		/// said (ear.retiring-lost), never a fault; a replacement's before the commit point falls the change back
		/// (resource-lost). False for any other loss, which fails the node as it always does.
		bool companionLostInChange(std::size_t index, std::uint64_t id, const std::string& detail) {
			Configuration& c = *config_;
			const bool retiring = std::ranges::find(c.retiredIds, id) != c.retiredIds.end() ||
			                      (c.change && std::ranges::any_of(c.change->replacements, [&](const auto& r) {
									   return r.retiringId == id && !r.broken;
								   }));
			if (retiring) {
				const std::string companion =
					index < c.engineAsks.size() ? c.engineAsks[index].target.engineNode : name_;
				diagnose({.code = "ear.retiring-lost",
				          .subject = companion,
				          .detail = companion + ", which a change in place replaces, was lost: " + detail});
				return true;
			}
			if (c.change && aggregate_.configuration().state() == lc::ConfigState::changing &&
			    std::ranges::any_of(c.change->replacements, [&](const auto& r) { return r.id == id; })) {
				fallBack(lc::NodeCause::FellBack::resourceLost, detail);
				return true;
			}
			return false;
		}

		/// The change's claims settle (docs/writing-a-node-type.md#claims-in-a-change). At the commit point
		/// (`committed`): every claim it added is the configuration's; then, last, the ports no longer asked are
		/// released (a replaced companion's its replacement does not ask, and every key unclaimed that the change
		/// does not claim again), so a node waiting for one is granted it only now. Ending uncommitted: the claims a
		/// replacement already made asks stay the configuration's, as that replacement does; every other claim it
		/// added is released, and every wait of it dropped.
		void settleClaims(Configuration::Change& ch, bool committed) {
			Configuration& c = *config_;
			const auto key = [](const Claim& claim) { return claimKey(claim); };
			const auto asks = [&](const std::vector<Claim>& claims, const std::string& k) {
				return std::ranges::any_of(claims, [&](const Claim& one) { return key(one) == k; });
			};
			for (const auto& added : ch.added) {
				const std::string k = key(added.claim);
				const bool kept = committed || std::ranges::any_of(ch.replacements, [&](const auto& r) {
									  return r.broken && asks(r.claims, k);
								  });
				if (kept) c.claims.push_back(ClaimSlot{added.claim});
				else host_.claimsDecided(host_.claims.release(c.generation, k));
			}
			std::vector<std::string> released;
			for (const auto& r : ch.replacements) {
				if (!r.broken) continue;
				for (const Claim& old : r.oldClaims)
					if (!asks(r.claims, key(old))) released.push_back(key(old));
			}
			if (committed)
				for (const std::string& k : ch.unclaimed)
					if (!asks(ch.claimed, k)) released.push_back(k);
			for (const std::string& k : released) {
				if (!holdsClaim(k)) continue;
				std::erase_if(c.claims, [&](const ClaimSlot& slot) { return key(slot.claim) == k; });
				host_.claimsDecided(host_.claims.release(c.generation, k));
			}
		}

		/// The commit point: the instance adopts the new settings, the configuration takes them, its staged posts
		/// are released to their loops in order, its plan and its code are the change's; driven, the plan is carried and a
		/// new code swapped in one invoke (its completion the commit's), else the commit completes in this turn; the holds
		/// fade back in. Undriven, the old code is released now; driven, once the swap completed.
		void commit(std::uint64_t barrier) {
			Configuration& c = *config_;
			if (!c.change) return;
			Configuration::Change ch = std::move(*c.change);
			c.change.reset();
			ch.deadline.reset();
			ch.holdBound.reset();
			c.instance->adopt(*ch.target);
			c.config = ch.target; // the old config goes once nothing reads it: the instance reads the new one
			for (auto& [loop, work] : ch.posts) c.loops.at(loop)->postState(std::move(work));
			c.plan = std::move(ch.plan);
			if (ch.latency) c.latency = std::move(*ch.latency);
			if (ch.codeGiven) {
				c.retiringCode = std::move(c.code);
				c.retiringPrepared = c.prepared;
				c.code = std::move(ch.code);
				c.pairs = std::move(ch.pairs);
				c.prepared = ch.codePrepared;
			}
			std::vector<std::uint8_t> held(c.held.size(), 0);
			std::swap(held, c.held);
			if (ch.reshape) {
				// From the commit point the code is laid out for the new ports: the holds it releases are named there,
				// and every output of the new ports is faded (a reshape in place has no sink's bank).
				held = outputsIn(ch.target->ports(), c.ports, held);
				c.ports = ch.target->ports();
				c.fadedCount = c.ports.outputs().size() + c.ports.insideOutputs().size();
				c.faded = std::make_unique<bool[]>(c.fadedCount);
				std::fill_n(c.faded.get(), c.fadedCount, true);
				c.held.assign(c.fadedCount, 0);
			}
			retireMadeFirst(ch);
			const bool rings = swapRings(ch);
			refreshCompanions();
			if (barrier == 0) { // nothing driven: the machine is back where it began
				commitCompleted();
				settleClaims(ch, true); // last
				return retireCode();
			}
			if (!host_.wedged) block_->carryLatency(c.plan);
			if (!ch.codeGiven && !rings) {
				host_.inputFor(serial_, [generation = c.generation, barrier](Node& node) {
					if (!node.current(generation)) return;
					node.commitCompleted();
					node.feed(node.aggregate_.committed(barrier));
				});
			} else if (host_.wedged) {
				overdueAtOnce(barrier, Bound::barrier);
			} else {
				block_->swap(DriveRequest{c.code.get(), std::span<const bool>(c.faded.get(), c.fadedCount), c.bindings,
				                          c.sourceBindings, &c.ports},
				             swapped(barrier));
				arm(barrier, Bound::barrier);
			}
			if (!host_.wedged && std::ranges::any_of(held, [](std::uint8_t one) { return one != 0; }))
				block_->release(std::move(held));
			settleClaims(ch, true); // last: a released port may grant another node
		}

		/// At the commit point: each ring companion the change replaced gives its bank to its replacement, whose binding
		/// the commit's invoke carries (so the old sink takes the cycles before it and the new one those from it, and two
		/// sources never play into one bank). The old sinks are retired, closed once nothing taps them; the old sources'
		/// writers are closed now. Whether there was any.
		bool swapRings(Configuration::Change& ch) {
			Configuration& c = *config_;
			for (auto& r : ch.sinks) {
				r.slot->item = c.sinks[r.index]->item; // its readiness is the bank's, which the replacement now holds
				c.retiredSinks.push_back(std::exchange(c.sinks[r.index], std::move(r.slot)));
				c.bindings[r.index] = c.sinks[r.index]->binding;
			}
			for (auto& r : ch.sources) {
				c.sources[r.index].source->close();
				c.retiredSources.push_back(std::exchange(c.sources[r.index], std::move(r.slot)));
				c.sourceBindings[r.index] = c.sources[r.index].binding;
			}
			for (auto& r : ch.recordings) {
				c.retiredRecordings.push_back(std::exchange(c.recordings[r.index], std::move(r.slot)));
				c.retiredRecordings.back()->awaiting = true;
			}
			return !ch.sinks.empty() || !ch.sources.empty();
		}

		/// The commit completed (its swap, or a commit with no invoke): the recordings it retired close once no sink it
		/// replaced still writes them.
		void commitCompleted() {
			closeRetiredSinks(); // nothing taps them from here
			for (auto& slot : config_->retiredRecordings)
				if (std::exchange(slot->awaiting, false)) slot->due = true;
			closeRetiredRecordings();
		}

		/// A change ending uncommitted: each replacement sink is closed (never tapped, its consumer finished if it
		/// began), each replacement source's writer closed; once the continuation was posted they are kept until step
		/// 5, since it may hold them.
		void dropRings(Configuration::Change& ch) {
			Configuration& c = *config_;
			for (auto& r : ch.sinks) {
				r.slot->kept = ch.continuationPosted;
				c.retiredSinks.push_back(std::move(r.slot));
			}
			ch.sinks.clear();
			for (auto& r : ch.sources) {
				r.slot.source->close();
				c.retiredSources.push_back(std::move(r.slot));
			}
			ch.sources.clear();
			for (auto& r : ch.recordings) {
				r.slot->close(); // before settleClaims releases its claim
				c.retiredRecordings.push_back(std::move(r.slot));
			}
			ch.recordings.clear();
			closeRetiredSinks();
		}

		/// The swap's completion: the old code released, then the commit's completion heard.
		std::move_only_function<void(std::uint64_t)> swapped(std::uint64_t barrier) {
			return [&host = host_, serial = serial_, generation = config_->generation, barrier](std::uint64_t) {
				if (Node* node = host.find(serial)) ++node->completionsRun_;
				host.inputFor(serial, [generation, barrier](Node& node) {
					if (!node.current(generation)) return;
					if (node.aggregate_.configuration().barrier() == barrier) node.config_->bound.reset();
					node.retireCode();
					node.commitCompleted();
					node.feed(node.aggregate_.committed(barrier));
				});
			};
		}

		/// The code a swap replaced: released (it was prepared) and freed, now that no process() of it runs.
		void retireCode() {
			if (!config_ || !config_->retiringCode) return;
			if (std::exchange(config_->retiringPrepared, false)) {
				const MainLoopActivity::Scope activity("release", name_);
				config_->retiringCode->release();
			}
			config_->retiringCode.reset();
		}

		/// The change ends uncommitted: its staged posts destroyed unrun, its new code released and freed, its deadline
		/// cancelled; its holds stay (a newer change keeps them; a teardown's silence ends them). Superseded (still
		/// changing: a newer change follows at once), ear.reconfigure-superseded says what was abandoned.
		void dropChange() {
			if (!config_ || !config_->change) return;
			claimWaitsEnded();
			Configuration::Change& ch = *config_->change;
			if (aggregate_.configuration().state() == lc::ConfigState::changing) {
				std::string keys;
				for (const std::string& key : ch.keys) keys += (keys.empty() ? "" : ", ") + key;
				diagnose({.code = "ear.reconfigure-superseded",
				          .detail = "a newer change superseded the change of " + keys + " before its commit point"});
			}
			if (ch.code && ch.codePrepared) {
				const MainLoopActivity::Scope activity("release", name_);
				ch.code->release();
			}
			restoreMadeFirst(ch);
			dropRings(ch);
			settleClaims(ch, false); // a companion already replaced break-first stays the bank's, with its claims
			config_->change.reset();
			refreshCompanions();
		}

		/// T5's completion: the instance's start(), timed; one that threw is a fault once this batch was carried out
		/// (it never set started, so no stop() pairs with it).
		void startHook() {
			if (!config_ || !config_->instance) return;
			// A context call it made and caught (a context kept past configure) still fails it.
			if (auto started = callType(hooks::start, [&] { config_->instance->start(); }); !started)
				startThrew_ = std::move(started.error().detail);
		}

		/// Before a silence that follows a start() that returned: the instance's stop(), timed. One that threw during a
		/// stop is a fault, held until the node rests; during a teardown facts are dropped, so it is only
		/// logged (docs/writing-a-node-type.md#rules-the-framework-enforces).
		void stopHook() {
			if (!config_ || !config_->instance) return;
			if (aggregate_.intent() != lc::Intent::stop) closeSources(); // teardown step 1 begins
			idleSources();
			auto stopped = callType(hooks::stop, [&] { config_->instance->stop(); });
			if (stopped) return;
			if (aggregate_.intent() == lc::Intent::stop) stopThrew_ = std::move(stopped.error().detail);
			else host_.logged(LogEntry{LogLevel::error, "ear", "stop() threw: " + stopped.error().what, {}, name_});
		}

		/// The code's prepare (T3, and again after each stop's silence released it). One that threw leaves the code
		/// unprepared: a drive in its batch drives no code, and the fault, once the batch was carried out, tears the
		/// configuration down (docs/writing-a-node-type.md#rules-the-framework-enforces).
		void prepare() {
			if (auto prepared = callType(hooks::prepare, [&] { config_->code->prepare(host_.format); }); !prepared) {
				prepareThrew_ = std::move(prepared.error().detail);
				return;
			}
			config_->prepared = true;
			// Configured: what was sent to its loops while it readied runs now, in the order sent (once: a stop's
			// silence releases the code and a start prepares it again).
			if (!std::exchange(config_->loopsReleased, true))
				for (const auto& slot : config_->loops) slot->release();
		}

		/// Readiness's companions step: the engine companions, made in the flush in request order once the
		/// block is ready (their ports exist); with none pending, every resource is ready at once (in this turn, as its
		/// own input). An adapter that tells ready() inside make() is heard after that item, and every ready is judged
		/// against every engine companion made.
		void askResources() {
			if (!config_ || config_->startFailed || config_->readyTold ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			if (c.engineAsks.empty()) {
				host_.inputFor(serial_, [generation = c.generation](Node& node) { node.checkReady(generation); });
				return;
			}
			// Made only once their step was asked (every earlier step ready) and the block is ready.
			if (aggregate_.block().state() != lc::BlockState::ready || c.companionsAsked ||
			    !c.readiness.isAsked(c.engineAsks.front().item))
				return;
			c.companionsAsked = true;
			c.engineCompanions.reserve(c.engineAsks.size());
			for (std::size_t i = 0; i < c.engineAsks.size(); ++i)
				host_.queue(FlushKind::makeCompanion, name_,
				            [&host = host_, serial = serial_, generation = c.generation, i] {
								if (Node* node = host.find(serial)) node->makeCompanion(generation, i);
							});
		}

		/// Every resource ready: T3, which the machine hears.
		void checkReady(std::uint64_t generation) {
			if (!current(generation) || config_->startFailed || config_->readyTold ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			// An engine companion's item is ready only once it was made, so a done list holds every one made.
			if (!c.readiness.done()) return;
			c.readyTold = true;
			feed(aggregate_.resourcesReady());
		}

		/// T5: the latency plan first, then the code, by an asynchronous drive that fades in every output the code
		/// writes; the barrier's bound (5 s) armed. A code whose prepare threw in this batch is never run: the drive
		/// carries no code, so the machine, told the fault behind it, silences a block that ran nothing of it.
		void drive(std::uint64_t barrier) {
			if (host_.wedged) return overdueAtOnce(barrier, Bound::barrier);
			Configuration& c = *config_;
			if (!c.prepared) {
				block_->drive(DriveRequest{nullptr, {}}, completion(barrier, &lc::NodeAggregate::driven));
				arm(barrier, Bound::barrier);
				return;
			}
			// The plan configure's return checked against the ports (latencyPlan), so carrying it cannot throw.
			block_->carryLatency(c.plan);
			std::ranges::fill(c.held, std::uint8_t{0}); // a drive ends every hold
			block_->drive(DriveRequest{c.code.get(), std::span<const bool>(c.faded.get(), c.fadedCount), c.bindings,
			                           c.sourceBindings, &c.ports},
			              completion(barrier, &lc::NodeAggregate::driven));
			arm(barrier, Bound::barrier);
		}

		/// Before a planned silence: every faded output ramps to zero over 5 ms; its acknowledgement's bound
		/// (100 ms) armed.
		void fade(std::uint64_t barrier) {
			if (aggregate_.intent() != lc::Intent::stop) closeSources();
			idleSources();
			if (host_.wedged) return overdueAtOnce(barrier, Bound::fade);
			block_->fadeOut([faded = completion(barrier, &lc::NodeAggregate::faded)]() mutable { faded(0); });
			arm(barrier, Bound::fade);
		}

		/// The barrier on the block's data loop: from its completion no process() of the code runs. A teardown's
		/// silence ends what the configuration's threads can tell. Its bound (5 s) armed.
		void silence(std::uint64_t barrier) {
			if (aggregate_.intent() != lc::Intent::stop) {
				config_->channel->closing();
				claimWaitsEnded();
				closeSources();
			}
			if (host_.wedged) return overdueAtOnce(barrier, Bound::barrier);
			block_->drive(DriveRequest{nullptr, {}}, completion(barrier, &lc::NodeAggregate::silenced, true));
			arm(barrier, Bound::barrier);
		}

		/// A stop or teardown begins: every source's writer counts as idle, so what the type's stop() does to its
		/// writer leaves no shortfall in the fade and the silence. Idempotent.
		void idleSources() noexcept {
			if (!config_) return;
			for (SourceSlot& slot : config_->sources) slot.source->stopping();
		}

		/// Teardown step 1: every source's writer closed (a write or idle waiting returns at once), its loops closed to
		/// outside work, and the configuration's companions leave its list (told at the end of the input, after the
		/// health that began the teardown). Idempotent.
		void closeSources() noexcept {
			if (!config_) return;
			config_->ending = true;
			// Its claims are going: a claimant waits for step 5 rather than being refused; its own waits leave.
			host_.claims.going(config_->generation);
			for (SourceSlot& slot : config_->sources) slot.source->close();
			// Its loops refuse outside work from here (slice 5's actions, a program's feed), keeping the node's own.
			for (const auto& slot : config_->loops)
				if (slot->made) slot->made->closeToOthers();
			// Its program is told to end; teardown step 3 waits for its reaping.
			if (config_->program && config_->program->open() && !std::exchange(config_->program->stopped, true))
				config_->program->running->stop();
		}

		/// The replacement of ring companion `index` in `replacements`, if the change has one.
		template <class Replacements>
		static const typename Replacements::value_type* findIndex(const Replacements& replacements, std::size_t index) {
			const auto at = std::ranges::find_if(replacements, [&](const auto& r) { return r.index == index; });
			return at == replacements.end() ? nullptr : &*at;
		}

	public:
		/// The configuration's companions as a client sees them (docs/engine.md#companions-in-a-nodes-list), in request
		/// order; none before configure returned applied, after it failed, or from teardown step 1.
		std::vector<CompanionView> companions() const {
			std::vector<CompanionView> list;
			if (!config_ || config_->startFailed || config_->ending) return list;
			const Configuration& c = *config_;
			for (const Asked& asked : c.requestOrder) switch (asked.family) {
				case Asked::Family::engine: {
					if (asked.index >= c.engineAsks.size()) break;
					const auto view = [&](const EngineAsk& ask, bool ready, bool retiring) {
						return CompanionView{
							.name = ask.target.engineNode,
							.kind = std::visit([&](const auto& shape) { return engineKind(shape, ask.target.bank); },
						                       ask.shape),
							.bank = ask.target.insideBank,
							.ready = ready,
							.retiring = retiring};
					};
					// A companion a make-first replacement was made beside is listed retiring, before its replacement,
					// which shares its name for a moment (docs/control-port.md#companions).
					for (const auto& retiring : c.retiring)
						if (retiring.index == asked.index) list.push_back(view(retiring.ask, true, true));
					const bool replacing = c.change && std::ranges::any_of(c.change->replacements, [&](const auto& r) {
											   return r.index == asked.index && (r.broken || r.made) && !r.ready;
										   });
					const EngineAsk& ask = c.engineAsks[asked.index];
					list.push_back(view(ask, c.readiness.isReady(ask.item) && !replacing, false));
					break;
				}
				case Asked::Family::sink: {
					if (asked.index >= c.sinks.size()) break;
					const SinkSlot& slot = *c.sinks[asked.index];
					const auto view = [&](const SinkSlot& one, bool ready, bool retiring) {
						return CompanionView{.name = name_ + "." + one.bank,
						                     .kind = CompanionKind::sink,
						                     .bank = one.bank,
						                     .ready = ready,
						                     .retiring = retiring,
						                     .dropped = one.sink->droppedFrames()};
					};
					// A sink a change replaces is listed retiring, before its replacement, as an engine companion is.
					const auto* replacing = c.change ? findIndex(c.change->sinks, asked.index) : nullptr;
					list.push_back(view(slot, c.readiness.isReady(slot.item), replacing != nullptr));
					if (replacing != nullptr) list.push_back(view(*replacing->slot, replacing->ready, false));
					break;
				}
				case Asked::Family::source: {
					if (asked.index >= c.sources.size()) break;
					const auto view = [&](const SourceSlot& one, bool retiring) {
						return CompanionView{.name = name_ + "." + one.bank,
						                     .kind = CompanionKind::source,
						                     .bank = one.bank,
						                     .ready = true,
						                     .retiring = retiring,
						                     .shortfall = one.source->shortfallFrames()};
					};
					const auto* replacing = c.change ? findIndex(c.change->sources, asked.index) : nullptr;
					list.push_back(view(c.sources[asked.index], replacing != nullptr));
					if (replacing != nullptr) list.push_back(view(replacing->slot, false));
					break;
				}
				}
			return list;
		}

	private:
		/// The list changed in what NodeHost tells (an entry added or removed, or its `ready`), not in a counter alone,
		/// which CounterWatch samples: told to its listeners, unless a client no longer holds this node.
		void refreshCompanions() {
			std::vector<CompanionView> now = companions();
			const auto sameEntry = [](const CompanionView& a, const CompanionView& b) {
				return a.name == b.name && a.kind == b.kind && a.bank == b.bank && a.ready == b.ready &&
				       a.retiring == b.retiring;
			};
			if (std::ranges::equal(now, companionsTold_, sameEntry)) return;
			companionsTold_ = now;
			if (!superseded_) host_.companionsChanged(name_, now);
		}

		/// The configuration ends (teardown step 5, or a leak): its sinks' and sources' final counts, for the records.
		void companionsEnded() {
			std::vector<CompanionView> finals;
			for (const auto& slot : config_->sinks)
				finals.push_back(CompanionView{.name = name_ + "." + slot->bank,
				                               .kind = CompanionKind::sink,
				                               .bank = slot->bank,
				                               .ready = config_->readiness.isReady(slot->item),
				                               .dropped = slot->sink->droppedFrames()});
			for (const SourceSlot& slot : config_->sources)
				finals.push_back(CompanionView{.name = name_ + "." + slot.bank,
				                               .kind = CompanionKind::source,
				                               .bank = slot.bank,
				                               .ready = true,
				                               .shortfall = slot.source->shortfallFrames()});
			if (finals.empty()) return;
			for (auto* listener : host_.countsListeners) listener->companionsEnded(name_, span_, finals);
		}

		/// A completion as the block runs it on the main loop: an input of its own, heard only by the configuration and
		/// the request it was made for; the aggregate drops an older barrier's. The request's bound goes with it.
		/// A silence's (`unbound`) also ends its sources' cycles until the next drive: once it completed, and no later
		/// request was posted, each source's ring is dropped (Source::stopped), so a start never plays what was written
		/// before it.
		std::move_only_function<void(std::uint64_t)> completion(std::uint64_t barrier,
		                                                        lc::Actions (lc::NodeAggregate::*event)(std::uint64_t),
		                                                        bool unbound = false) {
			return [&host = host_, serial = serial_, generation = config_->generation, barrier, event,
			        unbound](std::uint64_t) {
				if (Node* node = host.find(serial)) ++node->completionsRun_;
				host.inputFor(serial, [generation, barrier, event, unbound](Node& node) {
					if (!node.current(generation)) return;
					if (node.aggregate_.configuration().barrier() == barrier) {
						node.config_->bound.reset();
						if (unbound)
							for (SourceSlot& slot : node.config_->sources) slot.source->stopped();
					}
					node.feed((node.aggregate_.*event)(barrier));
				});
			};
		}

		/// Which bound a request has: the fade's acknowledgement, or the barrier's completion.
		enum class Bound { fade, barrier };

		/// How late past its due time a bound's timer may fire before NodeHost takes the main loop to have stalled too.
		static constexpr std::chrono::milliseconds lateSlack{250};
		/// The grace a bound gets instead of its judgement when the data loop had no fair chance (below).
		static constexpr std::chrono::milliseconds graceOf(Bound bound) {
			return bound == Bound::fade ? fadeAcknowledgeBound : std::chrono::milliseconds(1'000);
		}

		/// Arms the bound of the request just posted.
		void arm(std::uint64_t barrier, Bound bound) {
			config_->postedAt = host_.timers.now();
			armTimer(barrier, bound, bound == Bound::fade ? fadeAcknowledgeBound : barrierCompletionBound);
		}

		/// When the bound's timer fires the block is settled first, as an input of its own, and the request judged in
		/// the input behind whatever that settle ran: a completion a stalled main loop had not dispatched yet is
		/// heard first, and then nothing is judged. The data loop had no fair chance, so the request gets a grace
		/// instead of a judgement, when the settle ran a `done` of this block (it may then have posted the request's
		/// invoke only now, behind a completion whose event had not been dispatched) or when the timer fired late (the
		/// whole process was stalled, frozen or paused, not the data loop alone).
		void armTimer(std::uint64_t barrier, Bound bound, std::chrono::milliseconds delay) {
			Configuration& c = *config_;
			const auto due = host_.timers.now() + delay;
			c.bound = host_.timers.after(
				delay, [&host = host_, serial = serial_, generation = c.generation, barrier, bound, due] {
					host.inputFor(serial, [generation, barrier, bound, due](Node& node) {
						if (!node.current(generation) || node.host_.wedged) return;
						const std::uint64_t before = node.completionsRun_;
						if (node.block_) node.block_->settle();
						const bool unfair = node.completionsRun_ != before || node.host_.timers.now() - due > lateSlack;
						node.host_.inputFor(node.serial_, [generation, barrier, bound, unfair](Node& later) {
							later.judge(generation, barrier, bound, unfair);
						});
					});
				});
		}

		/// The bound of request `barrier` passed and its completion was not heard after the block settled.
		void judge(std::uint64_t generation, std::uint64_t barrier, Bound bound, bool unfair) {
			if (!current(generation) || aggregate_.configuration().barrier() != barrier) return;
			const lc::ConfigState state = aggregate_.configuration().state();
			const bool pending = bound == Bound::fade
			                         ? state == lc::ConfigState::fading
			                         : state == lc::ConfigState::silencing || state == lc::ConfigState::arming ||
			                               state == lc::ConfigState::committing;
			if (!pending) return;
			if (unfair) return armTimer(barrier, bound, graceOf(bound));
			judgedMs_ = millisecondsBetween(config_->postedAt, host_.timers.now());
			feed(bound == Bound::fade ? aggregate_.fadeBound() : aggregate_.barrierBound());
		}

		/// On a wedged loop a request is never posted: it is judged overdue at once, as an input of its own, saying
		/// nothing (the wedge was said once), so its configuration is leaked and the node moves on.
		void overdueAtOnce(std::uint64_t barrier, Bound bound) {
			host_.inputFor(serial_, [generation = config_->generation, barrier, bound](Node& node) {
				if (!node.current(generation) || node.aggregate_.configuration().barrier() != barrier) return;
				node.feedSilently(bound);
			});
		}

		/// The bound's event, with its diagnostics left unsaid (the wedge already was).
		void feedSilently(Bound bound) {
			synthetic_ = true;
			judgedMs_ = 0;
			try {
				feed(bound == Bound::fade ? aggregate_.fadeBound() : aggregate_.barrierBound());
			} catch (...) {
				synthetic_ = false;
				throw;
			}
			synthetic_ = false;
		}

	public:
		/// Another node's barrier found the data loop wedged: this node is never configured again; a configuration not
		/// yet driven fails engine-refused and tears down with no barrier; one whose request is in flight (it can never
		/// complete) is leaked now. A driving one runs on until its stop or removal, whose fade and silence are then
		/// judged overdue at once. Nothing is settled: settling may post to the wedged loop.
		void dataLoopWedged() {
			feed(aggregate_.dataLoopWedged());
			switch (aggregate_.configuration().state()) {
			case lc::ConfigState::readying:
			case lc::ConfigState::prepared:
				feed(aggregate_.fault(lc::Fact::engine, std::string(lc::wedgedDetail)));
				break;
			case lc::ConfigState::arming:
			case lc::ConfigState::silencing:
			case lc::ConfigState::committing: // the commit's invoke, like a drive's, can never complete
				feedSilently(Bound::barrier);
				break;
			case lc::ConfigState::fading:
				feedSilently(Bound::fade);
				break;
			case lc::ConfigState::none:
			case lc::ConfigState::driving:
			case lc::ConfigState::changing: // nothing in flight on the data loop
			case lc::ConfigState::tearing:
				break;
			}
		}

	private:
		void release() {
			retireCode(); // a swap's old code, if the silence came before the swap's completion was heard
			// A silent block carries nothing through and adds nothing; on a wedged loop no PipeWire call is made.
			if (block_ && !host_.wedged) block_->carryLatency({});
			if (config_->prepared) {
				const MainLoopActivity::Scope activity("release", name_);
				config_->code->release();
			}
			config_->prepared = false;
		}

		/// teardown steps 1's rest to 3: the engine companions; the sinks closed, each on its own thread. Steps 3
		/// and 4 have the close deadline. The rest goes once every sink has closed.
		void teardown() {
			Configuration& c = *config_;
			c.channel->closing();
			claimWaitsEnded();
			closeSources();
			for (auto& companion : c.engineCompanions)
				host_.queue(
					FlushKind::destroyCompanion, name_, [&host = host_, companion = std::move(companion)]() mutable {
						if (!host.wedged) {
							companion.reset();
							return;
						}
						static_cast<void>(companion.release()); // leaked: destroying it calls into the wedged loop
						++host.leaked;
					});
			c.engineCompanions.clear();
			for (auto& retiring : c.retiring)
				if (retiring.companion) destroyCompanionLater(std::move(retiring.companion));
			c.retiring.clear();
			c.engineAsks.clear();
			c.readiness.disarm();
			// What its loops were sent before it was configured never runs: destroyed here, unrun.
			for (const auto& slot : c.loops) slot->dropStaged();
			// The deadline judges only a close still under way. One that completed has its `closed` queued for the
			// machine already (closeLoops), perhaps behind this input: it is heard next, and nothing is overdue.
			c.deadline =
				host_.timers.after(host_.closeDeadline, [&host = host_, serial = serial_, generation = c.generation] {
					host.inputFor(serial, [generation](Node& node) {
						if (node.current(generation) && node.config_->tearing && !node.config_->closedTold)
							node.feed(node.aggregate_.closeBound());
					});
				});
			c.tearing = true;
			c.closeBegan = host_.timers.now();
			c.sinksClosed = 0;
			closingSinks_ = true;
			// A sink drains what its ring holds and finishes a consumer that began.
			for (std::size_t i = 0; i < c.sinks.size(); ++i) c.sinks[i]->sink->close(c.channel->sinkClosedCall(i));
			closeRetiredSinks(); // a commit's, if its swap's completion never came: nothing taps them since the silence
			closingSinks_ = false;
			closeLoops();
		}

		/// teardown step 4, once every sink reported closed and the program was reaped, and never inside a sink's
		/// close() call: the loops finish and close, and once they have the machine hears closed, once.
		void closeLoops() {
			if (!config_ || !config_->tearing || closingSinks_ || config_->closedTold ||
			    config_->sinksClosed != config_->sinks.size() || !retiredSinksClosed(*config_) ||
			    (config_->program && config_->program->open()))
				return;
			if (config_->loopsStep == Configuration::LoopsStep::open) finishLoops();
			if (config_->loopsStep != Configuration::LoopsStep::closed) return;
			config_->closedTold = true;
			host_.inputFor(serial_, [generation = config_->generation](Node& node) {
				if (node.current(generation)) node.feed(node.aggregate_.closed());
			});
		}

		/// Every sink a change retired has closed (a teardown's step 4 waits for each, as for its sinks).
		static bool retiredSinksClosed(const Configuration& c) {
			return std::ranges::all_of(c.retiredSinks, [](const auto& slot) { return slot->closed; });
		}

		/// Teardown step 4's loops, once every sink closed: each loop that was made (and did not fail to
		/// start) runs what was posted and destroys its state on its own thread; once all have, each closes, cancelling
		/// the sources left armed; once all have, what each left is said (ear.loop-source-left) and the configuration
		/// has closed. A loop never made has nothing to close: its state goes with the configuration (step 5).
		void finishLoops() { finishLoopsOf(*config_); }

		/// The steps of finishLoops, on a configuration the node holds or its leaked record does: each loop's finished
		/// and closed land through the configuration's Channel, so on whichever holds it then.
		static void finishLoopsOf(Configuration& c) {
			c.loopsStep = Configuration::LoopsStep::finishing;
			std::vector<std::size_t> open;
			for (std::size_t i = 0; i < c.loops.size(); ++i)
				if (c.loops[i]->made && !c.loops[i]->failedHeard) open.push_back(i);
			if (open.empty()) {
				c.loopsStep = Configuration::LoopsStep::closed;
				return;
			}
			for (std::size_t i = 0; i < c.loops.size(); ++i)
				if (std::ranges::find(open, i) == open.end()) c.loops[i]->finished = c.loops[i]->closed = true;
			for (const std::size_t i : open)
				c.loops[i]->made->finish(c.loops[i]->lastCall(), c.channel->loopFinishedCall(i));
		}
		/// Loop `index` finished; once every loop has, each closes.
		static void loopFinishedIn(Configuration& c, std::size_t index) {
			if (c.loopsStep != Configuration::LoopsStep::finishing || index >= c.loops.size()) return;
			c.loops[index]->finished = true;
			if (!std::ranges::all_of(c.loops, [](const auto& slot) { return slot->finished; })) return;
			c.loopsStep = Configuration::LoopsStep::closing;
			for (std::size_t i = 0; i < c.loops.size(); ++i)
				if (!c.loops[i]->closed) c.loops[i]->made->close(c.channel->loopClosedCall(i));
		}
		/// Loop `index` closed; true once every loop has (the step is then closed), with what each left armed said.
		static bool loopClosedIn(Impl& host, std::uint64_t serial, const NodeName& name, Configuration& c,
		                         std::size_t index) {
			if (c.loopsStep != Configuration::LoopsStep::closing || index >= c.loops.size()) return false;
			c.loops[index]->closed = true;
			if (!std::ranges::all_of(c.loops, [](const auto& slot) { return slot->closed; })) return false;
			c.loopsStep = Configuration::LoopsStep::closed;
			// A state destroys the sources it armed: one left is a type bug, cancelled by its loop's close and said.
			for (const auto& slot : c.loops)
				if (const std::size_t left = slot->armedLeft(); left > 0) {
					const std::array fields{DiagnosticField{"sources", static_cast<std::int64_t>(left)}};
					host.diagnosed(serial, name,
					               {.code = "ear.loop-source-left",
					                .subject = std::string(slot->declared.name),
					                .detail = std::to_string(left) + (left == 1 ? " source was" : " sources were") +
					                          " still armed on loop " + std::string(slot->declared.name) +
					                          " when the node's configuration closed",
					                .fields = {fields.begin(), fields.end()}});
				}
			return true;
		}

		/// A loop of the configuration of `generation` finished, or closed: an input of its own.
		void loopFinishedHeard(std::uint64_t generation, std::size_t index) {
			host_.inputFor(serial_, [generation, index](Node& node) {
				if (node.current(generation)) loopFinishedIn(*node.config_, index);
			});
		}
		void loopClosedHeard(std::uint64_t generation, std::size_t index) {
			host_.inputFor(serial_, [generation, index](Node& node) {
				if (node.current(generation) &&
				    loopClosedIn(node.host_, node.serial_, node.name_, *node.config_, index))
					node.closeLoops();
			});
		}

		/// The run's own threads say it is dead: the node fails (start-failed before it rested, lost after).
		void insidesLost(std::string detail) {
			host_.inputFor(serial_, [generation = config_->generation, detail = std::move(detail)](Node& node) mutable {
				if (node.current(generation)) node.feed(node.aggregate_.fault(lc::Fact::nodeLost, std::move(detail)));
			});
		}

		/// A consumer of the configuration threw while facts reach it (Channel::threw): it fails as a hook that throws
		/// does (start-failed before it rested, lost after); one the machine drops is a log record (recordDropped).
		void consumerThrew(std::string detail) {
			host_.inputFor(serial_, [generation = config_->generation, detail = std::move(detail)](Node& node) mutable {
				if (node.current(generation)) node.feed(node.aggregate_.fault(lc::Fact::hookThrew, std::move(detail)));
			});
		}

		/// A fault the machine dropped (a teardown, a failure or a held fault already outranks it). A throw of the
		/// type's code is a bug that must not vanish, so it is an `ear` log record; every other dropped fact (a
		/// configuration's insides saying lost, an engine companion lost, a deadline) is what a teardown expects of
		/// what it stops, and stays dropped (docs/architecture.md#teardown-by-dependency).
		void recordDropped(const lc::Dropped& dropped) {
			if (dropped.fact != lc::Fact::hookThrew) return;
			host_.logged(LogEntry{LogLevel::error, "ear", dropped.detail, {}, name_});
		}

		/// teardown step 5, on the main loop: every sink and loop closed (a dedicated loop's thread already returned)
		/// or never begun, the program reaped. What the configuration holds goes in its order
		/// (Configuration::freeInOrder) while the node still holds it, then the rest with its context, channel and
		/// config, and last its claims.
		void freeConfiguration() {
			const MainLoopActivity::Scope activity("free configuration", name_);
			Configuration& c = *config_;
			c.deadline.reset();
			closeStatus(c);
			companionsEnded(); // closed (no tap or play since the barrier), so their counts are final
			c.freeInOrder();
			const std::uint64_t generation = c.generation;
			config_.reset();
			// Last: its claims released, each waiter judged again in arrival order.
			host_.claimsDecided(host_.claims.release(generation));
			// A node left with no configuration (disabled, failed) shows the ports of its newest settings.
			adoptPorts();
		}

		/// The close deadline passed with step 3 or 4 unfinished: said, then leaked.
		void closeOverdue() {
			const std::size_t sinksOpen = config_->sinks.size() - config_->sinksClosed +
			                              static_cast<std::size_t>(std::ranges::count_if(
											  config_->retiredSinks, [](const auto& slot) { return !slot->closed; }));
			// What step 4 waits for (finishLoopsOf). Before it began: every loop it will ask, one still up, since it
			// has to close before the configuration can go; never one that could not start, which runs nothing and is
			// never asked. Once it began: every loop asked that has not closed (the rest were marked closed there).
			const bool asked = config_->loopsStep != Configuration::LoopsStep::open;
			const auto loopsOpen =
				static_cast<std::size_t>(std::ranges::count_if(config_->loops, [asked](const auto& slot) {
					return slot->made && !slot->closed && (asked || !slot->failedHeard);
				}));
			const std::size_t programsOpen = config_->program && config_->program->open() ? 1 : 0;
			std::string open;
			const auto count = [&](std::size_t n, std::string_view what) {
				if (n == 0) return;
				if (!open.empty()) open += ", ";
				open += std::to_string(n) + " " + std::string(what) + " open";
			};
			count(sinksOpen, "sinks");
			count(loopsOpen, "loops");
			count(programsOpen, "programs");
			diagnose({.code = "ear.close-overdue",
			          .detail = "the configuration did not close within " +
			                    std::to_string(host_.closeDeadline.count()) + " ms and is left running (" + open +
			                    ")"});
		}

		/// What its threads can reach is leaked, never freed, and the node moves on: nothing the configuration tells
		/// reaches the node again. One leaked at the close deadline (its teardown began, so no code runs) keeps
		/// listening: its Channel is re-pointed at its LeakedRecord, which hears its late close. One leaked on a barrier
		/// (the data loop may still run its code) is detached: it never closes, and counts in leaked() for good.
		void leak() {
			Configuration& c = *config_;
			c.deadline.reset();
			c.bound.reset();
			c.readiness.disarm();
			companionsEnded(); // their counts as they stand: the configuration reaches no node again
			const bool listening = c.tearing;
			c.storesWritable->store(false, std::memory_order_release); // a write not begun is dropped
			closeStatus(c, listening); // a listening record's channel is re-pointed below, which detaches it
			if (!listening) c.channel->detach();
			// Its ports abandoned, its stores and recordings kept: what waited for them is judged now. One that listens
			// lists the nodes its claims refuse, which its late close starts again.
			if (listening) host_.refusedByLeaked.try_emplace(c.generation);
			host_.claimsDecided(host_.claims.leaked(c.generation, listening));
			// Not deleted here: the configuration's threads may still touch any of it. A listening record deletes
			// itself once they all reported closed (LeakedRecord::closedLate); any other is never deleted.
			auto* record = new LeakedRecord(host_, name_, serial_, std::move(config_));
			if (listening) record->repoint();
			++host_.leaked;
		}

		Impl& host_;
		NodeName name_;
		std::string type_; // its NodeSpec type, a log entry's source when the type gave none
		std::uint64_t serial_;
		lc::NodeAggregate aggregate_;
		Ports ports_;
		Ports blockPorts_;                          // the ports the block was made or last reshaped with
		std::shared_ptr<const NodeConfig> queued_;  // the newest committed config, the next configure's
		std::uint64_t version_ = 0;                 // queued_'s, as the aggregate compares it
		std::optional<NodeHealth> told_;            // the health last told
		NodePhase wantedPhase_ = NodePhase::added;  // the phase last told, whose waits are shown
		std::vector<WaitingFor> waitingShown_;      // what it waits for, as health shows it
		std::vector<CompanionView> companionsTold_; // its companions' list as last told (or due to be, if silent)
		bool waitingAllowed_ = false;               // the phase has lasted 2 s
		std::unique_ptr<Timer> phaseTimer_;         // the phase's 2 s
		std::unique_ptr<Timer> paceTimer_;          // the 1 s pace of a changed list
		bool superseded_ = false;                   // a newer node of its name was added: standard output alone
		std::vector<NodeDiagnostic> diagnostics_;   // per code and subject, kept for the node's life (diagnose)
		std::unique_ptr<detail::StatusBook> book_;  // its status view and paces, from its first configuration
		std::unique_ptr<Configuration> config_;     // the current configuration (at most one)
		std::unique_ptr<PortBlock> block_;
		bool configuring_ = false;         // inside the instance's making and configure: the contexts' calls are valid
		ChangeScope changeScope_{*this};   // a change version's context, valid only inside its call
		std::optional<EarFault> earFault_; // the first fault of ear's own in the hook running (guarded)
		std::exception_ptr refused_;       // an adapter's refusal, thrown on through guarded as itself
		std::optional<std::string> brokenRule_;   // the first rule a context call found broken in the hook running
		std::optional<std::string> prepareThrew_; // a code's prepare that threw, a fault after its batch
		std::optional<std::string> startThrew_;   // a start() that threw, likewise
		std::optional<std::string> stopThrew_;    // a stop() that threw during a stop, likewise
		std::set<std::string_view, std::less<>> hooksReportedSlow_; // ear.hook-slow said, for the node's life
		bool closingSinks_ = false;        // inside the sinks' close() calls of teardown step 3
		bool makePending_ = false;         // the block's making is queued in the flush
		bool destroyPending_ = false;      // the block's destruction is queued in the flush
		bool goneHeld_ = false;            // removed: told gone once its block is gone
		bool gone_ = false;                // told gone
		bool blockLeaked_ = false;         // its block was leaked on a wedged loop: never freed, nor is this node
		bool synthetic_ = false;           // a bound judged at once on a wedged loop: its diagnostics are not said
		std::uint64_t completionsRun_ = 0; // `done`s its block ran, by event or settle(): a bound sees one it settled
		std::int64_t judgedMs_ = 0;        // how long the judged request waited, for its diagnostics
		std::uint64_t dropped_ = 0;
		std::uint64_t refusedGeneration_ =
			0;                    // its configuration a leaked configuration's claim refused, until the next
		std::uint64_t span_;      // its span of counts: its serial
		NodeCounts blockCounted_; // what its block counted, once the block went
	};

	Impl(AudioFormat format_, PortBlockFactory& blocks_, ResourceRegistry& resources_, Executor& mainLoop_,
	     Timers& timers_, std::chrono::milliseconds closeDeadline_)
		: format(format_), blocks(blocks_), resources(resources_), mainLoop(mainLoop_), timers(timers_),
		  closeDeadline(closeDeadline_) {}

	AudioFormat format;
	PortBlockFactory& blocks;
	ResourceRegistry& resources;
	Executor& mainLoop;
	Timers& timers;
	std::chrono::milliseconds closeDeadline;
	std::vector<NodeHealthListener*> listeners;
	std::vector<NodeNoteListener*> noteListeners;
	std::vector<NodeDiagnosticListener*> diagnosticListeners;
	std::vector<NodeStatusListener*> statusListeners;
	/// The status tick (docs/architecture.md#status-and-events): at most one per statusTickPace, armed only when a node
	/// has records waiting or something due.
	std::unique_ptr<Timer> statusTimer;
	std::chrono::steady_clock::time_point statusTimerAt{};
	std::optional<std::chrono::steady_clock::time_point> lastStatusTick; // none before the first
	std::vector<EngineDiagnosticListener*> engineListeners;
	std::vector<NodeCountsListener*> countsListeners;
	std::vector<NodeCompanionsListener*> companionListeners;
	std::vector<LogSink*> logSinks;
	std::vector<InternalErrorListener*> internalErrorListeners;
	bool wedged = false;                      // the data loop was judged wedged: no PipeWire call is made again
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; removed ones are pruned on add
	std::uint64_t generations = 0;            // every configuration's generation, across nodes, so none is ever reused
	std::uint64_t companionIds = 0;           // every engine companion's id, so none is ever reused
	std::uint64_t serials = 0;                // every node's serial, so a completion never finds a newer node
	std::size_t leaked = 0;                   // configurations, blocks and engine companions never freed
	detail::ClaimRegistry claims;             // every claim ear-wide (docs/writing-a-node-type.md#claims)
	/// By the generation of each configuration leaked at the close deadline and still open: the nodes its claims refused,
	/// each with its configuration refused, started again when it closes late.
	std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> refusedByLeaked;
	bool stopRequested = false;
	std::move_only_function<void()> allGone;
	std::deque<std::move_only_function<void()>> inputs; // what waits for the input being handled
	bool handling = false;
	bool inItem = false;          // a flush item runs: an input it causes waits for it to return
	std::vector<FlushItem> flush; // this turn's queued object changes
	bool flushPosted = false;     // the flush task is posted and has not begun
	bool flushing = false;        // the flush runs: what is queued now runs behind its snapshot
	std::shared_ptr<char> alive = std::make_shared<char>(); // a posted flush finds the host gone by it

	/// One input at a time: one that arrives while another is handled, from an action or a callback inside it,
	/// or while a flush item runs, is handled after it, in arrival order (by drain()). An input that throws (ear's
	/// own bug: a type's throw never leaves callType, nor an adapter's bug NodeHost caught) does not strand the inputs
	/// queued behind it, which may be other nodes' (a stop whose allGone would then never run): they are handled first,
	/// and the first exception is rethrown once the queue is empty, with the place it was thrown from. The node it was
	/// thrown out of is left as the throw left it.
	void input(std::move_only_function<void()> work) {
		inputs.push_back(std::move(work));
		if (handling || inItem) return;
		if (const KeptThrow thrown = drain()) thrown.rethrow();
	}

	/// Handles every queued input, and what they queue; the first exception, if any.
	KeptThrow drain() {
		handling = true;
		KeptThrow thrown;
		while (!inputs.empty()) {
			auto next = std::move(inputs.front());
			inputs.pop_front();
			try {
				next();
			} catch (...) {
				thrown.keepCaught();
			}
		}
		handling = false;
		return thrown;
	}

	/// Each claim judged, told to its claimant's configuration as an input of its own (Node::claimJudged).
	void claimsDecided(std::vector<detail::ClaimDecided> decided) {
		for (detail::ClaimDecided& one : decided)
			inputFor(one.waiter.serial, [one = std::move(one)](Node& node) mutable {
				node.claimJudged(one.waiter.generation, one.claim, std::move(one.judged));
			});
	}

	/// The node of this serial, or none once it is gone (nodes are looked up by serial, never by pointer).
	Node* find(std::uint64_t serial) const noexcept {
		for (const auto& node : nodes)
			if (node->serial() == serial) return node.get();
		return nullptr;
	}

	/// An input for the node of this serial, dropped if it is gone.
	void inputFor(std::uint64_t serial, std::move_only_function<void(Node&)> work) {
		input([this, serial, work = std::move(work)]() mutable {
			if (Node* node = find(serial)) work(*node);
		});
	}

	/// Queues one PipeWire object change into this turn's flush, posting the flush on the turn's first.
	/// During the flush it runs behind the flush's snapshot, in the same flush.
	void queue(FlushKind kind, const NodeName& node, std::move_only_function<void()> run) {
		flush.push_back(FlushItem{kind, node, std::move(run)});
		if (flushing || flushPosted) return;
		flushPosted = true;
		mainLoop.post([this, alive = std::weak_ptr<char>(this->alive)] {
			if (alive.lock()) runFlush();
		});
	}

	/// The turn's flush: the queued changes as a snapshot, destroys first; each item names itself in
	/// MainLoopActivity while it runs, and the inputs it caused (an adapter's event told inside it) are handled once it
	/// returned; what they queue runs behind the snapshot, in this flush. An item or input that throws (ear's bug)
	/// strands nothing: the flush ends, then the first exception is rethrown, with the place it was thrown from.
	void runFlush() {
		flushPosted = false; // a later change posts a flush of its own, even past the throw below
		if (handling || inItem || flushing)
			throw std::logic_error("the flush ran inside an input or another flush: the executor ran a post inline");
		flushing = true;
		KeptThrow thrown;
		std::deque<FlushItem> pending;
		const auto take = [&] {
			std::vector<FlushItem> batch = std::exchange(flush, {});
			std::ranges::stable_sort(batch, {}, &FlushItem::kind);
			for (FlushItem& item : batch) pending.push_back(std::move(item));
		};
		take();
		while (!pending.empty()) {
			FlushItem item = std::move(pending.front());
			pending.pop_front();
			inItem = true;
			{
				const MainLoopActivity::Scope activity(callName(item.kind), item.node);
				try {
					item.run();
				} catch (...) {
					thrown.keepCaught(); // inside the item's scope: an item with no inner scope is placed by its own
				}
				item.run = nullptr; // a destroy's object goes inside its activity, whether or not the item threw
			}
			inItem = false;
			if (KeptThrow caused = drain(); caused && !thrown) thrown = std::move(caused);
			take();
		}
		flushing = false;
		if (thrown) thrown.rethrow();
	}

	static constexpr std::string_view callName(FlushKind kind) noexcept {
		switch (kind) {
		case FlushKind::destroyCompanion:
			return "destroy engine companion";
		case FlushKind::destroyBlock:
			return "destroy block";
		case FlushKind::makeBlock:
			return "make block";
		case FlushKind::makeCompanion:
			return "make engine companion";
		}
		return "flush";
	}

	void requireOpen(const char* event, const NodeName& node) const {
		if (stopRequested)
			throw std::logic_error(std::string(event) + " for node " + node + " after stopAll(): a wiring bug");
	}

	/// The wanted node of this name: the newest one not removing.
	Node& wanted(const NodeName& name) {
		if (Node* node = wantedOrNull(name)) return *node;
		throw std::logic_error("node " + name + " is not a wanted node: a wiring bug");
	}
	Node* wantedOrNull(const NodeName& name) {
		for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
			if ((*it)->name() == name && !(*it)->removing() && !(*it)->removed()) return it->get();
		return nullptr;
	}

	/// A node added (a replace's new node too): it waits for older nodes of its name (T1, rule 1), which are
	/// superseded from now on.
	void add(const NodeSpec& spec, std::shared_ptr<const NodeConfig> config) {
		requireOpen("nodeAdded", spec.name);
		std::erase_if(nodes, [](auto& known) {
			if (!known->removed()) return false;
			if (known->blockLeaked()) static_cast<void>(known.release()); // its leaked block may still tell it
			return true;
		});
		const std::uint64_t serial = ++serials;
		nodes.emplace_back(std::make_unique<Node>(*this, spec.name, spec.type, serial));
		inputFor(serial, [this, spec, config = std::move(config)](Node& added) mutable {
			// Older nodes of the name, as they stand when this input is handled (T1, rule 1).
			bool olderBlock = false;
			bool olderNode = false;
			for (const auto& known : nodes) {
				if (known.get() == &added) break;
				if (known->name() != added.name() || known->removed()) continue;
				olderNode = true;
				olderBlock = olderBlock || known->holdsBlock();
				known->supersede();
			}
			// Every node begins a span of its name's counts from zero.
			for (auto* listener : countsListeners) listener->countsBegan(added.name(), added.span());
			added.add(spec, std::move(config), olderBlock, olderNode);
		});
	}

	void changed(const NodeHealth& health, bool superseded) {
		for (auto* listener : listeners)
			if (!superseded || listener->hearsSuperseded()) listener->nodeChanged(health);
	}

	/// An operation: refused stopping during stopAll and unknown-node for a name no wanted node has; else the
	/// wanted node's aggregate answers, in an input of its own, before the call returns.
	LifecycleReply operate(const NodeName& name, Operated (NodeAggregate::*operation)(),
	                       NodeOperations::OperationJudged judged) {
		if (handling || inItem || flushing)
			throw std::logic_error("an operation on node " + name + " inside NodeHost's own input: a wiring bug");
		const auto refuse = [&](LifecycleRefusal::Code code) {
			LifecycleReply reply = std::unexpected(LifecycleRefusal{code});
			if (judged) judged(reply);
			return reply;
		};
		if (stopRequested) return refuse(LifecycleRefusal::Code::stopping);
		Node* node = wantedOrNull(name);
		if (node == nullptr) return refuse(LifecycleRefusal::Code::unknownNode);
		std::optional<LifecycleReply> reply;
		inputFor(node->serial(), [&](Node& wanted) { reply = wanted.operate(operation, judged); });
		if (!reply) return refuse(LifecycleRefusal::Code::unknownNode);
		return *reply;
	}

	void companionsChanged(const NodeName& node, std::span<const CompanionView> companions) {
		for (auto* listener : companionListeners) listener->companionsChanged(node, companions);
	}

	void noted(const NodeName& node, const std::string& text) {
		for (auto* listener : noteListeners) listener->nodeSays(node, text);
	}

	void logged(const LogEntry& entry) {
		for (auto* sink : logSinks) sink->log(entry);
	}

	void internalError(const InternalError& error) {
		for (auto* listener : internalErrorListeners) listener->internalError(error);
	}

	/// A node's records wait: the tick is armed, if it is not, for 33 ms after the last (docs/architecture.md#status-and-events).
	void statusDirty() { armStatusTick(std::nullopt); }
	void armStatusTick(std::optional<std::chrono::steady_clock::time_point> due) {
		const auto now = timers.now();
		auto at = lastStatusTick ? *lastStatusTick + statusTickPace : now;
		if (due && *due > at) at = *due;
		if (statusTimer && at >= statusTimerAt) return; // one sooner is armed
		statusTimerAt = at;
		const auto delay =
			at > now ? std::chrono::ceil<std::chrono::milliseconds>(at - now) : std::chrono::milliseconds(0);
		statusTimer = timers.after(delay, [this] {
			statusTimer.reset();
			tickStatus();
		});
	}
	/// Every node's records drained and its book ticked, in arrival order; armed again for what a book still has due.
	void tickStatus() {
		const auto now = timers.now();
		lastStatusTick = now;
		std::optional<std::chrono::steady_clock::time_point> due;
		for (const auto& node : nodes) {
			if (node->removed()) continue;
			node->statusTick(now);
			if (const auto next = node->statusDue(); next && (!due || *next < *due)) due = next;
		}
		if (due) armStatusTick(due);
	}

	/// A diagnostic about the node of `serial`, kept on it while it is not gone (Node::diagnose), else told only to the
	/// listeners that hear a superseded node, as said once.
	void diagnosed(std::uint64_t serial, const NodeName& name, NodeDiagnostic diagnostic) {
		if (Node* node = find(serial); node != nullptr && !node->removed())
			return node->diagnose(std::move(diagnostic));
		diagnostic.at = timers.now();
		toldDiagnostic(name, diagnostic, true);
	}
	void toldDiagnostic(const NodeName& name, const NodeDiagnostic& diagnostic, bool superseded) {
		for (auto* listener : diagnosticListeners)
			if (!superseded || listener->hearsSuperseded()) listener->nodeDiagnostic(name, diagnostic);
	}

	/// A barrier of `node` did not complete within `ms`: the data loop runs no invoke. Said once; then every node
	/// hears it, each as an input of its own after this one.
	void markWedged(const NodeName& node, std::int64_t ms) {
		if (wedged) return;
		wedged = true;
		const EngineDiagnostic diagnostic{
			"ear.data-loop-wedged",
			"node " + node + "'s barrier did not complete within " + std::to_string(ms) +
				" ms: the data loop runs no invoke, so no further engine change is applied",
			{DiagnosticField{"ms", ms}}};
		for (auto* listener : engineListeners) listener->engineDiagnostic(diagnostic);
		for (const auto& known : nodes)
			if (!known->removed()) inputFor(known->serial(), [](Node& each) { each.dataLoopWedged(); });
	}

	/// A node is gone; the oldest node left of its name, which no older node holds back now, may configure.
	void gone(Node& node, std::uint64_t droppedFrames) {
		node.markGone();
		// Its span's final counts, which nothing answers once it is gone, a superseded node's too.
		for (auto* listener : countsListeners) listener->countsEnded(node.name(), node.span(), node.counts());
		for (auto* listener : listeners) {
			if (!node.superseded()) listener->nodeGone(node.name(), droppedFrames);
			else if (listener->hearsSuperseded()) listener->supersededGone(node.name(), droppedFrames);
		}
		const auto next = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->name() == node.name() && !known->removed(); });
		if (next != nodes.end()) inputFor((*next)->serial(), [](Node& waiting) { waiting.olderGone(); });
		input([this] { allGoneIfDone(); });
	}

	/// A block of this name was destroyed: the block waiting for it (at most one: only the wanted node of a name
	/// waits) is made, in this flush, once no block of the name is left (T1).
	void blockDestroyed(const NodeName& name) {
		if (std::ranges::any_of(nodes, [&](const auto& known) { return known->name() == name && known->holdsBlock(); }))
			return;
		const auto waiting = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->name() == name && known->blockWaiting(); });
		if (waiting != nodes.end()) inputFor((*waiting)->serial(), [](Node& node) { node.olderDestroyed(); });
	}

	void allGoneIfDone() {
		if (!allGone) return;
		for (const auto& node : nodes)
			if (!node->removed()) return;
		std::exchange(allGone, nullptr)();
	}
};

NodeHost::NodeHost(AudioFormat format, PortBlockFactory& blocks, ResourceRegistry& resources, Executor& mainLoop,
                   Timers& timers, std::chrono::milliseconds closeDeadline)
	: impl_(std::make_unique<Impl>(format, blocks, resources, mainLoop, timers, closeDeadline)) {
	markMainLoopThread(); // made on the main loop's thread, where nothing may wait (a store's replace refuses it)
}

NodeHost::~NodeHost() {
	// On a wedged loop, leaked blocks, engine companions and configurations still point into the host: it is never
	// freed (main.cpp ends through std::_Exit before this runs; a test's host is kept for good).
	if (impl_->wedged) static_cast<void>(impl_.release());
}

void NodeHost::subscribe(NodeHealthListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::subscribeNotes(NodeNoteListener& listener) { impl_->noteListeners.push_back(&listener); }

void NodeHost::subscribeDiagnostics(NodeDiagnosticListener& listener) {
	impl_->diagnosticListeners.push_back(&listener);
}

void NodeHost::subscribeStatus(NodeStatusListener& listener) { impl_->statusListeners.push_back(&listener); }

void NodeHost::drainStatus() {
	impl_->statusTimer.reset();
	impl_->tickStatus();
}

std::vector<FieldValue> NodeHost::statusOf(const NodeName& node) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->name() == node && !(*it)->removed()) return (*it)->status();
	return {};
}

void NodeHost::subscribeEngineDiagnostics(EngineDiagnosticListener& listener) {
	impl_->engineListeners.push_back(&listener);
}

void NodeHost::subscribeCounts(NodeCountsListener& listener) { impl_->countsListeners.push_back(&listener); }

void NodeHost::subscribeCompanions(NodeCompanionsListener& listener) { impl_->companionListeners.push_back(&listener); }

void NodeHost::subscribeLog(LogSink& sink) { impl_->logSinks.push_back(&sink); }

void NodeHost::subscribeInternalErrors(InternalErrorListener& listener) {
	impl_->internalErrorListeners.push_back(&listener);
}

void NodeHost::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->add(node, std::move(config));
}

void NodeHost::nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
	static_cast<void>(change); // another type under the name: a new node
	impl_->add(node, std::move(config));
}

void NodeHost::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	nodeChanged(node, std::move(config), NodeChange{NodeOutcome::restart, {}});
}

void NodeHost::nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
	impl_->requireOpen("nodeChanged", node.name);
	impl_->inputFor(
		impl_->wanted(node.name).serial(),
		[config = std::move(config), change](Impl::Node& wanted) mutable { wanted.change(std::move(config), change); });
}

void NodeHost::nodeRemoved(const NodeName& node) {
	impl_->requireOpen("nodeRemoved", node);
	impl_->inputFor(impl_->wanted(node).serial(), [](Impl::Node& wanted) { wanted.remove(); });
}

void NodeHost::nodeReplacing(const NodeName& node, const NodeChange& change) {
	impl_->requireOpen("nodeReplacing", node);
	static_cast<void>(change);
	impl_->inputFor(impl_->wanted(node).serial(),
	                [](Impl::Node& wanted) { wanted.remove(Impl::Node::Replaced::superseded); });
}

LifecycleReply NodeHost::start(const NodeName& node, OperationJudged judged) {
	return impl_->operate(node, &NodeAggregate::start, std::move(judged));
}

LifecycleReply NodeHost::stop(const NodeName& node, OperationJudged judged) {
	return impl_->operate(node, &NodeAggregate::stop, std::move(judged));
}

LifecycleReply NodeHost::restart(const NodeName& node, OperationJudged judged) {
	return impl_->operate(node, &NodeAggregate::restart, std::move(judged));
}

void NodeHost::stopAll(GraphClosed, std::move_only_function<void()> allGone) {
	if (impl_->stopRequested) throw std::logic_error("stopAll() was called twice");
	impl_->stopRequested = true;
	impl_->allGone = std::move(allGone);
	impl_->input([impl = impl_.get()] {
		for (const auto& node : impl->nodes)
			if (!node->removing()) node->remove();
		impl->allGoneIfDone();
	});
}

std::size_t NodeHost::leaked() const noexcept { return impl_->leaked; }

std::optional<NodeCounts> NodeHost::spanCounts(std::uint64_t span) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->span() == span && !(*it)->removed()) return (*it)->counts();
	return std::nullopt;
}

std::optional<NodeCounts> NodeHost::counts(const NodeName& node) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->name() == node && !(*it)->removed()) return (*it)->counts();
	return std::nullopt;
}

std::vector<CompanionView> NodeHost::companionsOf(const NodeName& node) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->name() == node && !(*it)->removed()) return (*it)->companions();
	return {};
}

std::vector<NodeDiagnostic> NodeHost::diagnosticsOf(const NodeName& node) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->name() == node && !(*it)->removed()) return (*it)->diagnostics();
	return {};
}

namespace {
std::string framesCounted(std::uint64_t frames) {
	return std::to_string(frames) + (frames == 1 ? " frame" : " frames");
}
} // namespace

void NodeHost::companionFramesLost(const NodeName& node, const CompanionView& companion, std::uint64_t frames,
                                   std::uint64_t totalFrames) {
	const bool sink = companion.kind == CompanionKind::sink;
	NodeDiagnostic diagnostic{.code = sink ? "ear.sink-dropped" : "ear.source-shortfall",
	                          .subject = companion.name,
	                          .detail = sink ? companion.name + " dropped " + framesCounted(frames) +
	                                               " its ring had no room for; " + std::to_string(totalFrames) +
	                                               " since it was made"
	                                         : companion.name + " played " + framesCounted(frames) +
	                                               " short while its writer was not idle; " +
	                                               std::to_string(totalFrames) + " since it was made",
	                          .texts = {DiagnosticText{"companion", companion.name}},
	                          .fields = {DiagnosticField{"frames", static_cast<std::int64_t>(frames)},
	                                     DiagnosticField{"totalFrames", static_cast<std::int64_t>(totalFrames)}}};
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->name() == node && !(*it)->removed()) return (*it)->diagnose(std::move(diagnostic));
	diagnostic.at = impl_->timers.now();
	impl_->toldDiagnostic(node, diagnostic, true);
}

std::vector<CompanionView> NodeHost::spanCompanions(std::uint64_t span) const {
	for (auto it = impl_->nodes.rbegin(); it != impl_->nodes.rend(); ++it)
		if ((*it)->span() == span && !(*it)->removed()) return (*it)->companions();
	return {};
}

std::vector<NodeHealth> NodeHost::health() const {
	std::vector<NodeHealth> result;
	for (const auto& node : impl_->nodes)
		if (!node->removed()) result.push_back(node->health());
	return result;
}

} // namespace ear
