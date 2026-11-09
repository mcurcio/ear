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
// Step L-3a builds no fade (no output is faded, so a drive or silence is never faded), no per-turn flush (a block is
// made and destroyed, and an outside node made, inside the action that asks for it), no wait for an older block (T1: a
// name added back gets its block at once, and its configuration waits for the older node to be gone, as the
// aggregate's predecessor), no bound but the close deadline, and the old NodeHealth: the aggregate's health is told
// mapped onto it, only when that changed, so standard output and the wire see what they saw before.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

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

bool sameHealth(const NodeHealth& a, const NodeHealth& b) {
	if (a.state != b.state || a.reason.has_value() != b.reason.has_value()) return false;
	return !a.reason || (a.reason->code == b.reason->code && a.reason->detail == b.reason->detail);
}

} // namespace

ResourceRegistry::ResourceRegistry(SoundCardAdapter& cards, RtpReceiverAdapter& receivers,
                                   HandoffAdapter& handoffs) noexcept
	: ResourceRegistry(cards, receivers, handoffs, noRecordings) {}

struct NodeHost::Impl {
	/// One wanted node: its aggregate, its port block and its configuration. Members are declared in reverse teardown
	/// order, so destroying a node that never stopped still removes its port block first.
	class Node final : public PortBlockEvents, public StartContext, public Ready {
	public:
		Node(Impl& host, NodeName name, std::uint64_t serial) : host_(host), name_(std::move(name)), serial_(serial) {}

		const NodeName& name() const noexcept { return name_; }
		std::uint64_t serial() const noexcept { return serial_; }
		/// Gone: its block destroyed and its configuration gone (4.3's row 1).
		bool removed() const noexcept { return aggregate_.removed(); }
		/// No longer wanted: removed by an edit or by stopAll, still stopping or gone.
		bool removing() const noexcept { return aggregate_.intent() == lc::Intent::remove; }
		NodeHealth health() const { return mapped(aggregate_.health()); }

		/// The port block at once; a configuration once no older node of this name is left (the aggregate's
		/// predecessor), so an old and a new configuration never share what the name owns (a listening port, a key
		/// file).
		void add(std::shared_ptr<const NodeConfig> config, bool olderGoing) {
			ports_ = config->ports();
			queued_ = std::move(config);
			feed(aggregate_.add(lc::AddFacts{.predecessor = olderGoing, .queued = queued()}));
		}

		/// A new configuration from `config` behind the same port block. One that arrives while the configuration is
		/// still going replaces the config the next one takes (4.3, rule 2). A lost block is final: nothing is kept.
		void update(std::shared_ptr<const NodeConfig> config) {
			queued_ = aggregate_.block().state() == lc::BlockState::lost ? nullptr : std::move(config);
			++version_;
			feed(aggregate_.change(queued(), {}));
		}

		void remove() {
			queued_.reset(); // never configured again
			feed(aggregate_.remove());
		}

		/// The older node of this name is gone.
		void olderGone() { feed(aggregate_.predecessorGone()); }

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
			Outside(Node& node, std::uint64_t generation, std::size_t index)
				: node_(node), generation_(generation), index_(index) {}
			std::unique_ptr<OutsideNode> node;
			bool isReady = false;
			void ready() override {
				node_.host_.inputFor(node_.serial_, [generation = generation_, index = index_](Node& node) {
					if (!node.current(generation)) return;
					node.config_->outside.at(index)->isReady = true;
					node.checkReady(generation);
				});
			}
			void lost(std::string detail) override {
				node_.host_.inputFor(node_.serial_,
				                     [generation = generation_, detail = std::move(detail)](Node& node) mutable {
										 if (node.current(generation))
											 node.feed(node.aggregate_.fault(lc::Fact::engine, std::move(detail)));
									 });
			}

		private:
			Node& node_;
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
					if (Node* node = self->node()) node->handoffClosed(droppedFrames);
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
			std::vector<BankPair> pairs;                          // the pass-through's pairs, for the latency plan
			std::vector<std::pair<std::string, Latency>> latency; // what start() declared, by bank
			bool startFailed = false; // start() failed or threw: nothing to make, prepare, drive or close
			bool recipeRan = false;   // T3 ran (or there was no recipe)
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
		};

