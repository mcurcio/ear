// Frozen interface (docs/design/batch2.md section 4; the outside node by docs/design/rtp.md section 7.1, batch2.md
// section 11's erratum; NodeNoteListener and the executor by docs/design/sendspin.md section 14, S1; the resource
// registry with one adapter per kind, NodeHealth, the two-phase start, teardown by dependency, run generations, the close
// deadline and NodeDiagnosticListener by docs/design/node-framework.md sections 6, 7.2 and 7.3, slice 1; the recording
// kind by its slice 2; LatencyPlan and PortBlock::carryLatency by its section 9.4, slice 7; DriveRequest and PortBlock's
// asynchronous drive, fadeOut, settle, takenAt and blockId by docs/design/node-lifecycle.md section 5.3, step L-2).
// Change it only through a design page and a re-cut, never a local edit.
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/desired_graph.hpp"
#include "ear/core/executor.hpp"
#include "ear/core/node_lifecycle.hpp"
#include "ear/core/timers.hpp"
namespace ear {
/// A node's health: its state and why it failed, the framework's judgement alone (docs/design/node-framework.md
/// section 4).
struct NodeHealth {
	NodeName name;
	NodeState state;
	std::optional<FailureReason> reason;
};
class NodeHealthListener {
public:
	virtual void nodeChanged(const NodeHealth& health) = 0;
	/// Everything the node made is gone. droppedFrames: frames its handoffs refused (ring full).
	virtual void nodeGone(const NodeName& node, std::uint64_t droppedFrames) = 0;

protected:
	~NodeHealthListener() = default;
};
/// What a node's run says: a sentence, never a state (InsidesEvents::say).
class NodeNoteListener {
public:
	virtual void nodeSays(const NodeName& node, const std::string& text) = 0; // main thread
protected:
	~NodeNoteListener() = default;
};
/// What the framework itself observes about a node (docs/design/node-framework.md section 8.1), by code: today only
/// "ear.close-overdue", a run that did not close within the close deadline and was leaked (I5a).
class NodeDiagnosticListener {
public:
	virtual void nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail) = 0; // main
protected:
	~NodeDiagnosticListener() = default;
};
/// How one run's latency crosses its block (docs/design/node-framework.md section 9.4): the pairs of banks it is
/// carried between (a pass-through's pairs), and what the run declared it adds on a bank (StartContext::declareLatency).
/// An output bank tells its linked ports its internal latency plus the most upstream of any input bank paired with it;
/// an input bank tells its internal latency plus the most downstream of any output bank paired with it. A bank in no
/// pair tells its internal latency alone.
struct LatencyPlan {
	std::vector<BankPair> pairs;
	std::vector<std::pair<std::string, Latency>> internal;
};
/// What an asynchronous drive hands a block (docs/design/node-lifecycle.md section 5.3): the code (null: silence) and
/// which outputs its fades touch, one flag per output in process()'s order (outputs, then inside outputs; a shorter span
/// leaves the rest unfaded). The interim sinks join it in step L-4b. All of it outlives the drive's completion or the
/// block.
struct DriveRequest {
	AudioCode* code = nullptr;
	std::span<const bool> faded;
};
/// Engine side of a port block. Destroying it removes its ports and their links; after the
/// destructor returns, no process() of any code is running or will run.
class PortBlock {
public:
	virtual ~PortBlock() = default;
	/// Main thread. Carries latency through the block by `plan` from now on, replacing the last plan, and tells the
	/// linked ports what changed. Ignored by a block that carries no latency (a test's fake).
	virtual void carryLatency(const LatencyPlan& plan) { static_cast<void>(plan); }
	/// Main thread. The realtime thread runs `code` from its next cycle; nullptr silences every
	/// output. Returns only when the realtime thread can no longer be inside a process() of the
	/// code it ran before: a blocking barrier with the block's data loop. On a block the engine
	/// refused or lost that has no data loop, nothing can run, so it returns at once; the barrier
	/// holds either way. `code` must outlive the next drive() or the block. Today's NodeHost's; step L-3a moves it
	/// onto the asynchronous drive below and removes this one.
	virtual void drive(AudioCode* code) = 0;
	/// Main thread; returns at once (docs/design/node-lifecycle.md section 5.3, D-L29). The data loop applies the
	/// request inside an invoke, between two cycles, whatever the block's state (PAUSED, its driver gone, its engine
	/// node destroyed): the code and a fade gain reset to the fade-in's start for the faded outputs together, any
	/// pending fade-out cancelled (its `done` is dropped, never run). From the next cycle no process() of the previous
	/// code runs. `done(P)` then runs on the main loop, never inside this call, with P the position the request takes
	/// effect from (the next cycle's expected position, as takenAt). A block with no data loop applies it on the main
	/// thread and still completes by signal. What `request` points to outlives `done` or the block. Requests to one
	/// block complete in the order they were made, whichever of drive and fadeOut made them.
	virtual void drive(DriveRequest request, std::move_only_function<void(std::uint64_t taken)> done) = 0;
	/// Main thread; returns at once. From the next cycle every faded output ramps linearly to zero over 5 ms (rate * 5 /
	/// 1000 frames) and stays there, the code still running; `done()` runs on the main loop once a cycle ended at zero
	/// (a cycle too large to run counts as one). A block that is not processing (not STREAMING, or no data loop)
	/// completes it by signal, never inline, and so does a block that leaves STREAMING before the fade ended.
	virtual void fadeOut(std::move_only_function<void()> done) = 0;
	/// Main thread: runs every `done` whose request the data loop already completed, as the block's completion event
	/// would. A caller calls it before judging any bound (section 5.4), so a main loop that stalled never mistakes a
	/// late dispatch for a late data loop.
	virtual void settle() = 0;
	/// Any thread: the position from which the block runs what it was last driven with (0 before any cycle ran).
	virtual std::uint64_t takenAt() const noexcept = 0;
	/// Main thread: the block's engine node's global id, read once the block was first STREAMING
	/// (pw_filter_get_node_id), so an outside node links it by id, not by name (section 5.7); before that, or on a
	/// block with no engine node, 0xFFFFFFFF (no id).
	virtual std::uint32_t blockId() const noexcept = 0;
};
class PortBlockEvents { // main thread
public:
	virtual void ready() = 0;                  // processing in the engine; at most once
	virtual void lost(std::string detail) = 0; // engine refused or dropped the block
protected:
	~PortBlockEvents() = default;
};
class PortBlockFactory {
public:
	/// Main thread. One engine node named `node` with one 32-bit float mono port per name in
	/// `ports` (public and inside), in EAR's graph group, always processing, silent until drive().
	/// Its public ports' levels are published through the factory's LevelReader. Refusal is told
	/// via events.lost later, never thrown or told inside this call. events outlives the block.
	virtual std::unique_ptr<PortBlock> make(const NodeName& node, const Ports& ports, const AudioFormat& format,
	                                        PortBlockEvents& events) = 0;

protected:
	~PortBlockFactory() = default;
};
class Handoff {
public:
	virtual ~Handoff() = default; // main thread; only after `closed` ran, or before any write
	virtual RtWriter& writer() noexcept = 0;
	/// Main thread, once, after the writing port block can no longer run the code that writes: drains the rest to the
	/// consumer, calls consumer.finish() on the handoff thread, then `closed` on the main thread.
	virtual void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) = 0;
};
/// The handoff kind's adapter.
class HandoffAdapter {
public:
	/// Main thread; starts the handoff's own ordinary thread.
	virtual std::unique_ptr<Handoff> make(const NodeName& node, std::uint32_t channels, std::uint32_t capacityFrames,
	                                      HandoffConsumer& consumer) = 0;

protected:
	~HandoffAdapter() = default;
};
/// An outside node: a PipeWire node a run asked for (a sound card or an RTP receiver), plus the links joining its
/// channels to the node's inside ports. Destroying it (main thread) removes both, in any state, and tells nothing.
class OutsideNode {
public:
	virtual ~OutsideNode() = default;
};
/// One outside node's lifecycle, the same for every kind (main thread).
class OutsideNodeEvents {
public:
	/// The node runs (a card's PCM runs; a receiver's socket is bound) and every channel is
	/// linked to its block port; at most once.
	virtual void ready() = 0;
	/// At most once, before or after ready, never inside make(). When it is told, the outside
	/// node's engine node and links are already destroyed, so it no longer clocks or holds any
	/// part of the graph.
	virtual void lost(std::string detail) = 0;

protected:
	~OutsideNodeEvents() = default;
};
/// Where an outside node goes, worked out by the framework from the request's bank: the one engine name it is made
/// with ("<node>.<suffix>", I9), the block it joins, and the block's ports it joins, channel i to blockPorts[i].
struct OutsideTarget {
	NodeName engineNode;
	NodeName blockNode;
	std::vector<PortName> blockPorts;
	AudioFormat format;
};
/// One outside kind's adapter.
///  - SoundCardRequest: one engine node for request.card with blockPorts.size() channels at format.rate. It joins the
///    graph's clock only through its links to the block, made once its PCM runs.
///  - RtpReceiverRequest: one engine node receiving the stream the request names, with blockPorts.size() channels. It
///    follows the graph's clock and never drives it; its channels are linked once it runs (its socket is bound).
///    Packets that stop or never come change nothing: it stays ready and its channels are silent.
template <typename Request> class OutsideAdapter {
public:
	/// Main thread; only once port block `target.blockNode` is ready (its ports exist).
	virtual std::unique_ptr<OutsideNode> make(const OutsideTarget& target, const Request& request,
	                                          OutsideNodeEvents& events) = 0;

protected:
	~OutsideAdapter() = default;
};
using SoundCardAdapter = OutsideAdapter<SoundCardRequest>;
using RtpReceiverAdapter = OutsideAdapter<RtpReceiverRequest>;
/// One open recording file (RecordingRequest). Destroying it (main thread) closes it.
class RecordingFile {
public:
	virtual ~RecordingFile() = default;
	virtual int descriptor() const noexcept = 0;
};
/// The recording kind's adapter: the recordings folder, held open from boot.
class RecordingAdapter {
public:
	/// Main thread. `path` created or truncated beneath the folder only; the error is why not (it names no path).
	virtual std::expected<std::unique_ptr<RecordingFile>, std::string> open(const NodeName& node,
	                                                                        const std::string& path) = 0;

protected:
	~RecordingAdapter() = default;
};
/// The resource registry (docs/design/node-framework.md section 7.3): one adapter per kind of ResourceRequest, so a new
/// kind is one adapter here and nothing more in NodeHost's constructor.
class ResourceRegistry {
public:
	/// All outlive the registry.
	ResourceRegistry(SoundCardAdapter& cards, RtpReceiverAdapter& receivers, HandoffAdapter& handoffs,
	                 RecordingAdapter& recordings) noexcept
		: cards_(cards), receivers_(receivers), handoffs_(handoffs), recordings_(&recordings) {}
	/// With no recordings folder: every recording is refused ("no recordings folder"). For hosts that run no "file"
	/// node (tests).
	ResourceRegistry(SoundCardAdapter& cards, RtpReceiverAdapter& receivers, HandoffAdapter& handoffs) noexcept;
	SoundCardAdapter& adapterFor(const SoundCardRequest&) const noexcept { return cards_; }
	RtpReceiverAdapter& adapterFor(const RtpReceiverRequest&) const noexcept { return receivers_; }
	HandoffAdapter& adapterFor(const HandoffRequest&) const noexcept { return handoffs_; }
	RecordingAdapter& adapterFor(const RecordingRequest&) const noexcept { return *recordings_; }

private:
	SoundCardAdapter& cards_;
	RtpReceiverAdapter& receivers_;
	HandoffAdapter& handoffs_;
	RecordingAdapter* recordings_;
};
/// How long a run's teardown may wait on other threads (I5 steps 3 and 4) before the framework stops waiting (I5a).
inline constexpr std::chrono::milliseconds defaultCloseDeadline{10'000};
class NodeHost final : public DesiredNodeListener {
public:
	/// All outlive it. `mainLoop` runs on the thread that calls everything below; a run reaches it through there (its
	/// events, its handoffs' and run object's `closed`). `timers` run there too.
	NodeHost(AudioFormat format, PortBlockFactory& blocks, ResourceRegistry& resources, Executor& mainLoop,
	         Timers& timers, std::chrono::milliseconds closeDeadline = defaultCloseDeadline);
	~NodeHost(); // precondition: stopAll completed (or no node was ever added)
	void subscribe(NodeHealthListener& listener);
	void subscribeNotes(NodeNoteListener& listener);
	void subscribeDiagnostics(NodeDiagnosticListener& listener);
	/// starting; port block made; then a run starts (docs/design/node-framework.md section 7.2): config.start(context)
	/// acquires only; code.prepare; block.drive(&code) at once; the outside nodes the run asked for are made once the
	/// block is ready, in request order, named "<node>.<suffix>" ("playback" / "capture" for cards, "rtp" for the
	/// receiver); once every resource is ready the recipe builds the run object; the run is ready when the recipe
	/// returned (at once with no outside node). A failure fails the node; its port block stays.
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// Restart (I5, I7): the barrier drive(nullptr), release(); outside nodes destroyed; handoffs closed; the run object
	/// closed; then, on the main loop, the code destroyed, then the run object, the recipe and the config; then a new
	/// run from `config` as in nodeAdded. The port block and routes stay. An update arriving during a restart replaces
	/// the config to start. If the block was lost: the old run is torn down, nothing starts, the node stays failed.
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// stopping; port block destroyed (the barrier); then as the restart's teardown; nodeGone. Others untouched.
	void nodeRemoved(const NodeName& node) override;
	/// `closed` proves the graph takes no more edits, so no nodeAdded, nodeUpdated or nodeRemoved
	/// can follow (one would throw std::logic_error: a wiring bug). Every node: stopping; torn
	/// down; gone. `allGone` runs once on the main thread after the last (at once if none).
	void stopAll(GraphClosed closed, std::move_only_function<void()> allGone);
	/// Nodes not gone, in arrival order (a removed node still stopping comes before a newer node
	/// of the same name).
	std::vector<NodeHealth> health() const;
	/// Runs left running past the close deadline (I5a). Their threads may still post to `mainLoop` or touch engine
	/// objects at any time, so once any was leaked the process must end without destroying those (main.cpp ends with
	/// std::_Exit).
	std::size_t leakedRuns() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear
