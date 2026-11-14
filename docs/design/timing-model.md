# The timing model: one tick, one rate, clocks at the edges

Status: designed to module level (2026-11-13), part of slice 3c (`node-primitives.md`); Q-TM1 answered (RTP is the
converter's first user), Q-TM2 narrowed and its default decided unless the owner objects; T1, T2 and T3 built (T3: the
source keeps no time and a source's clock decides its skip, now `docs/writing-a-node-type.md#a-sources-clock`; 4.6),
T4 built (the converter's base in module-run mode, RTP on it: 5.6); nothing else of it built beyond what section 2
says is already true. It states how audio time works in ear (the owner's model, section 1), takes the
framework's timekeeping out of the source (`SourceRing`'s `Played` and its keep-in-step), moves `sendspin`'s sync onto
the spike's "honest sink plus local skip" (section 6), and adds the converter companion (section 5). The model itself
is `docs/architecture.md#the-timing-model`, which says where the code still departs from it; this page designs the
change that removes each departure. It reverses `cycle-facts.md`'s Q-CF2 and slice 3b's design that a source keeps
time by default and says what played (section 8). When it is built it is deleted, and what lasts moves into
`docs/architecture.md`, `docs/engine.md`, `docs/writing-a-node-type.md`, `docs/node-types.md#clock` and the `sendspin`
skill.

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
| TM4 | **One rate inside the graph.** Every port inside the graph runs at the graph rate; conversion happens only in edge nodes. The graph rate is configurable (`graph { rate ...; }`, flag fallback) but fixed for the engine's life, and it is the only allowed rate. Mixed-rate graphs (two graphs joined by a converting edge) are out of scope | Built; the converter's node-rate side is never a graph port (5.1) |
| TM5 | **Conversion is a converter companion.** For rate and drift conversion the framework provides a companion: PipeWire's converter (audioconvert in an adapter, with its resampler) wired between the node's own side and the graph, hidden from the graph file, owned by the node. The node either sets the ratio or lets the converter track a rate it reports. Library functions remain fine for simple in-code work such as sample-format conversion. This generalises what cards and RTP receivers already do | Section 5 |
| TM6 | **The internal clock drives.** Cards follow it and adapt to the graph: the existing choice, kept and stated | Built (`docs/engine.md#the-internal-clock`) |
| TM8 | (Q-TM1, 2026-11-13) **RTP is the converter companion's first user.** Build the converter out of the RTP companion's wiring (module load, finding its node, linking, binding, latency, lifecycle and failure) with a third mode, module-run, where a PipeWire module owns the stream, its resampler and its loop. RTP moves onto the shared code with no behaviour change, its tests unchanged; RTP reception is not reimplemented (PipeWire keeps the jitter buffer and the clock tracking). Set and tracked are built on that base with their first user (AirPlay for tracked). The step moves earlier, with spike S7 folded in; check whether the card fits the base | Section 5; step T4 |
| TM9 | (Q-TM2, 2026-11-13, narrowed by the owner) A host stall stalls the whole graph, one tick and one data thread, so every node inside ear resumes together and stays in time with the others. The jump-or-speed-up default concerns only a source whose own clock falls behind while the graph ticks on, and staying in time with what is outside ear after a graph-wide stall | Section 11; the default "jump" decided unless the owner objects |
| TM7 | **Accepted consequences.** `SourceRing`'s timekeeping leaves the framework (its Played count and dating, its keep-in-step across stalls), reversing Q-CF2; the source node keeps its own time. `sendspin`'s sync stays in its node with the spike's winning design, "honest sink plus local skip": play every frame in order; on a tick stating a catch-up or absent jump skip min(jump, what the feed holds now); report every tick played = skipped + read with an honest finish that never steps back; no debt, no deferral, no `LibraryView`. Only `cmake/sendspin-late-chunk.patch` remains of the timing patches (`sendspin-starved-floor.patch` is dropped). C4's machinery is deleted (`LibraryView`, owed/catchingUp/untoldBound, `Feed::skipThenRead`, the overdue deferral, their tests); `GraphCycle::overdueNs` stays, as a tick fact | Sections 4, 6 and 7 |

*Amended by the design review (2026-11-13, M1):* TM7's "defers nothing" is narrowed. `sendspin` keeps C4's rule
that an overdue tick tells the library nothing, under a bound, because in a burst that loses nothing every other
dating makes the library correct twice (6.1); what goes is the debt, `LibraryView`, `skipThenRead` and the bound's
owing role. The owner can overturn it.

*Amended by the re-check (2026-11-13):* TM7's "only `cmake/sendspin-late-chunk.patch` remains" is narrowed too.
`cmake/sendspin-starved-floor.patch` is **kept**, with the deferral, as C4's proven pair: while ear defers, the
library's playtime runs open-loop and trails now, and upstream's floor would fire on it mid-burst (6.1, 6.5). Dropping
it later is a separate step, gated on evidence [U: 6.5].


## 2. What is built, against the model

| Rule | Built | Departs |
|---|---|---|
| TM1 | `GraphCycle` states the tick: `startNs`, `startRawNs`, `position`, `rateDiff`, `driverId`, `nextNs`, `overdueNs`, `facts` (`docs/engine.md#graphcycle`) [V]; since T3 `SourceRing` keeps no time: a source skips only what its `SourceClock` asks (`docs/writing-a-node-type.md#a-sources-clock`) [V] | — (until T3: `Played`, the played wake, and the keep-in-step of a `catchUp` with the rest owed) |
| TM2 | The pass-through (`card`, `rtp`, `sendspin`'s public banks) reads and writes on the tick; no other transform exists yet | — |
| TM3 | A card's ALSA node and an RTP receiver reconcile their clocks in PipeWire (a delay-locked loop steering a resampler) [V]; `sendspin` reconciles the server's clock in its code | `sendspin`'s code owes lost time and models the library (`LibraryView`, the debt, `untoldBound`'s owing role, `Feed::skipThenRead`) (`docs/node-types.md#clock`) [V]; its overdue deferral and `sendspin-starved-floor.patch` stay |
| TM4 | One rate and one quantum (`docs/engine.md#the-clock`, `#the-quantum-guard`); a 44.1 kHz RTP sender into a 48 kHz graph is resampled by its receiver [V] | — |
| TM5 | Cards and RTP receivers are PipeWire's converter inside their engine companions (5.5); the converter's base in module-run mode, RTP its user (T4, `docs/engine.md#module-run-converters`) | No converter companion for a ring whose clock is a type's own (set and tracked, T5) |
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
with a clock reconciles from them. The **converter companion** is PipeWire's converter on an inside bank, built from
the RTP receiver's wiring: in **module-run** mode a PipeWire module owns the stream, its resampler and its loop (RTP,
moved onto the shared code first with no change of behaviour); in **set** and **tracked** modes, built with their
first users, an ear-made stream plays a ring at the node's own rate, the ratio set by the node's clock or tracked from
a rate it reports. Its delay enters the latency plan beyond its bank; the card shares its lifecycle but not its base. `sendspin`'s source clock is the honest sink plus local skip, telling the library
only from a tick that began on time (under a 2 s bound), so every report is true when given; C4's debt, `LibraryView`
and `skipThenRead` go; the deferral and the floor patch stay, C4's proven pair.

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
class that keeping every source rule in core's one `SourceRing` removed (`docs/writing-a-node-type.md#sources`). The
hook leaves the ring's rules in core and gives the node only the two decisions that are its clock's: what to skip, and
what to say.

*Why a wake on the source and not a type-owned wake:* the source already owns an eventfd-backed wake for the realtime
side (`EventSources`), the clock's report is its only use, and `node-primitives.md` G23 (types get no wakes of their
own) can stand.

