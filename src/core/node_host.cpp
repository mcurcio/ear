// The node framework (docs/design/m1-white-to-file.md sections 1 and 2, node_host.hpp): runs each
// wanted node's state machine, owns everything the node made, and tears it down in one order:
// port block, release(), handoffs, insides. It reaches the engine only through the factory
// interfaces node_host.hpp declares.
#include "ear/core/node_host.hpp"

#include <stdexcept>
#include <utility>

namespace ear {

struct NodeHost::Impl {
	/// One wanted node and everything it made. Members are declared in reverse teardown order,
	/// so destroying a node that never stopped still removes its port block first.
	class Node final : public PortBlockEvents, public NodeGrant {
	public:
		Node(Impl& host, NodeName name) : host_(host), name_(std::move(name)) {}

		const NodeName& name() const noexcept { return name_; }
		const NodeLifecycle& lifecycle() const noexcept { return lifecycle_; }
		NodeStatus status() const { return {name_, lifecycle_.state(), lifecycle_.reason()}; }

		void start(const NodeConfig& config) {
			granting_ = true;
			auto insides = config.start(*this);
			granting_ = false;
			if (!insides) {
				// No write happened (there is no port block), so the handoffs may go now; the
				// consumers they were given went with the type's failed start.
				handoffs_.clear();
				if (lifecycle_.refused(std::move(insides.error()))) host_.changed(*this);
				return;
			}
			insides_ = std::move(*insides);
			AudioCode& code = insides_->audioCode();
			code.prepare(host_.format);
			prepared_ = true;
			block_ = host_.blocks.make(name_, config.ports(), host_.format, code, *this);
		}

		/// Stopping: port block, release(), handoffs, insides; then gone.
		void stop() {
			if (lifecycle_.stopRequested()) host_.changed(*this);
			block_.reset();
			if (prepared_) insides_->audioCode().release();
			prepared_ = false;
			closing_ = true;
			for (auto& handoff : handoffs_)
				handoff->close([this](std::uint64_t droppedFrames) {
					dropped_ += droppedFrames;
					++closed_;
					finishIfClosed();
				});
			closing_ = false;
			finishIfClosed();
		}

		// PortBlockEvents
		void ready() override {
			if (lifecycle_.portBlockReady()) host_.changed(*this);
		}
		void lost(std::string detail) override {
			if (lifecycle_.refused({FailureReason::Code::engineRefused, std::move(detail)})) host_.changed(*this);
		}

		// NodeGrant
		AudioFormat format() const override { return host_.format; }
		RtWriter& handoff(std::uint32_t channels, std::uint32_t capacityFrames, HandoffConsumer& consumer) override {
			if (!granting_) throw std::logic_error("node " + name_ + ": a handoff was asked for outside start()");
			handoffs_.push_back(host_.handoffs.make(name_, channels, capacityFrames, consumer));
			return handoffs_.back()->writer();
		}

	private:
		/// Runs once every handoff reported closed, and never inside a close() call, so no
		/// handoff is destroyed while this loop is still calling it.
		void finishIfClosed() {
			if (closing_ || closed_ != handoffs_.size() || lifecycle_.gone()) return;
			handoffs_.clear();
			insides_.reset();
			lifecycle_.stopped();
			host_.gone(*this, dropped_);
		}

		Impl& host_;
		NodeName name_;
		NodeLifecycle lifecycle_;
		std::unique_ptr<NodeInsides> insides_;
		std::vector<std::unique_ptr<Handoff>> handoffs_;
		std::unique_ptr<PortBlock> block_;
		bool granting_ = false;
		bool prepared_ = false;
		bool closing_ = false;
		std::size_t closed_ = 0;
		std::uint64_t dropped_ = 0;
	};

	AudioFormat format;
	PortBlockFactory& blocks;
	HandoffFactory& handoffs;
	std::vector<NodeStatusListener*> listeners;
	std::vector<std::unique_ptr<Node>> nodes;   // in arrival order; kept until the host goes
	bool stopRequested = false;
	std::move_only_function<void()> allGone;

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

NodeHost::NodeHost(AudioFormat format, PortBlockFactory& blocks, HandoffFactory& handoffs)
	: impl_(new Impl{format, blocks, handoffs, {}, {}, false, nullptr}) {}

NodeHost::~NodeHost() = default;

void NodeHost::subscribe(NodeStatusListener& listener) { impl_->listeners.push_back(&listener); }

void NodeHost::nodeAdded(const NodeName& node, const NodeConfig& config) {
	if (impl_->stopRequested) throw std::logic_error("node " + node + " was added after stopAll()");
	auto& added = *impl_->nodes.emplace_back(std::make_unique<Impl::Node>(*impl_, node));
	impl_->changed(added);
	added.start(config);
}

void NodeHost::stopAll(std::move_only_function<void()> allGone) {
	if (impl_->stopRequested) throw std::logic_error("stopAll() was called twice");
	impl_->stopRequested = true;
	impl_->allGone = std::move(allGone);
	for (const auto& node : impl_->nodes) node->stop();
	impl_->allGoneIfDone();
}

std::vector<NodeStatus> NodeHost::status() const {
	std::vector<NodeStatus> result;
	for (const auto& node : impl_->nodes)
		if (!node->lifecycle().gone()) result.push_back(node->status());
	return result;
}

} // namespace ear
