// EAR's composition root (docs/architecture.md#boot-and-stop): opens the graph file folder and the recordings folder
// (apart from each other and the web root), reads ear.conf, starts the engine at the file's rate and block size (else
// the flags') and prints how its audio thread is scheduled, wires the packages together, starts the graph file keeper
// (its first offer runs the file, or nothing runs and the editor shows why), opens the control port and runs until
// SIGINT or SIGTERM. Once the flags are read, before any thread starts, it captures descriptors 1 and 2
// (docs/output-records.md): from then on everything on standard output is one JSON record a line, from the one encoder
// (StatusLog), written by the output thread. Stopping closes the graph to edits, stops the keeper taking edits, stops
// every node, then closes the control port, stops the control thread and ends the main loop; the keeper's last write
// runs as its thread is joined.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>

#include <unistd.h>

#include "ear/all_node_types/all_node_types.hpp"
#include "ear/config/graph_file_keeper.hpp"
#include "ear/control/gateway.hpp"
#include "ear/control/status_log.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/reconciler.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/output/descriptor_capture.hpp"
#include "ear/output/output_writer.hpp"
#include "ear/output/stall_watch.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/engine_log.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/pipewire/sources.hpp"
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

/// The recording kind's adapter (docs/writing-a-node-type.md#configure-and-createof): files made beneath the recordings
/// folder, which no node type holds. Glue only, as WriterLines is: ear_folder and ear_core do not see each other.
class ConfinedRecordings final : public ear::RecordingAdapter {
public:
	explicit ConfinedRecordings(ear::folder::ConfinedFolder folder) : folder_(std::move(folder)) {}
	std::expected<std::unique_ptr<ear::RecordingFile>, std::string> open(const ear::NodeName&,
	                                                                     const std::string& path) override {
		auto made = folder_.create(path);
		if (!made) return std::unexpected(std::move(made.error().detail));
		return std::make_unique<Recording>(*made);
	}

private:
	class Recording final : public ear::RecordingFile {
	public:
		explicit Recording(int fd) noexcept : fd_(fd) {}
		Recording(const Recording&) = delete;
		Recording& operator=(const Recording&) = delete;
		~Recording() override { ::close(fd_); }
		int descriptor() const noexcept override { return fd_; }

	private:
		int fd_;
	};
	ear::folder::ConfinedFolder folder_;
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

/// The glue between NodeHost's judgement and the engine's adapters
/// (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog): once ear.data-loop-wedged is said, the engine
/// is marked, so Links, the outside nodes and the port blocks make no call that would wait on the wedged data loop
/// (ear/pipewire/data_loop_wedge.hpp). Main thread, before any later input.
class WedgeMarker final : public ear::EngineDiagnosticListener {
public:
	explicit WedgeMarker(const ear::pipewire::Engine& engine) : engine_(engine) {}
	void engineDiagnostic(const ear::EngineDiagnostic& diagnostic) override {
		if (diagnostic.code == "ear.data-loop-wedged") ear::pipewire::markDataLoopWedged(engine_);
	}

private:
	const ear::pipewire::Engine& engine_;
};

/// The main loop's heartbeat (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog): a main-loop timer
/// that beats the watchdog every 100 ms, armed again each time it runs. Its first beat arms the watchdog, so boot
/// before the loop runs is no stall.
class Heartbeat {
public:
	Heartbeat(ear::Timers& timers, ear::output::StallWatch& watch) : timers_(timers), watch_(watch) { arm(); }

private:
	void arm() {
		timer_ = timers_.after(std::chrono::milliseconds(100), [this] {
			watch_.beat();
			arm();
		});
	}
	ear::Timers& timers_;
	ear::output::StallWatch& watch_;
	std::unique_ptr<ear::Timer> timer_;
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
	// A crash's last words: std::terminate writes out what is still queued, then the pipes, then its own message.
	capture->onCrash([&] { writer.spill(); });
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
	const ear::NodeCatalogue catalogue(ear::nodes::allNodeTypes({.config = config, .log = &log}));
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
	// The main-loop watchdog: its thread reads the heartbeat, asks MainLoopActivity where the loop is (ear_output sees
	// nothing of EAR's) and writes its two records straight to the output queue, never waiting on the main loop.
	ear::output::StallWatch stallWatch([] { return ear::MainLoopActivity::current(); },
	                                   [&log](std::int64_t ms, const std::string& in) { log.mainLoopStalled(ms, in); },
	                                   [&log](std::int64_t ms) { log.mainLoopResumed(ms); });
	Heartbeat heartbeat(mainLoop, stallWatch); // not const: its timer re-arms itself
	// After Engine::create, so its thread inherits the blocked stop signals.
	ear::server::ControlThread control;
	ear::server::LevelBell bell;