### 4.3 One tick, in order

One pure function plays a source for a tick, `SourceRing::play(bank, tick, clock)`, so the block, a converter and the
harness all run the same code (as every source rule already is, `docs/writing-a-node-type.md#sources`):

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
| System tests' windows that assume a source keeps in step | **Checked in T3, the code kept; the note adjusted** (4.6). The windows this row named, `rampDueBefore`, `rampDueAfter` and `unpaidRampSkips`' reckoning of each ramp skip's due time (`test/system/sendspin_system.hpp`), never read `SourceRing`: no shipped type uses a source, and the only node upstream of a file in a system test whose skips they reckon is `sendspin`, on its own feed. They rely on `sendspin`'s own clock (T2, `docs/node-types.md#clock`), which skips at most the graph's jump (min(jump, what its feed holds)), and on a cycle cut after its block showing as the block it lost: so the skips before a WAV frame never add up to more than the cycles the file node lost before it, and a due time worked out from the skips is never later than the frame was due, only earlier by what a stall past the feed left unskipped. T3 changes neither, so they hold as they stand; their comments and `docs/testing.md#the-counters-own-word` now say what they rely on. The premise this row feared, a clockless source upstream of a file (a cut while its ring held audio drops nothing and skips nothing, so the WAV loses time with no step), exists in no system test; **the first step that puts a source with no clock, or one whose clock skips on its own reckoning (Q-TM2's jump), upstream of a file in a system test reworks the reckoning then**, from that source's own reports (`SourcePlayed::skipped`) rather than the WAV's steps. `sendspin` on a source (S3c.6) keeps T2's rule as its `SendspinClock`, so these windows hold there too. No other window was found relying on keep-in-step (the source stamps' window is `sendspin`'s capture, dated by the graph's cycles, not a source's play); the stamp window's 2 s `captureLag` was replaced in T3 by the measured arrival (4.6) |
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

### 4.6 Found building T3

T3 is built (2026-11-13); 4.1 to 4.3 hold as designed, now `docs/writing-a-node-type.md#sources` and
`#a-sources-clock`, `docs/engine.md#sources`. What building it settled:

