// EAR's composition root (docs/design/batch2.md section 4.7; docs/design/knobly-config.md, "Boot
// and stop"): opens the graph file folder and the recordings folder (apart from each other and the
// web root), reads ear.conf, starts the engine at the file's rate and block size (else the flags')
// and prints how its audio thread is scheduled, wires the packages together, starts the graph file
// keeper (its first offer runs the file, or nothing runs and the editor shows why), opens the
// control port and runs until SIGINT or SIGTERM. Once the flags are read, before any thread starts, it
// captures descriptors 1 and 2 (docs/design/node-framework.md section 8.8): from then on everything on
// standard output is one JSON record a line, from the one encoder (StatusLog), written by the output thread. Stopping closes the graph to edits, stops the
// keeper taking edits, stops every node, then closes the control port, stops the control thread
// and ends the main loop; the keeper's last write runs as its thread is joined.

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>

#include "ear/all_node_types/all_node_types.hpp"
#include "ear/config/graph_file_keeper.hpp"
#include "ear/control/gateway.hpp"
#include "ear/control/status_log.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/output/descriptor_capture.hpp"
#include "ear/output/output_writer.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/engine_log.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/server/control_port.hpp"
#include "ear/server/control_thread.hpp"

namespace po = boost::program_options;

namespace {

/// engine.conf and the built editor (web/) sit beside the binary; nothing else is looked up.
std::filesystem::path binaryDirectory() { return std::filesystem::read_symlink("/proc/self/exe").parent_path(); }

/// A whole number above 0 that fits the engine's 32 bits, or nothing.
std::optional<std::uint32_t> positive(std::int64_t value) {
	if (value <= 0 || value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
	return static_cast<std::uint32_t>(value);
}

/// The encoder's lines, to the output thread's queue.
class WriterLines final : public ear::control::OutputLines {
public:
	explicit WriterLines(ear::output::OutputWriter& writer) : writer_(writer) {}
	bool offer(std::string line) override { return writer_.offer(std::move(line)); }

private:
	ear::output::OutputWriter& writer_;
};

/// Stops the capture before the encoder it feeds is destroyed: what the pipes and the engine's log still hold is
/// recorded, and a write after it goes to the real descriptors.
class CaptureStopper {
public:
	explicit CaptureStopper(ear::output::DescriptorCapture& capture) : capture_(capture) {}
	~CaptureStopper() { capture_.stop(); }
	CaptureStopper(const CaptureStopper&) = delete;
	CaptureStopper& operator=(const CaptureStopper&) = delete;

private:
	ear::output::DescriptorCapture& capture_;
};

} // namespace

int main(int argc, char** argv) {
	po::options_description options("Options");
	options.add_options()("help,h", "print these options and exit")(
		"config-dir", po::value<std::string>()->default_value("config"),
		"the graph file folder (ear.conf; made if missing, not its parents)")(
		"graph-frequency", po::value<std::int64_t>()->default_value(48000),
		"the engine's rate when ear.conf cannot be read or does not give one")(
		"block-size", po::value<std::int64_t>()->default_value(256),
		"the engine's block size when ear.conf cannot be read or does not give one")(
		"control-address", po::value<std::string>()->default_value("0.0.0.0"), "address the control port listens on")(
		"control-port", po::value<std::uint16_t>()->default_value(7777), "control port (0: any free port)")(
		"web-root", po::value<std::string>(), "the graph editor's files (default: <directory of ear>/web)")(
		"allowed-host", po::value<std::vector<std::string>>()->composing(),
		"a DNS name requests may name in Host (repeatable); IP addresses and localhost always may")(
		"recordings-dir", po::value<std::string>()->default_value("recordings"),
		"the folder file nodes write into (made if missing, not its parents); their paths are "
		"relative to it");

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

	const auto flagRate = positive(arguments["graph-frequency"].as<std::int64_t>());
	const auto flagBlock = positive(arguments["block-size"].as<std::int64_t>());
	if (!flagRate || !flagBlock) {
		std::cerr << "ear: --graph-frequency and --block-size must be whole numbers above 0\n";
		return 1;
	}

	// Standard output's one way out, before any thread: descriptors 1 and 2 become pipes the capture thread reads into
	// unattributed log records, the output thread writes every record to the real standard output, and PipeWire logs
	// through ear's own pw_log, drained on the capture thread. Declaration order is what stopping needs: the capture
	// stops (recording what it still holds) before the encoder goes, and the writer writes everything after.
	auto installed = ear::output::DescriptorCapture::install();
	if (!installed) {
		std::cerr << "ear: cannot capture standard output: " << installed.error() << '\n';
		return 1;
	}
	const std::unique_ptr<ear::output::DescriptorCapture> capture = std::move(*installed);
	ear::output::OutputWriter writer(capture->realOutput());
	WriterLines lines(writer);
	ear::control::StatusLog log(lines);
	ear::pipewire::EngineLog& engineLog = ear::pipewire::EngineLog::install();
	capture->watch(engineLog.bell(), [&] { engineLog.drain(log); });
	capture->start([&](ear::output::DescriptorCapture::Line line) {
		log.log({.level = line.source == "stderr" ? ear::LogLevel::warn : ear::LogLevel::info,
		         .source = std::string(line.source),
		         .text = std::move(line.text),
		         .cut = line.cut});
	});
	const CaptureStopper stopCapture(*capture);

	const std::filesystem::path configDir = arguments["config-dir"].as<std::string>();
	const std::filesystem::path recordingsDir = arguments["recordings-dir"].as<std::string>();
	auto opened = ear::folder::ConfinedFolder::open(configDir);
	if (!opened) {
		// Each folder names itself "folder <path>"; the role is main's to say.
		log.exiting(1, "graph file " + opened.error().detail);
		return 1;
	}
	const auto config = std::make_shared<const ear::folder::ConfinedFolder>(std::move(*opened));
	auto recordings = ear::folder::ConfinedFolder::open(recordingsDir);
	if (!recordings) {
		log.exiting(1, "recordings " + recordings.error().detail);
		return 1;
	}
	const std::array<std::filesystem::path, 3> folders{configDir, recordingsDir, portSettings.webRoot};
	if (const auto apart = ear::folder::requireApart(folders); !apart) {
		log.exiting(1, apart.error().detail);
		return 1;
	}

	// ear.conf is read once here, on the main thread, before the keeper's thread starts. Its two
	// format lines set the engine when both read; otherwise the flags do (the file's own values then
	// show as pending restart).
	const ear::NodeCatalogue catalogue(ear::nodes::allNodeTypes(
		{.recordings = std::make_shared<const ear::folder::ConfinedFolder>(std::move(*recordings)),
	     .config = config,
	     .log = &log}));
	auto first = ear::config::GraphFileKeeper::load(*config, catalogue);
	const ear::AudioFormat format = first.format().value_or(ear::AudioFormat{*flagRate, *flagBlock});

	// Declaration order is boot order; destruction runs the other way: port, gateway, keeper, core,
	// adapters, control thread, main-loop executor, engine.
	auto created = ear::pipewire::Engine::create({binaryDirectory() / "engine.conf", format.rate, format.maxBlock});
	if (!created) {
		log.exiting(1, created.error().detail);
		return 1;
	}
	const std::unique_ptr<ear::pipewire::Engine> engine = std::move(*created);

	const ear::pipewire::Scheduling scheduling = engine->dataThreadScheduling();
	log.engineScheduling(scheduling.realtime, scheduling.detail);

	ear::pipewire::MainLoopExecutor mainLoop(*engine);
	// After Engine::create, so its thread inherits the blocked stop signals.
	ear::server::ControlThread control;
	ear::server::LevelBell bell;

	ear::pipewire::FilterPortBlocks blocks(*engine, bell);
	ear::pipewire::ThreadHandoffs handoffs(engine->mainLoop());
	ear::pipewire::AlsaSoundCards cards(*engine);
	ear::pipewire::RtpReceivers receivers(*engine);
	ear::pipewire::Links links(*engine);
	ear::pipewire::LiveGraph live(*engine);

	ear::DesiredGraph graph(catalogue);
	ear::Reconciler reconciler(links);
	ear::NodeHost nodes(format, blocks, handoffs, cards, receivers, mainLoop);
	const ear::GraphStatus status({format, scheduling.realtime, scheduling.detail}, catalogue, graph, nodes,
	                              reconciler);

	ear::config::GraphFileKeeper keeper(std::move(first), *config, format, graph, status, mainLoop);
	ear::control::MainLoopGateway gateway(mainLoop, control, keeper);

	// Observers hear each change before the subscribers act on it, so a client gets node.added
	// before the node.state it causes.
	graph.observe(static_cast<ear::DesiredNodeListener&>(gateway));
	graph.observe(static_cast<ear::DesiredRouteListener&>(gateway));
	graph.subscribe(nodes);
	graph.subscribe(reconciler);
	nodes.subscribe(log);
	nodes.subscribe(gateway);
	nodes.subscribeNotes(log);
	reconciler.subscribe(log);
	reconciler.subscribe(gateway);
	live.subscribe(reconciler);
	links.subscribe(reconciler);
	// Each graph file change is told to the gateway, then the status log, before its graph events.
	keeper.subscribe(gateway);
	keeper.subscribe(log);

	// Before the control port, so every request finds the keeper's thread; after Engine::create, so
	// the thread inherits the blocked stop signals. Its first offer waits for the main loop to run.
	if (const auto started = keeper.start(); !started) {
		log.exiting(1, "graph file " + started.error());
		return 1;
	}

	auto started = ear::server::ControlPort::start(portSettings, control, gateway, blocks, bell);
	if (!started) {
		log.exiting(1, started.error().detail);
		return 1;
	}
	const std::unique_ptr<ear::server::ControlPort> port = std::move(*started);
	log.controlListening(portSettings.address, port->port());

	// Stopping refuses edits at once (clients get "stopping"), lets the keeper answer what it holds,
	// lets every node finish (its file is closed on the main loop, so the loop runs until then), and
	// only then closes the port, stops the control thread and ends the loop. A file that does not
	// read, or that the graph refuses, runs nothing and keeps ear up, so the editor can fix it.
	bool stopping = false;
	auto stop = [&] {
		if (stopping) return;
		stopping = true;
		const ear::GraphClosed closed = graph.close();
		keeper.stop();
		nodes.stopAll(closed, [&] {
			port->stop();
			control.stop();
			engine->quit();
		});
	};
	engine->onStopSignal(stop);
	engine->run();

	// The keeper's last write runs as its thread ends; its failure reaches standard output only.
	if (const auto failed = keeper.join()) log.graphFileWriteFailed(*failed);
	return 0;
}
