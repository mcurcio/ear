// The one crossing between the main loop and the control thread (docs/design/batch2.md sections
// 2 and 4.5, gateway.hpp; docs/design/knobly-config.md, crossings C1 and C4): the Active Object
// pattern. Commands go whole to the graph file keeper; its reply, every graph file change and
// every graph event cross back as work posted to the control executor, all from the main loop, so
// the control thread sees one stream in main-loop order.
//
// Posted work captures what it needs by value and the executors by reference (they outlive the
// gateway), never the gateway itself, except watch(), which runs before any command.
#include "ear/control/gateway.hpp"

#include <utility>

namespace ear::control {

struct MainLoopGateway::Impl {
	Executor& mainLoop;
	Executor& control;
	GraphRequests& requests;
	EventSink* sink = nullptr;   // main loop only

	/// Main loop: `call(*sink)` on the control executor; dropped before watch().
	template <class Call> void toSink(Call call) {
		if (sink == nullptr) return;
		control.post([sink = sink, call = std::move(call)]() mutable { call(*sink); });
	}
	void post(GraphEvent event) {
		toSink([event = std::move(event)](EventSink& to) mutable { to.graphChanged(std::move(event)); });
	}
};

MainLoopGateway::MainLoopGateway(Executor& mainLoop, Executor& control, GraphRequests& requests)
	: impl_(std::make_unique<Impl>(Impl{mainLoop, control, requests})) {}

MainLoopGateway::~MainLoopGateway() = default;

void MainLoopGateway::run(std::vector<Command> commands,
                          std::move_only_function<void(std::vector<CommandResult>)> done) {
	// The keeper calls the reply on the main loop after every event the batch caused was told, so
	// posting `done` from there puts it behind them in the control executor's queue.
	impl_->requests.submit(std::move(commands), [&control = impl_->control, done = std::move(done)](
	                                                std::vector<CommandResult> results) mutable {
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
	const Ports& ports = config->ports();
	impl_->post(NodeAdded{node, Ports{ports.inputs, ports.outputs}});
}

void MainLoopGateway::nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig>) {
	impl_->post(NodeUpdated{node.name, node.settings});
}

void MainLoopGateway::nodeRemoved(const NodeName& node) { impl_->post(NodeRemoved{node}); }

void MainLoopGateway::routeAdded(const Route& route) { impl_->post(RouteAdded{route}); }

void MainLoopGateway::routeRemoved(const Route& route) { impl_->post(RouteRemoved{route}); }

void MainLoopGateway::nodeChanged(const NodeStatus& status) { impl_->post(NodeChanged{status}); }

void MainLoopGateway::nodeGone(const NodeName& node, std::uint64_t droppedFrames) {
	impl_->post(NodeStopped{node, droppedFrames});
}

void MainLoopGateway::routeChanged(const RouteStatus& status) { impl_->post(RouteChanged{status}); }

} // namespace ear::control