- **The source owns its clock, so the tap carries it.** `SourceAdapter::make` takes the clock and the made `Source`
  keeps it, freed with it at teardown step 5 (`node_host_test.cpp` holds the order: after the instance, after the
  source's own destructor runs). `SourceTap::play(bank, SourceTick)` is the realtime end, implemented by
  `EventSource` and the harness's fake source by calling `SourceRing::play(bank, tick, clock)`; `SourceRing` is no
  longer a `SourceTap`, and `SourceBinding` is unchanged. NodeHost only hands the clock over.
- **`SourceRing::play` returns what it told the clock**, the same `SourcePlayed` with no clock: the harness's
  `lastPlayed` is that return, so a clockless source's counts are visible to a type's test too.
- **The clock is asked on every tick**, a clearing one included (its answer then ignored), so it sees every tick it is
  told about; a stop's first tick asks with `held` empty.
- **`zeros` covers a clear's fade past the ended stream's frames**: a clear with less than 5 ms written plays those
  frames (`ended`) and then decays from the last of them, which is neither stream's audio.
- **A skip is no drop**: `droppedFrames` (the node's `dropped=` count) stays a clear's and a stop's; a skip that
  leaves the ring dry makes a shortfall, as any dry frame does while the writer is not idle.
- **The real block, with a clock**: on S5 part A's 30 stalls of 12 ms the clock asked each catch-up's jump and the ring
  dropped exactly it (30 skips, 15,360 frames, none past what it held), the listener after the block hearing every
  frame the clock was told it read; with a clock that asks nothing, the ring played on in order across the judge's
  cuts.
- **The system tests' windows (4.4)**: checked, not reworked: they rely on `sendspin`'s own clock, which T3 does not
  touch. **The stamp window's follow-up was folded in**: a source stamp's skip is now dated from the chunk before it,
  its stamp read onto the wall clock by the quickest arrival measured (each chunk's server-side lag and when the test
  read it) and less 100 ms, in place of the arrival before it less a fixed 2 s `captureLag`.

## 5. The converter companion

### 5.1 What it is: one base, three modes

A converter companion is an engine companion that joins one inside bank and is the one place a clock and a rate other
than the graph's meet the graph: PipeWire's converter (audioconvert in an adapter, with its resampler) between the
node's own side and the bank, its graph side linked channel for channel to the bank, its own side never a port of the
graph (TM4). Who keeps the ratio is its **mode**:

| Mode | Who owns the stream, the resampler and the loop | First user | Built in |
|---|---|---|---|
| **module-run** (no enum: a shape's own) | A PipeWire module: it makes the stream, keeps its own buffer and runs the loop that steers its resampler. ear loads it, finds its node, links it, carries its latency and judges its life | `rtp`: its receiver is `module-rtp-source`, with its jitter buffer and its clock tracking (the owner, Q-TM1) | T4 |
| **set** | ear: an ear-made stream plays (or fills) a ring at the node's rate; the node's clock gives the ratio | none yet | T5, with its first user |
| **tracked** | ear: as set, but the converter runs a loop on the ring's fill, the rate the node reports as its feed-forward | AirPlay's paced source (slice 9, `programs.md` 2.2) | T5, with AirPlay |

**The base is built out of the RTP receiver's wiring** (Q-TM1). Everything `RtpReceivers` does today that is not about
RTP becomes the converter's, and RTP becomes its first module-run user with no change of behaviour:

| What the base does | Today [V] | After T4 |
|---|---|---|
| Load the module, its arguments built only from parsed values | `RtpReceiverBody::load`, `moduleArguments` (`rtp_receivers.cpp`) | `ModuleConverterBody::load`; the arguments stay the module's own function |
| Find the node the module made: a registry and a `pw_core_sync`; nodes of the name seen before its done are stale; the first other one is bound; a stale id is forgotten when its global goes | `RtpReceiverBody` (`stale_`, `loading_`, `syncSeq_`) | `ModuleConverterBody`, unchanged |
| Hear its state (`tellNodeState`: RUNNING, ERROR) | the body's `onInfo` | the same |
| Link, ready, lost, faulted, torn down; links by global id | `joinEngineCompanion` (`engine_companions.hpp`) | unchanged: already every engine kind's |
| Latency: what the module buffers lies `beyond` the bank (rtp-source's ports report 0) | NodeHost's plan: an RTP receiver's `latencyMs` as a `Delay` | the shape's `moduleRunDelay`, one rule for every module-run converter |
| Readiness deadline and its failure text | `rtpReceiverReadinessBound` (5 s), `the RTP receiver never ran (UDP port <p> held?)` (`node_host.cpp`) | per module; RTP's unchanged |
| No clock property on what ear makes | the property table's `Home::rtpStream` rows | unchanged |

RTP reception is not reimplemented: PipeWire keeps the jitter buffer, the tracking of the sender's clock and the
resampler, and every quirk `docs/engine.md#rtp-receivers` lists stays PipeWire's. What moves is ear's code around it.

### 5.2 The interfaces

**`ear_core` (`companions.hpp`).** The type's API does not change for RTP: `cx.rtpReceiver(bank, RtpReceiverShape)`,
`CompanionKind::rtpReceiver` and the wire's `"rtp-receiver"` stay. T4 gives core only the one latency rule, so
NodeHost's plan names no RTP field; module-run is a shape's own (`ModuleRun<Shape>`), so no enum names it:

```cpp
/// A module-run converter's shape is its module's own (RtpReceiverShape today). Each says what the module buffers beyond
/// the bank, at the graph rate: the one rule NodeHost's latency plan reads, in place of its RTP branch.
Delay moduleRunDelay(const RtpReceiverShape& shape, FrameClock graph); // latencyMs, through FrameClock
```

**`ear_pipewire` (private).** The receiver's body becomes the module-run converter's, and a module says only what is
its own:

```cpp
// converter_modules.hpp: what one module-run converter's module is.
struct ConverterModule {
	std::string_view kind;                     // "RTP receiver": its loss details' word, as today
	std::string_view module;                   // "libpipewire-module-rtp-source"
	ChannelFlow flow;                          // nodeToBlock: a receiver feeds an inside input bank
	PortName (*nodePort)(std::size_t channel); // "receive_AUX<n>" (a node.network stream's ports)
};
template <class Shape> struct ModuleRun; // per shape: `static constexpr ConverterModule module;` and
                                         // `static std::string arguments(const NodeName&, const Shape&, std::size_t)`

// module_converters.hpp: the shared body and the adapter every module-run shape uses.
class ModuleConverterBody final : public CompanionBody, public FaultOwner { /* today's RtpReceiverBody, generalised */ };
template <class Shape> class ModuleConverters final : public EngineCompanionAdapter<Shape> {
public:
	explicit ModuleConverters(Engine& engine);
	std::unique_ptr<EngineCompanion> make(const CompanionTarget& target, const Shape& shape,
	                                      EngineCompanionEvents& events) override; // joinEngineCompanion + the body
};
using RtpReceivers = ModuleConverters<RtpReceiverShape>; // the name main.cpp and the tests use, kept
```

**The set and tracked modes (T5).** Designed now so T4's base leaves room for them, built with their first user,
`RatioMode` with them (it chooses only between them; nothing in T4 reads it): an
ear-made `pw_stream` body (`StreamConverterBody`, its events through the callback boundary as a `StreamCallbacks`
table; the guard already refuses `pw_stream_new_simple`) on the same `joinEngineCompanion`, playing a source's ring
through `SourceRing::play` for the frames the resampler asks (`pw_buffer::requested`, since 0.3.49 [V]), or filling a
sink's ring:

```cpp
/// Who keeps an ear-made converter's ratio (T5).
enum class RatioMode { set, tracked };
/// A converter between a ring at `nodeRate` and an inside bank at the graph rate (set and tracked modes).
struct ConverterShape {
	std::uint32_t nodeRate;     // 8000-768000 Hz; the graph frequency is allowed (drift only, no rate change)
	std::uint32_t targetFrames; // tracked: the ring's fill the converter steers to, in node frames; set: 0
};
class SetRatio {   // RatioMode::set: any thread, the realtime one included (a source's clock); lock-free
public:
	/// Node frames per graph frame from the converter's next tick on; 1.0 is no correction. Clamped [U: T4's checks].
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
- **Tracked** runs a delay-locked loop on the ring's fill (as `module-rtp`'s receiver does on its buffer [U: T4 reads
  `module-rtp/audio.c` for its constants]), the reported rate its feed-forward, applied the same way. **A skip
  re-targets the loop:** the converter plays the ring, so it sees each tick's `SourcePlayed::skipped`; on a tick that
  skipped it resets the loop's integrator and takes the fill after the skip as its new reference, so the loop never
  steers fast for frames the clock already dropped. One controller acts on the fill at a time: the clock by a skip,
  the loop by the ratio between skips. T5's tests include a skip of half the ring while tracking: the ratio after it
  stays within the loop's steady range, with no run toward the old fill.
- A ring behind a converter is in node frames: `SourceTick::rate` is `nodeRate`, `SourceTick::frames` what the
  resampler asked, and a clock converts a graph-frame fact (a `jumpFrames`) through `FrameClock`. A converted sink's
  chunk keeps its tick's facts and carries node frames [U: `cutChunk` and `SinkRing` gain the ring's rate, in T5].
- *Why an engine node, when ring companions are served by their block* (`docs/architecture.md#companions`, the
  owner's answer "Served by the node"): the resampler is PipeWire's only inside an adapter (TM5). What that answer was
  for holds: the stream plays only ticks the code is driven for, bound and unbound by the same drive invoke on the same
  data loop (for a source it runs before the block, for a sink after it). An unconverted source or sink stays served by its block.
- **No mode skips.** Lost ticks are the node's clock's (`SourceClock::skipBefore`); in tracked mode a backlog above
  `targetFrames` drains through the loop unless the clock skips it, which by Q-TM2's default it does.

### 5.3 Latency (R1's one model)

A converter's delay is latency beyond its bank, entered once as `LatencyPlan::beyond` (`docs/engine.md#latency-per-bank-pair`), and
only what no engine node reports, so nothing counts twice:

- **module-run**: the module's buffer, by the shape's `moduleRunDelay` (RTP: `latencyMs`, exactly as today; T4 moves
  the rule, not its value).
- **set and tracked**: the ring (`Latency{0, capacity}`, or `targetFrames` to capacity when tracked) and the
  resampler's delay (`spa_io_rate_match::delay` and `delay_frac`, at the node's rate [V: `spa/node/io.h`];
  `pw_stream_control::buffered` [V]), converted to graph frames by `FrameClock`, unless the stream's ports report the
  delay to the block as a peer [U: T4's checks].

A source's clock gets the bank's playback latency plus that delay in `SourceTick::playback`.

### 5.4 The card: the same lifecycle, not the converter's base

A sound card is PipeWire's converter too (audioconvert inside the ALSA adapter, the follower's loop on the device
buffer steering it: `docs/engine.md#sound-cards`): in the model's words a PipeWire-run converter. It **stays as it
is**, because what the converter base adds over the shared lifecycle is exactly what a card does not need:

| | Module-run converter (RTP) | Card |
|---|---|---|
| Made by | loading a module, which makes the node on its own connection | `pw_core_create_object("adapter", ...)` on ear's connection: ear holds the proxy |
| Found by | a registry, a sync and stale names | its own proxy: nothing to find |
| Ports | the module's (`adapter.auto-port-config`) | ear sends `PortConfig` itself (auto-port-config fails for the ALSA adapter on 1.4.2) |
| Latency | the module's buffer, `beyond` the bank (its ports report 0) | reported through its links (ALSA's headroom); nothing `beyond` |
| Clock | follows the graph | follows the graph, with the quantum guard's period sizing and `refuseClockProperties` |
| Lifecycle, links, readiness, loss, faults | `joinEngineCompanion` | `joinEngineCompanion` |

The two already share what they have in common, the lifecycle; moving the card onto the module base would add a
registry search it does not need and gain nothing. T4 confirms this against the code and says it in
`docs/engine.md#engine-companions`.

### 5.5 Step T4: the base, RTP on it, and S7's checks folded in

**What it builds.** `ModuleConverterBody` and `ModuleConverters<Shape>` from `RtpReceiverBody` and `RtpReceivers`;
`ModuleRun<RtpReceiverShape>` holding what is RTP's (the module, its arguments, its port names, its word, its failure
text); `moduleRunDelay` in core, NodeHost's latency plan and readiness text reading them in place of
its RTP branches. `RtpReceivers` stays the name `main.cpp` and the tests use. Docs: `docs/engine.md#engine-companions`
and `#rtp-receivers` (where the code lives; nothing an operator sees), and `docs/architecture.md#the-timing-model`'s
converter departure narrowed to the set and tracked modes.

**No behaviour change, proven by tests that do not change.** Every RTP test passes unedited:
`rtp_receiver_test.cpp` (among them "L16 sent at 44100 into the 48000 graph is resampled", the property table's
`receiverResamples` row), the RTP rows of `node_host_test.cpp` and of the readiness deadlines, `test/system/rtp_test.cpp`
and the `rtp` type's own. The property table's checks pass unchanged (the arguments are the same keys from the same
rows). A test that needs editing means behaviour moved, and the step is wrong.

**New tests.** `ModuleConverterBody` through a second, test-only module-run shape [U: which PipeWire 1.4.2 module
serves as a stand-in; T4 picks]: the stale-name rule, a module that will not load, a node that never runs past its
deadline, a node removed after ready, each through the shared code with the module's own word in the detail.

**S7's checks, folded in** (test only, in `spikes_test.cpp`, kept as regressions; S4 to S6 are taken): what set and
tracked will rely on, measured now while the base is fresh, so T5 starts from facts. An in-process `pw_stream` at
44100 Hz linked to a block's inside input bank in a 48000 Hz graph: it runs in the graph group without moving the
graph's rate and without a force property (the receiver at 44.1 kHz is the precedent [V]); `requested` each cycle,
and `pw_stream_set_rate` honoured from its process callback, with its range and clamp; its `io_changed` giving an
`SPA_IO_Position` a tick can be stated from (`CycleAccount` over the engine's cut record), and its process running
before the block's in every cycle; the resampler's delay per quality, and whether its ports report it as latency;
`module-rtp/audio.c`'s loop constants. The findings go into 5.2 and 5.3.

**T5's tests** (set and tracked, with the first user): a 44.1 kHz sine through a converted source into a 48 kHz graph,
its frequency per tenth of a second; set: a ratio of 1.001 for 2 s moves the ring's fill by the expected frames,
within a block; tracked: a writer paced at 44100 x (1 + 200 ppm) on the raw clock holds the fill within a block of
`targetFrames`, with no underrun; the told latency against the model; the lifecycle and deadline as every engine
kind's; bound and unbound by the drive; a compile-fail case (a set converter asked for a `RateReport`); the harness's
fake converter (frames 1:1, each `set` and `report` recorded).

### 5.6 Found building T4

What T4 built is described where it lives (`docs/engine.md#module-run-converters` and `#rtp-receivers`,
`docs/architecture.md#the-timing-model`, `docs/testing.md`'s `ear_core_tests` and `ear_pipewire_tests` rows, the
`pipewire-engine` skill); this section keeps only where it departs from 5.1 to 5.5, and what T5 must know. Every RTP
test passed unedited, "L16 sent at 44100 into the 48000 graph is resampled" among them.

- **The body is a template over the shape, and names itself to the boundary.** The callback boundary names a guarded
  callback by its class's spelling, so a shared non-template `ModuleConverterBody` would have renamed RTP's fault
  reports (`RtpReceiverBody::onCoreDone` to `ModuleConverterBody::onCoreDone`), which an RTP test pins: behaviour that
  moved. So `ModuleConverterBody<Shape>` is a class template and the boundary takes a class's own name where it gives
  one (`NamesItself`: a static `boundaryName`, the module's `ConverterModule::body`). A report still says which
  module's converter threw. T5's `StreamConverterBody` needs no such name unless it is shared the same way.
- **`ConverterModule` is the module's words, not its loading**: `kind` (the loss details' word), `word` (`cannot load
  PipeWire's <word> module`), `body`, `flow` and `nodePort` (returning a `std::string`). The module's file name moved
  into `ModuleRun<Shape>::load`, with `using Arguments` beside `arguments(...)`: a shipped module's arguments are
  `ModuleArguments<Home>` and it loads through `engine_property_set`'s `loadModule`, so the property guard holds; a
  test's stand-in, which has no rows in the property table, loads its own under a tests folder, as tests may.
- **`ModuleConverters<Shape>` is public** (`ear/pipewire/module_converters.hpp`), because `RtpReceivers`, its alias, is
  named in a public header (`main.cpp` and the tests use it). Its `make` is defined in the private body header and
  instantiated once per shipped shape (`template class ModuleConverters<RtpReceiverShape>` in `rtp_receivers.cpp`,
  `extern template` in `rtp_receivers.hpp`).
- **Core gained `ModuleRunShape`** beside `moduleRunDelay`: a shape is module-run when it has the rule, and
  NodeHost's plan visits each engine ask and enters the rule for those, naming no shape. The readiness text stayed as
  it was (`neverReady`, one overload per shape, which was never a branch on RTP); its deadline is the kind's.
- **The stand-in module is `libpipewire-module-pipe-tunnel`** in source mode (`module_converter_test.cpp`): of 1.4.2's
  modules it is the one closest to rtp-source that needs no network (it owns its stream, its buffer and a loop steering
  its resampler, reads from outside the graph, a FIFO, and makes its node on its own connection), it refuses to load on
  a bad `pipe.filename` (so a load failure is real, not injected), and its ports are `capture_AUX<n>`, not
  `receive_AUX<n>`, so the port names provably come from the module's description. `module-loopback` makes two nodes,
  `vban-recv` makes its node only once packets arrive, and `roc-source` needs a sender of its own.
- **S7 settled, for module-run** (`spikes_test.cpp`): (a) the module's node finishes before its block wakes in every
  cycle (PipeWire's `module-profiler`, loaded into a copy of `engine.conf` so it sees the Dummy-Driver made, over 2,000
  cycles of the real receiver resampling a 44.1 kHz sender: every cycle both ran to the end, the receiver finished
  before the block was signalled, none reported an earlier cycle; the stream is not `node.async`); (b) the module-run
  latency entry is right as built: rtp-source's port tells 0 frames once linked, at the graph's rate and resampling
  44.1 kHz alike, and the block tells exactly `moduleRunDelay`, so nothing counts twice and the resampler's delay is
  counted nowhere. The ear-made stream's checks in 5.5's list (a `pw_stream` at 44.1 kHz in the graph group,
  `requested`, `pw_stream_set_rate` and its clamp, `io_changed`'s position, the resampler's delay per quality and
  whether an ear-made stream's ports report it, `module-rtp/audio.c`'s constants) are the set and tracked modes' own
  and stay T5's [U]; (b) suggests an adapter's ports report no resampler delay, which T5 checks for its own stream.
- **The card stays on its own body** (5.4, D-TM10), confirmed against `sound_cards.cpp`: made by
  `pw_core_create_object("adapter")` with ear's proxy (nothing to find by name), its `PortConfig` sent by ear, its
  latency told through its links (nothing `beyond`), either direction, and its quantum-guard sizing and test seam its
  own. `docs/engine.md#module-run-converters` says so.

## 6. `sendspin` under the model

### 6.1 The wrapper's contract: the honest sink plus local skip, told only on time

`sendspin`'s audio from the server is a source on `from_server` (S3c.6, `node-primitives.md` 4.1) with a clock,
`SendspinClock : SourceClock` (`sendspin_clock.hpp`, replacing `sendspin_code.*`'s playback half):

- **It plays every frame in order.** It never skips but as below, and never pads: a tick the feed leaves short plays
  zeros, which are not counted.
- **`skipBefore`**: on a tick whose facts state `catchUp` or `absent` with `jumpFrames > 0`, it answers `jumpFrames`;
  the ring skips min(jump, what it holds now). On any other tick, 0. A clearing tick skips nothing (the ring's rule).
  The skip happens on the tick that states the jump, overdue or not.
- **`played`**: `played_ += skipped + read` every tick (never `ended` nor `zeros`: the library wrote neither for its
  current stream). Then it **tells the library only from a tick that began on time** (`overdueNs == 0`): it publishes
  `PlayProgress{played_, finish}` (6.2) to its mailbox and returns true. An overdue tick, one of a catch-up's burst,
  tells nothing; its frames are told with the next on-time tick's report, which counts them. The worker's notifier,
  watching the clock wake, hands the library the difference with `notify_audio_played`.
- **One bound.** If the library has gone untold for `untoldBound` (2 s of the graph's nominal time: the ticks'
  `startRawNs`, so a burst of short ticks does not stretch it), the clock tells from the next tick even if it is
  overdue, so a host that never runs a tick on time does not leave the library untold for good (6.2 says how that
  report is dated). The bound has no other role: there is no debt for it to forget.
- **It keeps no debt and has no `LibraryView`.** What the ring did not hold of a jump is the library's to correct: it
  sees it in the next honest finish (its next chunk late by that much) and corrects once.

*Why the deferral stays* (the design review, M1, amending TM7's "defers nothing"; the owner can overturn it): every
report is true when it is given. An overdue tick's start is the driver's nominal time, behind now by the graph's lag
`L` until the burst ends, and its run time is now; neither is where the library's next frame plays while the burst
runs. Told the nominal time, the library sees its playtime behind now and, without the floor patch, floors it at now
mid-burst (`cycle-facts.md` 9.9, finding 1). Told the run time, it puts its playtime at now plus what it holds; in the
runner's burst, where every tick runs whole and nothing is lost, its next chunk then looks late by `L` and it drops
`L` mid-burst, the output runs about `L` ahead once the burst ends, and it adds `L` of silence back: two corrections and
silence where nothing was lost. Waiting for the first on-time tick tells it a finish that is true then. This is the
node's own clock logic (TM3), not the framework's.

*Why the floor patch stays with it* (the re-check): while ear defers, the library hears nothing and keeps its
playtime open-loop, the last finish plus what it has written since. Through a burst that trails now by about the lag
`L`, in either shape. Upstream's `starved_playtime_floor` floors whenever the playtime is more than 5 ms behind now,
whatever is buffered, so without the patch it would floor mid-burst and bring the double correction back. The patch
floors only when nothing is buffered, the spec's underrun rule, the library's own: it fits TM3. C4 only ever ran the
deferral with the patch; the spike's `honest-local` told every tick and never ran the deferral at all.

**The evidence, and what T2 must prove.**

- **The cut shape, measured** (the spike, `honest-local`, 5 runs each, the dev host, whose bursts cut ticks: every
  stall run lost frames) [V]. The spike's wrapper told every tick, overdue ones dated by run time; T2 measures it again
  with the deferral.

  | Case | C4 as built (`real-all`) | Honest plus local skip (`honest-local`) |
  |---|---|---|
  | A 200 ms or 1 s stall, made upstream or in the block | no correction by the library; 27 to 160 ms of ear's own silence after the burst; back on the stamps 27 to 160 ms after it | exactly one correction by the library per run; ear's silence one block (5.3 ms); back on the stamps 11 to 101 ms after the burst (median 101: about one 96 ms chunk); until then audio plays late by the stall's remainder (160 to 1093 ms) |
  | A 1.5-cycle hiccup made upstream (one block lost, the feed holding it) | no correction | no correction (without the local skip: one per run) |
  | A starving network gap | two corrections | one correction (the late-chunk patch's) |
  | Steady on the dummy beside `yes` x 4 | no drop | no library drop and no lost sync (without the local skip: 13 to 50 drops and 22 to 46 lost syncs a run) |

  4,250 of 4,250 judged cycles were within 1 ms after recovery in every variant. The trade the owner accepted (TM7):
  after a stall past the feed, about one library chunk of audio plays late by what the feed did not hold, then the
  library corrects once; C4 played ear's silence instead and needed no correction.
- **The uncut shape, not measured.** No spike run made it: the dev host's bursts always cut. It is the shape the
  deferral exists for, and the one a GitHub runner makes (`cycle-facts.md` 9.10, 9.14). **T2 must prove it on CI**:
  the in-block stall tests (200 ms and 1 s) run there with zero lost frames, and for a run with zero lost frames they
  require zero library corrections, no silence and every report's finish at or after now, as for a run with cuts they
  require 6.4's one correction. A run of either shape passes only its own shape's rule, and the test names which shape
  it judged, so CI's log shows the uncut shape was met.

### 6.2 The dating rule, and the one case the clamp covers

The finish told is where the library's next frame plays: the end of the telling tick, once what lies downstream has
played, `FrameClock(rate).heardRawNs(start, tick.frames, tick.playback)`. With the deferral the telling tick is on time
in every case but one, so `start` is its `startRawNs`, the driver's date and the true one.

The one case is a report the bound forces from an overdue tick. Its nominal start trails now by the lag, which the
report would be untrue when given (the library would take its next chunk for late by the lag); so it is dated from
when the tick really ran, a fact of the tick whose home
is `GraphCycle`, stated once for every node:

```cpp
// GraphCycle (ear/core/node_type.hpp), a member function: no new field, no state.
/// When the block really began this tick, on CLOCK_MONOTONIC_RAW: startRawNs when it began on time; when it began
/// overdue, startRawNs + (nextNs - startNs) + overdueNs. 0 nextNs (a test's own tick) is taken as on time.
constexpr std::int64_t ranRawNs() const noexcept;
```

(CLOCK_MONOTONIC and the raw clock differ by parts per million over the few milliseconds between, nanoseconds [V: the
spike's `realStartRawNs`].) For an on-time tick `ranRawNs()` is `startRawNs`, so the clock always dates by it.

**The clamp is kept for that case alone.** A report forced from an overdue tick is dated by its run time; the next
on-time tick is dated by its nominal start, earlier by that tick's `overdueNs`, so without a clamp the finish would
step back (the spike saw such steps when it told every overdue tick: `honest-local`, one in-block 200 ms run of five,
three steps [V]). So `finish = max(heard, lastFinish_)`. Ticks told on time only follow one another by their nominal
starts and never step back, so with the bound not reached the clamp never acts; a test proves it holds still (6.4).
A forced report means the host ran no tick on time for 2 s, and the library may correct after it; that is the bound's
price, the same as C4's.

`lastFinish_` lives for the configuration: a stop and a start come later in time anyway, and a clear does not reset it
(the library re-anchors from the next finish, which is later still).

### 6.3 What is deleted, and what stays

| Deleted | Where |
|---|---|
| `LibraryView` | `library_view.hpp` |
| The debt: `owed_`, `catchingUp_`, `owingSuspended_`, the payment of a catch-up's remainder, and the owing role of `untoldBound` (forgetting a debt when due) | `sendspin_code.*` |
| The rule that a lossy tick (a `catchUp` or `absent`) tells nothing: with no debt, a lossy tick on time tells | `sendspin_code.*` |
| `Feed::skipThenRead`, `Feed::Taken`, `onPaymentStep`, the payment's two passes | `feed.*` (S3c.6 then deletes the feed itself) |
| Their tests: `code_test.cpp`'s debt, starved-library and payment rows; `feed_test.cpp`'s `skipThenRead` and payment-race rows; `clock_test.cpp`'s payment requirements (a remainder above 0 to pay, back within 2 + ceil(remainder / (feed - block)) blocks, `corrections == 0` after a stall with cuts, no debt forgotten while owed) and `Feed::holdWriter`'s use for them; `CycleJudge::librarySettle`'s causes `starvedCatchUp` and `forgottenDebt` | `src/nodes/sendspin/tests/`, `test/support/cycle_judge.hpp` |

**Stays**: `GraphCycle::overdueNs` and its use for telling (an overdue tick tells nothing); `untoldBound` and
`untoldSince_`, as the deferral's bound only; `code_test.cpp`'s rows for the deferral and its bound;
`cmake/sendspin-starved-floor.patch` with its library test and upstream report (6.5);
`cmake/sendspin-late-chunk.patch` and its gap test; `StallProbe` and the in-block stalls (6.4 uses them);
`Feed::dryReads`, `dryFrames` until S3c.6 (then the source's shortfall); the settle causes `dry` and `logged`.

### 6.4 The tests that replace C4's

Each through the real server and the real engine (`clock_test.cpp`), judged by the cycle judge, except the unit rows:

- **One library correction after a stall.** The 200 ms and 1 s stalls, made upstream (`StallProbe`) and in the block:
  every run, exactly one correction by the library (a logged hard sync or a late chunk's front dropped) of what the
  clock could not skip (the stall's lost frames less what the feed held), within a tolerance T2 fixes from the spike's
  runs and states with its reason; ear's own silence after the
  burst at most one block; back on the server's stamps within one chunk and two blocks of the burst's end; no
  `Buffered frames underflow`; every report's frames exactly `skipped + read` since the last report (6.1), every
  report from an on-time tick, and every finish by 6.2.
- **No correction after a stall that loses nothing** (the uncut shape, 6.1). The same in-block stall tests, on a run
  whose burst lost no frames (a GitHub runner's): zero library corrections, no silence, nothing told from an overdue
  tick, and the first report after the burst dated by its on-time tick. T2 is not done until CI has run this shape.
- **An exact skip on a host-missed cycle.** The 1.5-cycle hiccup upstream of the node (one block cut, the feed holding
  it): the clock skips exactly `jumpFrames`, the ramp steps by exactly that, the report moves by `jumpFrames` plus the
  tick's read, and the library makes no correction, every run. A unit row on the harness with scripted facts: a
  `catchUp` larger than what is held skips what is held and no more; `absent` skips; `reposition`, `driverChange`,
  `start` and `oversized` skip nothing; a clearing tick skips nothing.
- **The deferral, its bound and the clamp.** Unit rows on scripted ticks: a burst of overdue ticks tells nothing and
  the first on-time tick tells every frame of it, dated nominally; one overdue whole tick under load tells nothing; a
  lossy tick on time tells; overdue ticks for 2 s of nominal time force one report, dated by `ranRawNs()`, and the
  on-time tick after it is clamped to it, then nominal again; with the bound never reached the clamp never acts. In
  the clock tests every report's finish is checked against the last under the in-block stalls and the loaded steady
  runs (a step back fails the test, naming both ticks' facts).
- **Steady and gap**, kept: the steady tests on the dummy and a following card (750 cycles within 1 ms; the judge's four
  induced stalls each a local skip with no library correction), and the gap test (one correction, the late chunk's).

### 6.5 The upstream reports

- **The floor patch and its report stay** (the re-check, amending TM7). Upstream's floor is right for reports dated
  truly when they are given, which is every report the honest wrapper gives. It is not right for the playtime the
  library keeps open-loop between reports while ear defers: that trails now by the graph's lag through a burst, with
  frames still buffered, and upstream floors it. So the report's first half (floor only when nothing is buffered, the
  spec's underrun recovery) stands, its reason restated for the deferral rather than for reports dated behind now.
- **Dropping the patch is a later step, gated on evidence** [U]. T2 may also measure both burst shapes without the
  patch (the dev host's cut bursts, and CI's uncut ones), beside its required runs with it. Only if both shapes show no
  floor mid-burst and no correction beyond 6.4's does a separate step drop it, with its own review; until then it is
  carried as built.
- **The late chunk's report stays**, as today: a sink that reports honestly still starves the library in a
  network gap, and `decode_chunk` then skips a chunk whose front is late but whose end is not (the gap test fails 5 of
  5 with the patch's change reverted [V: `cycle-facts.md` 9.9]). The `sendspin` skill's draft keeps both halves and
  their three tests; T2 replaces its measurement with the honest wrapper's.

## 7. Order of build steps

Each step is one reviewable change, green on every test and `make format-check`, changing the library pages it makes
stale in the same commit.

| Step | What | Deletes | Behaviour | Tests it adds |
|---|---|---|---|---|
| **T1** (**done**, 2026-11-13) | `GraphCycle::ranRawNs()` (for the report the bound forces, 6.2) | nothing | none | the rule's rows: on time, overdue, a test's tick with no `nextNs` (`cycle_facts_test.cpp`). *Found building:* a member function of a `constexpr`-ready aggregate, so one row is a `static_assert`; the rows put the raw clock 7 ms off the monotonic one, so a rule that mixed the two clocks fails; now `docs/engine.md#graphcycle`'s row |
| **T2** (**done**, 2026-11-13; the uncut shape still to be read in CI's log) | `sendspin` honest, on today's feed and the legacy adapter: `SendspinCode` keeps the capture half and becomes the honest wrapper with the local skip, told only on time under the bound, and the clamp for the forced report (6.1, 6.2); both carried patches kept; optionally, runs of both burst shapes without the floor patch, as evidence for a later step (6.5); `docs/node-types.md#clock` and the skill (and its report's measurement, 6.5) | 6.3's table, and `Feed::holdWriter` with it (no use was left) | yes: one library correction after a stall with cuts, where ear's silence was; none after a stall that loses nothing, proven on CI | 6.4 (`code_test.cpp`, `clock_test.cpp`; the burst shape printed and shown by CI's `test` job). *Found building:* the library's correction is the unskipped frames **plus the zeros ear played meanwhile** (a skip that empties the feed leaves its cycle reading nothing), within 4 frames, so the tolerance is the library's 500 us settle threshold; it lands at the next chunk it syncs, after the feed and the rest of its chunk, so the return bound is the feed, a chunk and two blocks (26 blocks seen), not a chunk and two; under heavy load a settle step of one or two blocks followed it twice, with no starved cycle between, so the test allows one only with a dry or empty-feed cycle between as its evidence and lists the cycles otherwise; a pause skips too; the clamp is literally only the forced case, so a shrinking downstream latency is told as it is (the clamp-idle row's revert proof); `LibrarySettle`'s `starvedCatchUp` and `forgottenDebt` became one cause, `unskipped`; every burst on the dev host was one catch-up told on time, so the runs without the floor patch (20 of 20 passed) are evidence for the cut shape only |
| **T3** (**done**, 2026-11-13) | The clockless source: `SourceClock`, `SourceTick`, `SourceHeld`, `SourcePlayed`, `clockWake`; `SourceRing` without time (4.1 to 4.3); `EventSources`, the block, the harness; `docs/writing-a-node-type.md#sources`, `docs/engine.md#sources`, `#latency-per-bank-pair`, `#dating` | `Played`, the played wake, `SourceCycle`, the keep-time and debt code | none for any shipped type (none uses a source) | 4.4's rows; the system tests' ramp-skip windows checked against `sendspin`'s own clock and kept, their premise said where it is owed (4.4). *Found building* (4.6): the source owns its clock (`SourceAdapter::make` takes it; `SourceRing::play` is called by the source's tap, so `SourceRing` is no tap); `play` returns the `SourcePlayed` it told, which the harness's `lastPlayed` reads; the clock is asked on every tick, a clearing one included; a clear's decay past the ended stream's frames counts as `zeros`; a skip is no drop; the stamp window's follow-up folded in (the measured arrival in place of the 2 s `captureLag`) |
| **T4** | **Done** (5.6). The converter base from RTP's wiring, module-run mode, RTP on it (5.1 to 5.5), with S7's checks folded in (test only) | `RtpReceiverBody` and NodeHost's RTP branches, moved | none: every RTP test passes unedited | 5.5's new tests and S7's checks |
| *S3c.1 to S3c.5* | As `node-primitives.md` 13 has them | | | |
| **S3c.6** | As planned, with the honest wrapper moved onto `from_server`'s source as `SendspinClock` (the feed, its skip and its dry counts go; the clock's report replaces the bell and the played wake in `node-primitives.md` 4.1's worker) | `feed.*`, `sendspin_code.*`'s playback half | as `node-primitives.md` 8 says, and no timing change: T2's tests pass unchanged on the source | T2's tests on the source; the clock's unit rows through `TypeHarness<spec, Sendspin>` |
| **T5** | Set and tracked modes (5.2), with their first user (AirPlay, slice 9, for tracked): `StreamConverterBody`, `ConverterShape`, `SetRatio`, `RateReport`, `convertedSource`, `convertedSink`, the harness's fake; `docs/engine.md#engine-companions`, `docs/writing-a-node-type.md#engine-companions` | nothing | none until its user | 5.5's T5 tests |

**How it fits slice 3c and the rest.** T1 to T3 come before S3c.1: they touch only the source, `GraphCycle` and
`sendspin`'s code, none of which S3c.1 to S3c.5 change, and they shrink S3c.6 (the source it moves onto keeps no time,
and its clock is already written and proven). T2 needs T1; T3 needs neither and may land first. T4 comes before S3c.1 too (the
owner moved it earlier): it touches only the RTP companion's code in `ear_pipewire` and NodeHost's latency plan and
readiness text, which 3c's steps do not, and it needs none of T1 to T3. `cycle-facts.md`'s open steps (D3, C6, C7, C8)
are independent of all of them. T5 comes after S3c.6 (it uses 3c's `ResourceAdapters` and the clockless source), with
its first user.

## 8. What this changes on other design pages

| Page | Change |
|---|---|
| `cycle-facts.md` | Q-CF2 reversed (TM7); 4.4's `SourceRing`, `Played` and `SendspinCode` rows superseded; C4a and C4b's machinery deleted by T2 and T3; the follow-ups section 9.15 lists are moot |
| Slice 3b's design (built; its page deleted) | A source keeping time by default and `Played`'s meaning reversed; its "keeping time" and "says what played" are this page's 4.1 and 4.2 |
| `node-primitives.md` | 4.1's worker watches the source's clock wake, not a played wake; 6.2's list of the framework's wakes; 12's `sendspin_code.*` and `mailbox.hpp`; 13's S3c.6 |
| `programs.md` | 2.2's paced source is a source with a converter companion in tracked mode (5.1, 5.2) |

## 9. Decided here (technical)

Each the designer's; the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| D-TM1 | A source's clock is a hook the ring calls around its play (`SourceClock`), not the type's code reading the ring | The ring's rules stay written once (core's `SourceRing`); the node decides only what its clock must |
| D-TM2 | The clock's report rides the source's own wake (`clockWake`), the report itself in a mailbox the type owns | One wake per source as built; G23 stands |
| D-TM3 | `SourcePlayed` splits a cleared stream's faded 5 ms (`ended`) and the zeros from what was read | A clock counting the old stream's tail as played would put the library's new stream 5 ms off |
| D-TM4 | `ranRawNs()` is a member of `GraphCycle`; the deferral, its bound and the clamp are `sendspin`'s | When a tick ran is a tick fact for every node (TM1); when to tell a library is that node's clock logic (TM3) |
| D-TM5 | Tell only from an on-time tick, under a 2 s bound; the clamp kept only for a report the bound forces from an overdue tick (6.1, 6.2) | Every report is true when given, in a burst of either shape; the clamp has one case left, and is proven idle otherwise |
| D-TM6 | Sinks unchanged; their consumers reconcile | A chunk is already the tick's facts and the model's latency |
| D-TM7 | The converter's base is the RTP receiver's body generalised (`ModuleConverterBody`, `ModuleConverters<Shape>`), on the shared `joinEngineCompanion`; set and tracked add an ear-made `pw_stream` body on the same lifecycle | TM8; the module-run mode is what RTP already is, and the stream body is the only way to PipeWire's resampler for a ring (TM5); binding by the drive keeps the exactness of ring companions served by their block (`docs/architecture.md#companions`) |
| D-TM10 | The card stays on its own body, sharing only `joinEngineCompanion` | It is made by ear's proxy, not a module: the base's registry search, `moduleRunDelay` and auto-port-config are all things a card does not use (5.4) |
| D-TM11 | RTP's type API, companion kind and wire name stay `rtpReceiver` | A change of where the code lives is no change an operator or a type author sees |
| D-TM8 | The converter never skips; skipping is the node's clock's | TM1: the framework keeps no audio time, the converter included |
| D-TM9 | T2 lands on today's feed before the source changes | The behaviour change is measured alone, against C4's tests, before any move |

## 10. Rejected alternatives

| Alternative | Why not |
|---|---|
| Keep `Played` beside the clock, as a framework count | A count the framework keeps is audio time it keeps (TM1); `sendspin` could not use it (it would count frames the library never wrote, `cycle-facts.md` 9.7) |
| The type's own code reads the ring through `Io` | Every type would write the fades, a clear's tail and the shortfall again; harness and block could drift |
| An in-code resampler for converted rings | TM5: conversion is PipeWire's converter |
| Reimplement RTP reception on the set or tracked converter (ear's own jitter buffer and clock tracking) | TM8: PipeWire keeps the jitter buffer and the clock tracking; RTP moves onto the shared wiring as module-run |
| Move the card onto the module-run base | 5.4: it would add a registry search and a latency rule the card does not use |
| The converter skipping a stall's backlog on `catchUp` | The framework would be keeping time again (TM1); a node that wants the skip gives a clock |
| Tell every tick, an overdue one dated by its run time and clamped (this page's first design) | In the runner's uncut burst every tick is whole and overdue: told its run time, the library puts its playtime at now plus what it holds, finds its next chunk late by the graph's lag `L`, drops `L` mid-burst, runs about `L` ahead once the burst ends and adds `L` of silence: two corrections and silence where nothing was lost (the design review, M1) |
| Tell every tick, an overdue one dated by its nominal start | The nominal start trails now by `L` until the burst ends; without the floor patch the library floors that at now with frames still buffered, mid-burst (`cycle-facts.md` 9.9, finding 1, and 9.10) |
| Keep the floor patch so nominal dating works | The patch is kept, but for the deferral, not for this: it does not save nominal dating, whose reports are untrue when given and would still be corrected once the library's buffer drains |
| Drop the floor patch with the deferral in place (this page's earlier text) | While ear defers, the library's open-loop playtime trails now through a burst of either shape and upstream floors it mid-burst: the double correction again. C4 never ran the deferral without the patch, and the spike never ran the deferral; dropping it waits for 6.5's evidence |

## 11. Questions for the owner, answered

**Q-TM1 (answered, 2026-11-13).** Should the converter companion be built now, or with the first node that needs it?
The owner: RTP is its first user. The converter is built out of the RTP companion's wiring with a module-run mode, RTP
moves onto it with no behaviour change, set and tracked come with their first users, and the step moves earlier with
spike S7 folded in (TM8; section 5, step T4).

**Q-TM2 (narrowed by the owner, 2026-11-13; the default decided unless the owner objects).** When a source falls
behind on its own clock, should it jump to be in time again, or play slightly fast until it catches up?

*Background.* A stall of the host is not this question: it stalls the whole graph, one tick and one data thread, so
every node inside ear resumes together and stays in time with every other (TM9). The question is about two other
cases. (a) A source's own clock falls behind while the graph keeps ticking: its network stalls, or its own thread is
held, and the audio it then holds is late against the graph. (b) After a graph-wide stall, ear itself is late against
what is outside it: speakers driven by other machines, or a picture on a screen. In both, jumping drops the late audio
at once; speeding up plays it slightly fast (a converter in tracked mode) until it is back in time, with no jump but
out of time meanwhile. `sendspin` already jumps (TM7: it skips what it holds of the lost time, and the library corrects
the rest). This is the default for the sources still to come (AirPlay's paced source, a later RTP sender), each of
which can still choose otherwise in its own clock.

*Scenario.* (a) An AirPlay node in ear feeds the kitchen and the living room, both through ear's cards, beside a
`sendspin` node feeding the porch; AirPlay's network stalls for 300 ms while the graph ticks on. Jump: the AirPlay rooms
skip 300 ms and are in time with the rest of the graph at once. Speed up: no skip, but the AirPlay rooms trail the
porch by 300 ms at first, for about a minute at a 0.5% speed-up. (The kitchen and the living room stay together either
way: one ear feeds both.) (b) ear's host stalls 300 ms while a TV elsewhere shows the picture: every room in ear goes
quiet and resumes together, but 300 ms behind the picture; jumping puts the sound back on the picture, speeding up
drifts back over a minute.

*Decided default: jump*, as `sendspin` does: it keeps the source in time with the other nodes of the graph, and keeps
ear in time with what is outside it. It stands as the default for sources that fall behind on their own clock unless
the owner objects.
