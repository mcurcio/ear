// The node framework (docs/design/node-framework.md sections 6, 7.2 and 7.3; node_host.hpp), on the node lifecycle's
// two pure machines (docs/design/node-lifecycle.md section 4, node_machines.hpp; step L-3a of slice 3a). Each node is a
// NodeAggregate: NodeHost feeds it every input (a commit, a block's or an outside node's event, a completion, a post
// from the node's own threads, a timer) and carries out the actions it returns (5.2), never waiting for another thread.
//
// The port block is made when the node is added and goes only once it is removed (I1); configurations come and go
// behind it, one at a time (I7). Every type runs through the legacy adapter (5.10): configure is the type's start(),
// which only acquires; the outside nodes it asked for are made once the block is ready; once every resource is ready
// the recipe builds the run object (T3), the code is prepared, and once the block is ready it is driven by an
// asynchronous drive (T5, 5.3), whose completion the block posts and NodeHost checks against the node's serial, its
// configuration's generation and the barrier number (5.6) before the machine hears it. A configuration is silenced the
// same way, then torn down by dependency (I5): release(); outside nodes; handoffs closed, each on its own thread; the
// run object closed, on its own threads; then, on the main loop, the code, the run object, the recipe, the recordings
// and the config. Steps 3 and 4 have the close deadline (I5a): past it the configuration is leaked, never freed.
// Everything a configuration's threads tell crosses to the main loop and lands only on that configuration (I2, I6).
//
// PipeWire's own object changes (a block made or destroyed, an outside node made or destroyed) are never made inside an
// input: they are queued into the turn's flush (5.5, step L-3b), posted to the main loop once per turn and run there in
// one go, destroys first (outside nodes, then blocks), then makes (blocks, then outside nodes, which need their block's
// ports). The flush works on a snapshot: an item an input causes while it runs (a waiting block freed by a destroy)
// runs behind the snapshot in the same flush, and an event an adapter tells inside an item (an outside node ready
// inside its making) is an input of its own, handled once that item returned. Each item, and each type's hook, names
// itself in MainLoopActivity while it runs. A name added back (T1) gets its block once the older block of the name was
// destroyed, in the flush that destroyed it, and its configuration once the older node is gone; an outside node joins
// its block by the block's global id (PortBlock::blockId, 5.7).
//
// Fades and bounds (step L-3c, 5.3 and 5.4): every output the code writes (public outputs and inside outputs, which feed
// a card) fades in with each drive and out before each planned silence; a fade unacknowledged within 100 ms is said
// (ear.fade-unacknowledged) and the silence posted unfaded, and a drive or silence not completed within 5 s is a data
// loop that runs no invoke (ear.barrier-overdue, then ear.data-loop-wedged once, through EngineDiagnosticListener).
// Every bound first settles the block, as an input of its own, and judges in the input behind what that settle ran, so a
// completion a stalled main loop has not dispatched yet is heard first and nothing is judged. Once wedged, NodeHost makes
// no PipeWire call again: the flush still runs its items in order, but a destruction leaks what it would destroy (a
// block, an outside node), a making makes nothing (a block is lost "the data loop is wedged"), latency is no longer
// carried, and every drive, fade or silence a configuration would post is judged overdue at once, its configuration
// leaked; a node added afterwards fails engine-refused "the data loop is wedged" before T1. So every removal still ends,
// stopAll's allGone still runs, and main.cpp leaves through std::_Exit (leaked() is above 0).
//
// The old NodeHealth stays until L-3d: the aggregate's health is told mapped onto it, only when that changed, so
// standard output and the wire see what they saw before.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_machines.hpp"
#include "ear/core/pass_through.hpp"
#include "ear/core/run_requests.hpp"

namespace ear {
namespace {

namespace lc = ear::lifecycle;

/// One outside node a configuration asked for, of any kind.
using OutsideRequest = std::variant<SoundCardRequest, RtpReceiverRequest>;

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

/// What a caught exception says, for a detail.
std::string whatOf(const std::exception_ptr& thrown) {
	try {
		std::rethrow_exception(thrown);
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an exception that is no std::exception";
	}
}

std::int64_t millisecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

bool sameHealth(const NodeHealth& a, const NodeHealth& b) {
	if (a.state != b.state || a.reason.has_value() != b.reason.has_value()) return false;
	return !a.reason || (a.reason->code == b.reason->code && a.reason->detail == b.reason->detail);
}

} // namespace

ResourceRegistry::ResourceRegistry(SoundCardAdapter& cards, RtpReceiverAdapter& receivers,
                                   HandoffAdapter& handoffs) noexcept
	: ResourceRegistry(cards, receivers, handoffs, noRecordings) {}

struct NodeHost::Impl {
	/// PipeWire's object changes a turn queues (5.5), in the order a flush runs them: destroys (outside nodes, then
	/// blocks), then makes (blocks, then outside nodes).
	enum class FlushKind { destroyOutside, destroyBlock, makeBlock, makeOutside };
	struct FlushItem {
		FlushKind kind;
		NodeName node;
		std::move_only_function<void()> run;
	};

