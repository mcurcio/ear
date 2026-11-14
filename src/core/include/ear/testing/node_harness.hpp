// Running a node type without PipeWire (docs/writing-a-node-type.md#testing-a-type): one real NodeHost over in-memory
// fakes, so a type's tests and NodeHost cannot disagree on any rule. NodeHarness takes any NodeType by names (the
// all-type tests get theirs from the generated list, typeless; the legacy types run through the legacy adapter);
// TypeHarness (type_harness.hpp) adds the spec's bank handles. The test's thread stands in for the main loop and the
// realtime thread both: every call pumps the harness's manual executor until nothing is queued, so each returns with
// the transition it asked for applied as far as the fakes allow. The harness does not observe a class's start() and
// stop(): their pairing is node_host_test.cpp's. Not yet: holds, which come with the slice that builds them
// (docs/design/). The store fake is an in-memory disk under the real store policy, its jobs on an I/O executor run on the
// test's thread as each call settles (or, with holdIo, at runIo()). The fake clock (advance) runs every main-loop timer NodeHost arms, each companion's readiness
// deadline included (docs/engine.md#readiness-deadlines), and every loop's timer. The loop fake runs each of the node's
// loops on the test's thread (docs/writing-a-node-type.md#loops): a queue and the fake clock's timers per loop, its
// kind kept (a strand's handlers run with StrandScope, so onStrand() judges them); started when the framework asks (at
// once, or, with holdLoops, at loopsReady()), its work run only by runLoops(), its timers fired only by advance(), its
// watches by runLoops() once rung (a source's clock wake) or readable(fd); a teardown turns each loop until it finished
// and closed, and a source a state left armed fails the call that tore it down (std::logic_error naming the loop)
// unless allowSourcesLeft. Where a fake acts as the engine does it runs the engine's own rule: the block's fade and
// stamps (ear/core/block_rules.hpp), its cycle facts (CycleAccount, over the harness's own cut record: loseCycles), its
// latency (workOut, with no peers), and an engine companion's bank as NodeHost names it. The sink fake is core's own
// SinkRing (every rule of a sink's rings) with its consumer on the test's thread: begin at configure's return (or, with
// holdSinks, at ready(bank)), each chunk drained at once from the tap written after the fade in process(), in two calls
// where it wraps the ring's end (a ring of sinkRingFrames, or the type's own size), finish at the close. The source
// fake is core's own SourceRing (every source rule, by the block's own SourceRing::play, with the type's clock if it
// gave one), its writer never waiting: the test feeds it (feed), process() plays it into its bank before the code, and
// the test reads what its last tick did (lastPlayed, exactly what a clock was told) and its shortfall.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "ear/core/companions.hpp"
#include "ear/core/cycle_counters.hpp"
#include "ear/core/cycle_facts.hpp"
#include "ear/core/desired_graph.hpp"
#include "ear/core/log.hpp"
#include "ear/core/loop.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/sink.hpp"
#include "ear/core/source.hpp"
namespace ear::testing {

/// One call a sink's fake made of its consumer's consume (on the test's thread, at once), copied, with the stamp it
/// carried: a chunk split at the ring's end is two, the second's stamp advanced (docs/engine.md#sinks).
struct SeenChunk {
	std::vector<float> interleaved;
	std::uint32_t frames;
	ChunkStamp stamp;
};

/// What the fakes made for one companion the configuration asked for (an engine companion, a sink or a source), by
/// bank, in request order.
struct CompanionFake {
	CompanionKind kind;
	std::string bank;
	NodeName name; // "<node>.playback", "<node>.capture", "<node>.rtp"; a sink's or source's "<node>.<bank>"
	std::variant<SoundCardShape, RtpReceiverShape, SinkShape, SourceShape> shape;
	bool made = false;      // an engine companion: the flush made it (the block was ready); a sink, a source: at once
	bool ready = false;     // an engine companion: ready(bank) told it; a sink: its consumer began; a source: at once
	bool destroyed = false; // an engine companion destroyed, a sink closed (its consumer finished), a source freed
	std::vector<SeenChunk> chunks; // a sink's, in order
};

/// One framework diagnostic the node got (NodeDiagnosticListener), in order.
struct SeenDiagnostic {
	std::string code;
	std::string detail;
	std::vector<std::pair<std::string, std::int64_t>> fields;
};

/// One bug of an adapter's NodeHost caught and told (InternalErrorListener), before its node failed lost, in order.
struct SeenInternalError {
	std::string callback;
	std::string node;
	std::string where;
	std::string what;
};

/// One legacy handoff a configuration made (its request's bank and capacity, the channels the framework gave it).
struct SeenHandoff {
	std::string bank;
	std::uint32_t channels;
	std::uint32_t capacityFrames;
};

/// How a lost cycle was lost, for NodeHarness::loseCycles (docs/writing-a-node-type.md#lost-cycles).
enum class Lost : std::uint8_t {
	cut,          // the driver cut it short: a stall of the graph
	notScheduled, // the driver ran it whole without the block: the block was paused
};

/// How a harness runs (outside the class, so a default argument can use its member initializers).
struct HarnessOptions {
	AudioFormat format = {.rate = SampleRate::fixed<48000>(), .maxBlock = 256};
	NodeName node = "node";
	/// A host with no recordings folder: every recording is refused ("cannot open <path>: no recordings folder").
	bool noRecordingsFolder = false;
	bool autoComplete = true; // fades acknowledged and barriers completed as soon as posted
	bool holdSinks = false;   // each sink's consumer begins only at ready(bank), not at configure's return
	/// Each sink's ring in frames, in place of the size the type asked for (rounded up to a power of two, as the
	/// engine's is): a ring that a test's cycles do not divide makes the sink hand chunks split at its end, as the
	/// engine's does (docs/engine.md#sinks).
	std::optional<std::uint32_t> sinkRingFrames;
	/// Each loop starts only at loopsReady(), not when the framework asks.
	bool holdLoops = false;
	/// Every loop the framework asks for cannot start, with this as the adapter's reason.
	std::optional<std::string> refuseLoops{};
	/// A source a loop's state left armed is only said (ear.loop-source-left), not a std::logic_error.
	bool allowSourcesLeft = false;
	/// The I/O executor's jobs (a store's open and writes, a recording's open and close) run only at runIo(), not as
	/// each call settles.
	bool holdIo = false;
};
/// How add() adds the node.
struct AddOptions {
	bool disabled = false;
};

/// Any NodeType, by names.
class NodeHarness {
public:
	using Options = HarnessOptions;
	/// `type` outlives the harness.
	explicit NodeHarness(const NodeType& type, Options options = {});
	/// Removes a node left in the graph and runs its teardown to the end (completing what autoComplete left), so no
	/// thread of a sink or a legacy run outlives the harness.
	~NodeHarness();
	NodeHarness(const NodeHarness&) = delete;
	NodeHarness& operator=(const NodeHarness&) = delete;

