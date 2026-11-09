// Running a node type without PipeWire (docs/writing-a-node-type.md#testing-a-type): one real NodeHost over in-memory
// fakes, so a type's tests and NodeHost cannot disagree on any rule. NodeHarness takes any NodeType by names (the
// all-type tests get theirs from the generated list, typeless; the legacy types run through the legacy adapter);
// TypeHarness (type_harness.hpp) adds the spec's bank handles. The test's thread stands in for the main loop and the
// realtime thread both: every call pumps the harness's manual executor until nothing is queued, so each returns with
// the transition it asked for applied as far as the fakes allow. The harness does not observe a class's start() and
// stop(): their pairing is node_host_test.cpp's. Not yet: feeds, holds and runLoops, which come with the slices that
// build them (docs/design/). The sink fake runs a consumer on the test's thread: begin at configure's return, each
// chunk at once from the tap written after the fade in process(), stamped with the upstream latency the latency plan
// gives its bank (the harness has no peers), finish at the close.
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
#include "ear/core/desired_graph.hpp"
#include "ear/core/node_host.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/sink.hpp"
namespace ear::testing {

/// One chunk a sink's fake handed its consumer (on the test's thread, at once), copied.
struct SeenChunk {
	std::vector<float> interleaved;
	std::uint32_t frames;
	ChunkStamp stamp;
};

/// What the fakes made for one engine object or sink the configuration asked for, by bank, in request order.
struct CompanionFake {
	enum class Kind { soundCard, rtpReceiver, sink };
	Kind kind;
	std::string bank;
	NodeName engineName; // "<node>.playback", "<node>.capture", "<node>.rtp"; a sink's "<node>.<bank>"
	std::variant<SoundCardRequest, RtpReceiverRequest, SinkShape> request;
	bool made = false;             // an outside node: the flush made it (the block was ready); a sink: at once
	bool ready = false;            // an outside node: ready(bank) told it; a sink: its consumer began
	bool destroyed = false;        // an outside node destroyed, or a sink closed (its consumer finished)
	std::vector<SeenChunk> chunks; // a sink's, in order
};

/// One framework diagnostic the node got (NodeDiagnosticListener), in order.
struct SeenDiagnostic {
	std::string code;
	std::string detail;
	std::vector<std::pair<std::string, std::int64_t>> fields;
};

/// One legacy handoff a configuration made (its request's bank and capacity, the channels the framework gave it).
struct SeenHandoff {
	std::string bank;
	std::uint32_t channels;
	std::uint32_t capacityFrames;
};

/// How a harness runs (outside the class, so a default argument can use its member initializers).
struct HarnessOptions {
	AudioFormat format = {.rate = 48000, .maxBlock = 256};
	NodeName node = "node";
	RecordingAdapter* recordings = nullptr; // null: every recording is refused ("no recordings folder"); until 3c
	bool autoComplete = true;               // fades acknowledged and barriers completed as soon as posted
};
/// How add() adds the node.
struct AddOptions {
	bool disabled = false;
};

/// Any NodeType, by names.
class NodeHarness {
public:
	using Options = HarnessOptions;
	/// `type` and `options.recordings` outlive the harness.
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
	/// The outside node on `bank` becomes ready; std::logic_error when none was made for it.
	void ready(std::string_view bank);
	/// The outside node on `bank` is lost: the node fails engine-refused with `detail`.
	void lose(std::string_view bank, std::string detail);
	/// With autoComplete off: the fade-out posted is acknowledged; the request posted (a drive or a silence) completes.
	/// std::logic_error when none is pending.
	void acknowledgeFade();
	void completeBarrier();
	/// The fake clock moves on; timers due fire (the 100 ms, 5 s and 10 s bounds, waitingFor's 2 s).
	void advance(std::chrono::milliseconds by);
	/// One cycle of `frames` (0: maxBlock) through the code the fake block was driven with, outputs NaN-filled first,
	/// with a GraphCycle the harness advances; then the fade's gain on the faded outputs, as a real block applies it
	/// (a 5 ms ramp from each drive, and to zero before each silence); then each bound sink's tap, its stamp's
	/// upstreamFrames the most capture latency the plan gives its bank (each paired input bank's `beyond`, plus what
	/// the configuration declared on the bank), as a real block with no peers would tell.
	void process(std::uint32_t frames = 0);
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
	const std::vector<std::string>& notes() const noexcept;
	const std::vector<SeenDiagnostic>& diagnostics() const noexcept;

	// Buffers and fakes.
	/// A port's buffer (public or inside), maxBlock frames; std::logic_error for a port the node lacks.
	std::span<float> input(std::string_view port);
	std::span<const float> output(std::string_view port) const;
	/// Output samples of the last process() left unwritten (NaN), over every output port.
	std::size_t unwritten() const;
	/// The fade gain the fake block applied to `port` at the last sample of the last process() (1 when none).
	float lastFadeGain(std::string_view port) const;
	/// The latency plan the fake block was last given (PortBlock::carryLatency, with each drive; cleared at a
	/// release): the pass-through's pairs, what configure declared and what companions add. Empty before the first
	/// drive.
	const LatencyPlan& latencyPlan() const noexcept;
	const std::vector<CompanionFake>& companions() const noexcept;
	/// The newest companion on `bank`; std::logic_error when none.
	CompanionFake& companion(std::string_view bank);
	/// Legacy only: frames handed through handoff `index` to its consumer, and the run object its recipe built.
	std::uint64_t handoffFrames(std::size_t index) const;
	RunObject* runObject() const noexcept;
	/// Every legacy handoff made, in order, over every configuration; and every recording asked for (CreateOf's
	/// openRecording or a legacy acquire), in order, whether or not the folder opened it.
	const std::vector<SeenHandoff>& handoffs() const noexcept;
	const std::vector<RecordingRequest>& recordingRequests() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace ear::testing