	/// One wanted node: its aggregate, its port block and its configuration. Members are declared in reverse teardown
	/// order, so destroying a node that never stopped still removes its port block first.
	class Node final : public PortBlockEvents, public StartContext, public Ready {
	public:
		Node(Impl& host, NodeName name, std::uint64_t serial) : host_(host), name_(std::move(name)), serial_(serial) {}
		/// On a wedged data loop nothing that loop may still reach is destroyed (5.4): destroying a block waits on it.
		~Node() {
			if (!host_.wedged) return;
			static_cast<void>(block_.release());
			static_cast<void>(config_.release());
		}
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		const NodeName& name() const noexcept { return name_; }
		std::uint64_t serial() const noexcept { return serial_; }
		/// Gone: its block destroyed (the flush ran its destruction) and its configuration gone (4.3's row 1), and so
		/// told.
		bool removed() const noexcept { return gone_; }
		/// Its block exists in the engine, or its making or destruction is still queued (T1: a newer block of its name
		/// waits for that).
		void markGone() noexcept { gone_ = true; }
		bool holdsBlock() const noexcept { return block_ != nullptr || makePending_ || destroyPending_; }
		/// Its block was leaked on a wedged loop: the block still holds this node as its PortBlockEvents, so the node is
		/// never freed either (5.4).
		bool blockLeaked() const noexcept { return blockLeaked_; }
		/// Its block waits for an older block of its name (T1).
		bool blockWaiting() const noexcept { return aggregate_.block().state() == lc::BlockState::waiting; }
		/// No longer wanted: removed by an edit or by stopAll, still stopping or gone.
		bool removing() const noexcept { return aggregate_.intent() == lc::Intent::remove; }
		NodeHealth health() const { return mapped(aggregate_.health()); }

		/// The port block once no older block of this name exists (T1, I9: two blocks never share a node.name), and a
		/// configuration once no older node of this name is left (rule 1), so an old and a new configuration never share
		/// what the name owns (a listening port, a key file).
		void add(std::shared_ptr<const NodeConfig> config, bool olderBlock, bool olderNode) {
			ports_ = config->ports();
			queued_ = std::move(config);
			// A node added on a wedged loop fails before T1, with no block (4.1, D-L10).
			feed(aggregate_.add(lc::AddFacts{
				.olderBlock = olderBlock, .olderNode = olderNode, .wedged = host_.wedged, .queued = queued()}));
		}

		/// A new configuration from `config` behind the same port block. One that arrives while the configuration is
		/// still going replaces the config the next one takes (4.3, rule 2). A lost block is final: nothing is kept.
		void update(std::shared_ptr<const NodeConfig> config) {
			const bool lost = aggregate_.block().state() == lc::BlockState::lost;
			queued_ = lost ? nullptr : std::move(config);
			++version_;
			// Today's records: an update of a node whose block stands says starting at once, before its teardown, a
			// failed node's included (whose retry waits for its failed configuration to go, mapped() shows it).
			if (!lost) tell(NodeHealth{name_, NodeState::starting, std::nullopt});
			feed(aggregate_.change(queued(), {}));
		}

		void remove() {
			queued_.reset(); // never configured again
			stopping();      // today's records: stopping before anything the removal does
			feed(aggregate_.remove());
		}

		/// Every older node of this name is gone.
		void olderGone() { feed(aggregate_.olderNodeGone()); }
		/// The older block of this name was destroyed (T1): this node's block is made in the same flush.
		void olderDestroyed() { feed(aggregate_.olderDestroyed()); }

		/// Flush items (5.5). The block's making, unless its destruction overtook it in the flush.
		void makeBlock() {
			if (!makePending_) return;
			makePending_ = false;
			if (host_.wedged) { // making a block would call into the wedged loop (5.4, D-L10)
				lost(std::string(lc::wedgedDetail));
				return;
			}
			try {
				block_ = host_.blocks.make(name_, ports_, host_.format, *this);
			} catch (...) {
				// A factory that throws (a bug) leaves no block: the node is lost, never left made with none.
				lost("making the port block threw: " + whatOf(std::current_exception()));
				throw;
			}
		}
		/// The block's destruction (after the removal's silence completed, or with no code ever driven, D-L7); a block
		/// whose making is still queued is never made. Then, as an input of its own: the node is told gone if its
		/// configuration is, and a waiting block of the name is made.
		void destroyBlock() {
			if (!destroyPending_) return;
			destroyPending_ = false;
			if (makePending_) {
				makePending_ = false;
			} else if (host_.wedged && block_) {
				static_cast<void>(block_.release()); // leaked: destroying it waits on the wedged loop (5.4)
				blockLeaked_ = true;
				++host_.leaked;
			} else {
				block_.reset();
			}
			host_.inputFor(serial_, [](Node& node) {
				if (std::exchange(node.goneWaiting_, false)) node.host_.gone(node, node.dropped_);
				node.host_.blockDestroyed(node.name_);
			});
		}
		/// The outside node `index` of the configuration of `generation`, in request order, once its block is ready;
		/// nothing for a configuration that moved on (torn down, or another one).
		void makeOutside(std::uint64_t generation, std::size_t index) {
			if (host_.wedged || !current(generation) || config_->startFailed || config_->tearing || !block_ ||
			    aggregate_.configuration().state() != lc::ConfigState::readying || index != config_->outside.size() ||
			    index >= config_->outsideRequests.size())
				return;
			Configuration& c = *config_;
			auto& outside = *c.outside.emplace_back(std::make_unique<Outside>(host_, serial_, c.generation, index));
			auto& [request, target] = c.outsideRequests[index];
			target.blockId = block_->blockId(); // ready, so read (5.7)
			try {
				outside.node = std::visit(
					[&](const auto& kind) { return host_.resources.adapterFor(kind).make(target, kind, outside); },
					request);
			} catch (...) {
				// An adapter that throws (a bug) strands nothing: the half-made entry goes, and the node fails as for
				// an outside node refused.
				std::string detail = "making " + target.engineNode + " threw: " + whatOf(std::current_exception());
				c.outside.pop_back();
				host_.inputFor(serial_, [generation, detail = std::move(detail)](Node& node) mutable {
					if (node.current(generation)) node.feed(node.aggregate_.fault(lc::Fact::engine, std::move(detail)));
				});
				throw;
			}
		}

