# The timing model: one tick, one rate, clocks at the edges

Status: designed to module level (2026-11-13), part of slice 3c (`node-primitives.md`); nothing of it built beyond what
section 2 says is already true. It states how audio time works in ear (the owner's model, section 1), takes the
framework's timekeeping out of the source (`SourceRing`'s `Played` and its keep-in-step), moves `sendspin`'s sync onto
the spike's "honest sink plus local skip" (section 6), and adds the converter companion (section 5). The model itself
is `docs/architecture.md#the-timing-model`, which says where the code still departs from it; this page designs the
change that removes each departure. It reverses `cycle-facts.md`'s Q-CF2 and `companions.md`'s D-C4 and D-3b3
(section 8). When it is built it is deleted, and what lasts moves into `docs/architecture.md`, `docs/engine.md`,
`docs/writing-a-node-type.md`, `docs/node-types.md#clock` and the `sendspin` skill.

[V] = verified in the code, PipeWire 1.4.2's installed headers (`pipewire-1.4.2/src/pipewire/stream.h`,
`spa/node/io.h`) or the spike's results (`scripts/spike/` on branch `worktree-agent-a573a8b29870d1f49`: `summary.md`,
`tables.md`, `steady.md`, `results.txt`), as of 2026-11-13; [U] = unverified, with the step that checks it.

## 1. The owner's words that bind this page

Decided by the owner on 2026-11-13 and given to the designer as decided; the wording is the brief's, not a quote.

| # | The owner's decision | What it means here |
|---|---|---|
| TM1 | **The tick.** The framework supplies one tick, the graph cycle, and its facts (start on the raw clock, overdue, what was lost or cut and why) at one graph rate. It keeps no audio time for any node | `GraphCycle` stays as built; `SourceRing` loses `Played` and its keep-in-step (section 4) |
| TM2 | **Node kinds.** Every node is a source, a sink or a transform. Transforms read their inputs, process and write their outputs purely on the tick | No transform skips, pads or dates; only an edge node's code reconciles a clock |
| TM3 | **Clocks belong to sources and sinks.** A source or sink with its own clock (a server, a network sender, a sound card) reconciles that clock with the tick inside itself: buffering, rate matching, drift, and recovery after lost ticks | A source's clock is the type's `SourceClock` (4.2); `sendspin`'s sync stays in its node (section 6) |
| TM4 | **One rate inside the graph.** Every port inside the graph runs at the graph rate; conversion happens only in edge nodes. The graph rate is configurable (`graph_frequency`, flag fallback) but fixed for the engine's life, and it is the only allowed rate. Mixed-rate graphs (two graphs joined by a converting edge) are out of scope | Built; the converter's node-rate side is never a graph port (5.1) |
| TM5 | **Conversion is a converter companion.** For rate and drift conversion the framework provides a companion: PipeWire's converter (audioconvert in an adapter, with its resampler) wired between the node's own side and the graph, hidden from the graph file, owned by the node. The node either sets the ratio or lets the converter track a rate it reports. Library functions remain fine for simple in-code work such as sample-format conversion. This generalises what cards and RTP receivers already do | Section 5 |
| TM6 | **The internal clock drives.** Cards follow it and adapt to the graph: the existing choice, kept and stated | Built (`docs/engine.md#the-internal-clock`) |
| TM7 | **Accepted consequences.** `SourceRing`'s timekeeping leaves the framework (its Played count and dating, its keep-in-step across stalls), reversing Q-CF2; the source node keeps its own time. `sendspin`'s sync stays in its node with the spike's winning design, "honest sink plus local skip": play every frame in order; on a tick stating a catch-up or absent jump skip min(jump, what the feed holds now); report every tick played = skipped + read with an honest finish that never steps back; no debt, no deferral, no `LibraryView`. Only `cmake/sendspin-late-chunk.patch` remains of the timing patches (`sendspin-starved-floor.patch` is dropped). C4's machinery is deleted (`LibraryView`, owed/catchingUp/untoldBound, `Feed::skipThenRead`, the overdue deferral, their tests); `GraphCycle::overdueNs` stays, as a tick fact | Sections 4, 6 and 7 |

## 2. What is built, against the model

