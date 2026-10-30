// The one crossing between the main loop and the control thread (docs/design/batch2.md sections
// 2 and 4.5, gateway.hpp): the Active Object pattern. Commands cross to the main loop as posted
// work; their results and every graph event cross back as work posted to the control executor,
// all from the main loop, so the control thread sees one stream in main-loop order.
//
// Posted work captures what it needs by value and the executors and graph by reference (they
// outlive the gateway), never the gateway itself, except watch(), which runs before any command.
#include "ear/control/gateway.hpp"

#include <utility>

namespace ear::control {

struct MainLoopGateway::Impl {
	Executor& mainLoop;
	Executor& control;
	GraphControl& graph;
	const GraphReader& reader;
	EventSink* sink = nullptr;   // main loop only

	/// Main loop: an event, copied, to the sink on the control executor; dropped before watch().
	void post(GraphEvent event) {
		if (sink == nullptr) return;
		control.post([sink = sink, event = std::move(event)]() mutable { sink->graphChanged(std::move(event)); });
	}
};

MainLoopGateway::MainLoopGateway(Executor& mainLoop, Executor& control, GraphControl& graph,
                                 const GraphReader& reader)
	: impl_(std::make_unique<Impl>(Impl{mainLoop, control, graph, reader})) {}

MainLoopGateway::~MainLoopGateway() = default;

void MainLoopGateway::run(std::vector<Command> commands,
                          std::move_only_function<void(std::vector<CommandResult>)> done) {
	impl_->mainLoop.post([&control = impl_->control, &graph = impl_->graph, &reader = impl_->reader,
	                      commands = std::move(commands), done = std::move(done)]() mutable {
		std::vector<CommandResult> results;
		results.reserve(commands.size());
		for (const Command& command : commands) results.push_back(runCommand(command, graph, reader));
		control.post([done = std::move(done), results = std::move(results)]() mutable { done(std::move(results)); });
	});
}

void MainLoopGateway::watch(EventSink& sink) {
	impl_->mainLoop.post([impl = impl_.get(), &sink] { impl->sink = &sink; });
}

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