		// PortBlockEvents: inputs of their own (5.2), whatever calls them.
		void ready() override {
			host_.inputFor(serial_, [](Node& node) {
				node.feed(node.aggregate_.blockReady());
				node.askResources();
			});
		}
		void lost(std::string detail) override {
			host_.inputFor(serial_, [detail = std::move(detail)](Node& node) mutable {
				node.feed(node.aggregate_.blockLost(std::move(detail)));
				node.queued_.reset(); // a lost block is final (I1): nothing is configured again
			});
		}

		// StartContext and Ready
		AudioFormat format() const override { return host_.format; }
		const NodeName& node() const override { return name_; }
		InsidesEvents& events() override {
			if (!starting_ && !building_)
				throw std::logic_error("node " + name_ + ": events were asked for outside start() and its recipe");
			return *config_->channel;
		}
		void declareLatency(const std::string& bank, Latency internal) override {
			if (!starting_) throw std::logic_error("node " + name_ + ": latency was declared outside start()");
			static_cast<void>(ports_.bank(bank)); // throws for a bank the node lacks
			if (std::ranges::any_of(config_->latency, [&](const auto& declared) { return declared.first == bank; }))
				throw std::logic_error("node " + name_ + ": latency declared twice for bank \"" + bank + "\"");
			config_->latency.emplace_back(bank, internal);
		}

	protected:
		Acquired acquireResource(const ResourceRequest& request) override {
			if (!starting_) throw std::logic_error("node " + name_ + ": a resource was asked for outside start()");
			return std::visit(Overloaded{
								  [&](const SoundCardRequest& card) -> Acquired {
									  askOutside(card, *requestedBank(ports_, name_, request), request);
									  return std::monostate{};
								  },
								  [&](const RtpReceiverRequest& receiver) -> Acquired {
									  askOutside(receiver, *requestedBank(ports_, name_, request), request);
									  return std::monostate{};
								  },
								  [&](const RecordingRequest& recording) -> Acquired {
									  auto opened = host_.resources.adapterFor(recording).open(name_, recording.path);
									  if (!opened) return std::unexpected(std::move(opened.error()));
									  return (*config_->recordings.emplace_back(std::move(*opened))).descriptor();
								  },
								  [&](const HandoffRequest& handoff) -> Acquired {
									  const Ports::Bank& bank = *requestedBank(ports_, name_, request);
									  auto& made =
										  config_->handoffs.emplace_back(host_.resources.adapterFor(handoff).make(
											  name_, static_cast<std::uint32_t>(bank.ports.size()),
											  handoff.capacityFrames, *handoff.consumer));
									  return &made->writer();
								  },
							  },
			                  request);
		}

	private:
		/// One outside node the configuration asked for (a sound card or an RTP receiver), and its events, which land
		/// only on the configuration that made it.
		class Outside final : public OutsideNodeEvents {
		public:
			Outside(Impl& host, std::uint64_t serial, std::uint64_t generation, std::size_t index)
				: host_(host), serial_(serial), generation_(generation), index_(index) {}
			std::unique_ptr<OutsideNode> node;
			bool isReady = false;
			// Told inside its making (during the flush) or later: an input of its own either way (D-L39).
			void ready() override {
				host_.inputFor(serial_, [generation = generation_, index = index_](Node& node) {
					// Still current while it tears down, when its outside nodes are already gone: nothing to ready.
					if (!node.current(generation) || node.config_->tearing || index >= node.config_->outside.size())
						return;
					node.config_->outside[index]->isReady = true;
					node.checkReady(generation);
				});
			}
			void lost(std::string detail) override {
				host_.inputFor(serial_, [generation = generation_, detail = std::move(detail)](Node& node) mutable {
					if (node.current(generation)) node.feed(node.aggregate_.fault(lc::Fact::engine, std::move(detail)));
				});
			}

		private:
			Impl& host_; // never the node: a queued destruction may outlive it
			std::uint64_t serial_;
			std::uint64_t generation_;
			std::size_t index_;
		};

		/// One configuration as its own threads reach the node: everything they tell crosses to the main loop, in
		/// order, and lands only while the configuration is attached. Each configuration has its own Channel, detached
		/// on every path that ends it (it closed, it failed to start, or it was leaked), before the next one can start:
		/// that is I6's generation, so no write of an old configuration lands on a new one. Whatever is still queued
		/// holds this object, not the node, so it may outlive the node.
		class Channel final : public InsidesEvents, public std::enable_shared_from_this<Channel> {
		public:
			Channel(Node& node, Executor& mainLoop) : node_(&node), mainLoop_(mainLoop) {}
			void say(std::string text) override {
				mainLoop_.post([self = shared_from_this(), text = std::move(text)] {
					if (Node* node = self->node()) node->host_.noted(node->name_, text);
				});
			}
			void lost(std::string detail) override {
				mainLoop_.post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					if (Node* node = self->node(); node != nullptr && !self->closing_)
						node->insidesLost(std::move(detail));
				});
			}
			/// Main thread. The configuration is going (I5 began): a fact no longer reaches the node.
			void closing() noexcept { closing_ = true; }
			/// Main thread. Nothing more of this configuration reaches the node.
			void detach() noexcept { node_ = nullptr; }
			/// The `closed` a handoff of this configuration gets: on the main thread, later.
			std::move_only_function<void(std::uint64_t)> handoffClosedCall() {
				return [self = shared_from_this()](std::uint64_t droppedFrames) {
					if (Node* node = self->node()) node->handoffClosed(node->config_->generation, droppedFrames);
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
					self->mainLoop_.post([self] {
						if (Node* node = self->node()) node->runClosed();
					});
				};
			}
			/// Main thread: close() on `object`. True if it called `closed` before it returned.
			bool close(RunObject& object) {
				insideClose_ = true;
				object.close(closedCall());
				insideClose_ = false;
				return closedInside_;
			}