| Rule | Built | Departs |
|---|---|---|
| TM1 | `GraphCycle` states the tick: `startNs`, `startRawNs`, `position`, `rateDiff`, `driverId`, `nextNs`, `overdueNs`, `facts` (`docs/engine.md#graphcycle`) [V] | `SourceRing` keeps a source's time: `Played{totalFrames, heardRawNs}`, the played wake, and on a `catchUp` the whole jump skipped and counted played with the rest owed from the writer's next frames (`docs/writing-a-node-type.md#sources`) [V] |
| TM2 | The pass-through (`card`, `rtp`, `sendspin`'s public banks) reads and writes on the tick; no other transform exists yet | — |
| TM3 | A card's ALSA node and an RTP receiver reconcile their clocks in PipeWire (a delay-locked loop steering a resampler) [V]; `sendspin` reconciles the server's clock in its code | `sendspin`'s code owes lost time, defers its reports and models the library (`LibraryView`, `untoldBound`, `Feed::skipThenRead`), and carries `sendspin-starved-floor.patch` (`docs/node-types.md#clock`) [V] |
| TM4 | One rate and one quantum (`docs/engine.md#the-clock`, `#the-quantum-guard`); a 44.1 kHz RTP sender into a 48 kHz graph is resampled by its receiver [V] | — |
| TM5 | Cards and RTP receivers are PipeWire's converter inside their engine companions (5.5) | No converter companion for a ring whose clock is a type's own |
| TM6 | Built [V] | — |

No shipped type uses a source yet (`docs/writing-a-node-type.md#sources`) [V]: today's users of `SourceRing` are
`EventSources` (`ear_pipewire`), the type harness, and their tests. So section 4's change moves no shipped node's
audio; `sendspin` meets it when S3c.6 moves it onto a source.

## 3. The design in one paragraph

A source's ring stays the framework's transport (the writer's wait and idle, clears, fades, the shortfall, the stop's
drop) and stops keeping time: no `Played`, no skip of its own, no debt. A type whose source has a clock of its own gives
the source a **`SourceClock`**, called on the realtime thread around each tick's play: before it, with the tick and what
the ring holds, it says how much to skip; after it, it is told what was skipped, read, faded and zeroed, and reports to
its own threads through a mailbox it owns and the source's clock wake. A source with no clock plays its ring on the tick
and nothing else. Sinks are unchanged: a chunk carries the tick's facts and the latency the block told, and a consumer
with a clock reconciles from them. A **converter companion** (a fourth engine kind) is an in-process PipeWire stream, an
adapter with audioconvert, that plays a source's ring at the node's own rate (or fills a sink's) and resamples to the
graph rate, the ratio set by the node's clock each tick or tracked from a rate the node reports; its delay enters the
latency plan beyond its bank. `sendspin`'s source clock is the honest sink plus local skip, dated by when its tick
really ran and clamped so a finish never steps back; C4's machinery and the floor patch go.

## 4. Sources

### 4.1 What the framework still does for a source, and what it stops doing

| `SourceRing` today | After |
|---|---|
| The ring, the writer's `write` with patience and `idle`, `clear`, `close` | **Kept** |
| Fades at its edges (in at a start or a return, an underrun's decay, a clear's 5 ms of the ended stream) | **Kept**: they shape samples, not time |
| The shortfall (zeros while the writer was not idle), dropped frames (a clear, a stop) | **Kept**: counters of the transport, read by `companions` (K1) |
| A stop's drop (`dropForStop`), the stop's idle (`stopping`) | **Kept** |
| Keeping time: on `catchUp`, the jump skipped and counted played; the rest owed while the writer is not idle; an idle latch that forgets the debt | **Deleted** (TM1). A skip happens only when the source's clock asks for one (4.2) |
| `Played{totalFrames, heardRawNs}`, its sequence lock, `playedChanged`, the played wake | **Deleted** (TM1). A clock reports in its own words (4.2) |
| `SourceCycle::facts`, `position`, `startRawNs`, `downstream` | **Replaced** by `SourceTick` (4.2): the tick is passed whole to the clock, and the ring itself reads none of it |

So a source with no clock, after a stall, plays its ring on in order: nothing is skipped and nothing is owed, and the
stall's time is simply time the writer did not need to fill (a writer that paces itself on another clock is a source
with a clock, and gives one).

### 4.2 The interfaces

In `ear/core/source.hpp` (the one home of the source, as built). `Played`, `SourceWriter::played()`,
`SourceWriter::playedWake()` and `SourceCycle` go; `Wake`, `SourceShape`, `SourceWriter`'s transport members,
`SourceBinding`, `Source` and `SourceAdapter` stay.

```cpp
/// What a source's ring holds as a tick begins, for its clock to decide on.
struct SourceHeld {
	std::uint32_t frames; // of the current stream, after any clear this tick applies
	bool clearing;        // this tick applies a clear: the ended stream's 5 ms plays faded, the rest is dropped
};

/// What one tick did with a source's ring, every count in the ring's frames (its rate: SourceTick::rate).
struct SourcePlayed {
	std::uint64_t skipped; // of the current stream, dropped unplayed at the clock's word: at most SourceHeld::frames
	std::uint32_t read;    // of the current stream, played into the bank (faded in where audio came back)
	std::uint32_t ended;   // of a stream a clear ended, played faded to zero: never the current stream's
	std::uint32_t zeros;   // played as zeros or an underrun's decay
	friend bool operator==(const SourcePlayed&, const SourcePlayed&) = default;
};

/// One tick as a source's clock sees it.
struct SourceTick {
	const GraphCycle& cycle; // the tick and its facts (docs/engine.md#graphcycle): the graph's, at the graph rate
	std::uint32_t frames;    // ring frames this tick plays: the cycle's, or behind a converter what its resampler asked
	std::uint32_t rate;      // the ring's rate: the graph frequency, or the converter's node rate (section 5)
	Latency playback;        // the most playback latency the block told on the bank, plus a converter's delay; graph frames
};

/// A source's own clock, reconciled with the tick inside the node (docs/architecture.md#the-timing-model, rule 3).
/// Realtime thread only, called by whoever plays the ring (the block, or the source's converter), never concurrently:
/// never blocks, locks, allocates or makes a system call. Owned by the source; freed with it (teardown step 5).
class SourceClock {
public:
	virtual ~SourceClock() = default;
	/// Before the ring plays this tick: how much of the current stream to drop unplayed first. The ring drops
	/// min(answer, held.frames), and nothing on a tick whose held.clearing is set (a clear ends the stream: nothing of
	/// the next is skipped for the old one's lost time). Any count: a stall has no bound.
	virtual std::uint64_t skipBefore(const SourceTick& tick, SourceHeld held) noexcept = 0;
	/// After it played. True rings the source's clock wake (SourceWriter::clockWake), at most once until answered.
	virtual bool played(const SourceTick& tick, const SourcePlayed& played) noexcept = 0;
};
```

`SourceWriter` loses `played()` and `playedWake()` and gains one member:

```cpp
	/// Rung when the source's clock asks after a tick (SourceClock::played returned true), at most once until answered.
	/// One watcher (a loop's onWake, slice 3c). Never rung for a source with no clock.
	virtual Wake& clockWake() noexcept = 0;
```

`CreateOf<spec>::source` (`ear/core/lifecycle.hpp`) and `CreateContext::source` behind it take the clock:

```cpp
/// A source on an inside input bank (checked at compile time, as built); `clock` null for a source with no clock.
template <class Bank> SourceWriter& source(Bank, SourceShape shape, std::unique_ptr<SourceClock> clock = nullptr);
```

A type's threads reach their clock's reports through the clock object it made (a reference kept before handing it
over: the source frees it at step 5, after the instance and every loop's state, so a reference held by them never
dangles). The report itself is the type's, in a lock-free mailbox it owns as a member of its clock (the realtime rules
already allow one, `docs/writing-a-node-type.md#the-code`); `sendspin` keeps its `Mailbox<PlayProgress>`.

