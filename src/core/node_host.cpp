// The node framework (docs/design/node-framework.md sections 6, 7.2 and 7.3; node_host.hpp): runs each wanted node's
// state machine and owns everything the node made. The port block is made when the node is added and goes only when it
// is removed (I1); runs start, restart and fail behind it, one at a time (I7). A run's start only acquires; the
// framework drives the block with the run's code at once, readies every resource the run acquired and only then has the
// recipe build the run object. A run is torn down by dependency, never waiting on another thread on the main loop
// (I5): the barrier on the block's data loop (I4) and release(); outside nodes; handoffs closed, each on its own
// thread; the run object closed, on its own threads; then, on the main loop, the run object, the code, the recipe and
// the config go. Steps 3 and 4 have a deadline (I5a): past it the run is leaked, never freed. Everything a run's threads
// tell crosses to the main loop and lands only on that run (I2, I6). It reaches the engine only through the port block
// factory and the resource registry's adapters.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "ear/core/pass_through.hpp"

namespace ear {
namespace {

/// One outside node a run asked for, of any kind.
using OutsideRequest = std::variant<SoundCardRequest, RtpReceiverRequest>;

template <typename... Kinds> struct Overloaded : Kinds... {
	using Kinds::operator()...;
};

/// The suffix of a run's outside node: the one home of "playback", "capture" (sound cards) and "rtp" (the RTP
/// receiver), so its engine node is "<node>.<suffix>" (I9). A node name never holds '.', so they cannot collide with a
/// node.
std::string_view outsideSuffix(const OutsideRequest& request) {
	return std::visit(Overloaded{
						  [](const SoundCardRequest& card) {
							  return card.direction == CardDirection::playback ? "playback" : "capture";
						  },
						  [](const RtpReceiverRequest&) { return "rtp"; },
					  },
	                  request);
}

} // namespace

struct NodeHost::Impl {
	/// One wanted node and everything it made. Members are declared in reverse teardown order, so destroying a node
	/// that never stopped still removes its port block first.
	class Node final : public PortBlockEvents, public StartContext, public Ready {
	public:
		Node(Impl& host, NodeName name) : host_(host), name_(std::move(name)) {}

		const NodeName& name() const noexcept { return name_; }
		const NodeLifecycle& lifecycle() const noexcept { return lifecycle_; }
		bool stopping() const noexcept { return stopping_; }
		NodeHealth health() const { return {name_, lifecycle_.state(), lifecycle_.reason()}; }

		/// The port block first, once for the node's life; then a run, which drives it: now, or once an older node of
		/// this name has gone, as a restart waits, so old and new runs never share what the name owns (a listening
		/// port, a key file).
		void add(std::shared_ptr<const NodeConfig> config, bool olderGoing) {
			ports_ = config->ports();
			block_ = host_.blocks.make(name_, ports_, host_.format, *this);
			if (olderGoing) waiting_ = std::move(config);
			else startRun(std::move(config));
		}

		/// The older node of this name is gone.
		void olderGone() {
			if (waiting_) startRun(std::exchange(waiting_, nullptr));
		}

		/// A new run from `config` behind the same port block, once the old one is gone. An update arriving while the
		/// old run is still going replaces the config to start.
		void update(std::shared_ptr<const NodeConfig> config) {
			if (waiting_) {
				waiting_ = lifecycle_.blockLost() ? nullptr : std::move(config);
				return;
			}
			if (lifecycle_.restartRequested()) host_.changed(*this);
			next_ = lifecycle_.blockLost() ? nullptr : std::move(config); // a lost block is final
			if (!tearingDown_) tearDown();
		}

		/// Stopping: port block (its destruction is the barrier), then the run's teardown; then gone.
		void stop() {
			stopping_ = true;
			next_.reset();
			waiting_.reset();
			if (lifecycle_.stopRequested()) host_.changed(*this);
			block_.reset();
			if (!tearingDown_) tearDown();
		}

		// PortBlockEvents
		void ready() override {
			blockReady_ = true;
			if (lifecycle_.portBlockReady()) host_.changed(*this);
			makeOutsideNodes();
		}
		void lost(std::string detail) override {
			if (lifecycle_.portBlockLost({FailureReason::Code::engineRefused, std::move(detail)})) host_.changed(*this);
		}

		// StartContext and Ready
		AudioFormat format() const override { return host_.format; }
		const NodeName& node() const override { return name_; }
		InsidesEvents& events() override {
			if (!starting_ && !building_)
				throw std::logic_error("node " + name_ + ": events were asked for outside start() and its recipe");
			return *run_->channel;
		}

