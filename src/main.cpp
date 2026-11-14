// EAR's composition root (docs/architecture.md#boot-and-stop): opens the graph file folder and the recordings folder
// (apart from each other and the web root), reads ear.conf, starts the engine at the file's rate and block size (else
// the flags') and prints how its audio thread is scheduled, wires the packages together, starts the graph file keeper
// (its first offer runs the file, or nothing runs and the editor shows why), opens the control port and runs until
// SIGINT or SIGTERM. Once the flags are read, before any thread starts, it captures descriptors 1 and 2
// (docs/output-records.md): from then on everything on standard output is one JSON record a line, from the one encoder
// (StatusLog), written by the output thread. Stopping closes the graph to edits, stops the keeper taking edits, stops
// every node, then closes the control port, stops the control thread and ends the main loop; the keeper's last write
// runs as its thread is joined, the I/O thread stops last, and the record `ear: stopped` ends the stop.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <iostream>
#include <memory>
#include <stdexcept>
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
#include "ear/core/counter_watch.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/graph_view.hpp"
#include "ear/core/main_loop_activity.hpp"
#include "ear/core/node_catalogue.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/rate_text.hpp"
#include "ear/core/reconciler.hpp"
#include "ear/folder/confined_folder.hpp"
#include "ear/output/descriptor_capture.hpp"
#include "ear/output/output_writer.hpp"
#include "ear/output/stall_watch.hpp"
#include "ear/pipewire/data_loop_wedge.hpp"
#include "ear/pipewire/engine.hpp"
#include "ear/pipewire/engine_log.hpp"
#include "ear/pipewire/faults.hpp"
#include "ear/pipewire/handoffs.hpp"
#include "ear/pipewire/links.hpp"
#include "ear/pipewire/live_graph.hpp"
#include "ear/pipewire/main_loop_executor.hpp"
#include "ear/pipewire/port_blocks.hpp"
#include "ear/pipewire/rtp_receivers.hpp"
#include "ear/pipewire/sound_cards.hpp"
#include "ear/pipewire/sources.hpp"
#include "ear/runtime/io_thread.hpp"
#include "ear/runtime/loops.hpp"
#include "ear/runtime/stores.hpp"
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
/// is marked, so Links, the engine companions and the port blocks make no call that would wait on the wedged data loop
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
/// before the loop runs is no stall. Each beat also ticks the counters' pacer, which reads them there rather than on a
/// wake-up of its own (docs/engine.md#reporting-the-counters).
class Heartbeat {
public:
	Heartbeat(ear::Timers& timers, ear::output::StallWatch& watch, ear::CounterWatch& counters)
		: timers_(timers), watch_(watch), counters_(counters) {
		arm();
	}

private:
	void arm() {
		timer_ = timers_.after(std::chrono::milliseconds(100), [this] {
			watch_.beat();
			counters_.tick();
			arm();
		});
	}
	ear::Timers& timers_;
	ear::output::StallWatch& watch_;
	ear::CounterWatch& counters_;
	std::unique_ptr<ear::Timer> timer_;
};

} // namespace