	ear::pipewire::FilterPortBlocks blocks(*engine, bell);
	ear::pipewire::ThreadHandoffs handoffs(engine->mainLoop());
	ear::pipewire::ThreadSinks sinks(mainLoop); // a sink's begun and closed post where the nodes' events do
	ear::pipewire::EventSources sources;
	ear::pipewire::AlsaSoundCards cards(*engine);
	ear::pipewire::RtpReceivers receivers(*engine);
	ear::pipewire::Links links(*engine);
	ear::pipewire::LiveGraph live(*engine);

	ear::DesiredGraph graph(catalogue);
	ear::Reconciler reconciler(links);
	ConfinedRecordings recordingFiles(std::move(*recordings));
	ear::ResourceRegistry resources(cards, receivers, handoffs, sinks, sources, recordingFiles);
	ear::NodeHost nodes(format, blocks, resources, mainLoop, mainLoop);
	const ear::GraphStatus status({format, scheduling.realtime, scheduling.detail}, catalogue, graph, nodes,
	                              reconciler);

	ear::config::GraphFileKeeper keeper(std::move(first), *config, format, graph, status, nodes, mainLoop);
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
	nodes.subscribeDiagnostics(log); // ear.close-overdue, ear.fade-unacknowledged, ear.barrier-overdue, ear.hook-slow
	nodes.subscribeLog(log);         // a stop() that threw during a teardown
	nodes.subscribeEngineDiagnostics(log); // ear.data-loop-wedged, as a record with no node
	WedgeMarker wedgeMarker(*engine);
	nodes.subscribeEngineDiagnostics(wedgeMarker); // then the engine's adapters stop calling into the loop
	reconciler.subscribe(log);
	reconciler.subscribe(gateway);
	live.subscribe(reconciler);
	links.subscribe(reconciler);
	// Each graph file change is told to the gateway, then the status log, before its graph events.
	keeper.subscribe(gateway);
	keeper.subscribe(log);
	// Each answered node.start, node.stop and node.restart: an action record, before the node records it causes.
	keeper.subscribeActions(log);
	// Each snapshot's since, filled when the graph is read.
	keeper.stampSnapshots(gateway);

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
	// The loop has ended and beats no more: the keeper's last write is not a stall.
	stallWatch.stop();

	// The keeper's last write runs as its thread ends; its failure reaches standard output only.
	if (const auto failed = keeper.join()) log.graphFileWriteFailed(*failed);
	// A run leaked past its close deadline may still post to the main loop or signal an engine source from its own
	// threads, and a wedged data loop's blocks and outside nodes were leaked because destroying them would wait on it
	// (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog); destroying the executor and the engine under
	// them would be a use after free or a hang. ear.conf is written by now, so end here without running destructors,
	// once the output thread's queue (the diagnostics among it) is written out: standard output is a captured pipe now,
	// so flushing std::cout would write nothing.
	if (nodes.leaked() > 0) {
		writer.spill();
		std::_Exit(0);
	}
	return 0;
}
