// The node framework (docs/architecture.md#the-node-lifecycle; node_host.hpp), on the node lifecycle's
// two pure machines (docs/architecture.md#the-node-lifecycle, node_lifecycle.hpp). Each node is a
// NodeAggregate: NodeHost feeds it every input (a commit, a block's or an engine companion's event, a completion, a
// post from the node's own threads, a timer) and carries out the actions it returns, never waiting for another thread.
//
// The port block is made when the node is added and goes only once it is removed; configurations come and go
// behind it, one at a time. Each configuration has its instance (docs/writing-a-node-type.md),
// made by its config: the type's class (lifecycle.hpp's SpecInstance), or the legacy adapter over the old SPI's start
// (legacy_instance.hpp). Its configure acquires only, through CreateContext (the class) or StartContext (the
// legacy start), both implemented here on one set of internals, and gives the code; a rule it breaks at a call or at
// the return, and a configure that throws, fail the node start-failed. The engine companions it asked for are made
// once the block is ready; each of them and each sink's consumer is an item of the configuration's readiness list
// (readiness.hpp), with its kind's deadline from the request, past which the node fails
// (docs/engine.md#readiness-deadlines), and every companion is in the node's list a client
// sees (companionsOf; docs/engine.md#companions-in-a-nodes-list); once every resource is ready a legacy recipe
// builds the run object (T3), the code is prepared, and once the block is ready it is driven by an asynchronous
// drive (T5), whose completion the block
// posts and NodeHost checks against the node's serial, its configuration's generation and the barrier number
// before the machine hears it; then the instance's start() runs. Its stop() runs once before each silence that follows
// a start() that returned. Each hook is timed (ear.hook-slow, once per node and hook). Every call into a type's code
// (its configure, start() and stop(), its code's prepare, a legacy recipe and close, and a sink's or handoff's consumer
// on that one's own thread) goes through one catch (callType, ConsumerCalls), so a type's throw is a fact about its
// node (docs/writing-a-node-type.md#rules-the-framework-enforces). ear's own bugs are split by owner
// (docs/architecture.md#faults-by-owner): an adapter's that NodeHost caught where it called it (inside a context call,
// or a factory in the flush) is told as ear.internal-error and fails only its node, lost; what NodeHost throws itself
// leaves for the callback boundary, which ends the process. A configuration is silenced the
// same way, then torn down by dependency: release(); engine companions; handoffs closed, each on its own thread; the
// instance closed (a legacy run object, on its own threads); then, on the main loop, the code, the instance, the
// recordings and the config. Steps 3 and 4 have the close deadline: past it the configuration is leaked, never
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
// Fades and bounds: every output the code writes (public outputs and inside outputs, which feed a card) fades in with
// each drive and out before each planned silence; a fade unacknowledged within 100 ms is said (ear.fade-unacknowledged)
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
// change on a disabled node is stored, and a node added disabled is never configured. A replace whose type is
// unchanged (only its ports changed) carries the node's stop, its dropped frames and one health stream into its
// successor, the older node telling nobody; the older node of any other name added back tells only the
// listeners that hear a superseded node (standard output, not the wire). NodeOperations' stop, start and restart are
// operations of their own, answered in the call.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
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
#include "completions.hpp"
#include "note_gate.hpp"
#include "readiness.hpp"

namespace ear {
namespace {

namespace lc = ::ear; // the lifecycle's machines (node_lifecycle.hpp)

template <typename... Kinds> struct Overloaded : Kinds... {
	using Kinds::operator()...;
};

/// The recordings adapter of a registry given none.
class NoRecordings final : public RecordingAdapter {
public:
	std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName&, const std::string&) override {
		return std::unexpected(std::string("no recordings folder"));
	}
};

NoRecordings noRecordings; // stateless

/// The fact a type's own failure is: an engine refusal when the type says so (none does today), else `fact`.
lc::Fact factOf(const FailureReason& reason, lc::Fact fact) noexcept {
	return reason.code == FailureReason::Code::engineRefused ? lc::Fact::engine : fact;
}

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
inline constexpr TypeHook recipe{"recipe", "recipe", false};    // a legacy recipe
inline constexpr TypeHook close{"close", "close run", false};   // a legacy run object's close
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

/// An engine companion's kind: a card's by its bank's direction.
CompanionKind engineKind(const SoundCardShape&, Direction bank) noexcept {
	return bank == Direction::output ? CompanionKind::cardPlayback : CompanionKind::cardCapture;
}
CompanionKind engineKind(const RtpReceiverShape&, Direction) noexcept { return CompanionKind::rtpReceiver; }

/// Why an engine companion that missed its readiness deadline failed its node (engine-refused,
/// docs/engine.md#readiness-deadlines).
std::string neverReady(const SoundCardShape& shape) { return "the card never started: " + shape.card.text(); }
std::string neverReady(const RtpReceiverShape& shape) {
	return "the RTP receiver never ran (UDP port " + std::to_string(shape.port) + " held?)";
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
	}
	return "unknown";
}

