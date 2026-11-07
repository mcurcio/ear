// The node framework (docs/design/batch2.md section 4.1, node_host.hpp, rule 1): runs each wanted
// node's state machine and owns everything the node made. The port block is made when the node
// is added and goes only when it is removed; the insides (the type's code, its handoffs, its
// outside nodes: sound cards and RTP receivers) start, restart and fail behind it. Insides are
// torn down in one order: the block stops running their code, release(), outside nodes,
// handoffs, the insides' close, insides, config. Every kind of outside node shares one list, one naming function and
// one ready and loss path (docs/design/rtp.md section 6, rule 1). It reaches the engine only
// through the factory interfaces node_host.hpp declares.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace ear {
namespace {

/// One outside node the insides asked for, of any kind.
using OutsideRequest = std::variant<SoundCardRequest, RtpReceiverRequest>;

template <typename... Kinds> struct Overloaded : Kinds... {
	using Kinds::operator()...;
};

/// The engine node name of a node's outside node: the one home of "<node>.playback",
/// "<node>.capture" (sound cards) and "<node>.rtp" (the RTP receiver). A node name never holds
/// '.', so they cannot collide with a node.
NodeName outsideNodeName(const NodeName& node, const OutsideRequest& request) {
	const char* suffix = std::visit(Overloaded{
										[](const SoundCardRequest& card) {
											return card.direction == CardDirection::playback ? ".playback" : ".capture";
										},
										[](const RtpReceiverRequest&) { return ".rtp"; },
									},
	                                request);
	return node + suffix;
}

} // namespace

struct NodeHost::Impl {
	/// One wanted node and everything it made. Members are declared in reverse teardown order,
	/// so destroying a node that never stopped still removes its port block first and lets its
	/// config go last.
	class Node final : public PortBlockEvents, public NodeGrant {
	public:
		Node(Impl& host, NodeName name) : host_(host), name_(std::move(name)) {}

		const NodeName& name() const noexcept { return name_; }
		const NodeLifecycle& lifecycle() const noexcept { return lifecycle_; }
		bool stopping() const noexcept { return stopping_; }
		NodeHealth health() const { return {name_, lifecycle_.state(), lifecycle_.reason()}; }

		/// The port block first, once for the node's life; then the insides, which drive it: now, or
		/// once an older node of this name has gone, as a restart waits, so old and new insides
		/// never share what the name owns (a listening port, a key file).
		void add(std::shared_ptr<const NodeConfig> config, bool olderGoing) {
			block_ = host_.blocks.make(name_, config->ports(), host_.format, *this);
			if (olderGoing) waiting_ = std::move(config);
			else startInsides(std::move(config));
		}

		/// The older node of this name is gone.
		void olderGone() {
			if (waiting_) startInsides(std::exchange(waiting_, nullptr));
		}

		/// New insides from `config` behind the same port block, once the old ones are gone. An
		/// update arriving while the old ones are still going replaces the config to start.
		void update(std::shared_ptr<const NodeConfig> config) {
			if (waiting_) {
				waiting_ = lifecycle_.blockLost() ? nullptr : std::move(config);
				return;
			}
			if (lifecycle_.restartRequested()) host_.changed(*this);
			next_ = lifecycle_.blockLost() ? nullptr : std::move(config); // a lost block is final
			if (!tearingDown_) tearDownInsides();
		}

