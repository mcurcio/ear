// The node framework (docs/design/batch2.md section 4.1, node_host.hpp, rule 1): runs each wanted
// node's state machine and owns everything the node made. The port block is made when the node
// is added and goes only when it is removed; the insides (the type's code, its handoffs, its
// sound cards) start, restart and fail behind it. Insides are torn down in one order: the block
// stops running their code, release(), sound cards, handoffs, insides, config. It reaches the
// engine only through the factory interfaces node_host.hpp declares.
#include "ear/core/node_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ear {
namespace {

/// The engine node name of a node's sound card: the one home of "<node>.playback" and
/// "<node>.capture" (a node name never holds '.', so they cannot collide with a node).
NodeName cardNodeName(const NodeName& node, CardDirection direction) {
	return node + (direction == CardDirection::playback ? ".playback" : ".capture");
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
		NodeStatus status() const { return {name_, lifecycle_.state(), lifecycle_.reason()}; }

		/// The port block first, once for the node's life; then the insides, which drive it.
		void add(std::shared_ptr<const NodeConfig> config) {
			block_ = host_.blocks.make(name_, config->ports(), host_.format, *this);
			startInsides(std::move(config));
		}

		/// New insides from `config` behind the same port block, once the old ones are gone. An
		/// update arriving while the old ones are still going replaces the config to start.
		void update(std::shared_ptr<const NodeConfig> config) {
			if (lifecycle_.restartRequested()) host_.changed(*this);
			next_ = lifecycle_.blockLost() ? nullptr : std::move(config);   // a lost block is final
			if (!tearingDown_) tearDownInsides();
		}

		/// Stopping: port block, then the insides' teardown; then gone.
		void stop() {
			stopping_ = true;
			next_.reset();
			if (lifecycle_.stopRequested()) host_.changed(*this);
			block_.reset();
			if (!tearingDown_) tearDownInsides();
		}

		// PortBlockEvents
		void ready() override {
			blockReady_ = true;
			if (lifecycle_.portBlockReady()) host_.changed(*this);
			makeCards();
		}
		void lost(std::string detail) override {
			if (lifecycle_.portBlockLost({FailureReason::Code::engineRefused, std::move(detail)}))
				host_.changed(*this);
		}

		// NodeGrant
		AudioFormat format() const override { return host_.format; }
		RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames, HandoffConsumer& consumer) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": a handoff was asked for outside start()");
			handoffs_.push_back(host_.handoffs.make(name_, channels, capacityFrames, consumer));
			return handoffs_.back()->writer();
		}
		void soundCard(const SoundCardRequest& request) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": a sound card was asked for outside start()");
			cardRequests_.push_back(request);
		}

	private:
		/// One sound card the insides asked for, and its events.
		class Card final : public SoundCardEvents {
		public:
			Card(Node& node, std::size_t index) : node_(node), index_(index) {}
			std::unique_ptr<SoundCard> card;
			bool isReady = false;
			void ready() override { node_.cardReady(index_); }
			void lost(std::string detail) override { node_.cardLost(std::move(detail)); }

		private:
			Node& node_;
			std::size_t index_;
		};

		void startInsides(std::shared_ptr<const NodeConfig> config) {
			config_ = std::move(config);
			granting_ = true;
			auto insides = config_->start(*this);
			granting_ = false;
			if (!insides) {
				// No write happened (the block was never driven by this node's code), so the
				// handoffs may go now; the consumers they were given went with the type's failed
				// start. No card was made. The port block stays.
				handoffs_.clear();
				cardRequests_.clear();
				if (lifecycle_.insidesFailed(std::move(insides.error()))) host_.changed(*this);
				return;
			}
			insides_ = std::move(*insides);
			AudioCode& code = insides_->audioCode();
			code.prepare(host_.format);
			prepared_ = true;
			block_->drive(&code);
			if (cardRequests_.empty()) {
				if (lifecycle_.insidesReady()) host_.changed(*this);
				return;
			}
			makeCards();
		}

		/// The cards the insides asked for, once the block is ready (its ports exist), the
		/// insides run and no card is made yet.
		void makeCards() {
			if (!blockReady_ || !insides_ || tearingDown_ || stopping_ || lifecycle_.blockLost() ||
			    cardRequests_.empty() || !cards_.empty())
				return;
			cards_.reserve(cardRequests_.size());
			for (std::size_t i = 0; i < cardRequests_.size(); ++i) {
				auto& card = *cards_.emplace_back(std::make_unique<Card>(*this, i));
				const auto& request = cardRequests_[i];
				card.card = host_.cards.make(cardNodeName(name_, request.direction), name_, request, host_.format, card);
			}
		}

		void cardReady(std::size_t index) {
			cards_.at(index)->isReady = true;
			if (std::all_of(cards_.begin(), cards_.end(), [](const auto& card) { return card->isReady; }) &&
			    lifecycle_.insidesReady())
				host_.changed(*this);
		}

		void cardLost(std::string detail) {
			if (lifecycle_.insidesFailed({FailureReason::Code::engineRefused, std::move(detail)})) host_.changed(*this);
		}

		/// The block stops running the code; release(); cards; handoffs closed. The rest goes
		/// in finishTeardown once every handoff has closed.
		void tearDownInsides() {
			tearingDown_ = true;
			if (block_) block_->drive(nullptr);
			if (prepared_) insides_->audioCode().release();
			prepared_ = false;
			cards_.clear();
			cardRequests_.clear();
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
		/// handoff is destroyed while that loop is still calling it. The config goes after the
		/// insides it started.
		void finishTeardown() {
			if (!tearingDown_ || closing_ || closed_ != handoffs_.size()) return;
			handoffs_.clear();
			insides_.reset();
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
		std::shared_ptr<const NodeConfig> next_;   // the config a restart starts once teardown ends
		std::shared_ptr<const NodeConfig> config_;
		std::unique_ptr<NodeInsides> insides_;
		std::vector<std::unique_ptr<Handoff>> handoffs_;
		std::vector<SoundCardRequest> cardRequests_;
		std::vector<std::unique_ptr<Card>> cards_;
		std::unique_ptr<PortBlock> block_;
		bool blockReady_ = false;
		bool granting_ = false;
		bool prepared_ = false;
		bool tearingDown_ = false;
		bool stopping_ = false;
		bool closing_ = false;
		std::size_t closed_ = 0;
		std::uint64_t dropped_ = 0;
	};

	AudioFormat format;
	PortBlockFactory& blocks;
	HandoffFactory& handoffs;
	SoundCardFactory& cards;
	std::vector<NodeStatusListener*> listeners;
	std::vector<std::unique_ptr<Node>> nodes;   // in arrival order; gone ones are pruned on add
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
		const NodeStatus status = node.status();
		for (auto* listener : listeners) listener->nodeChanged(status);
	}

	void gone(const Node& node, std::uint64_t droppedFrames) {
		for (auto* listener : listeners) listener->nodeGone(node.name(), droppedFrames);
		allGoneIfDone();
	}

	void allGoneIfDone() {
		if (!allGone) return;
		for (const auto& node : nodes)
			if (!node->lifecycle().gone()) return;
		std::exchange(allGone, nullptr)();
	}
};

NodeHost::NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs,
                   SoundCardFactory& cards)
	: impl_(new Impl{format, blocks, handoffs, cards, {}, {}, false, nullptr}) {}

NodeHost::~NodeHost() = default;

void NodeHost::subscribe(NodeStatusListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	impl_->requireOpen("nodeAdded", node.name);
	std::erase_if(impl_->nodes, [](const auto& known) { return known->lifecycle().gone(); });
	auto& added = *impl_->nodes.emplace_back(std::make_unique<Impl::Node>(*impl_, node.name));
	impl_->changed(added);
	added.add(std::move(config));
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

std::vector<NodeStatus> NodeHost::status() const {
	std::vector<NodeStatus> result;
	for (const auto& node : impl_->nodes)
		if (!node->lifecycle().gone()) result.push_back(node->status());
	return result;
}

} // namespace ear
