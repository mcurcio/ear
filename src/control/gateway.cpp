// The one crossing between the main loop and the control thread (docs/design/batch2.md sections
// 2 and 4.5, gateway.hpp; docs/design/knobly-config.md, crossings C1 and C4): the Active Object
// pattern. Commands go whole to the graph file keeper; its reply, every graph file change and
// every graph event cross back as work posted to the control executor, all from the main loop, so
// the control thread sees one stream in main-loop order.
//
// Each node's health as last sent and when its phase began live here on the main loop
// (docs/design/node-lifecycle-modules.md 5.4, D-M11): node.state goes out when any member changes, `since` moves only
// with the phase, and a snapshot's `since` is filled from the same map in the keeper's reply, before it is posted.
//
// Posted work captures what it needs by value and the executors by reference (they outlive the
// gateway), never the gateway itself, except watch(), which runs before any command. The reply
// holds the map by a shared pointer, so a reply the keeper sends late never reaches a gone gateway.
#include "ear/control/gateway.hpp"

#include <map>
#include <optional>
#include <utility>

namespace ear::control {
namespace {

/// A node's health as last sent and when its phase began (main loop only).
struct Sent {
	NodeHealth health;
	std::chrono::system_clock::time_point since;
};
using SentMap = std::map<NodeName, Sent, std::less<>>;

/// The health as the wire compares it: `superseded` is never on the wire.
NodeHealth onWire(NodeHealth health) {
	health.superseded = false;
	return health;
}

} // namespace

struct MainLoopGateway::Impl {
	Executor& mainLoop;
	Executor& control;
	GraphRequests& requests;
	WallClock clock;
	EventSink* sink = nullptr; // main loop only
	/// Each node's health as last sent (main loop only), forgotten when the node leaves the graph.
	std::shared_ptr<SentMap> sent = std::make_shared<SentMap>();

	/// Main loop: `call(*sink)` on the control executor; dropped before watch().
	template <class Call> void toSink(Call call) {
		if (sink == nullptr) return;
		control.post([sink = sink, call = std::move(call)]() mutable { call(*sink); });
	}
	void post(GraphEvent event) {
		toSink([event = std::move(event)](EventSink& to) mutable { to.graphChanged(std::move(event)); });
	}
};

MainLoopGateway::MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests, WallClock clock)
	: impl_(std::make_unique<Impl>(Impl{mainLoop, control, requests, std::move(clock)})) {}

MainLoopGateway::~MainLoopGateway() = default;

void MainLoopGateway::run(std::vector<Command> commands,
                          std::move_only_function<void(std::vector<CommandResult>)> done) {
	// The keeper calls the reply on the main loop after every event the batch caused was told, so
	// posting `done` from there puts it behind them in the control executor's queue.
	impl_->requests.submit(std::move(commands), [&control = impl_->control, sent = impl_->sent,
	                                             done = std::move(done)](std::vector<CommandResult> results) mutable {
		for (CommandResult& result : results)
			if (auto* view = std::get_if<GraphView>(&result))
				for (NodeView& node : view->nodes)
					if (const auto known = sent->find(node.spec.name); known != sent->end())
						node.since = known->second.since;
		control.post([done = std::move(done), results = std::move(results)]() mutable { done(std::move(results)); });
	});
}

void MainLoopGateway::watch(EventSink& sink) {
	impl_->mainLoop.post([impl = impl_.get(), &sink] { impl->sink = &sink; });
}

void MainLoopGateway::graphFileChanged(const GraphFileChanged& change) {
	impl_->toSink([change](EventSink& to) mutable { to.graphFileChanged(std::move(change)); });
}

void MainLoopGateway::graphFileWritten(const std::string&) {}

void MainLoopGateway::nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) {
	const NodeHealth health = healthFromSpec(node);
	const auto since = impl_->clock();
	impl_->sent->insert_or_assign(node.name, Sent{health, since});
	impl_->post(NodeAdded{node, config->ports().publicOnly(), health, since});
}

void MainLoopGateway::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) {
	impl_->post(NodeUpdated{node.name, node.settings, node.disabled, NodeOutcome::restart});
}

void MainLoopGateway::nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig>, const NodeChange& change) {
	impl_->post(NodeUpdated{node.name, node.settings, node.disabled, change.outcome});
}

void MainLoopGateway::nodeRemoved(const NodeName& node) {
	impl_->sent->erase(node);
	impl_->post(NodeRemoved{node});
}

void MainLoopGateway::routeAdded(const Route& route) { impl_->post(RouteAdded{route}); }

void MainLoopGateway::routeRemoved(const Route& route) { impl_->post(RouteRemoved{route}); }

void MainLoopGateway::nodeChanged(const NodeHealth& told) {
	NodeHealth health = onWire(told);
	auto known = impl_->sent->find(health.name);
	if (known != impl_->sent->end() && known->second.health == health) return;
	if (known == impl_->sent->end()) {
		known = impl_->sent->emplace(health.name, Sent{health, impl_->clock()}).first;
	} else {
		if (known->second.health.phase != health.phase) known->second.since = impl_->clock();
		known->second.health = health;
	}
	impl_->post(NodeChanged{std::move(health), known->second.since});
}

void MainLoopGateway::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	impl_->sent->erase(node);
	impl_->post(NodeStopped{node, droppedFrames});
	impl_->post(NodeGone{node, droppedFrames});
}

void MainLoopGateway::routeChanged(const RouteStatus& status) { impl_->post(RouteChanged{status}); }

} // namespace ear::control