int main(int argc, char** argv) {
	po::options_description options("Options");
	options.add_options()("help,h", "print these options and exit")(
		"config-dir", po::value<std::string>()->default_value("config"),
		"the graph file folder (ear.conf; made if missing, not its parents)")(
		"rate", po::value<std::string>()->default_value("48khz"),
		"the engine's rate (8khz to 768khz, written as ear.conf writes it: 48khz, 44.1khz, 48000hz) when ear.conf "
		"cannot be read or does not give one")(
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
	} catch (const po::unknown_option& error) {
		// The flag the graph file's old graph_frequency had (docs/architecture.md#boot-and-stop): no compatibility is
		// kept, so it is refused, saying what replaced it.
		if (error.get_option_name() == "--graph-frequency") {
			std::cerr << "ear: --graph-frequency is gone: write --rate with a unit, such as --rate 48khz\n";
			return 1;
		}
		std::cerr << "ear: " << error.what() << "\n\n" << options << '\n';
		return 1;
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

	// The same grammar and range as ear.conf's graph { rate ...; } (docs/graph-file.md#rates).
	const auto flagRate = ear::readRate(arguments["rate"].as<std::string>(), ear::graphRates);
	if (!flagRate) {
		std::cerr << "ear: --rate: " << flagRate.error() << '\n';
		return 1;
	}
	const auto flagBlock = positive(arguments["block-size"].as<std::int64_t>());
	if (!flagBlock) {
		std::cerr << "ear: --block-size must be a whole number above 0\n";
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

	// ear.conf is read once here, on the main thread, before the keeper's thread starts. Its graph
	// block's rate and block_size set the engine when both read; otherwise the flags do (the file's own
	// values then show as pending restart).
	std::vector<std::unique_ptr<const ear::NodeType>> types = ear::nodes::allNodeTypes({.config = config, .log = &log});
	// Every declared store's files narrowed before any node exists (docs/writing-a-node-type.md#stores); what could not
	// be is a log record from the store's type.
	for (const ear::runtime::StoreSweepFinding& finding : ear::runtime::sweepStores(*config, types))
		log.log({.level = ear::LogLevel::warn, .source = finding.type, .text = finding.detail, .cut = false});
	// The types' registration check (two of one name, a name not 1 or more of [a-z0-9]) is a build's bug: exit 1, saying
	// which.
	std::optional<ear::NodeCatalogue> registered;
	try {
		registered.emplace(std::move(types));
	} catch (const std::invalid_argument& refused) {
		log.exiting(1, refused.what());
		return 1;
	}
	const ear::NodeCatalogue& catalogue = *registered;
	auto first = ear::config::GraphFileKeeper::load(*config, catalogue);
	const ear::AudioFormat format = first.format().value_or(ear::AudioFormat{*flagRate, *flagBlock});

	// Declaration order is boot order; destruction runs the other way: port, gateway, keeper, core,
	// adapters, control thread, main-loop executor, I/O thread, engine.
	// Where the engine's callback boundary reports a caught exception, split by owner
	// (docs/architecture.md#faults-by-owner): the record first; a fault no adapter owns then has the capture write out
	// what is queued (the record among it) and what its pipes hold, then its line straight to the real standard error
	// (never through a pipe whose thread could take it and lose it as the process dies), and ends the process.
	ear::pipewire::RecordThenEnd faults([&log](const ear::InternalError& error) { log.internalError(error); },
	                                    [&capture](std::string_view line) { capture->lastWords(line); });
	auto created =
		ear::pipewire::Engine::create({binaryDirectory() / "engine.conf", format.rate, format.maxBlock, &faults});
	if (!created) {
		log.exiting(1, created.error().detail);
		return 1;
	}
	const std::unique_ptr<ear::pipewire::Engine> engine = std::move(*created);

	const ear::pipewire::Scheduling scheduling = engine->dataThreadScheduling();
	log.engineScheduling(scheduling.realtime, scheduling.detail);

	// The framework's one thread for blocking work (docs/architecture.md#threads), before anything that posts to it and
	// stopped after all of them; after Engine::create, so it inherits the blocked stop signals.
	ear::runtime::IoThread io(log); // a job's throw no adapter caught: ear.internal-error, then the end
	// The nodes' stores (docs/writing-a-node-type.md#stores): files in the graph file folder, opened and written on the
	// I/O thread.
	ear::runtime::ConfinedStores stores(config, io);

	ear::pipewire::MainLoopExecutor mainLoop(*engine);
	// The main-loop watchdog: its thread reads the heartbeat, asks MainLoopActivity where the loop is (ear_output sees
	// nothing of EAR's) and writes its two records straight to the output queue, never waiting on the main loop.
	ear::output::StallWatch stallWatch([] { return ear::MainLoopActivity::current(); },
	                                   [&log](std::int64_t ms, const std::string& in) { log.mainLoopStalled(ms, in); },
	                                   [&log](std::int64_t ms) { log.mainLoopResumed(ms); });
	// After Engine::create, so its thread inherits the blocked stop signals.
	ear::server::ControlThread control;
	ear::server::LevelBell bell;

	ear::pipewire::FilterPortBlocks blocks(*engine, bell);
	ear::pipewire::ThreadHandoffs handoffs(engine->mainLoop());
	ear::pipewire::ThreadSinks sinks(mainLoop); // a sink's begun and closed post where the nodes' events do
	ear::pipewire::EventSources sources;
	// The nodes' loops (docs/writing-a-node-type.md#loops): no thread until a node asks for one; its events post where the
	// nodes' do. Made before NodeHost and freed after it, once every loop closed.
	ear::runtime::Loops loops(mainLoop);
	ear::pipewire::AlsaSoundCards cards(*engine);
	ear::pipewire::RtpReceivers receivers(*engine);
	ear::pipewire::Links links(*engine);
	ear::pipewire::LiveGraph live(*engine);

	ear::DesiredGraph graph(catalogue);
	ear::Reconciler reconciler(links);
	ConfinedRecordings recordingFiles(std::move(*recordings));
	ear::ResourceRegistry resources(ear::ResourceAdapters{.cards = &cards,
	                                                      .receivers = &receivers,
	                                                      .handoffs = &handoffs,
	                                                      .sinks = &sinks,
	                                                      .sources = &sources,
	                                                      .recordings = &recordingFiles,
	                                                      .loops = &loops,
	                                                      .stores = &stores});
	ear::NodeHost nodes(format, blocks, resources, mainLoop, mainLoop);
	const ear::GraphStatus status({format, scheduling.realtime, scheduling.detail}, catalogue, graph, nodes, reconciler,
	                              engine.get());

	ear::config::GraphFileKeeper keeper(std::move(first), *config, format, graph, status, nodes, mainLoop);
	ear::control::MainLoopGateway gateway(mainLoop, control, keeper);
	// The counters' reports (docs/engine.md#reporting-the-counters): read at the heartbeat, told to the wire and to
	// standard output.
	ear::CounterWatch counterWatch(mainLoop, *engine, nodes, format);
	Heartbeat heartbeat(mainLoop, stallWatch, counterWatch); // not const: its timer re-arms itself

	// Observers hear each change before the subscribers act on it, so a client gets node.added
	// before the node.state it causes.
	graph.observe(static_cast<ear::DesiredNodeListener&>(gateway));
	graph.observe(static_cast<ear::DesiredRouteListener&>(gateway));
	graph.subscribe(nodes);
	graph.subscribe(reconciler);
	nodes.subscribe(log);
	nodes.subscribe(gateway);
	nodes.subscribeNotes(log);
	nodes.subscribeDiagnostics(log); // ear.close-overdue, ear.close-late, ear.fade-unacknowledged, ear.barrier-overdue,
									 // ear.hook-slow
	nodes.subscribeLog(log);         // a stop() that threw during a teardown; each node handle's log
	nodes.subscribeInternalErrors(log);    // an adapter's bug NodeHost caught: ear.internal-error, then its node lost
	nodes.subscribeEngineDiagnostics(log); // ear.data-loop-wedged, as a record with no node
	WedgeMarker wedgeMarker(*engine);
	nodes.subscribeEngineDiagnostics(wedgeMarker); // then the engine's adapters stop calling into the loop
	nodes.subscribeCounts(counterWatch);           // where each name's counts begin and end
	counterWatch.subscribe(gateway);               // counters.changed
	nodes.subscribeCompanions(gateway);            // node.companionsChanged: a list's entries and readiness
	counterWatch.subscribeCompanions(gateway);     // and its counters' moves
	counterWatch.subscribe(
		log);                  // ear.graph-lost-cycles, ear.node-missed-cycles, ear.sink-dropped, ear.source-shortfall
	gateway.readCounts(nodes); // node.added's counters for a port-caused replace
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
		// The keeper's thread closes its inotify descriptor, which waits on the kernel, before ear's last record.
		keeper.stop();
		[[maybe_unused]] const auto unwritten = keeper.join(); // nothing was edited: nothing to write
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

	// What the counters' records still held back, so the records' sums are the totals (every node's went as it went).
	counterWatch.flush();

	// The keeper's last write runs as its thread ends; its failure reaches standard output only.
	if (const auto failed = keeper.join()) log.graphFileWriteFailed(*failed);
	// The I/O thread last, once nothing can post to it (every node gone, the keeper joined): what is queued is dropped,
	// and the job in flight gets its grace. One still running after it is said; its thread cannot be waited for, so ear
	// ends below as it does after a leak. ear.conf is written by now.
	const std::optional<std::string> ioBusy = io.stop();
	if (ioBusy)
		log.log({.level = ear::LogLevel::warn,
		         .source = "ear",
		         .text = "the I/O thread was still busy at exit: " + *ioBusy,
		         .cut = false});
	// ear's last record: what follows is destruction and the kernel's teardown of the process, ear's work no more.
	log.stopped();
	// A run leaked past its close deadline may still post to the main loop or signal an engine source from its own
	// threads, and a wedged data loop's blocks and engine companions were leaked because destroying them would wait on
	// it (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog); destroying the executor and the engine
	// under them would be a use after free or a hang. ear.conf is written by now, so end here without running
	// destructors, once the output thread's queue (the diagnostics among it) is written out: standard output is a
	// captured pipe now, so flushing std::cout would write nothing. An I/O thread still busy is the same: its destructor
	// would wait on the job for good.
	if (nodes.leaked() > 0 || ioBusy) {
		writer.spill();
		std::_Exit(0);
	}
	return 0;
}