RecordingAdapter& ResourceRegistry::adapterFor(const RecordingRequest&) const noexcept {
	return adapters_.recordings != nullptr ? *adapters_.recordings : noRecordings;
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
	class Node final : public PortBlockEvents, public StartContext, public Ready, public CreateContext {
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
		/// The span of counts it belongs to (docs/engine.md#reporting-the-counters): its own serial, or for a port-caused
		/// replace its older node's span, which it carries on.
		std::uint64_t span() const noexcept { return span_; }
		void carrySpan(std::uint64_t span) noexcept { span_ = span; }
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
		/// The older node of a port-caused replace: it tells nobody.
		bool silent() const noexcept { return silent_; }
		/// The serial of the older node this one is the port-caused replace of, while that one is not gone; else 0.
		std::uint64_t predecessor() const noexcept { return predecessor_; }
		/// A newer node of this name was added while this one closes: only standard output hears it. From now on only
		/// the listeners that hear a superseded node hear this one, told its health at once, so they keep it apart from
		/// the newer node told next.
		void supersede() {
			if (std::exchange(superseded_, true) || silent_) return;
			tell(aggregate_.health());
		}
		bool superseded() const noexcept { return superseded_; }
		/// The stop held, for a port-caused replace to carry.
		bool stopHeld() const noexcept { return aggregate_.stopHeld(); }
		bool disabled() const noexcept { return aggregate_.wanted() == lc::Wanted::disabled; }
		std::uint64_t dropped() const noexcept { return dropped_; }
		/// The node's counters from its add (docs/engine.md#counters): its block's now, or what it counted before it
		/// went, plus what the older node of a port-caused replace counted: read from that node while it is not gone,
		/// carried once it is. Main thread.
		NodeCounts counts() const noexcept {
			NodeCounts total = carried_ + (block_ ? block_->counts() : blockCounted_);
			if (predecessor_ != 0 && !predecessorCarried_)
				if (const Node* older = host_.find(predecessor_)) total = total + older->counts();
			return total;
		}
		/// The older node of this port-caused replace is gone: its counters are this node's from now on.
		void carryCounts(NodeCounts older) noexcept {
			carried_ = carried_ + older;
			predecessorCarried_ = true;
		}

		/// What a port-caused replace carries from its older node, or nothing.
		struct Carried {
			std::uint64_t predecessor = 0; // its serial: this node is gone only after it, and takes its dropped frames
			bool stopping = false;         // shown stopping (restarting) until it is gone: this node is enabled
			bool stopHeld = false;
			std::vector<std::string> keys;
		};

		/// The port block once no older block of this name exists (T1: two blocks never share a node.name), and a
		/// configuration once no older node of this name is left (rule 1), so an old and a new configuration never
		/// share what the name owns (a listening port, a key file). A node added disabled keeps its block and is never
		/// configured (T19).
		void add(const NodeSpec& spec, std::shared_ptr<const NodeConfig> config, bool olderBlock, bool olderNode,
		         Carried carried) {
			ports_ = config->ports();
			queued_ = std::move(config);
			predecessor_ = carried.predecessor;
			// A node added on a wedged loop fails before T1, with no block.
			feed(aggregate_.add(lc::AddFacts{.disabled = spec.disabled,
			                                 .stopHeld = carried.stopHeld,
			                                 .olderBlock = olderBlock,
			                                 .predecessor = carried.stopping,
			                                 .olderNode = olderNode,
			                                 .keys = std::move(carried.keys),
			                                 .wedged = host_.wedged,
			                                 .queued = queued()}));
		}

		/// The plan's outcome for a node kept by name. Its newest config is kept for the next configure
		/// (a lost block is final: nothing is kept). restart: a configuration settled restarts at once, one still going
		/// takes it next (rule 2), a failed node retries; disable: the configuration torn down (T20, T21), its
		/// stop dropped, its block and wires kept; enable: configured afresh (T22); stored: kept for the enable.
		void change(std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
			const bool lost = aggregate_.block().state() == lc::BlockState::lost;
			queued_ = lost ? nullptr : std::move(config);
			++version_;
			switch (change.outcome) {
			case NodeOutcome::restart:
			case NodeOutcome::stored:
				feed(aggregate_.change(queued(), change.keys));
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
		}

		/// How a removal relates to a node of the same name added in the same commit (a replace).
		enum class Replaced { no, superseded, portCaused };

		/// No longer wanted. A replaced node's newer node of the name comes in the same commit: from here on the older
		/// node of a port-caused replace tells nobody, any other only standard output.
		void remove(Replaced replaced = Replaced::no) {
			queued_.reset(); // never configured again
			silent_ = silent_ || replaced == Replaced::portCaused;
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
		/// The older node of this port-caused replace is gone (its configuration none, its block destroyed): its frames
		/// dropped are this node's.
		void predecessorGone(std::uint64_t droppedFrames) {
			predecessor_ = 0;
			dropped_ += droppedFrames;
			feed(aggregate_.predecessorGone());
			tellGone();
		}
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
			return std::holds_alternative<SoundCardShape>(shape) ? "SoundCardCompanions::make"
			                                                     : "RtpReceiverCompanions::make";
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
			auto& companion =
				*c.engineCompanions.emplace_back(std::make_unique<Companion>(host_, serial_, c.generation, index));
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

		// The contexts (docs/writing-a-node-type.md#configure-and-createof): CreateContext for the class, StartContext
		// for the legacy start, Ready for its recipe, on one set of internals. Each call is valid only inside its
		// configure (else a std::logic_error naming the call). Every rule a call finds broken is also recorded
		// (guarded), so the node fails with it even when the type catches the throw.
		AudioFormat format() const override { return host_.format; }
		const NodeName& node() const override { return name_; }
		const Ports& ports() const override { return ports_; }
		NodeHandle& events() override {
			return guarded([&]() -> NodeHandle& {
				if (!configuring_ && !building_) throw std::logic_error("events outside configure and its recipe");
				return *config_->channel;
			});
		}
		void use(NodeCode code) override {
			guarded([&] {
				requireConfiguring("use");
				Configuration& c = *config_;
				if (c.codeGiven) throw std::logic_error("use called twice");
				if (!code.own() && code.pairs().empty())
					throw std::logic_error("use given no code: a null code or a pass-through of no pair");
				c.codeGiven = true;
				c.pairs = code.pairs();
				c.code = std::move(code.own());
			});
		}
		void soundCard(std::string_view bank, SoundCardShape shape) override {
			guarded([&] {
				requireConfiguring("soundCard");
				askEngineCompanion(std::move(shape), cardBank(ports_, std::string(bank)));
			});
		}
		void rtpReceiver(std::string_view bank, RtpReceiverShape shape) override {
			guarded([&] {
				requireConfiguring("rtpReceiver");
				askEngineCompanion(std::move(shape),
				                   insideBank(ports_, std::string(bank), Direction::input, "an RTP receiver"));
			});
		}
		std::expected<int, std::string> openRecording(const RecordingRequest& request) override {
			return guarded([&]() -> std::expected<int, std::string> {
				requireConfiguring("openRecording");
				RecordingAdapter& recordings = host_.resources.adapterFor(request);
				auto opened = adapter("RecordingAdapter::open", [&] {
					auto file = recordings.open(name_, request.path);
					if (file && *file == nullptr) throw std::logic_error("the recording adapter opened no file");
					return file;
				});
				if (!opened) return std::unexpected(std::move(opened.error()));
				return (*config_->recordings.emplace_back(std::move(*opened))).descriptor();
			});
		}
		/// A sink (docs/writing-a-node-type.md#sinks): made at once on the inside output bank, its thread
		/// started; its bank is never faded. An adapter that cannot make it (no thread) fails the node through
		/// configure, recorded so a type that catches it still fails; an adapter's own bug fails it lost (adapter()).
		SinkRequest& sink(std::string_view bank, SinkShape shape) override {
			return guarded([&]() -> SinkRequest& {
				requireConfiguring("sink");
				const std::string name(bank);
				const Ports::Bank& found = insideBank(ports_, name, Direction::output, "a sink");
				requireOneName(name_ + "." + found.name);
				requireFreeBank(found);
				Configuration& c = *config_;
				SinkAdapter& sinks = given(host_.resources.sinks(), "sinks");
				std::unique_ptr<Sink> made = adapter("SinkAdapter::make", [&] {
					auto sink = sinks.make(name_, found.name, static_cast<std::uint32_t>(found.ports.size()),
					                       shape.frames, host_.format.rate);
					if (!sink) throw std::logic_error("the sink adapter made no sink for bank " + found.name);
					return sink;
				});
				auto& slot = *c.sinks.emplace_back(std::make_unique<SinkSlot>(*this, found.name));
				slot.sink = std::move(made);
				slot.binding = SinkBinding{&slot.sink->tap(), found.first, found.ports.size()};
				c.requestOrder.push_back({Asked::Family::sink, c.sinks.size() - 1});
				for (std::size_t i = found.first; i < found.first + found.ports.size() && i < c.fadedCount; ++i)
					c.faded[i] = false;
				return slot;
			});
		}
		/// A source (docs/writing-a-node-type.md#sources): made at once on the inside input bank, ready at once, bound
		/// by every drive of the configuration; its capacity declared beyond the bank; its clock, if it has one, kept
		/// with it (the source owns it, freed with it at teardown step 5). An adapter that cannot make it
		/// (no eventfd) fails the node through configure, recorded so a type that catches it still fails; an adapter's
		/// own bug fails it lost (adapter()).
		SourceWriter& source(std::string_view bank, SourceShape shape, std::unique_ptr<SourceClock> clock) override {
			return guarded([&]() -> SourceWriter& {
				requireConfiguring("source");
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
		void declareLatency(const std::string& bank, Latency internal) override {
			guarded([&] {
				requireConfiguring("declareLatency");
				recordLatency(ports_.bank(bank).name, internal); // throws for a bank the node lacks
			});
		}
		[[noreturn]] void refuse(std::string rule) override {
			if (!brokenRule_) brokenRule_ = rule;
			throw std::logic_error(rule);
		}

	protected:
		Acquired acquireResource(const ResourceRequest& request) override {
			return guarded([&]() -> Acquired {
				requireConfiguring("acquire");
				return std::visit(
					Overloaded{
						[&](const RecordingRequest& recording) -> Acquired { return openRecording(recording); },
						[&](const HandoffRequest& handoff) -> Acquired {
							const Ports::Bank& bank = ports_.bank(handoff.bank);
							if (bank.direction != Direction::input || handoff.consumer == nullptr)
								throw std::logic_error("a handoff takes an input bank and a consumer, not \"" +
						                               handoff.bank + "\"");
							// The handoff calls the type's consumer through its catch, which outlives the handoff.
							auto& caught =
								*config_->handoffConsumers.emplace_back(std::make_unique<GuardedHandoffConsumer>(
									*handoff.consumer, config_->channel, bank.name));
							HandoffAdapter& handoffs = given(host_.resources.adapterFor(handoff), "handoffs");
							auto& made = config_->handoffs.emplace_back(adapter("HandoffAdapter::make", [&] {
								auto made = handoffs.make(name_, static_cast<std::uint32_t>(bank.ports.size()),
						                                  handoff.capacityFrames, caught);
								if (!made) throw std::logic_error("the handoff adapter made no handoff");
								return made;
							}));
							return &made->writer();
						},
					},
					request);
			});
		}

		/// Runs a context call, telling apart by owner what it throws (docs/architecture.md#faults-by-owner), each
		/// recorded, the first only, before it is thrown on to the type: a std::logic_error NodeHost's checks threw is a
		/// rule the type broke; std::bad_alloc is resource exhaustion, not a bug, and fails the node as a refusal does;
		/// an adapter's refusal or bug was recorded by adapter() and passes as it is, through any guarded() it is
		/// nested in (openRecording inside acquire): the refusal is known by its identity, which nothing clears until
		/// the type's call ends; anything else thrown by NodeHost's own code is NodeHost's bug, thrown on as an EarFault
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
			Companion(Impl& host, std::uint64_t serial, std::uint64_t generation, std::size_t index)
				: host_(host), serial_(serial), generation_(generation), index_(index) {}
			std::unique_ptr<EngineCompanion> made;
			// Told inside its making (during the flush) or later: an input of its own either way.
			void ready() override {
				host_.inputFor(serial_, [generation = generation_, index = index_](Node& node) {
					// Still current while it tears down, when its engine companions are already gone: nothing to ready.
					if (!node.current(generation) || node.config_->tearing ||
					    index >= node.config_->engineCompanions.size())
						return;
					node.itemReady(generation, node.config_->engineAsks[index].item);
				});
			}
			void lost(std::string detail) override { tell(lc::Fact::engine, std::move(detail)); }
			void faulted(std::string detail) override { tell(lc::Fact::adapterFaulted, std::move(detail)); }

		private:
			void tell(lc::Fact fact, std::string detail) {
				host_.inputFor(serial_,
				               [generation = generation_, fact, detail = std::move(detail)](Node& node) mutable {
								   if (node.current(generation))
									   node.feed(node.aggregate_.fault(fact, std::move(detail)));
							   });
			}
			Impl& host_; // never the node: a queued destruction may outlive it
			std::uint64_t serial_;
			std::uint64_t generation_;
			std::size_t index_;
		};

		/// One configuration as its own threads reach the node, and its handle's implementation
		/// (docs/writing-a-node-type.md#the-node-handle): everything they tell crosses to the main loop, in order, and
		/// lands only while the configuration is attached. Each configuration has its own Channel, detached on every path
		/// that ends it (it closed, it failed to start, or it was leaked), before the next one can start: that is the
		/// configuration's generation, so no write of an old configuration lands on a new one. Whatever is still queued
		/// holds this object, not the node, so it may outlive the node. Its note rate is its own, so it is per node and
		/// lasts as long as the configuration: a stop and a start keep it, a restart begins a fresh one.
		class Channel final : public NodeHandle, public std::enable_shared_from_this<Channel> {
		public:
			/// `sinks` are NodeHost's log sinks, all subscribed before the first node (subscribeLog); `timers` is the
			/// clock the note rate counts on.
			Channel(Node& node, Executor& mainLoop, Timers& timers, NodeName name, std::string type,
			        std::vector<LogSink*> sinks)
				: node_(&node), completions_(std::make_shared<detail::Completions>(mainLoop)), name_(std::move(name)),
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
			void lost(std::string detail) override {
				completions_->post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					if (Node* node = self->attachedNode(); node != nullptr && !self->closing_)
						node->insidesLost(std::move(detail));
				});
			}
			/// Straight to the sinks on the calling thread, as captured output goes; the attached flag is atomic, so a
			/// configuration that ended logs nothing, but for one entry that read the flag just before the detach
			/// (the detach does not wait for a log in progress).
			void log(LogEntry entry) override {
				if (!completions_->attached()) return;
				entry.node = name_;
				if (entry.source.empty()) entry.source = type_;
				for (LogSink* sink : sinks_) sink->log(entry);
			}
			/// Any thread: a consumer of this configuration threw (ConsumerCalls). A fact, as a hook's throw is, while
			/// facts reach the node; once its teardown began a log record, as a stop() that throws then is.
			void threw(std::string detail) {
				completions_->post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					Node* node = self->attachedNode();
					if (node == nullptr) return;
					if (self->closing_)
						node->host_.logged(LogEntry{LogLevel::error, "ear", std::move(detail), {}, node->name_});
					else node->consumerThrew(std::move(detail));
				});
			}
			/// Main thread. The configuration is going (its teardown began): a fact no longer reaches the node.
			void closing() noexcept { closing_ = true; }
			/// Main thread. Nothing more of this configuration reaches the node.
			void detach() noexcept {
				node_ = nullptr;
				completions_->detach();
			}
			/// The `closed` a handoff of this configuration gets: on the main thread, later.
			std::move_only_function<void(std::uint64_t)> handoffClosedCall() {
				return [self = shared_from_this()](std::uint64_t droppedFrames) {
					if (Node* node = self->attachedNode())
						node->handoffClosed(node->config_->generation, droppedFrames);
				};
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
				};
			}
			/// The `closed` the run object gets: from any thread, later or inside close().
			std::move_only_function<void()> closedCall() {
				return [self = shared_from_this(), main = std::this_thread::get_id()] {
					// Only the main thread reads insideClose_. A run object that holds no threads finishes inside
					// close(), so a restart or removal of a plain node takes no loop turn.
					if (std::this_thread::get_id() == main && self->insideClose_) {
						self->closedInside_ = true;
						return;
					}
					self->completions_->post([self] {
						if (Node* node = self->attachedNode()) node->runClosed();
					});
				};
			}
			/// Main thread: close() on `instance` (a legacy run object's close, or `closed` at once). True if it called
			/// `closed` before it returned.
			bool close(ErasedInstance& instance) {
				insideClose_ = true;
				try {
					instance.close(closedCall());
				} catch (...) {
					insideClose_ = false;
					throw;
				}
				insideClose_ = false;
				return closedInside_;
			}

		private:
			/// Main thread: the node, while this configuration is attached.
			Node* attachedNode() const noexcept { return node_; }
			Node* node_; // main thread only
			/// The way back to the main loop for everything this configuration's threads tell, detached with node_; its
			/// attached() is node_ for the threads that cannot read it. Slice 3c's I/O jobs report through it too.
			const std::shared_ptr<detail::Completions> completions_;
			const NodeName name_;    // a copy: a leaked configuration may outlive its node
			const std::string type_; // a log entry's source when the type gave none
			const std::vector<LogSink*> sinks_;
			std::mutex sayMutex_;
			detail::NoteGate gate_;     // sayMutex_
			bool closing_ = false;      // main thread only
			bool insideClose_ = false;  // main thread only
			bool closedInside_ = false; // main thread only
		};

		/// What the instance's NodeOf wraps (lifecycle.hpp): the node's own for one configuration, declared before the
		/// instance, so it outlives it.
		class Context final : public NodeContext {
		public:
			Context(AudioFormat format, NodeName name, Channel& channel) noexcept
				: format_(format), name_(std::move(name)), channel_(channel) {}
			AudioFormat format() const noexcept override { return format_; }
			const NodeName& node() const noexcept override { return name_; }
			NodeHandle& handle() noexcept override { return channel_; }

		private:
			AudioFormat format_;
			NodeName name_; // a copy: a leaked configuration may outlive its node
			Channel& channel_;
		};

		/// The catch a type's consumer is called through, on the one thread that calls it (its sink's or its legacy
		/// handoff's): each call through caughtFrom, so what it throws is told as its configuration's fact
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

		/// A legacy handoff's consumer as its handoff calls it (docs/writing-a-node-type.md#the-old-interface): after a
		/// consume that threw no more consume; finish still runs once, after the last.
		class GuardedHandoffConsumer final : public HandoffConsumer {
		public:
			GuardedHandoffConsumer(HandoffConsumer& type, std::shared_ptr<Channel> channel, const std::string& bank)
				: type_(type), calls_(std::move(channel), "the handoff " + bank + "'s consumer") {}
			void consume(std::span<const float> interleaved, std::uint32_t frames) override {
				if (!calls_.threw()) calls_("consume", [&] { type_.consume(interleaved, frames); });
			}
			void finish() override {
				calls_("finish", [&] { type_.finish(); });
			}

		private:
			HandoffConsumer& type_; // the type's, which outlives the handoff (HandoffRequest)
			ConsumerCalls calls_;
		};

		/// One cx.sink call (docs/writing-a-node-type.md#sinks): the sink, its one consumer until Sink::begin takes it,
		/// and what a drive binds. By pointer in the configuration, so a SinkRequest& the type holds survives a later
		/// sink.
		class SinkSlot final : public SinkRequest {
		public:
			SinkSlot(Node& node, std::string name) : bank(std::move(name)), node_(&node) {}
			/// Once, not null, inside configure; a broken rule is recorded as every context call's is.
			void consumedBy(std::unique_ptr<SinkConsumer> consumer) override {
				node_->guarded([&] {
					node_->requireConfiguring("consumedBy");
					if (given) throw std::logic_error("consumedBy called twice on sink " + bank);
					if (!consumer) throw std::logic_error("consumedBy given no consumer on sink " + bank);
					given = true;
					consumer_ = std::move(consumer);
				});
			}
			/// Main thread, at configure's applied return: the consumer handed to the sink, called through its catch.
			void begin(std::shared_ptr<Channel> channel, std::move_only_function<void()> begun) {
				sink->begin(std::make_unique<GuardedSinkConsumer>(std::move(consumer_), std::move(channel), bank),
				            std::move(begun));
			}

			std::string bank;
			std::unique_ptr<Sink> sink;
			SinkBinding binding{};
			bool given = false;                     // consumedBy was called
			bool closed = false;                    // its close reported
			std::size_t item = detail::noReadiness; // its consumer's readiness item: ready once it began

		private:
			Node* node_;
			std::unique_ptr<SinkConsumer> consumer_; // until Sink::begin takes it
		};

		/// One cx.source call (docs/writing-a-node-type.md#sources): the source, its bank, and what a drive binds.
		struct SourceSlot {
			std::string bank;
			std::unique_ptr<Source> source;
			SourceBinding binding{};
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

		/// One declareLatency: the bank it is declared on and, for a typed declaration, the input bank its path
		/// starts at.
		struct DeclaredLatency {
			std::optional<std::string> from;
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
			// Outlives the handoffs that write it and the instance (a legacy run object and recipe) that may hold it
			// open (file's WavWriter closes its SNDFILE on the descriptor when freed).
			std::vector<std::unique_ptr<RecordingFile>> recordings;
			// Each sink's consumer may write a recording above; freed after the code and instance (teardown step 5).
			std::vector<std::unique_ptr<SinkSlot>> sinks;
			std::vector<SinkBinding> bindings; // every sink's, built at the return checks; every drive points at it
			// Each source's writer may be held by the instance's threads; freed after the instance (teardown step 5).
			std::vector<SourceSlot> sources;
			std::vector<SourceBinding> sourceBindings; // every source's, built at the return checks, likewise
			std::unique_ptr<ErasedInstance> instance;  // the legacy recipe and run object inside it
			std::unique_ptr<AudioCode> code;           // may read the instance, so goes first
			// Each handoff's catch around the type's consumer: outlives the handoff that calls it.
			std::vector<std::unique_ptr<GuardedHandoffConsumer>> handoffConsumers;
			std::vector<std::unique_ptr<Handoff>> handoffs;
			std::vector<EngineAsk> engineAsks; // in request order
			std::vector<std::unique_ptr<Companion>> engineCompanions;
			detail::ReadinessList readiness; // every engine ask's item, then every sink's, from configure's return
			std::vector<Asked> requestOrder; // every companion, as asked: the companions' list's order
			std::unique_ptr<Timer> deadline;
			std::unique_ptr<Timer> bound; // the request in flight's bound: the fade's or the barrier's
			std::chrono::steady_clock::time_point postedAt; // when that request was posted
			// One flag per output in process()'s order (outputs, then inside outputs): every one the code writes is
			// faded but a sink's bank. Read by the block until the drive's invoke ran.
			std::unique_ptr<bool[]> faded;
			std::size_t fadedCount = 0;
			std::vector<BankPair> pairs;          // the pass-through's pairs, for the latency plan
			std::vector<DeclaredLatency> latency; // what configure declared, one per bank
			LatencyPlan plan;                     // worked out at configure's return; carried with each drive
			bool codeGiven = false;               // use() was called
			bool startFailed = false;     // configure failed, threw or broke a rule: nothing to make, prepare or drive
			bool companionsAsked = false; // its engine companions' making is queued
			bool recipeRan = false;       // T3 ran (or there was no recipe)
			bool prepared = false;
			bool tearing = false; // teardown step 3 began
			bool ending = false;  // teardown step 1 began: its companions' list is empty from here
			std::size_t handoffsClosed = 0;
			std::size_t sinksClosed = 0;
		};

		/// Everything of a configuration that did not close in time and that its threads can still reach: never freed.
		struct Leaked {
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			std::unique_ptr<Context> context;
			std::unique_ptr<ErasedInstance> instance;
			std::unique_ptr<AudioCode> code;
			std::vector<std::unique_ptr<RecordingFile>> recordings;
			std::vector<std::unique_ptr<SinkSlot>> sinks;
			std::vector<SinkBinding> bindings;
			std::vector<SourceSlot> sources; // the block may still play them
			std::vector<SourceBinding> sourceBindings;
			std::vector<std::unique_ptr<GuardedHandoffConsumer>> handoffConsumers;
			std::vector<std::unique_ptr<Handoff>> handoffs;
			std::vector<std::unique_ptr<Companion>> engineCompanions; // a wedged loop's, never destroyed
			std::unique_ptr<bool[]> faded;
		};

		/// The newest committed settings, and whether a drive of them fades: some output the code writes exists.
		lc::Queued queued() const noexcept {
			return {version_, !ports_.outputs().empty() || !ports_.insideOutputs().empty()};
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
			                  superseded_};
		}

		/// What the node waits for now, unpaced (3a's kinds): an older node of its name; its engine companions
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
		/// resource ready at once, a run object closed inside close()) is queued behind them, never handled inside.
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
			    aggregate_.intent() != lc::Intent::none)
				config_->channel->closing();
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

		/// Removed (the aggregate's Gone): told gone once its block's destruction ran in the flush, not when
		/// queued, and, for a port-caused replace, once its older node is gone too, whose dropped frames it reports:
		/// everything of the name it stands for is gone only then.
		void tellGone() {
			if (!goneHeld_ || gone_ || destroyPending_ || predecessor_ != 0) return;
			host_.gone(*this, dropped_);
		}

		void tell(const lc::Health& health) {
			if (health.phase != wantedPhase_ || !told_) phaseBegan(health.phase);
			tell(composed(health));
		}

		void tell(NodeHealth now) {
			if (told_ && *told_ == now) return;
			told_ = now;
			if (!silent_) host_.changed(*told_, superseded_);
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
			case Kind::runRecipe:
				break; // ran before every resource was told ready (checkReady), so a failed recipe is a fault in readying
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
					host_.diagnosed(name_, "ear.fade-unacknowledged",
					                "the fade before its silence was not acknowledged within " +
					                    std::to_string(judgedMs_) + " ms, so the silence was taken unfaded",
					                fields);
				}
				break;
			case Kind::barrierOverdue:
				if (!synthetic_) {
					const std::array fields{DiagnosticField{"ms", judgedMs_}};
					host_.diagnosed(name_, "ear.barrier-overdue",
					                "a barrier posted to the data loop did not complete within " +
					                    std::to_string(judgedMs_) + " ms, so its configuration and block are leaked",
					                fields);
				}
				break;
			case Kind::markWedged:
				host_.markWedged(name_, judgedMs_);
				break;
			}
		}

		/// Records an engine companion to make once the block is ready, under its one name, on a bank of its own.
		template <class Shape> void askEngineCompanion(Shape shape, const Ports::Bank& bank) {
			given(host_.resources.adapterFor(shape),
			      std::is_same_v<Shape, SoundCardShape> ? "sound cards" : "RTP receivers");
			const NodeName engineNode = name_ + "." + std::string(engineSuffix(shape, bank.direction));
			requireOneName(engineNode);
			requireFreeBank(bank);
			config_->engineAsks.push_back(EngineAsk{std::move(shape), CompanionTarget{.engineNode = engineNode,
			                                                                          .blockNode = name_,
			                                                                          .bank = bank.direction,
			                                                                          .blockPorts = bank.ports,
			                                                                          .format = host_.format,
			                                                                          .insideBank = bank.name}});
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

		/// A typed declaration (`from` given) is keyed by its path, so a code with several inputs into one output (a
		/// mixer) declares each path once; a legacy declaration on one bank is keyed by its bank, once.
		void recordLatency(const std::string& bank, Latency latency, std::optional<std::string> from = std::nullopt) {
			if (std::ranges::any_of(config_->latency, [&](const DeclaredLatency& declared) {
					return declared.bank == bank && declared.from == from;
				}))
				throw std::logic_error(from ? "latency declared twice for the path " + *from + " to " + bank
				                            : "latency declared twice for bank " + bank);
			config_->latency.push_back(DeclaredLatency{std::move(from), bank, latency});
		}

		/// The configuration's latency plan, by role (docs/engine.md#latency-per-bank-pair). What each source's ring
		/// may hold is latency beyond its bank: the most its frames wait before they play; and a module-run
		/// converter's buffer (moduleRunDelay), which its ports report as zero. A typed declaration (from, to) is a
		/// path of its own: its pair is carried (added for a code of the type's own; a pass-through must already copy
		/// it, checked at configure's return) with the delay on it, both ways. A legacy declaration on one bank is the
		/// delay on the pass-through's pair that ends at the bank, or, on a bank no pair ends at, that bank's alone.
		LatencyPlan latencyPlan(const Configuration& c) const {
			LatencyPlan plan{.pairs = c.pairs};
			for (const SourceSlot& slot : c.sources)
				plan.beyond.emplace_back(slot.bank, Delay{Latency{0, slot.source->writer().capacityFrames()}});
			for (const EngineAsk& asked : c.engineAsks)
				std::visit(
					[&]<class Shape>(const Shape& shape) {
						if constexpr (ModuleRunShape<Shape>)
							plan.beyond.emplace_back(asked.target.insideBank,
						                             moduleRunDelay(shape, FrameClock(host_.format.rate)));
					},
					asked.shape);
			for (const DeclaredLatency& declared : c.latency) {
				if (declared.from) {
					const BankPair pair{*declared.from, declared.bank};
					if (std::ranges::find(plan.pairs, pair) == plan.pairs.end()) plan.pairs.push_back(pair);
					plan.onPair.emplace_back(pair, Delay{declared.latency});
					continue;
				}
				bool paired = false;
				for (const BankPair& pair : c.pairs)
					if (pair.to == declared.bank) {
						plan.onPair.emplace_back(pair, Delay{declared.latency});
						paired = true;
					}
				if (!paired) plan.onBank.emplace_back(declared.bank, Delay{declared.latency});
			}
			return plan;
		}

		/// Every context call is valid only inside its configure.
		void requireConfiguring(std::string_view call) const {
			if (!configuring_) throw std::logic_error(std::string(call) + " outside configure");
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
					host_.diagnosed(name_, "ear.hook-slow",
					                std::string(hook.name) + " ran " + std::to_string(ms) + " ms", fields);
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

		/// T2 (docs/writing-a-node-type.md#configure-and-createof): a new generation and its context; the instance made
		/// and its configure called, timed together as the hook `configure`; then its answer and the return checks. A
		/// throw, a failed answer or a broken rule is a fault in readying (start-failed); else the code is kept,
		/// unprepared, and the resources are asked for.
		void configure() {
			if (config_) throw std::logic_error("node " + name_ + ": a configuration began while another had not gone");
			config_ = std::make_unique<Configuration>();
			Configuration& c = *config_;
			c.generation = ++host_.generations;
			c.config = queued_;
			c.channel = std::make_shared<Channel>(*this, host_.mainLoop, host_.timers, name_, type_, host_.logSinks);
			c.context = std::make_unique<Context>(host_.format, name_, *c.channel);
			c.fadedCount = ports_.outputs().size() + ports_.insideOutputs().size();
			c.faded = std::make_unique<bool[]>(c.fadedCount);
			std::fill_n(c.faded.get(), c.fadedCount, true);
			// Until configure returned a code that holds, the configuration asks for nothing more.
			c.startFailed = true;
			configuring_ = true;
			std::expected<Configured, TypeFault> answer = callType(hooks::configure, [&] {
				c.instance = c.config->make(*c.context);
				InstanceContexts contexts{*this, *this};
				return c.instance->configure(contexts);
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
				return failConfigure(c.instance->engineRefused() ? lc::Fact::engine : lc::Fact::configureFailed,
				                     answer->detail());
			}
			// The return checks, on an applied answer only.
			if (!c.codeGiven) return failConfigure(lc::Fact::configureFailed, "configure gave no code");
			if (!c.code) {
				try {
					c.code = makePassThrough(ports_, c.pairs);
				} catch (const std::logic_error& error) {
					return failConfigure(lc::Fact::configureFailed,
					                     std::string("configure gave a pass-through: ") + error.what());
				}
			}
			// Each sink has exactly one consumer.
			for (const auto& slot : c.sinks)
				if (!slot->given)
					return failConfigure(lc::Fact::configureFailed,
					                     "configure gave sink " + slot->bank + " no consumer");
			// A declared path must exist: a pass-through copies only its pairs, so a delay declared on any other path
			// would lie on audio that never flows there.
			if (!c.pairs.empty())
				for (const DeclaredLatency& declared : c.latency)
					if (declared.from &&
					    std::ranges::find(c.pairs, BankPair{*declared.from, declared.bank}) == c.pairs.end())
						return failConfigure(lc::Fact::configureFailed, "configure declared latency on " +
						                                                    *declared.from + " to " + declared.bank +
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
			// Every drive of this configuration binds these taps; then each consumer begins on its sink's thread.
			c.bindings.reserve(c.sinks.size());
			for (const auto& slot : c.sinks) c.bindings.push_back(slot->binding);
			c.sourceBindings.reserve(c.sources.size());
			for (const auto& slot : c.sources) c.sourceBindings.push_back(slot.binding);
			// Every readiness item exists before a consumer begins, so a begun told at once still finds its item.
			addReadiness();
			for (std::size_t i = 0; i < c.sinks.size(); ++i) c.sinks[i]->begin(c.channel, c.channel->sinkBegunCall(i));
			c.readiness.begin([this, generation = c.generation](std::size_t item) -> std::unique_ptr<Timer> {
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
			}
			const auto sinkSeconds = std::chrono::duration_cast<std::chrono::seconds>(sinkReadinessBound).count();
			for (const auto& slot : c.sinks)
				slot->item = c.readiness.add(detail::ReadinessItem{
					.kind = WaitingFor::Kind::consumer,
					.name = {}, // the wire names no sink for a consumer's wait (docs/control-port.md#node-health)
					.step = detail::ReadinessStep::companions,
					.bound = readinessBound(CompanionKind::sink),
					.fact = lc::Fact::consumerNeverBegan,
					.overdue = [text = "the sink " + slot->bank + "'s consumer did not begin within " +
				                       std::to_string(sinkSeconds) + " s"] { return text; },
				});
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
			return current(generation) && !config_->startFailed && !config_->tearing && !config_->recipeRan &&
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
				node.closeRunObject();
			});
		}

		/// configure failed, threw or broke a rule: nothing was driven by its code, so the handoffs may go now (their
		/// consumers may have gone with a legacy start's result), and so may the sinks, none begun (a consumer given is
		/// freed unbegun, with nothing to finish); no engine companion was made. The recordings and the instance stay
		/// until step 5 frees them, the instance first. The fault is an input of its own.
		void failConfigure(lc::Fact fact, std::string detail) {
			Configuration& c = *config_;
			c.handoffs.clear();
			c.sinks.clear();
			closeSources(); // freed at step 5, after the instance that may hold their writers
			c.engineAsks.clear();
			c.channel->detach();
			host_.inputFor(serial_, [generation = c.generation, fact, detail = std::move(detail)](Node& node) mutable {
				if (!node.current(generation)) return;
				node.feed(node.aggregate_.fault(fact, std::move(detail)));
			});
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
		}

		/// Readiness, as far as slice 3a has it: the engine companions, made in the flush in request order once the
		/// block is ready (their ports exist); with none pending, every resource is ready at once (in this turn, as its
		/// own input). An adapter that tells ready() inside make() is heard after that item, and every ready is judged
		/// against every engine companion made.
		void askResources() {
			if (!config_ || config_->startFailed || config_->recipeRan ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			if (c.engineAsks.empty()) {
				host_.inputFor(serial_, [generation = c.generation](Node& node) { node.checkReady(generation); });
				return;
			}
			if (aggregate_.block().state() != lc::BlockState::ready || c.companionsAsked) return;
			c.companionsAsked = true;
			c.engineCompanions.reserve(c.engineAsks.size());
			for (std::size_t i = 0; i < c.engineAsks.size(); ++i)
				host_.queue(FlushKind::makeCompanion, name_,
				            [&host = host_, serial = serial_, generation = c.generation, i] {
								if (Node* node = host.find(serial)) node->makeCompanion(generation, i);
							});
		}

		/// Every resource ready: T3. The recipe builds the run object first, so a failed recipe is a fault in
		/// readying, before anything is prepared or driven; then the machine hears it.
		void checkReady(std::uint64_t generation) {
			if (!current(generation) || config_->startFailed || config_->recipeRan ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			// An engine companion's item is ready only once it was made, so a done list holds every one made.
			if (!c.readiness.done()) return;
			c.recipeRan = true;
			if (c.instance->hasRecipe()) {
				building_ = true;
				auto built = callType(hooks::recipe, [&] { return c.instance->ready(*this); });
				building_ = false;
				// A recipe that throws, or breaks a rule, is a type bug that fails its node like a failed recipe.
				if (!built) {
					feed(aggregate_.fault(lc::Fact::recipeFailed, std::move(built.error().detail)));
					return;
				}
				if (auto& made = *built; !made) {
					feed(
						aggregate_.fault(factOf(made.error(), lc::Fact::recipeFailed), std::move(made.error().detail)));
					return;
				}
			}
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
			block_->drive(DriveRequest{c.code.get(), std::span<const bool>(c.faded.get(), c.fadedCount), c.bindings,
			                           c.sourceBindings},
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

		/// Teardown step 1: every source's writer closed (a write or idle waiting returns at once), and the
		/// configuration's companions leave its list (told at the end of the input, after the health that began the
		/// teardown). Idempotent.
		void closeSources() noexcept {
			if (!config_) return;
			config_->ending = true;
			for (SourceSlot& slot : config_->sources) slot.source->close();
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
					const EngineAsk& ask = c.engineAsks[asked.index];
					list.push_back(CompanionView{
						.name = ask.target.engineNode,
						.kind = std::visit([&](const auto& shape) { return engineKind(shape, ask.target.bank); },
					                       ask.shape),
						.bank = ask.target.insideBank,
						.ready = c.readiness.isReady(ask.item)});
					break;
				}
				case Asked::Family::sink: {
					if (asked.index >= c.sinks.size()) break;
					const SinkSlot& slot = *c.sinks[asked.index];
					list.push_back(CompanionView{.name = name_ + "." + slot.bank,
					                             .kind = CompanionKind::sink,
					                             .bank = slot.bank,
					                             .ready = c.readiness.isReady(slot.item),
					                             .dropped = slot.sink->droppedFrames()});
					break;
				}
				case Asked::Family::source: {
					if (asked.index >= c.sources.size()) break;
					const SourceSlot& slot = c.sources[asked.index];
					list.push_back(CompanionView{.name = name_ + "." + slot.bank,
					                             .kind = CompanionKind::source,
					                             .bank = slot.bank,
					                             .ready = true,
					                             .shortfall = slot.source->shortfallFrames()});
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
				return a.name == b.name && a.kind == b.kind && a.bank == b.bank && a.ready == b.ready;
			};
			if (std::ranges::equal(now, companionsTold_, sameEntry)) return;
			companionsTold_ = now;
			if (!silent_ && !superseded_) host_.companionsChanged(name_, now);
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
			                         : state == lc::ConfigState::silencing || state == lc::ConfigState::arming;
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
				feedSilently(Bound::barrier);
				break;
			case lc::ConfigState::fading:
				feedSilently(Bound::fade);
				break;
			case lc::ConfigState::none:
			case lc::ConfigState::driving:
			case lc::ConfigState::tearing:
				break;
			}
		}

	private:
		void release() {
			// A silent block carries nothing through and adds nothing; on a wedged loop no PipeWire call is made.
			if (block_ && !host_.wedged) block_->carryLatency({});
			if (config_->prepared) {
				const MainLoopActivity::Scope activity("release", name_);
				config_->code->release();
			}
			config_->prepared = false;
		}

		/// teardown steps 1's rest to 3: the engine companions; the handoffs closed, each on its own thread. Steps 3
		/// and 4 have the close deadline. The rest goes once every handoff has closed.
		void teardown() {
			Configuration& c = *config_;
			c.channel->closing();
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
			c.engineAsks.clear();
			c.readiness.disarm();
			c.deadline =
				host_.timers.after(host_.closeDeadline, [&host = host_, serial = serial_, generation = c.generation] {
					host.inputFor(serial, [generation](Node& node) {
						if (node.current(generation) && node.config_->tearing) node.feed(node.aggregate_.closeBound());
					});
				});
			c.tearing = true;
			c.handoffsClosed = 0;
			c.sinksClosed = 0;
			closingHandoffs_ = true;
			// A sink drains what its ring holds and finishes a consumer that began; a legacy handoff likewise.
			for (std::size_t i = 0; i < c.sinks.size(); ++i) c.sinks[i]->sink->close(c.channel->sinkClosedCall(i));
			for (auto& handoff : c.handoffs) handoff->close(c.channel->handoffClosedCall());
			closingHandoffs_ = false;
			closeRunObject();
		}

		void handoffClosed(std::uint64_t generation, std::uint64_t droppedFrames) {
			host_.inputFor(serial_, [generation, droppedFrames](Node& node) {
				if (!node.current(generation)) return;
				node.dropped_ += droppedFrames;
				++node.config_->handoffsClosed;
				node.closeRunObject();
			});
		}

		/// teardown step 4, once every sink and handoff reported closed, and never inside a close() call, so no handoff
		/// is destroyed while that loop is still calling it. The rest goes once the run object says it has closed.
		void closeRunObject() {
			if (!config_ || !config_->tearing || closingHandoffs_ || runClosing_ ||
			    config_->handoffsClosed != config_->handoffs.size() || config_->sinksClosed != config_->sinks.size())
				return;
			config_->handoffs.clear();
			runClosing_ = true;
			const bool closedInside = [&] {
				if (!config_->instance) return true;
				auto closed = callType(hooks::close, [&] { return config_->channel->close(*config_->instance); });
				if (closed) return *closed;
				// A legacy RunObject::close that throws: written, and the configuration taken as closed (facts are
				// dropped once a teardown began), so the teardown goes on.
				host_.logged(LogEntry{LogLevel::error, "ear", std::move(closed.error().detail), {}, name_});
				return true;
			}();
			if (closedInside) runClosed();
		}

		/// The run object's threads are gone: every sink and handoff closed, then the run object (the machine's closed).
		void runClosed() {
			if (!runClosing_) return;
			runClosing_ = false;
			host_.inputFor(serial_, [generation = config_->generation](Node& node) {
				if (node.current(generation)) node.feed(node.aggregate_.closed());
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

		/// teardown step 5, on the main loop: the code (released and unreachable since the barrier; it may hold
		/// references into the instance, never the reverse), then the instance (a legacy run object and recipe in it),
		/// the recordings, and last its context, channel and config.
		void freeConfiguration() {
			const MainLoopActivity::Scope activity("free configuration", name_);
			Configuration& c = *config_;
			c.deadline.reset();
			c.channel->detach();
			companionsEnded(); // closed (no tap or play since the barrier), so their counts are final
			c.code.reset();
			c.instance.reset();
			c.sinks.clear();   // each closed (its thread idle) or never begun
			c.sources.clear(); // after the instance, whose threads may hold a writer
			c.recordings.clear();
			config_.reset();
		}

		/// The close deadline passed with step 3 or 4 unfinished: said, then leaked.
		void closeOverdue() {
			// Once every handoff closed they are cleared and the run object is closing.
			const std::size_t handoffsOpen = runClosing_ ? 0 : config_->handoffs.size() - config_->handoffsClosed;
			const std::size_t sinksOpen = runClosing_ ? 0 : config_->sinks.size() - config_->sinksClosed;
			host_.diagnosed(name_, "ear.close-overdue",
			                "the run did not close within " + std::to_string(host_.closeDeadline.count()) +
			                    " ms and is left running (" + std::to_string(handoffsOpen) + " handoffs open, " +
			                    (sinksOpen > 0 ? std::to_string(sinksOpen) + " sinks open, " : std::string()) + "run " +
			                    (runClosing_ ? "closing" : "not yet told to close") + ")",
			                {});
		}

		/// The configuration is detached, so nothing it tells lands; what its threads can reach is leaked, never freed;
		/// and the node moves on.
		void leak() {
			Configuration& c = *config_;
			c.channel->detach();
			companionsEnded(); // their counts as they stand: the configuration is never read again
			// Deliberately never deleted: the configuration's threads may still touch any of it.
			static_cast<void>(new Leaked{std::move(c.config), std::move(c.channel), std::move(c.context),
			                             std::move(c.instance), std::move(c.code), std::move(c.recordings),
			                             std::move(c.sinks), std::move(c.bindings), std::move(c.sources),
			                             std::move(c.sourceBindings), std::move(c.handoffConsumers),
			                             std::move(c.handoffs), std::move(c.engineCompanions), std::move(c.faded)});
			++host_.leaked;
			config_.reset();
			runClosing_ = false;
		}

		Impl& host_;
		NodeName name_;
		std::string type_; // its NodeSpec type, a log entry's source when the type gave none
		std::uint64_t serial_;
		lc::NodeAggregate aggregate_;
		Ports ports_;
		std::shared_ptr<const NodeConfig> queued_;  // the newest committed config, the next configure's
		std::uint64_t version_ = 0;                 // queued_'s, as the aggregate compares it
		std::optional<NodeHealth> told_;            // the health last told
		NodePhase wantedPhase_ = NodePhase::added;  // the phase last told, whose waits are shown
		std::vector<WaitingFor> waitingShown_;      // what it waits for, as health shows it
		std::vector<CompanionView> companionsTold_; // its companions' list as last told (or due to be, if silent)
		bool waitingAllowed_ = false;               // the phase has lasted 2 s
		std::unique_ptr<Timer> phaseTimer_;         // the phase's 2 s
		std::unique_ptr<Timer> paceTimer_;          // the 1 s pace of a changed list
		std::uint64_t predecessor_ = 0;             // the port-caused replace's older node, while not gone
		bool silent_ = false;                       // the older node of a port-caused replace: tells nobody
		bool superseded_ = false;                   // a newer node of its name was added: standard output alone
		std::unique_ptr<Configuration> config_;     // the current configuration (at most one)
		std::unique_ptr<PortBlock> block_;
		bool configuring_ = false;         // inside the instance's making and configure: the contexts' calls are valid
		bool building_ = false;            // inside a legacy recipe: events() is valid
		std::optional<EarFault> earFault_; // the first fault of ear's own in the hook running (guarded)
		std::exception_ptr refused_;       // an adapter's refusal, thrown on through guarded as itself
		std::optional<std::string> brokenRule_;   // the first rule a context call found broken in the hook running
		std::optional<std::string> prepareThrew_; // a code's prepare that threw, a fault after its batch
		std::optional<std::string> startThrew_;   // a start() that threw, likewise
		std::optional<std::string> stopThrew_;    // a stop() that threw during a stop, likewise
		std::set<std::string_view, std::less<>> hooksReportedSlow_; // ear.hook-slow said, for the node's life
		bool closingHandoffs_ = false;
		bool runClosing_ = false;
		bool makePending_ = false;         // the block's making is queued in the flush
		bool destroyPending_ = false;      // the block's destruction is queued in the flush
		bool goneHeld_ = false;            // removed: told gone once its block and its predecessor are gone
		bool gone_ = false;                // told gone
		bool blockLeaked_ = false;         // its block was leaked on a wedged loop: never freed, nor is this node
		bool synthetic_ = false;           // a bound judged at once on a wedged loop: its diagnostics are not said
		std::uint64_t completionsRun_ = 0; // `done`s its block ran, by event or settle(): a bound sees one it settled
		std::int64_t judgedMs_ = 0;        // how long the judged request waited, for its diagnostics
		std::uint64_t dropped_ = 0;
		std::uint64_t span_;      // its span of counts: its serial, or its port-caused older node's
		NodeCounts blockCounted_; // what its block counted, once the block went
		NodeCounts carried_;      // what the older node of its port-caused replace counted, once that one is gone
		bool predecessorCarried_ = false; // carried_ holds the older node's counters: they are not read from it again
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
	std::vector<EngineDiagnosticListener*> engineListeners;
	std::vector<NodeCountsListener*> countsListeners;
	std::vector<NodeCompanionsListener*> companionListeners;
	std::vector<LogSink*> logSinks;
	std::vector<InternalErrorListener*> internalErrorListeners;
	bool wedged = false;                      // the data loop was judged wedged: no PipeWire call is made again
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; removed ones are pruned on add
	std::uint64_t generations = 0;            // every configuration's generation, across nodes, so none is ever reused
	std::uint64_t serials = 0;                // every node's serial, so a completion never finds a newer node
	std::size_t leaked = 0;                   // configurations, blocks and engine companions never freed
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

	/// A node added (a replace's new node when `change` is given): it waits for older nodes of its name (T1, rule 1),
	/// which are superseded from now on; a port-caused replace carries its older node's stop when both are enabled,
	/// and its one health stream.
	void add(const NodeSpec& spec, std::shared_ptr<const NodeConfig> config, const NodeChange* change) {
		requireOpen("nodeAdded", spec.name);
		std::erase_if(nodes, [](auto& known) {
			if (!known->removed()) return false;
			if (known->blockLeaked()) static_cast<void>(known.release()); // its leaked block may still tell it
			return true;
		});
		const std::uint64_t serial = ++serials;
		nodes.emplace_back(std::make_unique<Node>(*this, spec.name, spec.type, serial));
		inputFor(serial, [this, spec, config = std::move(config),
		                  change = change ? std::optional<NodeChange>(*change) : std::nullopt](Node& added) mutable {
			// Older nodes of the name, as they stand when this input is handled (T1, rule 1).
			bool olderBlock = false;
			bool olderNode = false;
			Node* older = nullptr;
			for (const auto& known : nodes) {
				if (known.get() == &added) break;
				if (known->name() != added.name() || known->removed()) continue;
				olderNode = true;
				olderBlock = olderBlock || known->holdsBlock();
				if (!known->silent()) known->supersede();
				older = known.get();
			}
			Node::Carried carried;
			// A disabled new node is not shown stopping, and carries no stop, but it still stands for its older
			// node's removal and dropped frames.
			if (change && change->portCaused && older != nullptr && older->silent()) {
				carried.predecessor = older->serial();
				carried.stopping = !spec.disabled;
				carried.stopHeld = !spec.disabled && older->stopHeld() && !older->disabled();
				carried.keys = change->keys;
			}
			// A node that carries no older node's counts begins a span of its name's counts from zero; a port-caused
			// replace goes on in its older node's span.
			if (carried.predecessor == 0)
				for (auto* listener : countsListeners) listener->countsBegan(added.name(), added.span());
			else added.carrySpan(older->span());
			added.add(spec, std::move(config), olderBlock, olderNode, std::move(carried));
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

	void diagnosed(const NodeName& node, std::string_view code, const std::string& detail,
	               std::span<const DiagnosticField> fields) {
		for (auto* listener : diagnosticListeners) listener->nodeDiagnostic(node, code, detail, fields);
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

	/// A node is gone; the oldest node left of its name, which no older node holds back now, may configure. The older
	/// node of a port-caused replace tells nobody, and its successor takes its dropped frames.
	void gone(Node& node, std::uint64_t droppedFrames) {
		node.markGone();
		// Its span's final counts, which nothing answers once it is gone, a superseded node's too; a port-caused
		// replace's older node's counts go on in its newer node's span (below), so it ends none.
		if (!node.silent())
			for (auto* listener : countsListeners) listener->countsEnded(node.name(), node.span(), node.counts());
		if (!node.silent())
			for (auto* listener : listeners) {
				if (!node.superseded()) listener->nodeGone(node.name(), droppedFrames);
				else if (listener->hearsSuperseded()) listener->supersededGone(node.name(), droppedFrames);
			}
		const auto successor = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->predecessor() == node.serial() && !known->removed(); });
		if (successor != nodes.end()) {
			// Carried at once, not in the input below, so no reading between the two counts the older node twice or not
			// at all.
			(*successor)->carryCounts(node.counts());
			inputFor((*successor)->serial(), [droppedFrames](Node& next) { next.predecessorGone(droppedFrames); });
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
	: impl_(std::make_unique<Impl>(format, blocks, resources, mainLoop, timers, closeDeadline)) {}

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
	impl_->add(node, std::move(config), nullptr);
}

void NodeHost::nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) {
	impl_->add(node, std::move(config), &change);
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
	impl_->inputFor(impl_->wanted(node).serial(), [portCaused = change.portCaused](Impl::Node& wanted) {
		wanted.remove(portCaused ? Impl::Node::Replaced::portCaused : Impl::Node::Replaced::superseded);
	});
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
