// The interim sink of slice 3a (docs/writing-a-node-type.md#sinks, the home of these declarations; docs/engine.md#sinks
// its rules; docs/engine.md#sinks the chunk's contract; docs/design/node-primitives.md#65-a-sinks-consumer the
// consumer's in slice 3c). One home for the sink: what a type writes against (Chunk, SinkConsumer, SinkShape) and what
// the framework runs it with (ChunkStamp, SinkTap, SinkBinding, Sink, SinkAdapter). NodeHost implements CreateOf::sink
// on it, ResourceRegistry takes a SinkAdapter, DriveRequest carries SinkBindings, ear_pipewire implements SinkAdapter
// as ThreadSinks, and `file` consumes one. Pure: nothing but the standard library and ear_core.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include "ear/core/cycle_facts.hpp"
#include "ear/core/graph_spec.hpp"
#include "ear/core/latency.hpp"
namespace ear {

// ---- What a type writes against ----

/// One cycle's frames of a sink's inside output bank, or what the ring held of it (docs/engine.md#sinks),
/// stamped with that cycle's facts. `interleaved.size() == frames * channels`, channels the bank's size. A chunk that
/// wraps the ring's end is consumed in two calls, the second with `position`, `startNs` and `startRawNs` advanced by the
/// first's frames (so each call's start is its own first frame's), no causes and no frames lost. Valid only during the
/// consume() it is handed to.
struct Chunk {
	std::span<const float> interleaved;
	std::uint32_t frames;
	std::uint64_t position;  // the driver's position of the chunk's first frame
	std::int64_t startNs;    // its cycle's start, CLOCK_MONOTONIC
	std::int64_t startRawNs; // the same on CLOCK_MONOTONIC_RAW
	/// The most upstream (capture) latency the block told on the sink's bank in the cycle the chunk was taken: what
	/// reaches the input banks paired with it plus what the configuration declared on it (docs/engine.md#sinks): the
	/// most of each part, so an upper bound when the bank mixes paths. The chunk's first frame entered the graph at
	/// FrameClock(rate).enteredRawNs(startRawNs, 0, {0, upstreamFrames}) (docs/engine.md#dating).
	std::uint32_t upstreamFrames;
	/// Why the cycle is a discontinuity (its GraphCycle::facts.causes, docs/writing-a-node-type.md#lost-cycles): `drive`
	/// on the first cycle after any drive (a start, a restart, and a re-drive of the same code too), `start` on the
	/// block's first, `driverChange`, `catchUp` (the graph lost cycles and caught them up), `absent`, `reposition` and
	/// `oversized`; none on a cycle that followed its last with nothing lost. A stop's gap is a drive with no loss
	/// stated: compare `position` with the last chunk's end to tell it from a re-drive.
	CycleCauses causes;
	/// The frames the graph lost before this chunk on a `catchUp` (its jumpFrames), 0 otherwise: a consumer may write
	/// them as silence or say them.
	std::uint64_t lostFrames;
	constexpr bool discontinuity() const noexcept { return !causes.empty(); }
};

/// A sink's consumer (slice 3c's design: docs/design/node-primitives.md#65-a-sinks-consumer). It has a public virtual
/// destructor, since a consumer given whole is owned and deleted by the sink). Every call is on the sink's own thread,
/// never the main loop or the realtime thread, one at a time, in this order: begin once, consume in chunk order, finish
/// once after the last consume. finish runs only if begin returned (a sink closed before it began calls neither). A
/// call that throws is a type bug the framework catches on the sink's thread: it fails the node, and after it no more
/// consume is called (docs/writing-a-node-type.md#sinks).
class SinkConsumer {
public:
	virtual ~SinkConsumer() = default;
	/// Once, before any consume and before the node is `configured` (readiness 4'). Opens what it writes to; a failure
	/// is the node handle's lost(detail) (in 3a NodeOf::handle(), an InsidesEvents&), which fails the node
	/// start-failed. May block on I/O.
	virtual void begin() {}
	/// In order; may block on I/O.
	virtual void consume(const Chunk& chunk) = 0;
	/// Once, after the last consume (teardown step 3).
	virtual void finish() = 0;
};

/// How much a sink's ring holds, in frames at the graph frequency (docs/writing-a-node-type.md#configure-and-createof
/// default: 2 s). The channel count is its bank's size.
struct SinkShape {
	std::uint32_t frames;
};

// ---- What the framework runs a sink with (no type sees these) ----

/// The stamp the block's tap writes beside a cycle's frames: GraphCycle's facts for that cycle (Chunk::causes and
/// lostFrames), and the most upstream latency the block last told on the sink's bank (Chunk::upstreamFrames).
struct ChunkStamp {
	std::uint64_t position;
	std::int64_t startNs;
	std::int64_t startRawNs;
	CycleCauses causes;
	std::uint64_t lostFrames;
	std::uint32_t upstreamFrames;
	constexpr bool discontinuity() const noexcept { return !causes.empty(); }
	friend bool operator==(const ChunkStamp&, const ChunkStamp&) = default;
};

/// The calls one stamped cycle becomes when `contiguous` of its `frames` lie before the ring's end
/// (docs/engine.md#sinks): one, or two, the second's position and start times advanced by the first's frames at the
/// graph's rate, with no causes and no frames lost (the loss came before the first), so each call's start is its own
/// first frame's.
struct ChunkCut {
	ChunkStamp first;
	std::uint32_t firstFrames;
	std::optional<ChunkStamp> second;
	std::uint32_t secondFrames;
};
/// The one home of that rule: SinkRing (ear/core/sink_ring.hpp) cuts every chunk with it, and so the engine's sinks and
/// the type harness's both. `contiguous` 0 is taken as no wrap: a ring that holds the chunk's frames always has some
/// before its end.
constexpr ChunkCut cutChunk(const ChunkStamp& stamp, std::uint32_t frames, std::uint32_t contiguous,
                            FrameClock clock) noexcept {
	if (contiguous == 0 || contiguous >= frames) return ChunkCut{stamp, frames, std::nullopt, 0};
	const std::int64_t advanced = clock.ns(contiguous);
	return ChunkCut{stamp, contiguous,
	                ChunkStamp{.position = stamp.position + contiguous,
	                           .startNs = stamp.startNs + advanced,
	                           .startRawNs = stamp.startRawNs + advanced,
	                           .causes = {},
	                           .lostFrames = 0,
	                           .upstreamFrames = stamp.upstreamFrames},
	                frames - contiguous};
}

/// The realtime end of one sink. The port block calls write() once per cycle the code it was driven with ran, after
/// that code, with the sink's inside output bank's spans (an unlinked one's scratch included).
class SinkTap {
public:
	/// Realtime thread only: never blocks, locks, allocates or makes a syscall but the wake. `bank.size()` is the
	/// sink's channel count, each span at least `frames` long. Interleaves the frames into the ring and pushes one
	/// stamp of the frames it accepted. The two rings drop together: with no free stamp entry it writes nothing and
	/// counts the whole chunk dropped; else it writes what the frame ring has room for and counts the rest dropped
	/// (docs/engine.md#sinks). Wakes the sink's thread at most once per call.
	virtual void write(std::span<const std::span<const float>> bank, std::uint32_t frames,
	                   const ChunkStamp& stamp) noexcept = 0;

protected:
	~SinkTap() = default;
};

/// What a drive hands the block for one sink (DriveRequest::sinks): its tap, and where its bank sits in
/// process()'s output spans (outputs, then inside outputs). Outlives the drive's completion or the block, as the rest
/// of a DriveRequest does.
struct SinkBinding {
	SinkTap* tap;
	std::size_t firstOutput; // index into process()'s output spans
	std::size_t channels;    // the bank's size
};

/// One interim sink: a ring, its stamp ring and its own thread (docs/engine.md#sinks). Made inside configure by
/// CreateOf::sink, for the configuration's life. Main thread for every call; `begun` and `closed` run on the main loop
/// through the adapter's Executor, the FIFO the node's InsidesEvents post through, so a lost() said inside begin()
/// always lands before `begun`.
class Sink {
public:
	/// Main thread; only after `closed` ran, or when neither begin() nor close() was ever called.
	virtual ~Sink() = default;
	/// The realtime end, for the configuration's DriveRequest. Valid for the sink's life.
	virtual SinkTap& tap() noexcept = 0;
	/// Main thread, at most once, when configure answered applied: hands the consumer to the sink, which owns it from
	/// now on, and asks its thread to call consumer->begin(), then post `begun`. A sink never begun is closed with
	/// nothing to finish.
	virtual void begin(std::unique_ptr<SinkConsumer> consumer, std::move_only_function<void()> begun) = 0;
	/// Main thread, once, after the block can no longer run a code that writes the tap (its silence completed, or it
	/// was never driven): the sink's thread drains what the ring holds to the consumer, calls finish() if begin() ran,
	/// then `closed(dropped)` runs on the main loop with the frames the tap refused over the sink's life. A stop closes
	/// nothing: the next drive binds the same tap again.
	virtual void close(std::move_only_function<void(std::uint64_t droppedFrames)> closed) = 0;
	/// Any thread: the frames the tap refused since the sink was made (the count `closed` reports once closed).
	virtual std::uint64_t droppedFrames() const noexcept = 0;
};

/// The sink kind's adapter (ResourceRegistry's slot). ear_pipewire's ThreadSinks implements it.
class SinkAdapter {
public:
	/// Main thread. A sink of `channels` channels and `capacityFrames` frames of ring for `node`'s inside output bank
	/// `bank`, its thread started (named for "<node>.<bank>"), at the graph's `rate` (a chunk handed in two calls dates
	/// the second call's first frame by it). Refuses only by std::bad_alloc or std::system_error (a thread it cannot
	/// start): the node fails start-failed. A std::logic_error is its own bug: the node fails lost, told as
	/// ear.internal-error (docs/architecture.md#faults-by-owner).
	virtual std::unique_ptr<Sink> make(const NodeName& node, std::string_view bank, std::uint32_t channels,
	                                   std::uint32_t capacityFrames, SampleRate rate) = 0;

protected:
	~SinkAdapter() = default;
};

} // namespace ear