	protected:
		Acquired acquireResource(const ResourceRequest& request) override {
			if (!starting_) throw std::logic_error("node " + name_ + ": a resource was asked for outside start()");
			return std::visit(Overloaded{
								  [&](const SoundCardRequest& card) -> Acquired {
									  const bool playback = card.direction == CardDirection::playback;
									  askOutside(
										  card, insideBank(card.bank, playback ? Direction::output : Direction::input));
									  return std::monostate{};
								  },
								  [&](const RtpReceiverRequest& receiver) -> Acquired {
									  askOutside(receiver, insideBank(receiver.bank, Direction::input));
									  return std::monostate{};
								  },
								  [&](const HandoffRequest& handoff) -> Acquired {
									  const Ports::Bank& bank = ports_.bank(handoff.bank);
									  if (bank.direction != Direction::input || handoff.consumer == nullptr)
										  throw std::logic_error("node " + name_ +
					                                             ": a handoff takes an input bank "
					                                             "and a consumer, not \"" +
					                                             handoff.bank + "\"");
									  auto& made = run_->handoffs.emplace_back(host_.resources.adapterFor(handoff).make(
										  name_, static_cast<std::uint32_t>(bank.ports.size()), handoff.capacityFrames,
										  *handoff.consumer));
									  return &made->writer();
								  },
							  },
			                  request);
		}

	private:
		/// One outside node the run asked for (a sound card or an RTP receiver), and its events.
		class Outside final : public OutsideNodeEvents {
		public:
			Outside(Node& node, std::size_t index) : node_(node), index_(index) {}
			std::unique_ptr<OutsideNode> node;
			bool isReady = false;
			void ready() override { node_.outsideReady(index_); }
			void lost(std::string detail) override { node_.outsideLost(std::move(detail)); }

		private:
			Node& node_;
			std::size_t index_;
		};

