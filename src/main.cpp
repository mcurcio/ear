// EAR's composition root (docs/design/m1-white-to-file.md section 2, "Boot in main"): reads the
// graph file, starts the engine, wires the packages together, replays the file as control calls
// and runs until SIGINT or SIGTERM, then stops every node before it exits.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <boost/program_options.hpp>

#include "ear/control/graph_file.hpp"
#include "ear/control/status_log.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
#include "ear/nodes/builtin.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/port_blocks.hpp"

namespace po = boost::program_options;

namespace {

/// engine.conf is copied beside the binary; the engine does no lookup of its own.
std::filesystem::path engineConfig() {
	return std::filesystem::read_symlink("/proc/self/exe").parent_path() / "engine.conf";
}

} // namespace

int main(int argc, char** argv) {
	po::options_description options("Options");
	options.add_options()
		("help,h", "print these options and exit")
		("config", po::value<std::string>()->default_value("ear.json"), "graph file path");

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

	const auto spec = ear::control::readGraphFile(arguments["config"].as<std::string>());
	if (!spec) {
		std::cerr << "ear: " << spec.error().detail << '\n';
		return 1;
	}

	// Declared first, so destroyed last: every adapter below uses it.
	auto created = ear::pipewire::Engine::create({engineConfig(), spec->graphFrequency, spec->blockSize});
	if (!created) {
		std::cerr << "ear: " << created.error().detail << '\n';
		return 1;
	}
	const std::unique_ptr<ear::pipewire::Engine> engine = std::move(*created);

	const ear::NodeCatalogue catalogue(ear::nodes::builtinNodeTypes());
	ear::DesiredGraph graph(catalogue);
	ear::pipewire::FilterPortBlocks blocks(*engine);
	ear::pipewire::ThreadHandoffs handoffs(engine->mainLoop());
	ear::NodeHost nodes({spec->graphFrequency, spec->blockSize}, blocks, handoffs);
	ear::pipewire::LiveGraph live(*engine);
	ear::pipewire::Links links(*engine);
	ear::Reconciler reconciler(links);
	ear::control::StatusLog log(std::cout);

	graph.subscribe(nodes);
	graph.subscribe(reconciler);
	nodes.subscribe(log);
	reconciler.subscribe(log);
	live.subscribe(reconciler);
	links.subscribe(reconciler);

	// Stopping ends the main loop once every node is gone (its file finished). The loop must
	// run for that: a node's handoffs report closed on it.
	bool stopping = false;
	bool allGone = false;
	auto stop = [&] {
		if (stopping) return;
		stopping = true;
		nodes.stopAll([&] {
			allGone = true;
			engine->quit();
		});
	};

	int status = 0;
	if (const auto replayed = ear::control::replay(*spec, graph); !replayed) {
		std::cerr << "ear: " << ear::describe(replayed.error()) << '\n';
		status = 1;
		stop();
	} else {
		engine->onStopSignal(stop);
	}
	if (!allGone) engine->run();
	return status;
}