		lc::Queued queued() const noexcept { return {version_, false}; } // L-3a fades nothing

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
							   [&](lc::Gone) { host_.gone(*this, dropped_); },
						   },
				           action);
			}
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
				block_ = host_.blocks.make(name_, ports_, host_.format, *this);
				break;
			case lc::BlockAction::destroy:
				block_.reset(); // after the removal's silence completed, or with no code ever driven (D-L7)
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
				config_->code->prepare(host_.format);
				config_->prepared = true;
				break;
			case Kind::drive:
				drive(action.barrier);
				break;
			case Kind::start:
			case Kind::stop:
				break; // a legacy type has no hooks (5.10)
			case Kind::fade:
				// Never posted in L-3a (nothing is faded, so the machine silences at once); its bound is L-3c's.
				block_->fadeOut(
					[faded = completion(action.barrier, &lc::NodeAggregate::faded)]() mutable { faded(0); });
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
			case Kind::barrierOverdue:
			case Kind::markWedged:
				break; // the fade's and the barrier's bounds are step L-3c's: no timer arms them yet
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
			// Until start() returned a code that holds, the configuration asks for nothing more: a type bug thrown out
			// of here leaves it readying, with no outside node made for it.
			c.startFailed = true;
			starting_ = true;
			StartResult started = [&] {
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

		/// 5.8's readiness, as far as L-3a has it: the outside nodes, made in request order once the block is ready
		/// (their ports exist); with none pending, every resource is ready at once (in this turn, as its own input).
		void askResources() {
			if (!config_ || config_->startFailed || config_->recipeRan ||
			    aggregate_.configuration().state() != lc::ConfigState::readying)
				return;
			Configuration& c = *config_;
			if (!c.outsideRequests.empty()) {
				if (aggregate_.block().state() != lc::BlockState::ready || !c.outside.empty()) return;
				c.outside.reserve(c.outsideRequests.size());
				for (std::size_t i = 0; i < c.outsideRequests.size(); ++i) {
					auto& outside = *c.outside.emplace_back(std::make_unique<Outside>(*this, c.generation, i));
					const auto& [request, target] = c.outsideRequests[i];
					outside.node = std::visit(
						[&](const auto& kind) { return host_.resources.adapterFor(kind).make(target, kind, outside); },
						request);
				}
			}
			// An adapter may have told ready() inside make(): that is queued behind this, so it is judged once every
			// outside node exists (7.2).
			host_.inputFor(serial_, [generation = c.generation](Node& node) { node.checkReady(generation); });
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

		/// T5: the latency plan first, then the code, by an asynchronous drive (5.3).
		void drive(std::uint64_t barrier) {
			block_->carryLatency(LatencyPlan{config_->pairs, config_->latency});
			block_->drive(DriveRequest{config_->code.get(), {}}, completion(barrier, &lc::NodeAggregate::driven));
		}

		/// The barrier on the block's data loop (I4): from its completion no process() of the code runs. A teardown's
		/// silence ends what the configuration's threads can tell (I6).
		void silence(std::uint64_t barrier) {
			if (aggregate_.intent() != lc::Intent::stop) config_->channel->closing();
			block_->drive(DriveRequest{nullptr, {}}, completion(barrier, &lc::NodeAggregate::silenced));
		}

		/// A completion as the block runs it on the main loop: an input of its own, heard only by the configuration and
		/// the request it was made for (5.6); the aggregate drops an older barrier's.
		std::move_only_function<void(std::uint64_t)>
		completion(std::uint64_t barrier, lc::Actions (lc::NodeAggregate::*event)(std::uint64_t)) {
			return [&host = host_, serial = serial_, generation = config_->generation, barrier, event](std::uint64_t) {
				host.inputFor(serial, [generation, barrier, event](Node& node) {
					if (node.current(generation)) node.feed((node.aggregate_.*event)(barrier));
				});
			};
		}

		void release() {
			if (block_) block_->carryLatency({}); // a silent block carries nothing through and adds nothing
			if (config_->prepared) config_->code->release();
			config_->prepared = false;
		}

		/// I5 steps 1's rest to 3: the outside nodes; the handoffs closed, each on its own thread. Steps 3 and 4 have
		/// the close deadline (I5a). The rest goes once every handoff has closed.
		void teardown() {
			Configuration& c = *config_;
			c.channel->closing();
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

		void handoffClosed(std::uint64_t droppedFrames) {
			host_.inputFor(serial_, [droppedFrames](Node& node) {
				if (!node.config_) return;
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
			if (!config_->object || config_->channel->close(*config_->object)) runClosed();
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
			                    (runClosing_ ? "closing" : "not yet told to close") + ")");
		}

		/// The configuration is detached, so nothing it tells lands; what its threads can reach is leaked, never freed;
		/// and the node moves on.
		void leak() {
			Configuration& c = *config_;
			c.channel->detach();
			// Deliberately never deleted: the configuration's threads may still touch any of it.
			static_cast<void>(new Leaked{std::move(c.config), std::move(c.channel), std::move(c.recipe),
			                             std::move(c.code), std::move(c.object), std::move(c.recordings),
			                             std::move(c.handoffs)});
			++host_.leakedRuns;
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
		std::uint64_t dropped_ = 0;
	};

	AudioFormat format;
	PortBlockFactory& blocks;
	ResourceRegistry& resources;
	Executor& mainLoop;
	Timers& timers;
	std::chrono::milliseconds closeDeadline;
	std::vector<NodeHealthListener*> listeners;
	std::vector<NodeNoteListener*> noteListeners;
	std::vector<NodeDiagnosticListener*> diagnosticListeners;
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; removed ones are pruned on add
	std::uint64_t generations = 0;            // every configuration's generation, across nodes, so none is ever reused
	std::uint64_t serials = 0;                // every node's serial, so a completion never finds a newer node (5.6)
	std::size_t leakedRuns = 0;               // configurations left running past the close deadline (I5a)
	bool stopRequested = false;
	std::move_only_function<void()> allGone;
	std::deque<std::move_only_function<void()>> inputs; // what waits for the input being handled (5.2)
	bool handling = false;

	/// One input at a time (5.2): one that arrives while another is handled, from an action or a callback inside it,
	/// is handled after it, in arrival order, before this returns.
	void input(std::move_only_function<void()> work) {
		inputs.push_back(std::move(work));
		if (handling) return;
		handling = true;
		try {
			while (!inputs.empty()) {
				auto next = std::move(inputs.front());
				inputs.pop_front();
				next();
			}
		} catch (...) {
			inputs.clear();
			handling = false;
			throw;
		}
		handling = false;
	}

	/// An input for the node of this serial, dropped if it is gone (nodes are looked up by serial, never by pointer).
	void inputFor(std::uint64_t serial, std::move_only_function<void(Node&)> work) {
		input([this, serial, work = std::move(work)]() mutable {
			for (const auto& node : nodes)
				if (node->serial() == serial) {
					work(*node);
					return;
				}
		});
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

	void diagnosed(const NodeName& node, std::string_view code, const std::string& detail) {
		for (auto* listener : diagnosticListeners) listener->nodeDiagnostic(node, code, detail);
	}

	/// A node is gone; the oldest node left of its name, if one waits for it, may configure now.
	void gone(const Node& node, std::uint64_t droppedFrames) {
		for (auto* listener : listeners) listener->nodeGone(node.name(), droppedFrames);
		const auto next = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->name() == node.name() && !known->removed(); });
		if (next != nodes.end()) inputFor((*next)->serial(), [](Node& waiting) { waiting.olderGone(); });
		input([this] { allGoneIfDone(); });
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
	: impl_(new Impl{format,
                     blocks,
                     resources,
                     mainLoop,
                     timers,
                     closeDeadline,
                     {},
                     {},
                     {},
                     {},
                     0,
                     0,
                     0,
                     false,
                     nullptr,
                     {},
                     false}) {}

NodeHost::~NodeHost() = default;

void NodeHost::subscribe(NodeHealthListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::subscribeNotes(NodeNoteListener& listener) { impl_->noteListeners.push_back(&listener); }

void NodeHost::subscribeDiagnostics(NodeDiagnosticListener& listener) {
	impl_->diagnosticListeners.push_back(&listener);
}

void NodeHost::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeAdded", node.name);
	std::erase_if(impl_->nodes, [](const auto& known) { return known->removed(); });
	const bool olderGoing =
		std::ranges::any_of(impl_->nodes, [&](const auto& known) { return known->name() == node.name; });
	const std::uint64_t serial = ++impl_->serials;
	impl_->nodes.emplace_back(std::make_unique<Impl::Node>(*impl_, node.name, serial));
	impl_->inputFor(serial, [config = std::move(config), olderGoing](Impl::Node& added) mutable {
		added.add(std::move(config), olderGoing);
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

std::size_t NodeHost::leakedRuns() const noexcept { return impl_->leakedRuns; }

std::vector<NodeHealth> NodeHost::health() const {
	std::vector<NodeHealth> result;
	for (const auto& node : impl_->nodes)
		if (!node->removed()) result.push_back(node->health());
	return result;
}

} // namespace ear
