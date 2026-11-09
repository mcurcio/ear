// Frozen interface (docs/design/batch2.md section 4; the outside node by docs/design/rtp.md section 7.1, batch2.md
// section 11's erratum; NodeNoteListener and the executor by docs/design/sendspin.md section 14, S1; the resource
// registry with one adapter per kind, NodeHealth, the two-phase start, teardown by dependency, run generations, the close
// deadline and NodeDiagnosticListener by docs/design/node-framework.md sections 6, 7.2 and 7.3, slice 1; the recording
// kind by its slice 2; LatencyPlan and PortBlock::carryLatency by its section 9.4, slice 7; DriveRequest and PortBlock's
// asynchronous drive, fadeOut, settle, takenAt and blockId by docs/design/node-lifecycle.md section 5.3, step L-2; the
// blocking drive removed and NodeHost on the lifecycle machines by its step L-3a; PipeWire's object changes batched per
// turn, T1's wait for an older block and OutsideTarget::blockId by its step L-3b; the fade before every planned silence,
// the fade's and the barrier's bounds, the wedged data loop, EngineDiagnosticListener, structured diagnostic fields and
// leaked() by its step L-3c; NodeHealth's lifecycle members, NodeOperations, the superseded node's listeners and the
// plan's outcomes honoured by its step L-3d). Change it only through a design page and a re-cut, never a local edit.
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
/// What a node still waits for, once its phase has lasted 2 s (control-plane.md 4.8; 3a's four kinds, D-L28):
/// an older node of its name still closing (`name` its name), the engine's block (`block`), a companion (`name`: an
/// outside node's engine name, `<node>.<suffix>`), a sink's consumer (`consumer`, from step L-4b).
struct WaitingFor {
	enum class Kind { olderNode, block, companion, consumer };
	Kind kind;
	std::string name{}; // olderNode and companion only
	friend bool operator==(const WaitingFor&, const WaitingFor&) = default;
};
std::string_view waitingKindName(WaitingFor::Kind kind) noexcept; // "older-node" | "block" | "companion" | "consumer"
/// A node's health, the framework's judgement alone (docs/design/node-framework.md sections 4 and 6.2;
/// docs/design/node-lifecycle.md 4.3 derives it, control-plane.md 4.8 puts it on the wire): its state (the phase's
/// colour), why it failed (while failed), its exact phase, what the graph wants of it, why it is stopping or
/// configuring again, what it still waits for (absent until its phase has lasted 2 s, then changed at most once a
/// second, D-L42), and whether its block is lost. `since` is each listener's, stamped when it is told (D-L22).
struct NodeHealth {
	NodeName name;
	NodeState state = NodeState::starting;
	std::optional<FailureReason> reason{};
	NodePhase phase = NodePhase::added;
	Wanted wanted = Wanted::running;
	std::optional<NodeCause> cause{};
	std::vector<WaitingFor> waitingFor{};
	bool blockLost = false;
	/// An older node of the name, still closing after a newer node of that name was added: told only to listeners that
	/// hear a superseded node (NodeHealthListener::hearsSuperseded), first with the health it has when superseded.
	bool superseded = false;
	friend bool operator==(const NodeHealth&, const NodeHealth&) = default;
};
/// Told after every machine transition that changes any member of a node's health (node-lifecycle.md 5.2, D-L34), so
/// a turn that walks several phases tells each, in order.
class NodeHealthListener {
public:
	virtual void nodeChanged(const NodeHealth& health) = 0;
	/// Everything the node made is gone (its `removed`). droppedFrames: frames its handoffs refused (ring full).
	virtual void nodeGone(const NodeName& node, std::uint64_t droppedFrames) = 0;
	/// Whether this listener hears a superseded node: an older node of a name still closing once a newer node of that
	/// name was added (another type, or a name added back by a later commit). The wire keys health by name and holds
	/// the newer node, so the gateway does not; standard output does (node-lifecycle.md 5.7, D-L36). The older node of
	/// a port-caused replace tells nobody: its successor carries one health stream for the name.
	virtual bool hearsSuperseded() const noexcept { return true; }
	/// A superseded node is gone (told only to listeners that hear one). The default tells nodeGone.
	virtual void supersededGone(const NodeName& node, std::uint64_t droppedFrames) { nodeGone(node, droppedFrames); }

protected:
	~NodeHealthListener() = default;
};
/// The operator's requests that edit nothing: a node's stop (held in memory, never written), its start, and a restart
/// (docs/design/node-lifecycle.md 5.2 and 4.4; control-plane.md 4.2 and 7.6). Main thread, never inside another of
/// NodeHost's calls. Each reply comes in the call, after every health change it caused was told; core names no wire
/// code (D-L35).
class NodeOperations {
public:
	virtual LifecycleReply start(const NodeName& node) = 0;
	virtual LifecycleReply stop(const NodeName& node) = 0;
	virtual LifecycleReply restart(const NodeName& node) = 0;

protected:
	~NodeOperations() = default;
};
/// What a node's run says: a sentence, never a state (InsidesEvents::say).
class NodeNoteListener {
public:
	virtual void nodeSays(const NodeName& node, const std::string& text) = 0; // main thread
protected:
	~NodeNoteListener() = default;
};
/// One structured field a framework diagnostic carries beside its detail (control-plane.md 4.6's Fields column): a
/// key (a string literal) and a whole number.
struct DiagnosticField {
	std::string_view key;
	std::int64_t value;
	friend bool operator==(const DiagnosticField&, const DiagnosticField&) = default;
};
/// What the framework itself observes about a node (docs/design/node-framework.md section 8.1; control-plane.md 4.6),
/// by code: "ear.close-overdue" (a configuration that did not close within the close deadline, leaked: I5a; no
/// field), "ear.fade-unacknowledged" (a planned silence's fade not acknowledged within 100 ms, so the barrier was taken
/// unfaded; `ms`) and "ear.barrier-overdue" (a barrier not completed within 5 s: the node's configuration and block
/// are leaked; `ms`). `fields` lives only for the call.
class NodeDiagnosticListener {
public:
	virtual void nodeDiagnostic(const NodeName& node, std::string_view code, const std::string& detail,
	                            std::span<const DiagnosticField> fields) = 0; // main thread
protected:
	~NodeDiagnosticListener() = default;
};
/// A framework diagnostic that names no node (docs/design/node-lifecycle.md 5.4, D-L44; control-plane.md 4.6): in 3a
/// "ear.data-loop-wedged", once, when a barrier's invoke has not run within 5 s (its detail names the node whose
/// barrier tripped it; `ms` the time it waited).
struct EngineDiagnostic {
	std::string_view code;
	std::string detail;
	std::vector<DiagnosticField> fields;
};
class EngineDiagnosticListener {
public:
	virtual void engineDiagnostic(const EngineDiagnostic& diagnostic) = 0; // main thread
protected:
	~EngineDiagnosticListener() = default;
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
/// Engine side of a port block. Silent until a drive applies a code. Destroying it removes its ports and their links;
/// after the destructor returns, no process() of any code is running or will run, and no `done` of its requests runs.
class PortBlock {
public:
	virtual ~PortBlock() = default;
	/// Main thread. Carries latency through the block by `plan` from now on, replacing the last plan, and tells the
	/// linked ports what changed. Ignored by a block that carries no latency (a test's fake).
	virtual void carryLatency(const LatencyPlan& plan) { static_cast<void>(plan); }
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
	/// `ports` (public and inside), in EAR's graph group, always processing, silent until a drive.
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
	/// part of the graph. Destroying the OutsideNode inside lost() is allowed (not inside ready()):
	/// the adapter touches nothing of it after telling (NodeHost tears a failed configuration
	/// down in the same call, docs/design/node-lifecycle.md section 4.2's T14).
	virtual void lost(std::string detail) = 0;

protected:
	~OutsideNodeEvents() = default;
};
/// Where an outside node goes, worked out by the framework from the request's bank: the one engine name it is made
/// with ("<node>.<suffix>", I9), the block it joins (its name, and its engine node's global id, PortBlock::blockId, which
/// its links name: docs/design/node-lifecycle.md section 5.7), and the block's ports it joins, channel i to
/// blockPorts[i].
struct OutsideTarget {
	NodeName engineNode;
	NodeName blockNode;
	std::vector<PortName> blockPorts;
	AudioFormat format;
	std::uint32_t blockId = 0xFFFFFFFFU; // no id: the join cannot link and the outside node is lost
};
/// One outside kind's adapter.
///  - SoundCardRequest: one engine node for request.card with blockPorts.size() channels at format.rate. It joins the
///    graph's clock only through its links to the block, made once its PCM runs.
///  - RtpReceiverRequest: one engine node receiving the stream the request names, with blockPorts.size() channels. It
///    follows the graph's clock and never drives it; its channels are linked once it runs (its socket is bound).
///    Packets that stop or never come change nothing: it stays ready and its channels are silent.
template <typename Request> class OutsideAdapter {
public:
	/// Main thread; only once port block `target.blockNode` is ready (its ports exist, and target.blockId is its id).
	/// NodeHost calls it inside its per-turn flush (docs/design/node-lifecycle.md section 5.5): a ready() told inside
	/// it is heard once it returned.
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
/// How long a planned silence's fade-out may go unacknowledged before the barrier is taken unfaded
/// (docs/design/node-lifecycle.md 5.4; ear.fade-unacknowledged). It judges one block, never the data loop.
inline constexpr std::chrono::milliseconds fadeAcknowledgeBound{100};
/// How long a barrier (a drive or a silence, applied inside its invoke) may go uncompleted before the data loop is
/// judged wedged (5.4, D-L30; ear.barrier-overdue, ear.data-loop-wedged).
inline constexpr std::chrono::milliseconds barrierCompletionBound{5'000};
/// How long a phase lasts before its waitingFor reaches health, and how often it may change after (control-plane.md
/// 4.8, node-lifecycle.md 4.5, D-L42).
inline constexpr std::chrono::milliseconds waitingForDelay{2'000};
inline constexpr std::chrono::milliseconds waitingForPace{1'000};
class NodeHost final : public DesiredNodeListener, public NodeOperations {
public:
	/// All outlive it. `mainLoop` runs on the thread that calls everything below; a run reaches it through there (its
	/// events, its handoffs' and run object's `closed`). `timers` run there too.
	NodeHost(AudioFormat format, PortBlockFactory& blocks, ResourceRegistry& resources, Executor& mainLoop,
	         Timers& timers, std::chrono::milliseconds closeDeadline = defaultCloseDeadline);
	~NodeHost(); // precondition: stopAll completed (or no node was ever added)
	void subscribe(NodeHealthListener& listener);
	void subscribeNotes(NodeNoteListener& listener);
	void subscribeDiagnostics(NodeDiagnosticListener& listener);
	void subscribeEngineDiagnostics(EngineDiagnosticListener& listener);
	/// starting; port block made (in the turn's flush, once no older block of the name exists); then a configuration (docs/design/node-lifecycle.md section 4; through the legacy
	/// adapter, its section 5.10): config.start(context) acquires only (a recipe beside an outside node is a
	/// std::logic_error, D-L40); the outside nodes it asked for are made once the block is ready, in request order,
	/// named "<node>.<suffix>" ("playback" / "capture" for cards, "rtp" for the receiver); once every resource is ready
	/// the recipe builds the run object and the code is prepared; once the block is ready the code drives it
	/// (PortBlock's asynchronous drive, fading in every output the code writes: public outputs and inside outputs, which
	/// feed a card), and the node is running once that drive completed. Every planned silence (a restart, a removal, a
	/// failure, stopAll) first fades those outputs out over 5 ms (unacknowledged within 100 ms: ear.fade-unacknowledged
	/// and the silence unfaded); a drive or silence not completed within 5 s marks the data loop wedged
	/// (ear.barrier-overdue, then ear.data-loop-wedged once): from then on NodeHost makes no PipeWire call, every
	/// configuration, block and outside node it would end is leaked (leaked()), and a node added fails engine-refused
	/// "the data loop is wedged" without a block. Every bound first settles the block, so a completion a stalled main
	/// loop has not yet dispatched is never judged late. A failure fails the node
	/// and tears its configuration down; its port block stays. A name added back gets its block in the flush that
	/// destroys the older block of the name (T1), and its configuration once the older node of the name is gone.
	///
	/// Health is told after every transition that changes any of its members (D-L34), what the node waits for once its
	/// phase has lasted 2 s and then at most once a second (D-L42). A node added disabled keeps its block, silent, and is
	/// never configured (T19).
	void nodeAdded(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// Restart (I5, I7): the barrier (a drive with no code, once it completed), release(); outside nodes destroyed;
	/// handoffs closed; the run object closed; then, on the main loop, the code destroyed, then the run object, the
	/// recipe, the recordings and the config; then a new configuration from `config` as in nodeAdded. The port block and
	/// routes stay. An update arriving during a restart replaces the config to start; one on a failed node configures it
	/// again once its failed configuration is gone. If the block was lost: nothing starts, the node stays failed.
	void nodeUpdated(const NodeSpec& node, std::shared_ptr<const NodeConfig> config) override;
	/// The plan's outcome (docs/design/node-lifecycle.md 8.4): restart as nodeUpdated (a stopped node rests stopped
	/// again); disable tears the configuration down (stop() and the fade if it runs), drops the node's stop and keeps
	/// its block and routes (T20, T21); enable configures it afresh (T22); stored keeps the config for the enable.
	void nodeChanged(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, const NodeChange& change) override;
	/// stopping; the barrier, then the port block's destruction queued into the turn's flush and the configuration torn
	/// down as the restart's; nodeGone once both are gone (the flush ran the destruction). Others untouched.
	void nodeRemoved(const NodeName& node) override;
	/// A replace (another type or other ports) is a removal and an addition of the name (D-R23, D-L5); when only the
	/// ports changed (change.portCaused), the new node carries the older one's stop (both enabled), its dropped frames
	/// and one health stream, stopping (restarting) until the older node is gone, the older node telling nobody; any
	/// other older node tells only the listeners that hear a superseded node (5.7, D-L36, D-L46).
	void nodeReplacing(const NodeName& node, const NodeChange& change) override;
	void nodeReplaced(const NodeSpec& node, std::shared_ptr<const NodeConfig> config,
	                  const NodeChange& change) override;
	/// NodeOperations (4.4): stop holds the node's stop (a running node fades, is silenced and rests stopped keeping its
	/// configuration), start clears it (a failed node retries), restart tears the configuration down and configures it
	/// again. Refused: stopping during stopAll, unknown-node for a name no wanted node has, node-disabled, and
	/// not-restartable as 4.4 says.
	LifecycleReply start(const NodeName& node) override;
	LifecycleReply stop(const NodeName& node) override;
	LifecycleReply restart(const NodeName& node) override;
	/// `closed` proves the graph takes no more edits, so no nodeAdded, nodeUpdated or nodeRemoved
	/// can follow (one would throw std::logic_error: a wiring bug). Every node: stopping; torn
	/// down; gone. `allGone` runs once on the main thread after the last (at once if none).
	void stopAll(GraphClosed closed, std::move_only_function<void()> allGone);
	/// Nodes not gone, in arrival order (a removed node still stopping comes before a newer node
	/// of the same name).
	std::vector<NodeHealth> health() const;
	/// What was leaked, never freed: configurations left running past the close deadline (I5a) or on a wedged data
	/// loop, and the blocks and outside nodes a wedged loop kept (5.4). Their threads may still post to `mainLoop` or
	/// touch engine objects at any time, so once any was leaked the process must end without destroying those (main.cpp
	/// ends with std::_Exit).
	std::size_t leaked() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace ear