	// Settings.
	/// The type's parse: nothing, or the refusal's whole text. The settings add() and change() use.
	std::optional<std::string> parse(const NodeSettings& settings);
	/// The parsed node's ports, inside banks included; std::logic_error before a parse that passed.
	const Ports& ports() const;

	// Operations and the lifecycle's steps (docs/architecture.md#phases-states-and-what-is-wanted), one call per
	// transition.
	using AddOptions = ear::testing::AddOptions;
	/// T1 and, once the fake block is made, T2: NodeHost::nodeAdded; the instance made, configure called, readiness as
	/// far as the fakes allow. {.disabled = true}: T19, nothing configured. std::logic_error after no or a refused parse.
	void add(AddOptions options = {});
	/// The fake block's PortBlockEvents::ready: T5 follows when wanted running (the drive, then start()).
	void blockReady();
	/// The fake block's PortBlockEvents::lost: T13.
	void blockLost(std::string detail);
	/// The newest companion on `bank` becomes ready: an engine companion is told ready; a sink held by holdSinks has
	/// its consumer begin; a sink that began, or a source (ready when made), is ready already. std::logic_error when
	/// no companion was made on `bank`, or the one made is no longer alive.
	void ready(std::string_view bank);
	/// The engine companion on `bank` is lost: the node fails engine-refused with `detail`. std::logic_error for a sink
	/// or a source, which the framework never loses (their type's code says lost through the node's handle).
	void lose(std::string_view bank, std::string detail);
	/// With autoComplete off: every fade-out waiting is acknowledged (its faded outputs at zero at once, as a block not
	/// processing acknowledges them); the oldest request posted (a drive or a silence) completes. std::logic_error when
	/// none waits: a cycle that ended a fade-out at zero acknowledged it, and a drive applied while it waited cancelled
	/// it (its `done` never ran), both by the real block's rule (BlockFade).
	void acknowledgeFade();
	void completeBarrier();
	/// The fake clock moves on; timers due fire, in time order (the 100 ms, 5 s and 10 s bounds, waitingFor's 2 s, each
	/// companion's readiness deadline: a card's 10 s, an RTP receiver's and a sink's consumer's 5 s; and each loop's
	/// timers, on its loop, an `every` once for each tick the move passes: a handler takes no time on the fake clock).
	void advance(std::chrono::milliseconds by);
	/// Every loop runs what was posted to it, and each watch rung (a wake rung, a descriptor made readable()), and what
	/// that posts, until all are idle; never a timer (advance's). Its handlers' notes and losses land on the main loop
	/// meanwhile.
	void runLoops();
	/// With holdLoops: every loop asked for and not yet started starts (its adapter tells it ready).
	void loopsReady();
	/// Every watch of `fd` (onReadable) runs on the next runLoops().
	void readable(int fd);
	/// The sources armed on the newest loop named `loop` whose handles still live (none: 0).
	std::size_t armed(std::string_view loop) const;
	/// Each loop the framework made, oldest first: its name, whether it closed to others, finished and closed.
	struct SeenLoop {
		std::string name;
		LoopKind kind;
		bool closedToOthers = false;
		bool finished = false;
		bool closed = false;
	};
	std::vector<SeenLoop> loops() const;