*Why a hook on the source and not the type's code reading the ring:* the fades, a clear's 5 ms, the shortfall and the
stop's drop would otherwise be written again by each type, and the harness's play and the block's could disagree, the
class `companions.md` D-3b4 removed. The hook leaves the ring's rules in core and gives the node only the two decisions
that are its clock's: what to skip, and what to say.

*Why a wake on the source and not a type-owned wake:* the source already owns an eventfd-backed wake for the realtime
side (`EventSources`), the clock's report is its only use, and `node-primitives.md` G23 (types get no wakes of their
own) can stand.

### 4.3 One tick, in order

One pure function plays a source for a tick, `SourceRing::play(bank, tick, clock)`, so the block, a converter and the
harness all run the same code (D-3b4):

1. Apply a clear asked for: the ended stream's next 5 ms are queued to play faded, the rest dropped (counted).
2. `held` = the current stream's frames in the ring, and whether this tick cleared.
3. `skipped` = `clock && !held.clearing ? min(clock->skipBefore(tick, held), held.frames) : 0`; drop them.
4. Play `tick.frames`: the ended stream's faded frames, then the current stream's (faded in where audio came back),
   then an underrun's decay and zeros; the shortfall counts zeros while the writer is not idle.
5. `if (clock && clock->played(tick, {skipped, read, ended, zeros}))` ring the clock wake.
6. Wake a writer waiting for room if anything was consumed.

The block calls it for each bound source before the code (`docs/engine.md#sources`, unchanged otherwise), with
`SourceTick{cycle, cycle frames, graph rate, the playback latency told on the bank}`; `block_rules.hpp`'s
`sourceCycle` becomes `sourceTick`. A cycle too large to run plays no source (as built), so the clock sees the next
tick's `oversized` and decides.

### 4.4 Who changes