		private:
			/// Main thread: the node, while this configuration is attached.
			Node* node() const noexcept { return node_; }
			Node* node_; // main thread only
			Executor& mainLoop_;
			bool closing_ = false;      // main thread only
			bool insideClose_ = false;  // main thread only
			bool closedInside_ = false; // main thread only
		};

		/// One configuration (docs/design/node-lifecycle.md 5.1, today's run), from configure to the end of its close.
		/// Members in reverse teardown order.
		struct Configuration {
			std::uint64_t generation = 0;
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			// Outlives the handoffs that write it and the run object and recipe that may hold it open (file's WavWriter
			// closes its SNDFILE on the descriptor when freed).
			std::vector<std::unique_ptr<RecordingFile>> recordings;
			RunRecipe recipe;
			std::unique_ptr<RunObject> object;
			std::unique_ptr<AudioCode> code; // reads the run object, so goes first
			std::vector<std::unique_ptr<Handoff>> handoffs;
			std::vector<std::pair<OutsideRequest, OutsideTarget>> outsideRequests;
			std::vector<std::unique_ptr<Outside>> outside;
			std::unique_ptr<Timer> deadline;
			std::unique_ptr<Timer> bound; // the request in flight's bound: the fade's or the barrier's
			std::chrono::steady_clock::time_point postedAt; // when that request was posted
			// One flag per output in process()'s order (outputs, then inside outputs): every one the code writes is
			// faded (5.1; no sink yet). Read by the block until the drive's invoke ran.
			std::unique_ptr<bool[]> faded;
			std::size_t fadedCount = 0;
			std::vector<BankPair> pairs;                          // the pass-through's pairs, for the latency plan
			std::vector<std::pair<std::string, Latency>> latency; // what start() declared, by bank
			bool startFailed = false;  // start() failed or threw: nothing to make, prepare, drive or close
			bool outsideAsked = false; // its outside nodes' making is queued
			bool recipeRan = false;    // T3 ran (or there was no recipe)
			bool prepared = false;
			bool tearing = false; // I5 step 3 began
			std::size_t handoffsClosed = 0;
		};

		/// Everything of a configuration that did not close in time and that its threads can still reach: never freed
		/// (I5a).
		struct Leaked {
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			RunRecipe recipe;
			std::unique_ptr<AudioCode> code;
			std::unique_ptr<RunObject> object;
			std::vector<std::unique_ptr<RecordingFile>> recordings;
			std::vector<std::unique_ptr<Handoff>> handoffs;
			std::vector<std::unique_ptr<Outside>> outside; // a wedged loop's, never destroyed
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

		/// Today's health from the aggregate's (step L-3a keeps NodeHealth as it is until L-3d): stopping only while
		/// removed, failed with the failure's reason or the block's loss, running, and starting for everything else
		/// (a restart's stopping, configuring, configured and starting all were starting).
		NodeHealth mapped(const lc::Health& health) const {
			if (health.phase == lc::NodePhase::stopping && health.cause &&
			    health.cause->kind == lc::NodeCause::Kind::removing)
				return {name_, NodeState::stopping, std::nullopt};
			if (health.phase == lc::NodePhase::failed && health.reason && aggregate_.retryAsked())
				return {name_, NodeState::starting, std::nullopt}; // a retry asked, its configure still to come
			if (health.phase == lc::NodePhase::failed && health.reason) {
				const auto code = health.reason->code == lc::FailureCode::engineRefused
				                      ? FailureReason::Code::engineRefused
				                      : FailureReason::Code::startFailed; // lost is L-3d's: start-failed until then
				return {name_, NodeState::failed, FailureReason{code, health.reason->detail}};
			}
			if (health.blockLost) // a name added back whose block was lost while it waited (I1)
				return {name_, NodeState::failed,
				        FailureReason{FailureReason::Code::engineRefused, aggregate_.block().lossDetail()}};
			if (health.phase == lc::NodePhase::running) return {name_, NodeState::running, std::nullopt};
			return {name_, NodeState::starting, std::nullopt};
		}

		/// Carries out what the aggregate returned, in order (5.2's steps 2 and 4). An input an action causes (a
		/// resource ready at once, a run object closed inside close()) is queued behind them, never handled inside.
		void feed(lc::Actions actions) {
			// A removal that ends in its own input (nothing was configured, or its configuration was already gone) is
			// told gone without a stopping before it; today's records say stopping first, so it is told here.
			if (std::ranges::any_of(actions,
			                        [](const auto& action) { return std::holds_alternative<lc::Gone>(action); }))
				stopping();
			for (const lc::NodeAction& action : actions) {
				std::visit(Overloaded{
							   [&](lc::BlockAction block) { act(block); },
							   [&](const lc::ConfigAction& config) { act(config); },
							   [&](const lc::Tell& told) { tell(told.health); },
							   [&](lc::Gone) {
								   // Gone once its block's destruction ran in the flush (5.7), not when queued.
								   if (destroyPending_) goneWaiting_ = true;
								   else host_.gone(*this, dropped_);
							   },
						   },
				           action);
			}
			// A teardown asked while the drive is in flight (arming, stopAsked) has begun: from here on nothing the
			// configuration's threads tell is a fact (I6), as from its silence on.
			if (config_ && aggregate_.configuration().state() == lc::ConfigState::arming &&
			    aggregate_.configuration().stopAsked() && aggregate_.intent() != lc::Intent::stop &&
			    aggregate_.intent() != lc::Intent::none)
				config_->channel->closing();
		}

