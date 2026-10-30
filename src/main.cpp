// EAR's composition root (docs/design/batch2.md section 4.7): reads the graph file, starts the
// engine and prints how its audio thread is scheduled, wires the packages together, opens the
// control port, replays the file as control calls and runs until SIGINT or SIGTERM. Stopping
// closes the graph to edits, stops every node, then closes the control port, stops the control
// thread and ends the main loop.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include "ear/control/gateway.hpp"
#include "ear/control/graph_file.hpp"
#include "ear/control/status_log.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
#include "ear/nodes/builtin.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/server/control_port.hpp"
#include "ear/server/control_thread.hpp"

namespace po = boost::program_options;

namespace {

/// engine.conf and the built editor (web/) sit beside the binary; nothing else is looked up.
std::filesystem::path binaryDirectory() {
	return std::filesystem::read_symlink("/proc/self/exe").parent_path();
}

} // namespace

int main(int argc, char** argv) {
	po::options_description options("Options");
	options.add_options()
		("help,h", "print these options and exit")
		("config", po::value<std::string>()->default_value("ear.json"), "graph file path")
		("control-address", po::value<std::string>()->default_value("0.0.0.0"),
		 "address the control port listens on")
		("control-port", po::value<std::uint16_t>()->default_value(7777),
		 "control port (0: any free port)")
		("web-root", po::value<std::string>(), "the graph editor's files (default: <directory of ear>/web)")
		("allowed-host", po::value<std::vector<std::string>>()->composing(),
		 "a DNS name requests may name in Host (repeatable); IP addresses and localhost always may");

	po::variables_map arguments;
	try {
		po::store(po::parse_command_line(argc, argv, options), arguments);
		po::notify(arguments);
	} catch (const po::error& error) {
		std::cerr << "ear: " << error.what() << "\n\n" << options << '\n';
		return 1;
	}

	if (arguments.count("help")) {
		std::cout << options << '\n';
		return 0;
	}

	const ear::server::ControlPortSettings portSettings{
	    .address = arguments["control-address"].as<std::string>(),
	    .port = arguments["control-port"].as<std::uint16_t>(),
	    .webRoot = arguments.count("web-root") ? std::filesystem::path(arguments["web-root"].as<std::string>())
	                                           : binaryDirectory() / "web",
	    .allowedHosts = arguments.count("allowed-host") ? arguments["allowed-host"].as<std::vector<std::string>>()
	                                                    : std::vector<std::string>{},
	};

	const auto spec = ear::control::readGraphFile(arguments["config"].as<std::string>());
	if (!spec) {
		std::cerr << "ear: " << spec.error().detail << '\n';
		return 1;
	}
	const ear::AudioFormat format{spec->graphFrequency, spec->blockSize};

	// Declaration order is boot order; destruction runs the other way: port, gateway, core,
	// adapters, control thread, main-loop executor, engine.
	auto created = ear::pipewire::Engine::create({binaryDirectory() / "engine.conf", format.rate, format.maxBlock});
	if (!created) {
		std::cerr << "ear: " << created.error().detail << '\n';
		return 1;
	}
	const std::unique_ptr<ear::pipewire::Engine> engine = std::move(*created);

	const ear::pipewire::Scheduling scheduling = engine->dataThreadScheduling();
	if (scheduling.realtime) {
		std::cout << "engine: realtime " << scheduling.detail << std::endl;
	} else {
		std::cout << "engine: not realtime (" << scheduling.detail << ")" << std::endl;
	}

	ear::pipewire::MainLoopExecutor mainLoop(*engine);
	// After Engine::create, so its thread inherits the blocked stop signals.
	ear::server::ControlThread control;
	ear::server::LevelBell bell;

	ear::pipewire::FilterPortBlocks blocks(*engine, bell);
	ear::pipewire::ThreadHandoffs handoffs(engine->mainLoop());
	ear::pipewire::AlsaSoundCards cards(*engine);
	ear::pipewire::Links links(*engine);
	ear::pipewire::LiveGraph live(*engine);

	const ear::NodeCatalogue catalogue(ear::nodes::builtinNodeTypes());
	ear::DesiredGraph graph(catalogue);
	ear::Reconciler reconciler(links);
	ear::NodeHost nodes(format, blocks, handoffs, cards);
	const ear::GraphStatus status({format, scheduling.realtime, scheduling.detail}, catalogue, graph, nodes, reconciler);
	ear::control::StatusLog log(std::cout);

	ear::control::MainLoopGateway gateway(mainLoop, control, graph, status);

	// Observers hear each change before the subscribers act on it, so a client gets node.added
	// before the node.state it causes.
	graph.observe(static_cast<ear::DesiredNodeListener&>(gateway));
	graph.observe(static_cast<ear::DesiredRouteListener&>(gateway));
	graph.subscribe(nodes);
	graph.subscribe(reconciler);
	nodes.subscribe(log);
	nodes.subscribe(gateway);
	reconciler.subscribe(log);
	reconciler.subscribe(gateway);
	live.subscribe(reconciler);
	links.subscribe(reconciler);

	auto started = ear::server::ControlPort::start(portSettings, control, gateway, blocks, bell);
	if (!started) {
		std::cerr << "ear: " << started.error().detail << '\n';
		return 1;
	}
	const std::unique_ptr<ear::server::ControlPort> port = std::move(*started);
	std::cout << "control listening on " << portSettings.address << ':' << port->port() << std::endl;

	// Stopping refuses edits at once (clients get "stopping"), lets every node finish (its file
	// is closed on the main loop, so the loop runs until then), and only then closes the port,
	// stops the control thread and ends the loop.
	bool stopping = false;
	bool allGone = false;
	auto stop = [&] {
		if (stopping) return;
		stopping = true;
		nodes.stopAll(graph.close(), [&] {
			allGone = true;
			port->stop();
			control.stop();
			engine->quit();
		});
	};

	int exitStatus = 0;
	if (const auto replayed = ear::control::replay(*spec, graph); !replayed) {
		std::cerr << "ear: " << ear::describe(replayed.error()) << '\n';
		exitStatus = 1;
		stop();
	} else {
		engine->onStopSignal(stop);
	}
	if (!allGone) engine->run();
	return exitStatus;
}