| Where | Change |
|---|---|
| `ear_core`: `source.hpp`, `source_ring.*`, `block_rules.hpp`, `node_host.*`, `lifecycle.hpp` | 4.1 and 4.2; `CreateOf::source` and `CreateContext::source` take the clock; NodeHost keeps it with the source |
| `ear_pipewire`: `sources.*`, `port_blocks.cpp` | `EventSources`: the played wake becomes the clock wake, `Played` goes; the block passes `SourceTick` |
| `ear_type_harness` | `h.played(bank)` goes; `h.lastPlayed(bank)` returns the last `SourcePlayed` (so a type's test can see what its clock was told); `h.feed` and `h.shortfall` stay. The harness calls a type's clock as the block does, by the same `SourceRing::play` |
| Tests | `source_ring_test.cpp`: the keep-time, debt, idle-latch and `Played` rows go; new rows: no skip without a clock (a `catchUp` of 2^32 + 8 frames plays the ring on), a clock's skip honoured exactly and capped at what is held, nothing skipped on a clearing tick, `SourcePlayed`'s four counts on every shape (a clear, an underrun, a return, a stop), the wake rung only on `true`. `source_test.cpp` (`EventSources` on real threads): the `Played` model and `judgeSource`'s dating go; a test clock records each tick, and the judge holds the ring's audio in order with each recorded skip, and the clock's `playback` against the latency model's. `port_block_test.cpp`: the harness and the real block compared on the recorded `SourcePlayed` and `SourceTick::playback` instead of `Played`. The latency table's source rows read `playback` through a test clock |
| `white`, `file`, `card`, `rtp` | **None.** `white` generates on the tick (a source whose clock is the tick); `file` is a sink; `card` and `rtp` reconcile in their engine companions (5.5) |
| `sendspin` | Section 6: first on today's feed (step T2), then onto a source with its clock (S3c.6) |
| Library docs | `docs/writing-a-node-type.md#sources` (keeping time, `Played` and the played wake replaced by the clock), `#configure-and-createof`'s row, `#testing-a-type`'s harness rows, `#latency`; `docs/engine.md#sources`, `#latency-per-bank-pair` and `#dating` (a source's `Played` goes from both); `docs/testing.md` where it names `Played` |

### 4.5 Sinks: a chunk is the tick's facts and nothing else

A sink keeps no time and never did: its ring stamps each tick's frames with that tick's `position`, `startNs`,
`startRawNs`, `causes` and `lostFrames` (a `catchUp`'s `jumpFrames`), and the capture latency the block told
(`upstreamFrames`), and splits a chunk at the ring's end by advancing the second call's start by the first's frames
(`cutChunk`, `docs/engine.md#sinks`) [V]. That is TM1 already: every field is a fact of the tick or of the latency
model, none a count the sink keeps across ticks. The rule this page adds, for the library docs: **a sink's ring never
skips, pads or re-dates audio; a consumer whose destination has a clock (sendspin's `to`, a later RTP sender)
reconciles in its consumer from the chunk's facts** (writing silence for `lostFrames`, dating by `enteredRawNs`).
`Chunk` and `ChunkStamp` do not change. Whether a capture's chunks in a catch-up's burst want `overdueNs` too is not
known [U: measured when `to` becomes a sink in S3c.6; captured audio waits in the card's buffer through a stall, so the
driver's nominal start may date it better than when the tick ran, the reverse of playback].

## 5. The converter companion

### 5.1 What it is, and where it sits

A converter companion joins one inside bank, like a card or an RTP receiver, and is the one place a ring at the node's
own rate meets the graph rate:

```
source:  type's thread --write--> ring (node rate) --> converter stream --links--> inside input bank --> code
sink:    code --> inside output bank --links--> converter stream --> ring (node rate) --consume--> type's thread
```

It is an engine node of ear's own making: a `pw_stream` made in-process by `ear_pipewire` (PipeWire's adapter around
the stream, with audioconvert and its resampler) whose format is the node's rate, linked channel for channel to the
bank by global id as every engine companion is (`docs/engine.md#engine-companions`). Its process callback, on the one
data loop, plays the source's ring through `SourceRing::play` for the frames the resampler asks (`pw_buffer::requested`,
since 0.3.49 [V]), or taps the sink's ring with what the resampler gives. The node's rate side is the stream's own
buffer, never a port of the graph, so TM4 holds: every port linked in the graph is at the graph rate.

*Why an engine node, when ring companions are served by their block (`companions.md` D-3b1, the owner's Q-3b1):* a
ring at another rate needs a resampler between it and the block, and TM5 makes that resampler PipeWire's, which runs
only in an adapter. What Q-3b1 was for holds: the converter plays only ticks the code is driven for, bound and unbound
by the same drive invoke on the same data loop (for a source the stream runs before the block in the cycle, its link
upstream of it; for a sink after it), so a start never plays audio written before it and a stop's silence ends both together. An unconverted source or
sink stays served by its block, as built.

### 5.2 How a node asks for one

On the request, beside the ring's shape (`ear/core/companions.hpp`, the closed list gains `CompanionKind::converter`,
wire name `"converter"`, engine name `<node>.<bank>.convert`):

```cpp
/// How a converter's ratio is kept.
enum class RatioMode { set, tracked };
/// A converter between a ring at `nodeRate` and an inside bank at the graph rate. Every member checked by the type's
/// parse first, as every engine companion's shape is.
struct ConverterShape {
	std::uint32_t nodeRate;     // 8000-768000 Hz; the graph frequency is allowed (drift only, no rate change)
	std::uint32_t targetFrames; // tracked: the ring's fill the converter steers to, in node frames; set: unused (0)
};

/// The node's hold on a converter's ratio. The mode is the request's template argument, so asking a set converter for
/// a rate report, or the reverse, does not compile.
class SetRatio {   // RatioMode::set: any thread, the realtime one included (a source's clock); lock-free
public:
	/// Node frames per graph frame from the converter's next tick on; 1.0 is no correction. Clamped to the converter's
	/// range [U: S7].
	virtual void set(double nodePerGraph) noexcept = 0;
protected:
	~SetRatio() = default;
};
class RateReport { // RatioMode::tracked: any thread
public:
	/// The node's clock as it measured it: its frames per second of CLOCK_MONOTONIC_RAW (a sender's nominal rate times
	/// its drift). The converter tracks it, and steers the ring's fill to targetFrames on top.
	virtual void report(double framesPerRawSecond) noexcept = 0;
protected:
	~RateReport() = default;
};
```

```cpp
// CreateOf<spec>:
template <RatioMode M, class Bank>   // an inside input bank (compile time)
ConvertedSource<M> convertedSource(Bank, SourceShape ring, ConverterShape conv,
                                   std::unique_ptr<SourceClock> clock = nullptr);
template <RatioMode M, class Bank>   // an inside output bank (compile time)
ConvertedSink<M> convertedSink(Bank, SinkShape ring, ConverterShape conv);
// ConvertedSource<set>{SourceWriter& writer; SetRatio& ratio;}, <tracked>{SourceWriter& writer; RateReport& rate;}
// ConvertedSink<M>{SinkRequest& sink; SetRatio& or RateReport& ...}: the sink's consumer is named as for any sink
```

The ring's `SourceShape::frames` and `SinkShape::frames` are in node frames. A source's clock behind a converter sees
`SourceTick::rate` = `nodeRate` and `SourceTick::frames` = what the resampler requested, and converts any graph-frame
fact it uses (a `jumpFrames`) through `FrameClock` (`docs/engine.md#dating`: through time, never a rate operand).
A converted sink's chunk carries its tick's facts unchanged and `frames` in node frames; its dating offsets go through
`FrameClock(nodeRate)` [U: `cutChunk` and `SinkRing` gain the ring's rate; designed in step T5 against S7].

### 5.3 Set versus tracked

- **Set.** The node runs the loop itself and gives the ratio (`SetRatio::set`: a source's clock each tick on the
  realtime thread, or a sink's consumer from its own thread, taken at the next tick). The converter applies it with `pw_stream_set_rate` (RT safe, "when the stream is using an
  adaptive resampler, adjust the resampler rate", since 1.4.0 [V]). For a node whose clock is visible only inside its
  protocol (a server's time filter).
- **Tracked.** The node reports its clock's rate when it learns it (`RateReport::report`, any thread); the converter's
  realtime side runs a delay-locked loop on the ring's fill against `targetFrames` (as `module-rtp`'s receiver runs one
  on its ring [U: S7 reads `module-rtp/audio.c` for its constants]) with the reported rate as its feed-forward, and
  applies the result the same way. For a source with no protocol clock (a pipe paced by a program,
  `programs.md` 2.2's paced source, which this mode is).

A converter never skips: lost ticks are its node's clock's to reconcile (`SourceClock::skipBefore` runs before the
ring plays, in node frames). In tracked mode a stall's backlog above `targetFrames` drains through the loop unless the
clock skips it (section 11, Q-TM2).

### 5.4 Latency (R1's one model)

The converter adds, beyond its bank: its ring (`Latency{0, capacity}` in node frames, as a source's ring is today, or
`targetFrames` to capacity in tracked mode) and the resampler's delay (`spa_io_rate_match::delay` and `delay_frac`, in
samples at the node's rate [V: `spa/node/io.h`]; `pw_stream_control::buffered` [V]). Both are converted to graph frames
once, by `FrameClock`, at the request (the delay for the quality chosen, [U: S7 measures that it is constant for a
quality and ratio range]), and entered as the bank's `LatencyPlan::beyond` `Delay`, as an RTP receiver's `latencyMs`
is (`companions.md` D-3b6). Only what no engine node reports goes in `beyond`: if the stream's ports report the
resampler's delay to the block as a peer [U: S7], the plan leaves it out, so nothing is counted twice. The source's
clock gets the bank's playback latency plus that delay in `SourceTick::playback`.

### 5.5 Cards and RTP receivers: already converters, kept as built

| Built | Its converter | Mode in this model's words | Change |
|---|---|---|---|
| Sound card (`AlsaSoundCards`) | audioconvert inside the ALSA adapter; the follower's loop on the device buffer steers it (`docs/engine.md#sound-cards`) [V] | tracked, the rate measured by PipeWire's ALSA node | none |
| RTP receiver (`RtpReceivers`) | the `module-rtp-source` stream's audioconvert; its loop on packet arrival and fill steers it (`docs/engine.md#rtp-receivers`) [V] | set, by PipeWire's module | none |
| Converter companion | an ear-made `pw_stream`'s audioconvert | set by the node's clock, or tracked from its report | new |

The converter generalises them for a ring whose clock is ear's code. Nothing moves: a card's and a receiver's loops are
PipeWire's own, tested on hardware or by the RTP tests, and rewriting them on the new kind would gain nothing. All three
are engine companions with one lifecycle (`joinEngineCompanion`), one readiness deadline each (the converter's: 5 s,
like a receiver's, [U: S7]) and no clock property (`docs/engine.md#the-quantum-guard`: the converter's test seam
refuses `node.force-rate` and friends as the card's does).

### 5.6 Spike S7, then tests

**S7** (test only, before any product code; S4 to S6 are taken): on PipeWire 1.4.2 in ear's in-process engine, an
in-process `pw_stream` at 44100 Hz linked to a block's inside input bank in a 48000 Hz graph:

- it runs in the graph group without moving the graph's rate (a single allowed rate: the receiver at 44.1 kHz is the
  precedent [V]), and without a force property;
- `requested` each cycle, and `pw_stream_set_rate` honoured from the process callback, its range and clamp;
- the stream's `io_changed` gives an `SPA_IO_Position` the converter can state a tick from (`CycleAccount` over the
  engine's cut record, so the clock behind it gets `SourceTick::cycle` with real facts), and the stream runs before the
  block in every cycle;
- the resampler's delay per quality, and whether the stream's ports report it as latency;
- `module-rtp/audio.c`'s loop constants, for tracked mode;
- the stream's events through the callback boundary (`pw_stream_new` plus a `StreamCallbacks` table; the guard
  already refuses `pw_stream_new_simple`).

**Tests, per executable** (step T5):

| Executable | Proves |
|---|---|
| `ear_core_tests` | The converter shape's checks; `convertedSource<RatioMode::set>` gives no `RateReport` (a compile-fail case); a converter on a bank with another companion is refused; the plan's `beyond` holds ring plus delay in graph frames, by `FrameClock` |
| `ear_pipewire_tests` | A 44.1 kHz sine through a converted source into a 48 kHz graph, recorded through a sink: its frequency per tenth of a second; set mode: a ratio of 1.001 for 2 s moves the ring's fill by the expected frames, within one block; tracked mode: a writer paced at 44100 x (1 + 200 ppm) on the raw clock holds the fill within a block of `targetFrames` after settling, with no underrun; the told latency against the model; ready and lost as an engine companion; the deadline; no force property (the seam refuses); bound and unbound by the drive (a stop plays nothing of the ring) |
| `ear_type_harness` | A fake converter that passes frames 1:1 and records each `set` and `report`, so a type's test checks what its clock asks for; real resampling is the engine's |

## 6. `sendspin` under the model

### 6.1 The wrapper's contract: the honest sink plus local skip

`sendspin`'s audio from the server is a source on `from_server` (S3c.6, `node-primitives.md` 4.1) with a clock,
`SendspinClock : SourceClock` (`sendspin_clock.hpp`, replacing `sendspin_code.*`'s playback half):

- **It plays every frame in order.** It never skips but as below, and never pads: a tick the feed leaves short plays
  zeros, which are not counted.
- **`skipBefore`**: on a tick whose facts state `catchUp` or `absent` with `jumpFrames > 0`, it answers `jumpFrames`;
  the ring skips min(jump, what it holds now). On any other tick, 0. A clearing tick skips nothing (the ring's rule).
- **`played`**: `played_ += skipped + read` (never `ended` nor `zeros`: the library wrote neither for its current
  stream), then publishes `PlayProgress{played_, finish}` (6.2) to its mailbox and returns true, every tick, with no
  condition. The worker's notifier, watching the clock wake, hands the library the difference with
  `notify_audio_played`.
- **It keeps no debt, defers nothing and has no `LibraryView`.** What the ring did not hold of a jump is the library's
  to correct: it sees it in the honest finish (its next chunk late by that much) and corrects once.

What the spike measured for exactly this wrapper (`honest-local`, 5 runs each, 4,250 of 4,250 judged cycles within
1 ms after recovery in every variant) [V]:

| Case | C4 as built (`real-all`) | Honest plus local skip (`honest-local`) |
|---|---|---|
| A 200 ms or 1 s stall, made upstream or in the block | no correction by the library; 27 to 160 ms of ear's own silence after the burst; back on the stamps 27 to 160 ms after it | exactly one correction by the library per run; ear's silence one block (5.3 ms); back on the stamps 11 to 101 ms after the burst (median 101: about one 96 ms chunk); until then audio plays late by the stall's remainder (160 to 1093 ms) |
| A 1.5-cycle hiccup made upstream (one block lost, the feed holding it) | no correction | no correction (without the local skip: one per run) |
| A starving network gap | two corrections | one correction (the late-chunk patch's) |
| Steady on the dummy beside `yes` x 4 | no drop | no library drop and no lost sync (without the local skip: 13 to 50 drops and 22 to 46 lost syncs a run) |

The trade the owner accepted (TM7): after a stall past the feed, about one library chunk of audio plays late by what
the feed did not hold, then the library corrects once; C4 played ear's silence instead and needed no correction.

### 6.2 The dating rule, and why it is clamped

The finish told is where the library's next frame plays: the end of this tick, from when it really ran, once what lies
downstream has played. For a tick that began on time that is its start as the driver dated it; for an overdue tick (a
catch-up's burst, run back to back) it is when the block began it, the next tick's due time plus how long that was
overdue, moved onto the raw clock (CLOCK_MONOTONIC and the raw clock differ by parts per million over the few
milliseconds between, nanoseconds [V: the spike's `realStartRawNs`]). That is a fact of the tick, so its home is
`GraphCycle`, stated once for every node:

```cpp
// GraphCycle (ear/core/node_type.hpp), a member function: no new field, no state.
/// When the block really began this tick, on CLOCK_MONOTONIC_RAW: startRawNs when it began on time; when it began
/// overdue, startRawNs + (nextNs - startNs) + overdueNs. 0 nextNs (a test's own tick) is taken as on time.
constexpr std::int64_t ranRawNs() const noexcept;
```

`SendspinClock::played` tells `finish = max(heard, lastFinish_)`, `heard = FrameClock(rate).heardRawNs(tick.cycle.ranRawNs(),
tick.frames, tick.playback)`.

**The clamp, decided.** An overdue tick's finish is dated from when it ran; the next tick, on time again, is dated from
its nominal start, which is earlier than the overdue tick's run by that tick's `overdueNs`; so the finish steps back by
it. The spike saw it under load (`honest-local`, one in-block 200 ms run of five: three steps of the reports) [V]. Two
fixes were open, clamping the finish, or dating an overdue tick that lost nothing by its nominal start. **The clamp is
taken**, because:

1. Dating a whole overdue tick nominally is C4's 9.10 finding again: a runner runs every tick of a burst and loses
   nothing, so every burst tick is whole and overdue, and nominal finishes trail now by up to the stall. Without the
   floor patch the library floors a playtime trailing now by 5 ms even with frames buffered (`cycle-facts.md` 9.9,
   finding 1), so it would realign mid-burst. Only the tick's run time is never behind now in a burst.
2. "Nominal for some overdue ticks" needs to tell a burst from one late wake, which no fact states (`overdueNs` is the
   same field in both); a rule built on a guess is the class `cycle-facts.md` removed.
3. The clamp's cost is bounded and one-sided: it holds the finish still for the tick after the last overdue one, late
   by at most the last overdue tick's `overdueNs` (less than a block plus that tick's work, since a tick overdue by a
   block or more has an overdue successor), and the tick after is nominal again and past it. The library sees one
   report less than a block late and never a step back; whether it reacts to that one report at all is what 6.4's
   step-back test measures under load [U: T2].

`lastFinish_` lives for the configuration: a stop and a start come later in time anyway, and a clear does not reset it
(the library re-anchors from the next finish, which is later still).

### 6.3 What is deleted

| Deleted | Where |
|---|---|
| `LibraryView` | `library_view.hpp` |
| The debt and the deferral: `owed_`, `catchingUp_`, `owingSuspended_`, `untoldSince_`, `untoldBound`, the overdue condition on telling | `sendspin_code.*` |
| `Feed::skipThenRead`, `Feed::Taken`, `onPaymentStep`, the payment's two passes | `feed.*` (S3c.6 then deletes the feed itself) |
| `cmake/sendspin-starved-floor.patch`, its entry in `cmake/SendspinDeps.cmake`, its library test's mention | the patch list, `AGENTS.md`, `docs/node-types.md#library-facts-and-carried-patches`, the `sendspin` skill |
| Their tests: `code_test.cpp`'s debt, starved-library, bound and payment rows; `feed_test.cpp`'s `skipThenRead` and payment-race rows; `clock_test.cpp`'s payment requirements (a remainder above 0 to pay, back within 2 + ceil(remainder / (feed - block)) blocks, `corrections == 0` after a stall, no debt forgotten while owed) and `Feed::holdWriter`'s use for them; `CycleJudge::librarySettle`'s causes `starvedCatchUp` and `forgottenDebt` | `src/nodes/sendspin/tests/`, `test/support/cycle_judge.hpp` |

Kept: `GraphCycle::overdueNs` (a tick fact, and the dating's input); `cmake/sendspin-late-chunk.patch` and its gap test;
`StallProbe` and the in-block stalls (the tests below use them); `Feed::dryReads`, `dryFrames` until S3c.6 (then the
source's shortfall); the settle causes `dry` and `logged`.

### 6.4 The tests that replace C4's

Each through the real server and the real engine (`clock_test.cpp`), judged by the cycle judge, except the unit rows:

- **One library correction after a stall.** The 200 ms and 1 s stalls, made upstream (`StallProbe`) and in the block:
  every run, exactly one correction by the library (a logged hard sync or a late chunk's front dropped) of what the
  clock could not skip (the stall's lost frames less what the feed held), within a tolerance T2 fixes from the spike's
  runs and states with its reason; ear's own silence after the
  burst at most one block; back on the server's stamps within one chunk and two blocks of the burst's end; no
  `Buffered frames underflow`; every report's frames exactly `skipped + read` (6.1) and every finish by 6.2.
- **An exact skip on a host-missed cycle.** The 1.5-cycle hiccup upstream of the node (one block cut, the feed holding
  it): the clock skips exactly `jumpFrames`, the ramp steps by exactly that, the report moves by `jumpFrames` plus the
  tick's read, and the library makes no correction, every run. A unit row on the harness with scripted facts: a
  `catchUp` larger than what is held skips what is held and no more; `absent` skips; `reposition`, `driverChange`,
  `start` and `oversized` skip nothing; a clearing tick skips nothing.
- **No step back in reported finishes.** A unit row: scripted ticks of a burst (overdue, then on time) and one overdue
  whole tick under load; every finish at least the last, the clamp holding exactly the tick after the last overdue one,
  and from the next tick on the nominal finish. In the clock tests every report's finish is checked against the last
  under the in-block stalls and the loaded steady runs (a step back fails the test, naming both ticks' facts).
- **Steady and gap**, kept: the steady tests on the dummy and a following card (750 cycles within 1 ms; the judge's four
  induced stalls each a local skip with no library correction), and the gap test (one correction, the late chunk's).

### 6.5 The upstream reports

- **The floor patch's report is withdrawn.** Its first half asked the library to floor only when nothing is buffered,
  because ear told finishes behind now during a burst. The honest finish never trails the tick's run, so the library's
  floor as upstream wrote it is right for ear, and the patch goes (TM7).
- **The late chunk's report stays**, rewritten as its own: a sink that reports honestly still starves the library in a
  network gap, and `decode_chunk` then skips a chunk whose front is late but whose end is not (the gap test fails 5 of
  5 with the patch's change reverted [V: `cycle-facts.md` 9.9]). The `sendspin` skill's draft keeps only that half and
  its two tests (`AChunkIsTooLateOnlyOnceItsLastFrameIs`, `AChunkLateAtItsStartGetsItsFrontDropped`), and the
  measurement becomes the honest wrapper's.

## 7. Order of build steps

Each step is one reviewable change, green on every test and `make format-check`, changing the library pages it makes
stale in the same commit.

| Step | What | Deletes | Behaviour | Tests it adds |
|---|---|---|---|---|
| **T1** | `GraphCycle::ranRawNs()` | nothing | none | the rule's rows: on time, overdue, a test's tick with no `nextNs` |
| **T2** | `sendspin` honest, on today's feed and the legacy adapter: `SendspinCode` keeps the capture half and becomes the honest wrapper with the local skip and the clamped finish (6.1, 6.2); the floor patch dropped; `docs/node-types.md#clock`, the skill (and its report, 6.5), `AGENTS.md`'s patch list | 6.3's table | yes: one library correction after a stall past the feed, where ear's silence was | 6.4 |
| **T3** | The clockless source: `SourceClock`, `SourceTick`, `SourceHeld`, `SourcePlayed`, `clockWake`; `SourceRing` without time (4.1 to 4.3); `EventSources`, the block, the harness; `docs/writing-a-node-type.md#sources`, `docs/engine.md#sources`, `#latency-per-bank-pair`, `#dating` | `Played`, the played wake, `SourceCycle`, the keep-time and debt code | none for any shipped type (none uses a source) | 4.4's rows |
| *S3c.1 to S3c.5* | As `node-primitives.md` 13 has them | | | |
| **S3c.6** | As planned, with the honest wrapper moved onto `from_server`'s source as `SendspinClock` (the feed, its skip and its dry counts go; the clock's report replaces the bell and the played wake in `node-primitives.md` 4.1's worker) | `feed.*`, `sendspin_code.*`'s playback half | as `node-primitives.md` 8 says, and no timing change: T2's tests pass unchanged on the source | T2's tests on the source; the clock's unit rows through `TypeHarness<spec, Sendspin>` |
| **T4** | Spike S7 (5.6), test only | nothing | none | S7's findings into this page |
| **T5** | The converter companion: `CompanionKind::converter`, `ConverterShape`, `SetRatio`, `RateReport`, `convertedSource`, `convertedSink`, `Converters` in `ear_pipewire`, the harness's fake; `docs/engine.md#engine-companions`, `docs/writing-a-node-type.md#engine-companions` | nothing | none until a type asks for one | 5.6's table |

**How it fits slice 3c and the rest.** T1 to T3 come before S3c.1: they touch only the source, `GraphCycle` and
`sendspin`'s code, none of which S3c.1 to S3c.5 change, and they shrink S3c.6 (the source it moves onto keeps no time,
and its clock is already written and proven). T2 needs T1; T3 needs neither and may land first. `cycle-facts.md`'s open
steps (D3, C6, C7, C8) are independent of all of them. T4 may run at any time; T5 comes after S3c.6 (it uses 3c's
`ResourceAdapters`), and is built when Q-TM1 says.

## 8. What this changes on other design pages

| Page | Change |
|---|---|
| `cycle-facts.md` | Q-CF2 reversed (TM7); 4.4's `SourceRing`, `Played` and `SendspinCode` rows superseded; C4a and C4b's machinery deleted by T2 and T3; the follow-ups section 9.15 lists are moot |
| `companions.md` | D-C4 (a source keeps time by default) and D-3b3 (`Played`'s meaning) reversed; 3.3's "keeping time" and "says what played" are this page's 4.1 and 4.2 |
| `node-primitives.md` | 4.1's worker watches the source's clock wake, not a played wake; 6.2's list of the framework's wakes; 12's `sendspin_code.*` and `mailbox.hpp`; 13's S3c.6 |
| `programs.md` | 2.2's paced source is a source with a converter companion in tracked mode (5.3) |

## 9. Decided here (technical)

Each the designer's; the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| D-TM1 | A source's clock is a hook the ring calls around its play (`SourceClock`), not the type's code reading the ring | The ring's rules stay written once (D-3b4); the node decides only what its clock must |
| D-TM2 | The clock's report rides the source's own wake (`clockWake`), the report itself in a mailbox the type owns | One wake per source as built; G23 stands |
| D-TM3 | `SourcePlayed` splits a cleared stream's faded 5 ms (`ended`) and the zeros from what was read | A clock counting the old stream's tail as played would put the library's new stream 5 ms off |
| D-TM4 | `ranRawNs()` is a member of `GraphCycle`, the clamp is `sendspin`'s | When a tick ran is a tick fact for every node (TM1); never stepping back is a promise to one library |
| D-TM5 | Clamp, not nominal dating, for an overdue tick (6.2) | Nominal dating of a burst re-creates 9.10 without the floor patch; the clamp's error is bounded and one-sided |
| D-TM6 | Sinks unchanged; their consumers reconcile | A chunk is already the tick's facts and the model's latency |
| D-TM7 | The converter is an ear-made `pw_stream` engine companion that plays (or fills) the ring at the node's rate; cards and receivers stay as built | The resampler is PipeWire's only inside an adapter (TM5); binding by the drive keeps Q-3b1's exactness |
| D-TM8 | The converter never skips; skipping is the node's clock's | TM1: the framework keeps no audio time, the converter included |
| D-TM9 | T2 lands on today's feed before the source changes | The behaviour change is measured alone, against C4's tests, before any move |

## 10. Rejected alternatives

| Alternative | Why not |
|---|---|
| Keep `Played` beside the clock, as a framework count | A count the framework keeps is audio time it keeps (TM1); `sendspin` could not use it (it would count frames the library never wrote, `cycle-facts.md` 9.7) |
| The type's own code reads the ring through `Io` | Every type would write the fades, a clear's tail and the shortfall again; harness and block could drift |
| An in-code resampler for converted rings | TM5: conversion is PipeWire's converter |
| Re-implement cards and RTP receivers on the converter kind | Their loops are PipeWire's and already right; nothing would be gained |
| The converter skipping a stall's backlog on `catchUp` | The framework would be keeping time again (TM1); a node that wants the skip gives a clock |
| Date an overdue tick that lost nothing by its nominal start | 6.2, reasons 1 and 2 |

## 11. Questions for the owner

**Q-TM1. Should the converter companion be built now, or with the first node that needs it?**

*Background.* Nothing in ear needs it today: `sendspin` asks the server for the graph's own rate (the server
resamples), and cards and RTP receivers already have PipeWire's converter inside them. Its first real user would be
AirPlay (slice 9: shairport-sync's audio is 44.1 kHz into a 48 kHz graph), or a Sendspin stream taken at its own rate.

*Scenario.* Built now (T5 after slice 3c), it is a few hundred lines and engine tests that only test code exercises
for months, and slice 9 starts with it ready. Built with AirPlay, slice 9 carries T5, and the design (section 5) and
the spike (S7) wait with it or run now.

*Recommendation:* run spike S7 now (cheap, test only, and it settles section 5's unknowns while PipeWire 1.4.2 is
fresh), and build T5 with its first user.

**Q-TM2. After the host stalls, should a source ear adds later jump to stay in time, or slow down to avoid the jump?**

*Background.* After a stall a network source holds audio that is now late. `sendspin` (decided, TM7) skips what it
holds of the lost time at once, and the library corrects the rest: a short jump, then in time. A source behind a
converter in tracked mode could instead play the late audio slightly fast until it is back on time: no jump, but out
of time meanwhile. This is a default for the sources still to come (AirPlay's paced source, a later RTP sender), each
of which can still choose otherwise in its own clock.

*Scenario.* The kitchen plays AirPlay in a group with other AirPlay speakers, and the host stalls 300 ms. Jump: you
hear a 300 ms skip and the kitchen is in time with the others at once. Slow catch-up: no skip, but the kitchen trails
the others, by 300 ms at first, for about a minute at 0.5% speed-up.

*Recommendation:* jump, as `sendspin` does: rooms out of time with each other are the more noticeable fault, and one
rule across ear's sources is easier to reason about.