		void tell(const lc::Health& health) { tell(mapped(health)); }

		void tell(NodeHealth now) {
			if (told_ && sameHealth(*told_, now)) return;
			told_ = now;
			host_.changed(*told_);
		}

		void stopping() { tell(NodeHealth{name_, NodeState::stopping, std::nullopt}); }

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
			case Kind::prepare: {
				const MainLoopActivity::Scope activity("prepare", name_);
				config_->code->prepare(host_.format);
				config_->prepared = true;
				break;
			}
			case Kind::drive:
				drive(action.barrier);
				break;
			case Kind::start:
			case Kind::stop:
				break; // a legacy type has no hooks (5.10)
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

		/// Records an outside node to make once the block is ready, under its one name (I9).
		void askOutside(OutsideRequest request, const Ports::Bank& bank, const ResourceRequest& asked) {
			const NodeName engineNode = name_ + "." + std::string(*outsideSuffix(asked));
			if (std::ranges::any_of(config_->outsideRequests,
			                        [&](const auto& asked) { return asked.second.engineNode == engineNode; }))
				throw std::logic_error("node " + name_ + ": a second resource named " + engineNode + " in one run");
			config_->outsideRequests.emplace_back(std::move(request),
			                                      OutsideTarget{engineNode, name_, bank.ports, host_.format});
		}

		/// T2 through the legacy adapter (5.10): a new generation; the type's start() acquires only. A failure is a
		/// fault in readying; a recipe beside an outside node is a type bug (D-L40); else the code is kept, unprepared,
		/// and the resources are asked for.
		void configure() {
			if (config_) throw std::logic_error("node " + name_ + ": a configuration began while another had not gone");
			config_ = std::make_unique<Configuration>();
			Configuration& c = *config_;
			c.generation = ++host_.generations;
			c.config = queued_;
			c.channel = std::make_shared<Channel>(*this, host_.mainLoop);
			c.fadedCount = ports_.outputs().size() + ports_.insideOutputs().size();
			c.faded = std::make_unique<bool[]>(c.fadedCount);
			std::fill_n(c.faded.get(), c.fadedCount, true);
			// Until start() returned a code that holds, the configuration asks for nothing more: a type bug thrown out
			// of here leaves it readying, with no outside node made for it.
			c.startFailed = true;
			starting_ = true;
			StartResult started = [&] {
				const MainLoopActivity::Scope activity("configure", name_);
				try {
					return c.config->start(*this);
				} catch (...) {
					starting_ = false;
					throw;
				}
			}();
			starting_ = false;
			if (started.failed()) {
				// No write happened (the block was never driven by this code), so the handoffs and recordings may go
				// now; the consumers they were given went with the type's failed start. No outside node was made.
				c.handoffs.clear();
				c.recordings.clear();
				c.outsideRequests.clear();
				c.channel->detach();
				host_.inputFor(serial_,
				               [generation = c.generation, failure = std::move(started.failure())](Node& node) mutable {
								   if (!node.current(generation)) return;
								   node.feed(node.aggregate_.fault(factOf(failure, lc::Fact::configureFailed),
					                                               std::move(failure.detail)));
							   });
				return;
			}
			if (started.recipe() && !c.outsideRequests.empty())
				throw std::logic_error(
					"node " + name_ +
					": a legacy type with a recipe may ask for no outside node: node-lifecycle.md 5.10");
			c.recipe = std::move(started.recipe());
			c.pairs = started.code().pairs();
			c.code = started.code().own() ? std::move(started.code().own()) : makePassThrough(ports_, c.pairs);
			c.startFailed = false;
			askResources();
		}

