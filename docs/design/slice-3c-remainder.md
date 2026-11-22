# What slice 3c left unbuilt

Status: designed, not built. Slice 3c gave every node type the primitives `sendspin` had built for itself (the node
handle, loops, the I/O executor, stores, claims, recordings), moved `sendspin` onto them, retired the old author
interface and built the timing model's source clock and its module-run converter. All of that is described as built in
the library docs (`docs/architecture.md`, `docs/engine.md`, `docs/writing-a-node-type.md`, `docs/node-types.md`), and
its design pages are gone. This page keeps the three pieces of it that are not built, each waiting on something
outside the slice:

| Step | What | Waits on |
|---|---|---|
| **S3c.7** | The library's own log lines attributed to their node: a carried sendspin-cpp patch (section 2) | the owner's priorities: `sendspin` is not first |
| **T5** | The converter companion's set and tracked modes (section 3) | a first user: no type needs one yet |
| — | Dropping `cmake/sendspin-starved-floor.patch` (section 4) | evidence from both burst shapes |

[V] = verified in the file named (files of the pinned sendspin-cpp tree, `.cache/deps/sendspin-src`, by their path in
it; PipeWire 1.4.2's installed headers); [U] = unverified, with the step or test that checks it.

## 1. The owner's words that bind it

| # | The owner's words or decision | Date | What it means here |
|---|---|---|---|
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07 | A node's threads log through its handle (built: `docs/writing-a-node-type.md#the-node-handle`); the lines a library it links writes are attributed to the node too (section 2) |
| TM5 | **Conversion is a converter companion.** For rate and drift conversion the framework provides a companion: PipeWire's converter (audioconvert in an adapter, with its resampler) wired between the node's own side and the graph, hidden from the graph file, owned by the node. The node either sets the ratio or lets the converter track a rate it reports (the brief's wording, not a quote) | 2026-11-13 | Module-run is built (`docs/engine.md#module-run-converters`); set and tracked are section 3 |
| TM8 | Set and tracked are built on the module-run base with their first user | 2026-11-13 | T5 has no date until a type needs it: `airplay` took the module-run mode ([`docs/node-types.md#airplay`](../node-types.md#airplay)) |
| TM9 | A host stall stalls the whole graph, one tick and one data thread, so every node inside ear resumes together and stays in time with the others. Whether to jump or to speed up concerns only a source whose own clock falls behind while the graph ticks on, and staying in time with what is outside ear after a graph-wide stall | 2026-11-13 | The default is to jump (section 3.4), decided unless the owner objects |

## 2. S3c.7: the library's log lines, by a carried sendspin-cpp patch

**Today.** On a host build `SS_LOG*` is a bare `fprintf(stderr, ...)` behind one process-wide level
(`src/platform/logging.h:56-63` in the pinned tree) [V]. ear sets `WARN`; its lines reach standard output as
unattributed `stderr` records at `warn`, and with two `sendspin` nodes nobody can tell whose. There are 533 `SS_LOG*`
call sites in 36 files [V]; none takes a client.

**The patch** (`cmake/sendspin-log.patch`, beside the three carried now):

1. `logging.h`, host branch: the five macros call `::sendspin::log_emit(level, tag, fmt, ...)`
   (`__attribute__((format(printf, 3, 4)))`), defined in a new `src/platform/logging.cpp`. It checks the level as now,
   formats into a 512-byte stack buffer (a longer line cut and marked), and calls the sink if one is set, else writes
   `stderr` exactly as today. No allocation.
2. The sink is process-wide, the context per thread:
   ```cpp
   namespace sendspin {
   using LogSink = void (*)(void* context, int level, const char* tag, const char* text, bool cut) noexcept;
   void set_log_sink(LogSink sink) noexcept;                 // atomic; null restores stderr
   class LogScope { public: explicit LogScope(void* context) noexcept; ~LogScope(); };   // this thread's context
   }
   ```
3. `SendspinClientConfig::log_context` (`void*`, default null). The client opens a `LogScope` with it in every public
   entry point (`start`, `loop`, `stop`, `connect_to`, `disconnect`, the destructor, `PlayerRole::notify_audio_played`,
   `SourceRole::write_audio`), so callbacks it makes from there, the persistence provider's included, inherit it.
4. The three library threads (`ProtocolTask::start`, `SyncTask::start`, `SourceTask::start`) capture the caller's
   context and open a scope at the thread's entry; they are started inside `client.start()`, which is scoped.
5. The three IXWebSocket callbacks the library registers capture the context and open a scope in the lambda.
   IXWebSocket's own `fprintf`s stay unattributed `stderr` records (`ixwebsocket-limits.patch` already bounds them).

**ear's side**, in `sendspin`'s worker state: `set_log_sink(&forward)` once per process, and `config.log_context` the
configuration's node handle. `forward` maps `SS_LOG_ERROR`, `WARN`, `INFO` and `DEBUG`/`VERBOSE` to ear's four levels
and calls the handle's `log` with a `LogEntry` whose `source` is `sendspin-cpp`, `topic` the library's tag and `cut`
the patch's mark. A null context is an unattributed `log` record from `sendspin-cpp`. A handle is good until its
configuration's teardown ended, and a call after that is dropped
([`docs/writing-a-node-type.md#the-node-handle`](../writing-a-node-type.md#the-node-handle)); each client is destroyed
on the worker before the worker's state is, so no thread of an old client logs through a newer configuration.
Configure refuses a tree without the patch (`cmake/CarriedPatch.cmake`, built); `sendspin_patched_dep` already takes a
list of patches.

**The stamp.** The worker counts the frames it has written to the source, and its log sink stamps each line with that
count, so a line names the frame it acts on.

**What it retires in the tests.** The `sendspin` clock tests match a library line within an interim reach, `kLogReach`
and `kLogLag`, and stretch a loss of sync by `kLogLag` (`inLoss`), because a line read from standard error is stamped
when it was read, not where its audio plays ([`docs/testing.md#the-librarys-own-word`](../testing.md#the-librarys-own-word));
with the stamp the three go. The system tests' matching of `Lost sync` by size, and their `stepsInLoss` bound, become
matching by place.

**Behaviour.** The library's lines become `log` records naming their node; nothing else an operator sees changes.

**Tests.** `ear_carried_patch_check` for the new patch; a library line from the worker and one from the sync thread
arrive as `log` records with the node; two nodes' lines told apart; a line outside any scope is unattributed. Revert
proof: without the patch, the two-node attribution test fails.

**Cost.** A fourth carried patch to rebase at each sendspin-cpp bump, touching about 15 places.

| Rejected | Why not |
|---|---|
| Passing the client at each `SS_LOG*` site | 533 sites: a patch no bump could carry |
| Leaving the lines unattributed until upstream has a hook | R9; and the clock tests' interim reach stays |

**Open.**

- [U] That every `SS_LOG*` line `sendspin`'s tests provoke runs inside a scope: the step's system test counts
  unattributed `sendspin-cpp` records over the pairing and connection tests; each one found is a missed entry point.
- [U] That the library calls the persistence provider only on threads in the client's scope (`client.cpp:1648-1741`,
  `player_role.cpp:751-778`, `record_store.cpp:43` [V] are the call sites; their threads are not traced). The store's
  `replace`, which `save_blob` calls, already refuses a strand (asserted in debug builds) and the main loop (a
  `std::logic_error`): [`docs/writing-a-node-type.md#stores`](../writing-a-node-type.md#stores). What stays unverified
  is only that every thread it runs on is inside the library's log scope.
- [U] That the source writer's closing contract ([`docs/writing-a-node-type.md#sources`](../writing-a-node-type.md#sources))
  keeps the library's sync thread asleep between calls from teardown step 1 to step 4: the ring's own tests prove it
  on the ring; the real sync thread across a teardown held 2 s by a slow consumer is not measured.
- Upstream's interest in the log hook is not asked yet.

## 3. T5: the converter companion's set and tracked modes

A converter companion is PipeWire's converter between a node's own side and one of its block's inside banks, the one
place a clock and a rate other than the graph's meet the graph
([`docs/architecture.md#the-timing-model`](../architecture.md#the-timing-model), rule 5). Who keeps the ratio is its
mode:

| Mode | Who owns the stream, the resampler and the loop | User | State |
|---|---|---|---|
| **module-run** | A PipeWire module | `rtp`'s receiver, `airplay`'s pipe receiver | built ([`docs/engine.md#module-run-converters`](../engine.md#module-run-converters)) |
| **set** | ear: an ear-made stream plays (or fills) a ring at the node's rate; the node's clock gives the ratio | none yet | this section |
| **tracked** | ear: as set, but the converter runs a loop on the ring's fill, the rate the node reports as its feed-forward | none yet | this section |

### 3.1 The interfaces

An ear-made `pw_stream` body (`StreamConverterBody`, its events through the callback boundary as a `StreamCallbacks`
table; the guard already refuses `pw_stream_new_simple`) on the same `joinEngineCompanion` every engine companion
uses, playing a source's ring through `SourceRing::play` for the frames the resampler asks (`pw_buffer::requested`,
since 0.3.49 [V]), or filling a sink's ring:

```cpp
/// Who keeps an ear-made converter's ratio.
enum class RatioMode { set, tracked };
/// A converter between a ring at `nodeRate` and an inside bank at the graph rate (set and tracked modes).
struct ConverterShape {
	SampleRate nodeRate;        // 8 to 768 kHz; the graph frequency is allowed (drift only, no rate change)
	std::uint32_t targetFrames; // tracked: the ring's fill the converter steers to, in node frames; set: 0
};
class SetRatio {   // RatioMode::set: any thread, the realtime one included (a source's clock); lock-free
public:
	/// Node frames per graph frame from the converter's next tick on; 1.0 is no correction. Clamped [U].
	virtual void set(double nodePerGraph) noexcept = 0;
protected:
	~SetRatio() = default;
};
class RateReport { // RatioMode::tracked: any thread
public:
	/// The node's clock as it measured it, in its frames per second of CLOCK_MONOTONIC_RAW; the converter tracks it and
	/// steers the ring's fill to targetFrames on top.
	virtual void report(double framesPerRawSecond) noexcept = 0;
protected:
	~RateReport() = default;
};
// CreateOf<spec>; the mode is a template argument, so asking a set converter for a rate report does not compile:
template <RatioMode M, class Bank> // an inside input bank (compile time); M is set or tracked
ConvertedSource<M> convertedSource(Bank, SourceShape ring, ConverterShape conv,
                                   std::unique_ptr<SourceClock> clock = nullptr);
template <RatioMode M, class Bank> // an inside output bank (compile time)
ConvertedSink<M> convertedSink(Bank, SinkShape ring, ConverterShape conv);
// ConvertedSource<set>{SourceWriter& writer; SetRatio& ratio;}, <tracked>{SourceWriter& writer; RateReport& rate;}
```

- **Set** applies the ratio with `pw_stream_set_rate` (RT safe, adjusts an adaptive resampler, since 1.4.0 [V]).
- **Tracked** runs a delay-locked loop on the ring's fill (as `module-rtp`'s receiver does on its buffer [U:
  `module-rtp/audio.c`'s constants are still to read]), the reported rate its feed-forward, applied the same way. **A
  skip re-targets the loop:** the converter plays the ring, so it sees each tick's `SourcePlayed::skipped`; on a tick
  that skipped it resets the loop's integrator and takes the fill after the skip as its new reference, so the loop
  never steers fast for frames the clock already dropped. One controller acts on the fill at a time: the clock by a
  skip, the loop by the ratio between skips.
- A ring behind a converter is in node frames: `SourceTick::rate` is `nodeRate`, `SourceTick::frames` what the
  resampler asked, and a clock converts a graph-frame fact (a `jumpFrames`) through `FrameClock`. A converted sink's
  chunk keeps its tick's facts and carries node frames [U: `cutChunk` and `SinkRing` gain the ring's rate].
- *Why an engine node, when ring companions are served by their block*
  ([`docs/architecture.md#companions`](../architecture.md#companions)): the resampler is PipeWire's only inside an
  adapter (TM5). What serving by the block was for still holds: the stream plays only ticks the code is driven for,
  bound and unbound by the same drive invoke on the same data loop (for a source it runs before the block, for a sink
  after it). An unconverted source or sink stays served by its block.
- **No mode skips.** The framework keeps no audio time, the converter included: lost ticks are the node's clock's
  (`SourceClock::skipBefore`); in tracked mode a backlog above `targetFrames` drains through the loop unless the clock
  skips it, which by the default (3.4) it does.
- The body needs no name of its own at the callback boundary unless it becomes a template shared between shapes, as
  the module-run body is ([`docs/engine.md#module-run-converters`](../engine.md#module-run-converters)).

### 3.2 Latency

A converter's delay is latency beyond its bank, entered once in the configuration's plan
([`docs/engine.md#latency-per-bank-pair`](../engine.md#latency-per-bank-pair)), and only what no engine node reports,
so nothing counts twice: the ring (`Latency{0, capacity}`, or `targetFrames` to capacity when tracked) and the
resampler's delay (`spa_io_rate_match::delay` and `delay_frac`, at the node's rate [V: `spa/node/io.h`];
`pw_stream_control::buffered` [V]), converted to graph frames by `FrameClock`, unless the stream's ports report the
delay to the block as a peer [U: 3.3]. A source's clock gets the bank's playback latency plus that delay in
`SourceTick::playback`.

### 3.3 Checks owed before it is built

The module-run mode's checks are settled (`spikes_test.cpp`; the `pipewire-engine` skill). These are the ear-made
stream's own, each a test kept as a regression. An in-process `pw_stream` at 44100 Hz linked to a block's inside
input bank in a 48000 Hz graph:

- [U] It runs in the graph group without moving the graph's rate and without a force property (the RTP receiver at
  44.1 kHz is the precedent [V]).
- [U] `requested` each cycle, and `pw_stream_set_rate` honoured from its process callback, with its range and clamp.
- [U] Its `io_changed` gives an `SPA_IO_Position` a tick can be stated from (`CycleAccount` over the engine's cut
  record), and its process runs before the block's in every cycle.
- [U] The resampler's delay per quality, and whether its ports report it as latency. rtp-source's port tells 0 frames
  once linked, resampling or not, which suggests an adapter's ports report no resampler delay.
- [U] `module-rtp/audio.c`'s loop constants.

### 3.4 The default when a source falls behind: jump

*Decided unless the owner objects* (TM9). A stall of the host is not this question: it stalls the whole graph, so
every node inside ear resumes together. Two other cases are. (a) A source's own clock falls behind while the graph
keeps ticking: its network stalls, or its own thread is held, and the audio it then holds is late against the graph.
(b) After a graph-wide stall, ear itself is late against what is outside it: speakers driven by other machines, or a
picture on a screen. Jumping drops the late audio at once; speeding up (a converter in tracked mode) plays it slightly
fast until it is back in time, with no jump but out of time meanwhile.

*Scenario.* (a) A network source in ear feeds the kitchen and the living room beside a `sendspin` node feeding the
porch, and its network stalls for 300 ms while the graph ticks on. Jump: its rooms skip 300 ms and are in time with
the rest of the graph at once. Speed up: no skip, but its rooms trail the porch by 300 ms at first, for about a minute
at a 0.5% speed-up. (b) ear's host stalls 300 ms while a TV elsewhere shows the picture: every room in ear goes quiet
and resumes together, but 300 ms behind the picture; jumping puts the sound back on the picture, speeding up drifts
back over a minute.

The default is to jump, as `sendspin` does ([`docs/node-types.md#clock`](../node-types.md#clock)): it keeps the
source in time with the other nodes of the graph, and ear in time with what is outside it. Each source can still
choose otherwise in its own clock.

### 3.5 Tests

A 44.1 kHz sine through a converted source into a 48 kHz graph, its frequency per tenth of a second; set: a ratio of
1.001 for 2 s moves the ring's fill by the expected frames, within a block; tracked: a writer paced at 44100 x (1 +
200 ppm) on the raw clock holds the fill within a block of `targetFrames`, with no underrun, and a skip of half the
ring while tracking leaves the ratio within the loop's steady range, with no run toward the old fill; the told latency
against the model; the lifecycle and deadline as every engine kind's; bound and unbound by the drive; a compile-fail
case (a set converter asked for a `RateReport`); the harness's fake converter (frames 1:1, each `set` and `report`
recorded). Docs with it: `docs/engine.md#engine-companions`, `docs/writing-a-node-type.md#engine-companions`, and
`docs/architecture.md#the-timing-model`'s departure removed.

### 3.6 Decided, and rejected

| Decision | Why |
|---|---|
| Set and tracked add an ear-made `pw_stream` body on the lifecycle every engine companion shares | The stream body is the only way to PipeWire's resampler for a ring (TM5); binding by the drive keeps the exactness of ring companions served by their block |
| The converter never skips; skipping is the node's clock's | The framework keeps no audio time ([`docs/architecture.md#the-timing-model`](../architecture.md#the-timing-model), rule 1) |
| `RatioMode` chooses only between set and tracked; module-run is a shape's own | Nothing built reads a mode: a module-run shape is one that has the latency rule |

| Rejected | Why not |
|---|---|
| An in-code resampler for converted rings | TM5: conversion is PipeWire's converter |
| Reimplementing RTP reception on the set or tracked converter (ear's own jitter buffer and clock tracking) | PipeWire keeps the jitter buffer and the clock tracking; RTP is module-run |
| The converter skipping a stall's backlog on `catchUp` | The framework would be keeping time again; a node that wants the skip gives a clock |

## 4. Dropping the starved-floor patch, gated on evidence

`cmake/sendspin-starved-floor.patch` is carried with the deferral
([`docs/node-types.md#library-facts-and-carried-patches`](../node-types.md#library-facts-and-carried-patches)): while
ear defers, the library's playtime runs open loop and trails now through a burst of either shape, with frames still
buffered, and upstream's floor would fire on it mid-burst. Dropping it is a separate step with its own review, taken
only if both burst shapes, run without the patch, show no floor mid-burst and no correction beyond the stall tests'
one [U]: the dev host's cut bursts passed 20 of 20, which is evidence for the cut shape only; CI's uncut ones are not
yet read in a runner's log (`out/Testing/sendspin-stall-shapes.txt`). Until then it is carried as built, and its
upstream report stands as drafted in the `sendspin` skill.