		/// Stopping: port block, then the insides' teardown; then gone.
		void stop() {
			stopping_ = true;
			next_.reset();
			waiting_.reset();
			if (lifecycle_.stopRequested()) host_.changed(*this);
			block_.reset();
			if (!tearingDown_) tearDownInsides();
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

		// NodeGrant
		AudioFormat format() const override { return host_.format; }
		InsidesEvents& events() override {
			if (!granting_) throw std::logic_error("node " + name_ + ": events were asked for outside start()");
			return *channel_;
		}
		RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames, HandoffConsumer& consumer) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": a handoff was asked for outside start()");
			handoffs_.push_back(host_.handoffs.make(name_, channels, capacityFrames, consumer));
			return handoffs_.back()->writer();
		}
		void soundCard(const SoundCardRequest& request) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": a sound card was asked for outside start()");
			outsideRequests_.emplace_back(request);
		}
		void rtpReceiver(const RtpReceiverRequest& request) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": an RTP receiver was asked for outside start()");
			if (std::ranges::any_of(outsideRequests_, [](const OutsideRequest& asked) {
					return std::holds_alternative<RtpReceiverRequest>(asked);
				}))
				throw std::logic_error("node " + name_ + ": a second RTP receiver was asked for in one start()");
			outsideRequests_.emplace_back(request);
		}

	private:
		/// One outside node the insides asked for (a sound card or an RTP receiver), and its
		/// events.
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

		/// One start's insides as their own threads reach the node: everything they tell crosses
		/// to the main loop, in order, and finds the node only until `closed`. Whatever is still
		/// queued then holds this object, not the node, so it may outlive the node.
		class Channel final : public InsidesEvents, public std::enable_shared_from_this<Channel> {
		public:
			Channel(Node& node, Executor& mainLoop) : node_(&node), mainLoop_(mainLoop) {}
			void say(std::string text) override {
				mainLoop_.post([self = shared_from_this(), text = std::move(text)] {
					if (self->node_ != nullptr) self->node_->host_.noted(self->node_->name_, text);
				});
			}
			void lost(std::string detail) override {
				mainLoop_.post([self = shared_from_this(), detail = std::move(detail)]() mutable {
					if (self->node_ != nullptr && !self->closing_) self->node_->insidesLost(std::move(detail));
				});
			}
			/// Main thread. The insides are going: a loss no longer fails the node.
			void closing() noexcept { closing_ = true; }
			/// Main thread. Nothing more reaches the node.
			void detach() noexcept { node_ = nullptr; }
			/// The `closed` the insides get: from any thread, later or inside close().
			std::move_only_function<void()> closedCall() {
				return [self = shared_from_this(), main = std::this_thread::get_id()] {
					// Only the main thread reads insideClose_. Insides that hold no threads finish
					// inside close(), so a stop or restart of a plain node takes no loop turn.
					if (std::this_thread::get_id() == main && self->insideClose_) {
						self->closedInside_ = true;
						return;
					}
					self->mainLoop_.post([self] {
						if (self->node_ != nullptr) self->node_->insidesClosed();
					});
				};
			}
			/// Main thread: close() on `insides`. True if it called `closed` before it returned.
			bool close(NodeInsides& insides) {
				insideClose_ = true;
				insides.close(closedCall());
				insideClose_ = false;
				return closedInside_;
			}

		private:
			Node* node_; // main thread only
			Executor& mainLoop_;
			bool closing_ = false;      // main thread only
			bool insideClose_ = false;  // main thread only
			bool closedInside_ = false; // main thread only
		};

		void startInsides(std::shared_ptr<const NodeConfig> config) {
			config_ = std::move(config);
			channel_ = std::make_shared<Channel>(*this, host_.mainLoop);
			granting_ = true;
			auto insides = config_->start(*this);
			granting_ = false;
			if (!insides) {
				// No write happened (the block was never driven by this node's code), so the
				// handoffs may go now; the consumers they were given went with the type's failed
				// start. No outside node was made. The port block stays.
				handoffs_.clear();
				outsideRequests_.clear();
				channel_->detach();
				channel_.reset();
				if (lifecycle_.insidesFailed(std::move(insides.error()))) host_.changed(*this);
				return;
			}
			insides_ = std::move(*insides);
			AudioCode& code = insides_->audioCode();
			code.prepare(host_.format);
			prepared_ = true;
			block_->drive(&code);
			if (outsideRequests_.empty()) {
				if (lifecycle_.insidesReady()) host_.changed(*this);
				return;
			}
			makeOutsideNodes();
		}

		/// The outside nodes the insides asked for, in request order, once the block is ready
		/// (its ports exist), the insides run and none is made yet.
		void makeOutsideNodes() {
			if (!blockReady_ || !insides_ || tearingDown_ || stopping_ || lifecycle_.blockLost() ||
			    outsideRequests_.empty() || !outside_.empty())
				return;
			outside_.reserve(outsideRequests_.size());
			for (std::size_t i = 0; i < outsideRequests_.size(); ++i) {
				auto& outside = *outside_.emplace_back(std::make_unique<Outside>(*this, i));
				const OutsideRequest& request = outsideRequests_[i];
				const NodeName engineNode = outsideNodeName(name_, request);
				outside.node =
					std::visit(Overloaded{
								   [&](const SoundCardRequest& card) {
									   return host_.cards.make(engineNode, name_, card, host_.format, outside);
								   },
								   [&](const RtpReceiverRequest& receiver) {
									   return host_.receivers.make(engineNode, name_, receiver, outside);
								   },
							   },
				               request);
			}
		}

		/// The insides are ready once every outside node is.
		void outsideReady(std::size_t index) {
			outside_.at(index)->isReady = true;
			if (std::ranges::all_of(outside_, [](const auto& outside) { return outside->isReady; }) &&
			    lifecycle_.insidesReady())
				host_.changed(*this);
		}

		/// Any outside node lost, before or after the node ran, fails it.
		void outsideLost(std::string detail) {
			if (lifecycle_.insidesFailed({FailureReason::Code::engineRefused, std::move(detail)})) host_.changed(*this);
		}

		/// The insides' own threads say they are dead: as an outside node's loss, it fails the node
		/// and its block and routes stay.
		void insidesLost(std::string detail) {
			if (lifecycle_.insidesFailed({FailureReason::Code::startFailed, std::move(detail)})) host_.changed(*this);
		}

		/// The block stops running the code; release(); outside nodes; handoffs closed. The rest
		/// goes in finishTeardown once every handoff has closed.
		void tearDownInsides() {
			tearingDown_ = true;
			if (channel_) channel_->closing();
			if (block_) block_->drive(nullptr);
			if (prepared_) insides_->audioCode().release();
			prepared_ = false;
			outside_.clear();
			outsideRequests_.clear();
			closed_ = 0;
			closing_ = true;
			for (auto& handoff : handoffs_)
				handoff->close([this](std::uint64_t droppedFrames) {
					dropped_ += droppedFrames;
					++closed_;
					finishTeardown();
				});
			closing_ = false;
			finishTeardown();
		}

		/// Runs once every handoff reported closed, and never inside a close() call, so no
		/// handoff is destroyed while that loop is still calling it. Then the insides close; the
		/// rest goes in insidesClosed once they say they have.
		void finishTeardown() {
			if (!tearingDown_ || closing_ || closed_ != handoffs_.size() || insidesClosing_) return;
			handoffs_.clear();
			insidesClosing_ = true;
			if (!insides_ || channel_->close(*insides_)) insidesClosed();
		}

		/// The insides' threads are gone. The config goes after the insides it started.
		void insidesClosed() {
			if (!insidesClosing_) return;
			insidesClosing_ = false;
			if (channel_) channel_->detach();
			insides_.reset();
			channel_.reset();
			config_.reset();
			tearingDown_ = false;
			if (stopping_) {
				lifecycle_.stopped();
				host_.gone(*this, dropped_);
				return;
			}
			if (next_) startInsides(std::exchange(next_, nullptr));
		}

		Impl& host_;
		NodeName name_;
		NodeLifecycle lifecycle_;
		std::shared_ptr<const NodeConfig> next_;    // the config a restart starts once teardown ends
		std::shared_ptr<const NodeConfig> waiting_; // the config to start once an older node of this name has gone
		std::shared_ptr<const NodeConfig> config_;
		std::shared_ptr<Channel> channel_; // from each start until its insides closed
		std::unique_ptr<NodeInsides> insides_;
		std::vector<std::unique_ptr<Handoff>> handoffs_;
		std::vector<OutsideRequest> outsideRequests_;
		std::vector<std::unique_ptr<Outside>> outside_;
		std::unique_ptr<PortBlock> block_;
		bool blockReady_ = false;
		bool granting_ = false;
		bool prepared_ = false;
		bool tearingDown_ = false;
		bool stopping_ = false;
		bool closing_ = false;
		bool insidesClosing_ = false;
		std::size_t closed_ = 0;
		std::uint64_t dropped_ = 0;
	};

	AudioFormat format;
	PortBlockFactory& blocks;
	HandoffFactory& handoffs;
	SoundCardFactory& cards;
	RtpReceiverFactory& receivers;
	Executor& mainLoop;
	std::vector<NodeHealthListener*> listeners;
	std::vector<NodeNoteListener*> noteListeners;
	std::vector<std::unique_ptr<Node>> nodes; // in arrival order; gone ones are pruned on add
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

NodeHost::NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs, SoundCardFactory& cards,
                   RtpReceiverFactory& receivers, Executor& mainLoop)
	: impl_(new Impl{format, blocks, handoffs, cards, receivers, mainLoop, {}, {}, {}, false, nullptr}) {}

NodeHost::~NodeHost() = default;

void NodeHost::subscribe(NodeHealthListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::subscribeNotes(NodeNoteListener& listener) { impl_->noteListeners.push_back(&listener); }

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