		/// 5.8's readiness, as far as 3a has it: the outside nodes, made in the flush in request order once the block
		/// is ready (their ports exist; 5.5); with none pending, every resource is ready at once (in this turn, as its
		/// own input). An adapter that tells ready() inside make() is heard after that item, and every ready is judged
		/// against every outside node made (7.2).
		void askResources() {
			if (!config_ || config_->startFailed || config_->recipeRan ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			if (c.outsideRequests.empty()) {
				host_.inputFor(serial_, [generation = c.generation](Node& node) { node.checkReady(generation); });
				return;
			}
			if (aggregate_.block().state() != lc::BlockState::ready || c.outsideAsked) return;
			c.outsideAsked = true;
			c.outside.reserve(c.outsideRequests.size());
			for (std::size_t i = 0; i < c.outsideRequests.size(); ++i)
				host_.queue(FlushKind::makeOutside, name_,
				            [&host = host_, serial = serial_, generation = c.generation, i] {
								if (Node* node = host.find(serial)) node->makeOutside(generation, i);
							});
		}

		/// Every resource ready: T3. The recipe builds the run object first (5.10), so a failed recipe is a fault in
		/// readying, before anything is prepared or driven; then the machine hears it.
		void checkReady(std::uint64_t generation) {
			if (!current(generation) || config_->startFailed || config_->recipeRan ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			if (c.outside.size() != c.outsideRequests.size() ||
			    !std::ranges::all_of(c.outside, [](const auto& outside) { return outside->isReady; }))
				return;
			c.recipeRan = true;
			if (c.recipe) {
				building_ = true;
				auto made = [&] {
					const MainLoopActivity::Scope activity("recipe", name_);
					try {
						return c.recipe(*this);
					} catch (...) {
						building_ = false;
						throw;
					}
				}();
				building_ = false;
				if (!made) {
					feed(
						aggregate_.fault(factOf(made.error(), lc::Fact::recipeFailed), std::move(made.error().detail)));
					return;
				}
				c.object = std::move(*made);
			}
			feed(aggregate_.resourcesReady());
		}

		/// T5: the latency plan first, then the code, by an asynchronous drive (5.3) that fades in every output the code
		/// writes; the barrier's bound (5 s) armed.
		void drive(std::uint64_t barrier) {
			if (host_.wedged) return overdueAtOnce(barrier, Bound::barrier);
			Configuration& c = *config_;
			block_->carryLatency(LatencyPlan{c.pairs, c.latency});
			block_->drive(DriveRequest{c.code.get(), std::span<const bool>(c.faded.get(), c.fadedCount)},
			              completion(barrier, &lc::NodeAggregate::driven));
			arm(barrier, Bound::barrier);
		}

		/// Before a planned silence: every faded output ramps to zero over 5 ms (5.3); its acknowledgement's bound
		/// (100 ms) armed.
		void fade(std::uint64_t barrier) {
			if (host_.wedged) return overdueAtOnce(barrier, Bound::fade);
			block_->fadeOut([faded = completion(barrier, &lc::NodeAggregate::faded)]() mutable { faded(0); });
			arm(barrier, Bound::fade);
		}

		/// The barrier on the block's data loop (I4): from its completion no process() of the code runs. A teardown's
		/// silence ends what the configuration's threads can tell (I6). Its bound (5 s) armed.
		void silence(std::uint64_t barrier) {
			if (aggregate_.intent() != lc::Intent::stop) config_->channel->closing();
			if (host_.wedged) return overdueAtOnce(barrier, Bound::barrier);
			block_->drive(DriveRequest{nullptr, {}}, completion(barrier, &lc::NodeAggregate::silenced));
			arm(barrier, Bound::barrier);
		}

		/// A completion as the block runs it on the main loop: an input of its own, heard only by the configuration and
		/// the request it was made for (5.6); the aggregate drops an older barrier's. The request's bound goes with it.
		std::move_only_function<void(std::uint64_t)>
		completion(std::uint64_t barrier, lc::Actions (lc::NodeAggregate::*event)(std::uint64_t)) {
			return [&host = host_, serial = serial_, generation = config_->generation, barrier, event](std::uint64_t) {
				if (Node* node = host.find(serial)) ++node->completionsRun_;
				host.inputFor(serial, [generation, barrier, event](Node& node) {
					if (!node.current(generation)) return;
					if (node.aggregate_.configuration().barrier() == barrier) node.config_->bound.reset();
					node.feed((node.aggregate_.*event)(barrier));
				});
			};
		}

		/// Which bound a request has (5.4): the fade's acknowledgement, or the barrier's completion.
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
		/// the input behind whatever that settle ran (D-L30): a completion a stalled main loop had not dispatched yet is
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

		/// On a wedged loop a request is never posted (5.4): it is judged overdue at once, as an input of its own, saying
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
		/// Another node's barrier found the data loop wedged (5.4): this node is never configured again; a
		/// configuration not yet driven fails engine-refused and tears down with no barrier; one whose request is in
		/// flight (it can never complete) is leaked now. A driving one runs on until its stop or removal, whose fade and
		/// silence are then judged overdue at once. Nothing is settled: settling may post to the wedged loop.
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

		/// I5 steps 1's rest to 3: the outside nodes; the handoffs closed, each on its own thread. Steps 3 and 4 have
		/// the close deadline (I5a). The rest goes once every handoff has closed.
		void teardown() {
			Configuration& c = *config_;
			c.channel->closing();
			for (auto& outside : c.outside)
				host_.queue(FlushKind::destroyOutside, name_, [&host = host_, outside = std::move(outside)]() mutable {
					if (!host.wedged) {
						outside.reset();
						return;
					}
					static_cast<void>(outside.release()); // leaked: destroying it calls into the wedged loop (5.4)
					++host.leaked;
				});
			c.outside.clear();
			c.outsideRequests.clear();
			c.deadline =
				host_.timers.after(host_.closeDeadline, [&host = host_, serial = serial_, generation = c.generation] {
					host.inputFor(serial, [generation](Node& node) {
						if (node.current(generation) && node.config_->tearing) node.feed(node.aggregate_.closeBound());
					});
				});
			c.tearing = true;
			c.handoffsClosed = 0;
			closingHandoffs_ = true;
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

		/// I5 step 4, once every handoff reported closed, and never inside a close() call, so no handoff is destroyed
		/// while that loop is still calling it. The rest goes once the run object says it has closed.
		void closeRunObject() {
			if (!config_ || !config_->tearing || closingHandoffs_ || runClosing_ ||
			    config_->handoffsClosed != config_->handoffs.size())
				return;
			config_->handoffs.clear();
			runClosing_ = true;
			const bool closedInside = [&] {
				if (!config_->object) return true;
				const MainLoopActivity::Scope activity("close run", name_);
				return config_->channel->close(*config_->object);
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

		/// The run's own threads say it is dead: the node fails (start-failed; lost once rested is L-3d's code).
		void insidesLost(std::string detail) {
			host_.inputFor(serial_, [generation = config_->generation, detail = std::move(detail)](Node& node) mutable {
				if (node.current(generation)) node.feed(node.aggregate_.fault(lc::Fact::nodeLost, std::move(detail)));
			});
		}

		/// I5 step 5, on the main loop: the code (released and unreachable since the barrier; it may hold references
		/// into the run object, never the reverse), then the run object, its recipe, the recordings and last its config.
		void freeConfiguration() {
			const MainLoopActivity::Scope activity("free configuration", name_);
			Configuration& c = *config_;
			c.deadline.reset();
			c.channel->detach();
			c.code.reset();
			c.object.reset();
			c.recipe = nullptr;
			c.recordings.clear();
			config_.reset();
		}

		/// The close deadline passed with step 3 or 4 unfinished (I5a): said, then leaked.
		void closeOverdue() {
			// Once every handoff closed they are cleared and the run object is closing.
			const std::size_t handoffsOpen = runClosing_ ? 0 : config_->handoffs.size() - config_->handoffsClosed;
			host_.diagnosed(name_, "ear.close-overdue",
			                "the run did not close within " + std::to_string(host_.closeDeadline.count()) +
			                    " ms and is left running (" + std::to_string(handoffsOpen) + " handoffs open, run " +
			                    (runClosing_ ? "closing" : "not yet told to close") + ")",
			                {});
		}

		/// The configuration is detached, so nothing it tells lands; what its threads can reach is leaked, never freed;
		/// and the node moves on.
		void leak() {
			Configuration& c = *config_;
			c.channel->detach();
			// Deliberately never deleted: the configuration's threads may still touch any of it.
			static_cast<void>(new Leaked{std::move(c.config), std::move(c.channel), std::move(c.recipe),
			                             std::move(c.code), std::move(c.object), std::move(c.recordings),
			                             std::move(c.handoffs), std::move(c.outside), std::move(c.faded)});
			++host_.leaked;
			config_.reset();
			runClosing_ = false;
		}

		Impl& host_;
		NodeName name_;
		std::uint64_t serial_;
		lc::NodeAggregate aggregate_;
		Ports ports_;
		std::shared_ptr<const NodeConfig> queued_; // the newest committed config, the next configure's
		std::uint64_t version_ = 0;                // queued_'s, as the aggregate compares it
		std::optional<NodeHealth> told_;           // the health last told
		std::unique_ptr<Configuration> config_;    // the current configuration (I7: at most one)
		std::unique_ptr<PortBlock> block_;
		bool starting_ = false;
		bool building_ = false;
		bool closingHandoffs_ = false;
		bool runClosing_ = false;
		bool makePending_ = false;         // the block's making is queued in the flush
		bool destroyPending_ = false;      // the block's destruction is queued in the flush
		bool goneWaiting_ = false;         // removed, told gone once its block's destruction ran
		bool gone_ = false;                // told gone
		bool blockLeaked_ = false;         // its block was leaked on a wedged loop: never freed, nor is this node
		bool synthetic_ = false;           // a bound judged at once on a wedged loop: its diagnostics are not said
		std::uint64_t completionsRun_ = 0; // `done`s its block ran, by event or settle(): a bound sees one it settled
		std::int64_t judgedMs_ = 0;        // how long the judged request waited, for its diagnostics
		std::uint64_t dropped_ = 0;
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
	bool wedged = false;                      // the data loop was judged wedged (5.4): no PipeWire call is made again
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; removed ones are pruned on add
	std::uint64_t generations = 0;            // every configuration's generation, across nodes, so none is ever reused
	std::uint64_t serials = 0;                // every node's serial, so a completion never finds a newer node (5.6)
	std::size_t leaked = 0;                   // configurations, blocks and outside nodes never freed (I5a, 5.4)
	bool stopRequested = false;
	std::move_only_function<void()> allGone;
	std::deque<std::move_only_function<void()>> inputs; // what waits for the input being handled (5.2)
	bool handling = false;
	bool inItem = false;          // a flush item runs: an input it causes waits for it to return (D-L39)
	std::vector<FlushItem> flush; // this turn's queued object changes (5.5)
	bool flushPosted = false;     // the flush task is posted and has not begun
	bool flushing = false;        // the flush runs: what is queued now runs behind its snapshot
	std::shared_ptr<char> alive = std::make_shared<char>(); // a posted flush finds the host gone by it

	/// One input at a time (5.2): one that arrives while another is handled, from an action or a callback inside it,
	/// or while a flush item runs, is handled after it, in arrival order (by drain()). An input that throws (a type
	/// bug: a std::logic_error from a type's start or recipe) does not strand the inputs queued behind it, which may be
	/// other nodes' (a stop whose allGone would then never run): they are handled first, and the first exception is
	/// rethrown once the queue is empty. The node it was thrown out of is left as the throw left it.
	void input(std::move_only_function<void()> work) {
		inputs.push_back(std::move(work));
		if (handling || inItem) return;
		if (std::exception_ptr thrown = drain()) std::rethrow_exception(thrown);
	}

	/// Handles every queued input, and what they queue; the first exception, if any.
	std::exception_ptr drain() {
		handling = true;
		std::exception_ptr thrown;
		while (!inputs.empty()) {
			auto next = std::move(inputs.front());
			inputs.pop_front();
			try {
				next();
			} catch (...) {
				if (!thrown) thrown = std::current_exception();
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

	/// Queues one PipeWire object change into this turn's flush, posting the flush on the turn's first (5.5, D-L8).
	/// During the flush it runs behind the flush's snapshot, in the same flush.
	void queue(FlushKind kind, const NodeName& node, std::move_only_function<void()> run) {
		flush.push_back(FlushItem{kind, node, std::move(run)});
		if (flushing || flushPosted) return;
		flushPosted = true;
		mainLoop.post([this, alive = std::weak_ptr<char>(this->alive)] {
			if (alive.lock()) runFlush();
		});
	}

	/// The turn's flush (5.5, D-L39): the queued changes as a snapshot, destroys first; each item names itself in
	/// MainLoopActivity while it runs, and the inputs it caused (an adapter's event told inside it) are handled once it
	/// returned; what they queue runs behind the snapshot, in this flush. An item or input that throws (a type bug)
	/// strands nothing: the flush ends, then the first exception is rethrown.
	void runFlush() {
		flushPosted = false; // a later change posts a flush of its own, even past the throw below
		if (handling || inItem || flushing)
			throw std::logic_error("the flush ran inside an input or another flush: the executor ran a post inline");
		flushing = true;
		std::exception_ptr thrown;
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
					if (!thrown) thrown = std::current_exception();
				}
				item.run = nullptr; // a destroy's object goes inside its activity, whether or not the item threw
			}
			inItem = false;
			if (std::exception_ptr caused = drain(); caused && !thrown) thrown = caused;
			take();
		}
		flushing = false;
		if (thrown) std::rethrow_exception(thrown);
	}

	static constexpr std::string_view callName(FlushKind kind) noexcept {
		switch (kind) {
		case FlushKind::destroyOutside:
			return "destroy outside node";
		case FlushKind::destroyBlock:
			return "destroy block";
		case FlushKind::makeBlock:
			return "make block";
		case FlushKind::makeOutside:
			return "make outside node";
		}
		return "flush";
	}

	void requireOpen(const char* event, const NodeName& node) const {
		if (stopRequested)
			throw std::logic_error(std::string(event) + " for node " + node + " after stopAll(): a wiring bug");
	}

	/// The wanted node of this name: the newest one not removing.
	Node& wanted(const NodeName& name) {
		for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
			if ((*it)->name() == name && !(*it)->removing()) return **it;
		throw std::logic_error("node " + name + " is not a wanted node: a wiring bug");
	}

	void changed(const NodeHealth& health) {
		for (auto* listener : listeners) listener->nodeChanged(health);
	}

	void noted(const NodeName& node, const std::string& text) {
		for (auto* listener : noteListeners) listener->nodeSays(node, text);
	}

	void diagnosed(const NodeName& node, std::string_view code, const std::string& detail,
	               std::span<const DiagnosticField> fields) {
		for (auto* listener : diagnosticListeners) listener->nodeDiagnostic(node, code, detail, fields);
	}

	/// A barrier of `node` did not complete within `ms` (5.4): the data loop runs no invoke. Said once; then every node
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
		for (auto* listener : listeners) listener->nodeGone(node.name(), droppedFrames);
		const auto next = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->name() == node.name() && !known->removed(); });
		if (next != nodes.end()) inputFor((*next)->serial(), [](Node& waiting) { waiting.olderGone(); });
		input([this] { allGoneIfDone(); });
	}

	/// A block of this name was destroyed: the block waiting for it (at most one: only the wanted node of a name
	/// waits) is made, in this flush, once no block of the name is left (T1, I9).
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
	// On a wedged loop, leaked blocks, outside nodes and configurations still point into the host: it is never freed
	// (main.cpp ends through std::_Exit before this runs; a test's host is kept for good).
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

void NodeHost::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeAdded", node.name);
	std::erase_if(impl_->nodes, [](auto& known) {
		if (!known->removed()) return false;
		if (known->blockLeaked()) static_cast<void>(known.release()); // its leaked block may still tell it (5.4)
		return true;
	});
	const std::uint64_t serial = ++impl_->serials;
	impl_->nodes.emplace_back(std::make_unique<Impl::Node>(*impl_, node.name, serial));
	impl_->inputFor(serial, [impl = impl_.get(), config = std::move(config)](Impl::Node& added) mutable {
		// Older nodes of the name, as they stand when this input is handled (T1, rule 1).
		bool olderBlock = false;
		bool olderNode = false;
		for (const auto& known : impl->nodes) {
			if (known.get() == &added) break;
			if (known->name() != added.name() || known->removed()) continue;
			olderNode = true;
			olderBlock = olderBlock || known->holdsBlock();
		}
		added.add(std::move(config), olderBlock, olderNode);
	});
}

void NodeHost::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeUpdated", node.name);
	impl_->inputFor(impl_->wanted(node.name).serial(),
	                [config = std::move(config)](Impl::Node& wanted) mutable { wanted.update(std::move(config)); });
}

void NodeHost::nodeRemoved(const NodeName& node) {
	impl_->requireOpen("nodeRemoved", node);
	impl_->inputFor(impl_->wanted(node).serial(), [](Impl::Node& wanted) { wanted.remove(); });
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

std::vector<NodeHealth> NodeHost::health() const {
	std::vector<NodeHealth> result;
	for (const auto& node : impl_->nodes)
		if (!node->removed()) result.push_back(node->health());
	return result;
}

} // namespace ear