		/// One run as its own threads reach the node: everything they tell crosses to the main loop, in order, and
		/// lands only while the node runs this run's generation; nothing lands once the run is detached (it closed, or
		/// it was leaked). Whatever is still queued holds this object, not the node, so it may outlive the node (I6).
		class Channel final : public InsidesEvents, public std::enable_shared_from_this<Channel> {
		public:
			Channel(Node& node, std::uint64_t generation, Executor& mainLoop)
				: node_(&node), generation_(generation), mainLoop_(mainLoop) {}
			void say(std::string text) override {
				mainLoop_.post([self = shared_from_this(), text = std::move(text)] {
					if (Node* node = self->node()) node->host_.noted(node->name_, text);
				});
			}
			void lost(std::string detail) override {
				mainLoop_.post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					if (Node* node = self->node(); node != nullptr && !self->closing_) node->runLost(std::move(detail));
				});
			}
			/// Main thread. The run is going (I5 began): a fact no longer reaches the node.
			void closing() noexcept { closing_ = true; }
			/// Main thread. Nothing more of this run reaches the node.
			void detach() noexcept { node_ = nullptr; }
			/// The `closed` a handoff of this run gets: on the main thread, later.
			std::move_only_function<void(std::uint64_t)> handoffClosedCall() {
				return [self = shared_from_this()](std::uint64_t droppedFrames) {
					if (Node* node = self->node()) node->handoffClosed(droppedFrames);
				};
			}
			/// The `closed` the run object gets: from any thread, later or inside close().
			std::move_only_function<void()> closedCall() {
				return [self = shared_from_this(), main = std::this_thread::get_id()] {
					// Only the main thread reads insideClose_. A run object that holds no threads finishes inside
					// close(), so a stop or restart of a plain node takes no loop turn.
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
			/// Main thread: the node, while it runs this run.
			Node* node() const noexcept {
				return node_ != nullptr && node_->run_ != nullptr && node_->run_->generation == generation_ ? node_
				                                                                                            : nullptr;
			}
			Node* node_; // main thread only
			std::uint64_t generation_;
			Executor& mainLoop_;
			bool closing_ = false;      // main thread only
			bool insideClose_ = false;  // main thread only
			bool closedInside_ = false; // main thread only
		};

		/// One start of the node, from start() to the end of its close. Members in reverse teardown order.
		struct Run {
			std::uint64_t generation = 0;
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			RunRecipe recipe;
			std::unique_ptr<AudioCode> code;
			std::unique_ptr<RunObject> object;
			std::vector<std::unique_ptr<Handoff>> handoffs;
			std::vector<std::pair<OutsideRequest, OutsideTarget>> outsideRequests;
			std::vector<std::unique_ptr<Outside>> outside;
			std::unique_ptr<Timer> deadline;
			bool prepared = false;
			bool built = false; // the recipe ran (or there was none)
			std::size_t handoffsClosed = 0;
		};

		/// Everything of a run that did not close in time and that its threads can still reach: never freed (I5a).
		struct Leaked {
			std::shared_ptr<const NodeConfig> config;
			std::shared_ptr<Channel> channel;
			RunRecipe recipe;
			std::unique_ptr<AudioCode> code;
			std::unique_ptr<RunObject> object;
			std::vector<std::unique_ptr<Handoff>> handoffs;
		};

		const Ports::Bank& insideBank(const std::string& name, Direction direction) const {
			const Ports::Bank& bank = ports_.bank(name);
			if (bank.reach != PortBank::Reach::inside || bank.direction != direction)
				throw std::logic_error("node " + name_ + ": an outside node joins an inside " +
				                       (direction == Direction::input ? "input" : "output") + " bank, not \"" + name +
				                       "\"");
			return bank;
		}

		/// Records an outside node to make once the block is ready, under its one name (I9).
		void askOutside(OutsideRequest request, const Ports::Bank& bank) {
			const NodeName engineNode = name_ + "." + std::string(outsideSuffix(request));
			if (std::ranges::any_of(run_->outsideRequests,
			                        [&](const auto& asked) { return asked.second.engineNode == engineNode; }))
				throw std::logic_error("node " + name_ + ": a second resource named " + engineNode + " in one run");
			run_->outsideRequests.emplace_back(std::move(request),
			                                   OutsideTarget{engineNode, name_, bank.ports, host_.format});
		}

		/// A new run (I7: only when none is left). start() acquires; the code drives the block at once; the resources
		/// are readied, and once all are, the recipe builds the run object.
		void startRun(std::shared_ptr<const NodeConfig> config) {
			if (run_) throw std::logic_error("node " + name_ + ": a run started while another had not gone");
			run_ = std::make_unique<Run>();
			run_->generation = ++host_.generations;
			run_->config = std::move(config);
			run_->channel = std::make_shared<Channel>(*this, run_->generation, host_.mainLoop);
			starting_ = true;
			StartResult started = [&] {
				try {
					return run_->config->start(*this);
				} catch (...) {
					starting_ = false;
					throw;
				}
			}();
			starting_ = false;
			if (!started) {
				// No write happened (the block was never driven by this run's code), so the handoffs may go now; the
				// consumers they were given went with the type's failed start. No outside node was made. The port
				// block stays; the config goes with the run's teardown.
				run_->handoffs.clear();
				run_->outsideRequests.clear();
				run_->channel->detach();
				run_->built = true;
				if (lifecycle_.insidesFailed(std::move(started.error()))) host_.changed(*this);
				return;
			}
			run_->recipe = std::move(started->recipe);
			run_->code =
				started->code.own() ? std::move(started->code.own()) : makePassThrough(ports_, started->code.pairs());
			run_->code->prepare(host_.format);
			run_->prepared = true;
			block_->drive(run_->code.get());
			if (run_->outsideRequests.empty()) buildRunObject();
			else makeOutsideNodes();
		}

		/// The outside nodes the run asked for, in request order, once the block is ready (its ports exist), the run
		/// started and none is made yet.
		void makeOutsideNodes() {
			if (!blockReady_ || !run_ || !run_->prepared || tearingDown_ || stopping_ || lifecycle_.blockLost() ||
			    run_->outsideRequests.empty() || !run_->outside.empty())
				return;
			run_->outside.reserve(run_->outsideRequests.size());
			for (std::size_t i = 0; i < run_->outsideRequests.size(); ++i) {
				auto& outside = *run_->outside.emplace_back(std::make_unique<Outside>(*this, i));
				const auto& [request, target] = run_->outsideRequests[i];
				outside.node = std::visit(
					[&](const auto& kind) { return host_.resources.adapterFor(kind).make(target, kind, outside); },
					request);
			}
		}

		/// Every resource is ready: the recipe builds the run object, and the run is ready once it returned.
		void buildRunObject() {
			run_->built = true;
			if (run_->recipe) {
				building_ = true;
				auto made = [&] {
					try {
						return run_->recipe(*this);
					} catch (...) {
						building_ = false;
						throw;
					}
				}();
				building_ = false;
				if (!made) {
					if (lifecycle_.insidesFailed(std::move(made.error()))) host_.changed(*this);
					return;
				}
				run_->object = std::move(*made);
			}
			if (lifecycle_.insidesReady()) host_.changed(*this);
		}

		/// The run is ready once every outside node is, and its run object is built.
		void outsideReady(std::size_t index) {
			run_->outside.at(index)->isReady = true;
			if (!run_->built && lifecycle_.state() != NodeState::failed &&
			    std::ranges::all_of(run_->outside, [](const auto& outside) { return outside->isReady; }))
				buildRunObject();
		}

		/// Any outside node lost, before or after the node ran, fails it.
		void outsideLost(std::string detail) {
			if (lifecycle_.insidesFailed({FailureReason::Code::engineRefused, std::move(detail)})) host_.changed(*this);
		}

		/// The run's own threads say it is dead: as an outside node's loss, it fails the node and its block and routes
		/// stay.
		void runLost(std::string detail) {
			if (lifecycle_.insidesFailed({FailureReason::Code::startFailed, std::move(detail)})) host_.changed(*this);
		}

		/// I5 steps 1 to 3: the barrier and release(); the outside nodes; the handoffs closed, each on its own thread.
		/// Steps 3 and 4 have the close deadline (I5a). The rest goes once every handoff has closed.
		void tearDown() {
			tearingDown_ = true;
			if (!run_) {
				finishRun();
				return;
			}
			Run& run = *run_;
			run.channel->closing();
			if (block_) block_->drive(nullptr);
			if (run.prepared) run.code->release();
			run.prepared = false;
			run.outside.clear();
			run.outsideRequests.clear();
			run.deadline = host_.timers.after(host_.closeDeadline,
			                                  [this, generation = run.generation] { closeOverdue(generation); });
			run.handoffsClosed = 0;
			closingHandoffs_ = true;
			for (auto& handoff : run.handoffs) handoff->close(run.channel->handoffClosedCall());
			closingHandoffs_ = false;
			closeRunObject();
		}

		void handoffClosed(std::uint64_t droppedFrames) {
			dropped_ += droppedFrames;
			++run_->handoffsClosed;
			closeRunObject();
		}

		/// I5 step 4, once every handoff reported closed, and never inside a close() call, so no handoff is destroyed
		/// while that loop is still calling it. The rest goes in runClosed once the run object says it has closed.
		void closeRunObject() {
			if (!tearingDown_ || closingHandoffs_ || runClosing_ || run_->handoffsClosed != run_->handoffs.size())
				return;
			run_->handoffs.clear();
			runClosing_ = true;
			if (!run_->object || run_->channel->close(*run_->object)) runClosed();
		}

		/// The run object's threads are gone.
		void runClosed() {
			if (!runClosing_) return;
			runClosing_ = false;
			finishRun();
		}

		/// I5 step 5, on the main loop: the run object, then its code, its recipe and last its config. Then the node is
		/// gone, or the next run starts (I7).
		void finishRun() {
			if (run_) {
				run_->deadline.reset();
				run_->channel->detach();
				run_->object.reset();
				run_->code.reset();
				run_->recipe = nullptr;
				run_.reset();
			}
			afterRun();
		}

		/// The close deadline passed with step 3 or 4 unfinished (I5a): the run is detached, so nothing it tells lands;
		/// what its threads can reach is leaked, never freed; and the node moves on.
		void closeOverdue(std::uint64_t generation) {
			if (!run_ || run_->generation != generation || !tearingDown_) return;
			run_->channel->detach();
			const std::size_t handoffsOpen = run_->handoffs.size() - run_->handoffsClosed;
			const bool objectOpen = runClosing_;
			// Deliberately never deleted: the run's threads may still touch any of it.
			static_cast<void>(new Leaked{std::move(run_->config), std::move(run_->channel), std::move(run_->recipe),
			                             std::move(run_->code), std::move(run_->object), std::move(run_->handoffs)});
			run_.reset();
			runClosing_ = false;
			host_.diagnosed(name_, "ear.close-overdue",
			                "the run did not close within " + std::to_string(host_.closeDeadline.count()) +
			                    " ms and is left running (" + std::to_string(handoffsOpen) + " handoffs open, run " +
			                    (objectOpen ? "closing" : "not yet told to close") + ")");
			afterRun();
		}

		void afterRun() {
			tearingDown_ = false;
			if (stopping_) {
				lifecycle_.stopped();
				host_.gone(*this, dropped_);
				return;
			}
			if (next_) startRun(std::exchange(next_, nullptr));
		}

		Impl& host_;
		NodeName name_;
		NodeLifecycle lifecycle_;
		Ports ports_;
		std::shared_ptr<const NodeConfig> next_;    // the config a restart starts once teardown ends
		std::shared_ptr<const NodeConfig> waiting_; // the config to start once an older node of this name has gone
		std::unique_ptr<Run> run_;                  // the current run (I7: at most one)
		std::unique_ptr<PortBlock> block_;
		bool blockReady_ = false;
		bool starting_ = false;
		bool building_ = false;
		bool tearingDown_ = false;
		bool stopping_ = false;
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
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; gone ones are pruned on add
	std::uint64_t generations = 0;            // every run's generation, across nodes, so none is ever reused
	bool stopRequested = false;
	std::move_only_function<void()> allGone;

	void requireOpen(const char* event, const NodeName& node) const {
		if (stopRequested)
			throw std::logic_error(std::string(event) + " for node " + node + " after stopAll(): a wiring bug");
	}

	/// The wanted node of this name: the newest one not stopping.
	Node& wanted(const NodeName& name) {
		for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
			if ((*it)->name() == name && !(*it)->stopping()) return **it;
		throw std::logic_error("node " + name + " is not a wanted node: a wiring bug");
	}

	void changed(const Node& node) {
		const NodeHealth health = node.health();
		for (auto* listener : listeners) listener->nodeChanged(health);
	}

	void noted(const NodeName& node, const std::string& text) {
		for (auto* listener : noteListeners) listener->nodeSays(node, text);
	}

	void diagnosed(const NodeName& node, std::string_view code, const std::string& detail) {
		for (auto* listener : diagnosticListeners) listener->nodeDiagnostic(node, code, detail);
	}

	void gone(const Node& node, std::uint64_t droppedFrames) {
		for (auto* listener : listeners) listener->nodeGone(node.name(), droppedFrames);
		olderGone(node.name());
		allGoneIfDone();
	}

	/// The oldest node left of this name may start now, if it is one that waited.
	void olderGone(const NodeName& name) {
		const auto next = std::ranges::find_if(
			nodes, [&](const auto& known) { return known->name() == name && !known->lifecycle().gone(); });
		if (next != nodes.end()) (*next)->olderGone();
	}

	void allGoneIfDone() {
		if (!allGone) return;
		for (const auto& node : nodes)
			if (!node->lifecycle().gone()) return;
		std::exchange(allGone, nullptr)();
	}
};

NodeHost::NodeHost(AudioFormat format, PortBlockFactory& blocks, ResourceRegistry& resources, Executor& mainLoop,
                   Timers& timers, std::chrono::milliseconds closeDeadline)
	: impl_(new Impl{format, blocks, resources, mainLoop, timers, closeDeadline, {}, {}, {}, {}, 0, false, nullptr}) {}

NodeHost::~NodeHost() = default;

void NodeHost::subscribe(NodeHealthListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::subscribeNotes(NodeNoteListener& listener) { impl_->noteListeners.push_back(&listener); }

void NodeHost::subscribeDiagnostics(NodeDiagnosticListener& listener) {
	impl_->diagnosticListeners.push_back(&listener);
}

void NodeHost::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeAdded", node.name);
	std::erase_if(impl_->nodes, [](const auto& known) { return known->lifecycle().gone(); });
	const bool olderGoing =
		std::ranges::any_of(impl_->nodes, [&](const auto& known) { return known->name() == node.name; });
	auto& added = *impl_->nodes.emplace_back(std::make_unique<Impl::Node>(*impl_, node.name));
	impl_->changed(added);
	added.add(std::move(config), olderGoing);
}

void NodeHost::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeUpdated", node.name);
	impl_->wanted(node.name).update(std::move(config));
}

void NodeHost::nodeRemoved(const NodeName& node) {
	impl_->requireOpen("nodeRemoved", node);
	impl_->wanted(node).stop();
}

void NodeHost::stopAll(GraphClosed, std::move_only_function<void()> allGone) {
	if (impl_->stopRequested) throw std::logic_error("stopAll() was called twice");
	impl_->stopRequested = true;
	impl_->allGone = std::move(allGone);
	for (const auto& node : impl_->nodes)
		if (!node->stopping()) node->stop();
	impl_->allGoneIfDone();
}

std::vector<NodeHealth> NodeHost::health() const {
	std::vector<NodeHealth> result;
	for (const auto& node : impl_->nodes)
		if (!node->lifecycle().gone()) result.push_back(node->health());
	return result;
}

} // namespace ear