	// The stores (docs/writing-a-node-type.md#stores), on an in-memory disk by file name ("<type>-<node>.<suffix>"),
	// opened and written by jobs on the harness's I/O executor, which runs on the test's thread.
	/// With holdIo: every job queued runs, and what each posts, in order.
	void runIo();
	/// Every I/O job run so far, by the name it was posted with ("open store <file>", "write store <file>", "release
	/// store <file>": a dropped open's handle released on the I/O executor; "open recording <path>", "close recording
	/// <path>", "release recording <path>"), in order.
	const std::vector<std::string>& ioJobs() const noexcept;
	/// The file of store `suffix` of the harness's node.
	std::string storeFile(std::string_view suffix) const;
	/// The file holds `text` with `mode` before the node opens it.
	void presetStore(std::string_view suffix, std::string text, std::uint32_t mode = 0600);
	/// What the file holds now (nothing: no file), and its mode (std::logic_error when it has none).
	std::optional<std::string> storeText(std::string_view suffix) const;
	std::uint32_t storeMode(std::string_view suffix) const;
	/// Every open's read of the file is refused with "<file>: <why>"; the next write of it likewise.
	void failStoreOpen(std::string_view suffix, std::string why);
	void failNextWrite(std::string_view suffix, std::string why);
	/// From now on every call of the stores' adapter throws std::logic_error(what): an adapter's bug.
	void breakStores(std::string what);
	/// The next blocking replace's wait gives up (its 5 s passed): `ran` true, after its job ran (it was under way, and
	/// landed); false, before it began (the job is left queued for the I/O executor, where it then writes nothing).
	void overdueNextWrite(bool ran);
	/// Every call the adapter was asked, in order: "readNarrowed <file>", "read <file>", "narrow <file>", "replace
	/// <file>".
	const std::vector<std::string>& storeCalls() const noexcept;
	// The recordings (docs/writing-a-node-type.md#recordings), in memory by path ("./a.wav" is "a.wav"), each a real
	// descriptor (a memfd, so libsndfile seeks and writes it), opened and closed by jobs on the harness's I/O executor.
	/// What the file at `path` holds now (nothing: never made).
	std::optional<std::string> recordingBytes(std::string_view path) const;
	/// The file at `path` holds `bytes` before the node opens it (which truncates it).
	void presetRecording(std::string_view path, std::string bytes);
	/// Every open of `path` is refused with `why` (the folder's reason: "cannot open <path>: <why>").
	void failRecordingOpen(std::string_view path, std::string why);
	/// Every open of `path` gives the write end of a pipe: made, but not a file libsndfile can write a WAV to.
	void pipeRecording(std::string_view path);
	/// From now on every open throws std::logic_error(what): an adapter's bug.
	void breakRecordings(std::string what);
	/// How many recordings' descriptors are open now (made by an open, not yet closed).
	std::size_t openRecordings() const;
	/// One cycle of `frames` (0: maxBlock) through the code the fake block was driven with, outputs NaN-filled first,
	/// with a GraphCycle the harness advances on its driver, its facts (GraphCycle::facts, and facts()) stated by the
	/// real block's rule,
	/// CycleAccount (a discontinuity on the first cycle, each drive's first, and wherever loseCycles, changeDriver or
	/// reposition made one), each bound source first played into its bank's input buffers (its SourceTick's
	/// playback the latency workOut gives its bank under the plan, with no peers); then the fade on the
	/// faded outputs by the real block's BlockFade (in over 5 ms from each drive; a fade-out ramps down from the next
	/// cycle and, once a cycle ended it at zero, is acknowledged); then each bound sink's tap, its stamp's
	/// upstreamFrames the most capture latency workOut gives its bank, as a real block with no peers tells
	/// (docs/engine.md#latency-per-bank-pair).
	void process(std::uint32_t frames = 0);
	/// `count` graph cycles start without the fake block running them: the next process() comes after them, its
	/// position and time moved on by their frames (that cycle's). `Lost::cut` marks each cut short in the harness's
	/// CutRecord, `Lost::notScheduled` leaves them whole; the next process() states them: on the harness's own clock
	/// (numbered), catchUp, or absent when any was not cut; after changeDriver (unnumbered) always absent, since nothing
	/// is confirmed off the numbered clock. Calls add up until that process(); std::logic_error past 2^32 - 1.
	void loseCycles(std::uint32_t count, Lost how = Lost::cut);
	/// The next process() and those after it run on driver `id` (a card made driver): driverChange, and no graph cycle
	/// numbers but on the harness's own clock (id 0, where it starts).
	void changeDriver(std::uint32_t id);
	/// The next process()'s position moves by `frames` on the same driver with no cycle missed and no time gone:
	/// reposition on the harness's own clock, whose numbers say no cycle was missed. After changeDriver (unnumbered) a
	/// forward move by whole cycles reads as that many missed (absent), any other move as reposition.
	/// std::logic_error for a move before the driver's first frame.
	void reposition(std::int64_t frames);
	/// The CutRecord the fake block's account reads.
	const CutRecord& cuts() const noexcept;
	/// The facts of the last process()'s cycle, as its code's GraphCycle carried them (none before the first).
	const CycleFacts& facts() const noexcept;
	/// The node's counters (docs/engine.md#counters), as NodeHost::counts gives them: its fake block's, counted by the
	/// real block's rule each process() (missed cycles not cut, overruns of the work process() times, repositions,
	/// driver changes); std::logic_error when the node is gone or was never added.
	NodeCounts counts() const;
	/// The graph's counters of the harness's own cut record: cycles started (each process() and each lost cycle), the
	/// cut ones (loseCycles with Lost::cut) and the stalls they made (one per loseCycles run of cuts).
	GraphCounts graphCounts() const noexcept;
	/// A committed change of settings (parse, then NodeHost::nodeChanged with outcome restart and the changed keys, or
	/// stored on a disabled node). std::logic_error for settings the type refuses.
	NodeOutcome change(const NodeSettings& settings);
	LifecycleReply stop();    // NodeOperations::stop
	LifecycleReply start();   // NodeOperations::start
	LifecycleReply restart(); // NodeOperations::restart
	void disable();           // nodeChanged, outcome disable (spec.disabled true)
	void enable();            // nodeChanged, outcome enable
	void remove();            // nodeRemoved

