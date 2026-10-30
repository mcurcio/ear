// The one crossing between the main loop and the control thread (docs/design/batch2.md sections
// 2 and 4.5, gateway.hpp). Slice s0 stub: slice R1 builds it.
#include "ear/control/gateway.hpp"

#include <stdexcept>
#include <string>

namespace ear::control {
namespace {

[[noreturn]] void notBuilt(const char* what) {
	throw std::logic_error(std::string("MainLoopGateway::") + what + ": not built yet (slice R1)");
}

} // namespace

struct MainLoopGateway::Impl {};

MainLoopGateway::MainLoopGateway(Executor&, Executor&, GraphControl&, const GraphReader&) { notBuilt("MainLoopGateway"); }

MainLoopGateway::~MainLoopGateway() = default;

void MainLoopGateway::run(std::vector<Command>, std::move_only_function<void(std::vector<CommandResult>)>) {
	notBuilt("run");
}

void MainLoopGateway::watch(EventSink&) { notBuilt("watch"); }

void MainLoopGateway::nodeAdded(const NodeSpec&, std::shared_ptr<const NodeConfig>) { notBuilt("nodeAdded"); }

void MainLoopGateway::nodeUpdated(const NodeSpec&, std::shared_ptr<const NodeConfig>) { notBuilt("nodeUpdated"); }

void MainLoopGateway::nodeRemoved(const NodeName&) { notBuilt("nodeRemoved"); }

void MainLoopGateway::routeAdded(const Route&) { notBuilt("routeAdded"); }

void MainLoopGateway::routeRemoved(const Route&) { notBuilt("routeRemoved"); }

void MainLoopGateway::nodeChanged(const NodeStatus&) { notBuilt("nodeChanged"); }

void MainLoopGateway::nodeGone(const NodeName&, std::uint64_t) { notBuilt("nodeGone"); }

void MainLoopGateway::routeChanged(const RouteStatus&) { notBuilt("routeChanged"); }

} // namespace ear::control