	// What the node shows.
	NodeHealth health() const;           // as last told; std::logic_error before anything was told
	std::string_view phase() const;      // phaseName of the last told health
	bool gone() const noexcept;          // nodeGone was told
	std::uint64_t droppedFrames() const; // nodeGone's count
	/// The node's companion list as NodeHost lists it for a client (NodeHost::companionsOf): its configuration's
	/// companions in request order, each ready or not, a sink's `dropped` and a source's `shortfall` read now.
	std::vector<CompanionView> companionList() const;
	/// Every note the node said, as the main loop heard it (after the note rate: docs/writing-a-node-type.md#the-node-handle).
	const std::vector<std::string>& notes() const noexcept;
	/// Every log entry attributed to the node (its handle's log, and the framework's own), in the order logged; a copy,
	/// as a type's own thread may log while it is read.
	std::vector<LogEntry> logs() const;
	const std::vector<SeenDiagnostic>& diagnostics() const noexcept;
	/// Every adapter bug told about the node (ear.internal-error's record), in order.
	const std::vector<SeenInternalError>& internalErrors() const noexcept;

	// Buffers and fakes.
	/// A port's buffer (public or inside), maxBlock frames; std::logic_error for a port the node lacks.
	std::span<float> input(std::string_view port);
	std::span<const float> output(std::string_view port) const;
	/// Output samples of the last process() left unwritten (NaN), over every output port.
	std::size_t unwritten() const;
	/// The fade gain the fake block applied to `port` at the last sample of the last process() (1 when none).
	float lastFadeGain(std::string_view port) const;
	/// The latency plan the fake block was last given (PortBlock::carryLatency, with each drive; cleared at a
	/// release), by role: the pass-through's pairs, what configure declared (onPair, onBank) and what companions add
	/// (beyond). Empty before the first drive.
	const LatencyPlan& latencyPlan() const noexcept;
	const std::vector<CompanionFake>& companions() const noexcept;
	/// The newest companion on `bank`; std::logic_error when none.
	CompanionFake& companion(std::string_view bank);
	/// The newest source on `bank` (std::logic_error when none): its writer takes what fits of `interleaved` (whole
	/// frames of the bank's channels) at once, never waiting; returns the frames taken. lastPlayed and shortfall read
	/// it: what the last process() did with its ring (all zero before the first), as SourceRing::play told its clock.
	std::uint32_t feed(std::string_view bank, std::span<const float> interleaved);
	SourcePlayed lastPlayed(std::string_view bank) const;
	std::uint64_t shortfall(std::string_view bank) const;
	/// Legacy only: frames handed through handoff `index` to its consumer, and the run object its recipe built.
	std::uint64_t handoffFrames(std::size_t index) const;
	RunObject* runObject() const noexcept;
	/// Every legacy handoff made, in order, over every configuration; and every recording asked for (CreateOf's
	/// recording), in order, whether or not the folder opened it.
	const std::vector<SeenHandoff>& handoffs() const noexcept;
	const std::vector<RecordingRequest>& recordingRequests() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace ear::testing
