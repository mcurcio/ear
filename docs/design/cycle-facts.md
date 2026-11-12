# Cycle facts, one test judge and a sample-rate type

Status: designed to module level (2026-11-10); Q-CF1 and Q-CF2 answered, diagnostics counters added (section 8);
spike S6 run (section 9.1: PipeWire reports every cycle a node misses, so Q-CF3 is not asked); step C1 built (section
9.2: the facts, `CycleAccount`, the table and the harness, described in `docs/writing-a-node-type.md#lost-cycles`
and `docs/testing.md`), the rest not. Its steps
(section 9) follow `root-classes.md`'s and come before slice 3c, whose `sendspin` on a source takes the source rule this page changes (section 4.4). The built system
it changes is described in `docs/engine.md` (`#graphcycle`, `#the-internal-clock`, `#sinks`, `#sources`, `#dating`,
`#the-one-data-loop-and-invokes`), `docs/testing.md` (`#making-the-rare-event`, `#the-sleep-guard`),
`docs/architecture.md#the-callback-boundary` and `docs/writing-a-node-type.md#sources`; this page cites them and says
only what changes. When it is built it is deleted, and what lasts moves into those pages.

[V] = verified in the code or PipeWire 1.4.2's source named, as of this page's writing; [U] = unverified, with the step
that checks it. S6 has checked every [U] that was its own (section 9.1).

## 1. The owner's words that bind this page

- (2026-11-09, `root-classes.md` section 1) "Reminder: always patch the root class of a bug, not the bug itself.
  Clusters of issues are a smell that signals an architecture problem."
- (2026-11-10, agreed in conversation; paraphrased, not quoted) The root class behind the test-luck reviews is that ear
  never states what happened to each cycle, so every test rebuilds it from a partial view and invents its own excuse,
  each looser than the event it excuses.
- (2026-11-10, Q-CF1) "Yes. We should include counters like this as part of the core diagnostics. Per node? Per
  graph?" Section 8 designs both and recommends both; the owner can still overturn that.
- (2026-11-10, Q-CF2) Keep in step: after a stall longer than a node's buffer, what ear reports as played counts the
  whole stall (section 4.4).

## 2. The root class

A graph cycle has facts nobody in ear states: whether the graph ran it whole, how many cycles a block missed since it
last ran, whether the driver cut those short, how much raw-clock time went with them, and why a cycle is a
discontinuity. `GraphCycle` carries two lossy summaries (`xruns`, a running count of forward jumps, and
`discontinuity`, one bit for five causes: `docs/engine.md#graphcycle`). Everyone who needs more derives it again from
what they can see, each from a different vantage point and with a different rule.

**Product code, five derivations** [V]:

| Where | What it derives | From |
|---|---|---|
| `CycleContinuity` (`ear/core/block_rules.hpp`) | `discontinuity` and `xruns` | position against the last end, driver id, a drive, an oversized cycle |
| `SourceRing::State::keepTime` (`src/core/source_ring.cpp:121-145`) | a skip, or "a new clock" | position against its own `expected`, the ring's capacity |
| `SendspinCode::missedFrames` (`src/nodes/sendspin/sendspin_code.cpp:13-17`) | a skip, or "a change of driver" | position against its own `nextPosition_`, the feed's capacity |
| `Played` and `PlayProgress` | how far the graph's clock moved | the two above, each adding only what it skipped |
| `ChunkStamp` / `Chunk::discontinuity` | the same bit as `GraphCycle`'s | a consumer told to "compare `position` with the last chunk's end" (`sink.hpp`) |

The two skip rules call a long stall on the same driver "a new clock" or "a change of driver": on the tracking dummy
driver neither is true (the driver did not change; time moved), and the count each publishes then lags the graph
for good. Nothing in ear can tell a long catch-up from a driver change, because nothing states which it was.

**Tests, at least seven judges** (from `main` and the two unmerged branches, `worktree-agent-ac4b07a6145554fa8` and
`worktree-agent-af4a96a389839d7b5`) [V]:

| Test | Its own rule | Vantage point |
|---|---|---|
| `port_block_test.cpp`, the ramp through a block's inside ports (`lostToCuts`) | a recorder cycle not clean is excused if its position is past the last end by exactly the graph cycles started between, each cut | its `Recorder`'s ledger plus the test-only `DriverCycles` listener |
| `sound_card_test.cpp` (`judgeRamp`, `Code::hostJumps`) | a block missing from the card's file must have run in a cut cycle; a discontinuity counts unless the jump is the cycles started since, all cut | a code's ledger, the card's WAV, `DriverCycles` |
| `source_test.cpp` (`judgeSource`, `judgeHeard`) | the source rule modelled cycle by cycle; every jump must be raw-clock time lost, within two cycles and 1 ms | its own read of `CLOCK_MONOTONIC_RAW` in the code |
| `spikes_test.cpp` S5 | the same raw-clock rule, written again | its own raw read |
| `engine_test.cpp`, `sink_test.cpp` take-ups | `CycleLedger::clean`, or the block's `xruns` unchanged | `CycleLedger` |
| `sendspin/tests/clock_test.cpp` (`Judge`) | 1.5 s not judged after any unclean cycle, any dry cycle, any jump past the feed; `excusedSteps`; a skip must move the ramp by `lastSkipped()` + 1 | `CycleLedger`, a product seam (`SendspinCode::lastSkipped`) |
| `rtp_receiver_test.cpp`, `test/system/rtp_test.cpp` | a tenth judged when its cycles and the half second before are clean and no late send lies within 600 ms | the recorder's ledger, the sender's own `lateSends()` |
| `test/system/*` (`checkRamp`, `isSkipStep`, the source's stamp steps) | a step is a skip if it is whole blocks to within a frame; a stamp step 200 µs longer than the chunk | the WAV alone, the server's stamps |

Each excuse is a window or a shape (1.5 s, 600 ms, "whole blocks within a frame", "position past by exactly the cycles
started"), never the event itself, and so each is looser than what it excuses: a bug that drops a block inside a
window passes. Reviews found three such (the ledger alone excusing what the driver never cut; the card's catch-up
counted as a host's; the raw clock excusing a block that skipped cycles while a card joined), each fixed in its own
test by a rule a little closer to the driver's account. The driver's account exists (PipeWire's realtime `incomplete`
event: `impl-node.c` `node_ready`, emitted for the cycle a node had not finished, before the next cycle's `start`
[V]); only a test listener reads it.

**The text guards, the same class one level up.** Four guards read source text for a rule a type could hold: sleeps
(`test/sleep_guard.cmake`), callbacks (`test/pipewire/callback_guard.cmake`), properties (replaced by
`PropertyKey<Home>`, `root-classes.md` 6.5) and frames (`test/frame_guard.cmake`, whose `SampleRate` type
`root-classes.md` 3.7 deferred). Each was bypassed in review by a spelling it did not know. One more is found writing
this page: the callback guard refuses `*_add_listener` but not `pw_impl_node_add_rt_listener`, which this page's
product listener needs, so the very registration section 4.2 adds would pass the guard unchecked.

## 3. The fix in one paragraph

The block states each cycle's facts once, on the data loop, from the driver's own account and its own scheduling:
`CycleFacts`, made by one pure rule (`CycleAccount`, `ear_core`) that the port block and the type harness both run, fed
by one product listener on the internal clock (`DriverCuts`, `ear_pipewire`). `GraphCycle` carries them, so a code,
a source's tap and a sink's stamp see the same facts; the two skip rules and `CycleContinuity` are replaced by reads of
them. Tests judge through one judge (`CycleJudge`, `test/support`) that excuses exactly the cycles the driver cut and
nothing else, induces cuts itself so every run proves its excuse path, and refuses to pass on nothing. The frame guard
becomes a type (`SampleRate`), and the callback and sleep guards become compile-time refusals (`#pragma GCC poison`
behind a forced include), each leaving a one-token text check at most. The same facts feed ear's diagnostics
counters, per graph and per node, on the control port and standard output (section 8).

## 4. Cycle facts

### 4.1 What a cycle states

Per block, per cycle the block runs:

| Fact | Meaning | Source |
|---|---|---|
| `graphCycle` | The internal clock's cycle this ran in, counted from the engine's start (1, 2, ...) | `DriverCuts::started()`, read in `onProcess` |
| `missed` | Graph cycles started on the same driver since this block last ran one | `graphCycle - last graphCycle - 1` |
| `missedCut` | Of them, how many the driver reported cut short | `DriverCuts` |
| `lastCut` | The cycle this block last ran was itself cut short after it: nodes after this block may not have heard it | `DriverCuts`, known now (the cut is told before the next start) |
| `jumpFrames` | The position against where the last cycle ended, on the same driver (negative: back) | `clock.position` |
| `lostRawNs` | Raw-clock time between the last cycle's end (its start plus its frames) and this start | `startRawNs` (both already measured) |
| `causes` | Why this cycle is a discontinuity, a set (below) | the rule |

The causes, a bit set because they coincide (a re-drive across a stall is both):

| Cause | When |
|---|---|
| `start` | The block's first cycle |
| `drive` | A new code's first cycle (any drive, a re-drive of the same code included) |
| `driverChange` | Another driver than the last cycle's (`clock.id`) |
| `catchUp` | The block missed cycles on the same driver and the position moved by exactly `missed` cycles' frames: the graph lost time and caught it up |
| `absent` | As `catchUp`, but some missed cycle was not cut: the block was not scheduled (paused, suspended, its driver taken away), not a stall |
| `oversized` | The cycle after one too large for the block (written as zeros, the code not run) |
| `reposition` | The position moved on the same driver by anything else: back, or not `missed` whole cycles |

`whole()` is "nothing lost before this cycle": no cause. `confirmed()` is `missed == missedCut`. A cycle may be `whole`
and still `lastCut` (a head of a chain whose tail lost the last cycle).

`catchUp` against `reposition` is what neither skip rule can say today. On the tracking dummy every timeout starts one
cycle and moves the position by one block (`node-driver.c` `on_timeout`: `position = clock->position +
clock->duration`, `resync.ms` beyond any stall) [V], so a forward jump of exactly the missed cycles is time lost. The
sendspin clock test saw jumps of 1127, 1156 and 1189 frames on an idle host (`clock_test.cpp`'s comment) that this
model calls `reposition`: S6 found them made by node-driver.c's default `resync.ms`, which `engine.conf` did not yet
override when they were seen, and reproduced them; with the shipped setting every jump of a running block is whole
missed cycles (section 9.1) [V].

### 4.2 Where they are produced: one owner, on the data loop

**`DriverCuts`** (`ear_pipewire`, private) listens to the internal clock's realtime events, as the branch's test-only
`DriverCycles` does, but through the callback boundary and owned by the engine:

- Made by `Engine::create` once the context has loaded `engine.conf`'s objects, before any port block; found by
  `node.name` (`engine_names.hpp`'s internal clock name), and `create` fails (`EngineError`) if it is missing.
- `start` (data thread, in `node_ready` after `clock.cycle++`, before the followers are triggered [V]): clears the
  bit of the cycle it starts, then `started_ + 1` with release.
- `incomplete` (data thread, in the next `node_ready`, before that cycle's `start` [V]): marks cycle `started_` cut.
  So when any block runs cycle N, every cut of cycles below N is already recorded, on the same thread.
- Storage made at create: a ring of 2^16 cut bits (8 KiB, about 350 s at 48 kHz / 256), the running count of cuts
  and the newest whole cycle. The counts have no reach, so a stall of any length is confirmed or not exactly; only
  which cycle was cut, asked of the ring, has one (9.2).
- Registered with a new boundary entry, `listenRealtime<Table>(pw_impl_node*, ...)`, whose table entries take only
  `realtime<>` members; removal (`pw_impl_node_remove_rt_listener`, a blocking invoke on the data loop [V]) is skipped
  and leaked on a wedged loop, as every such call in the boundary is.
- `PIPEWIRE_DEBUG`'s own xrun log is untouched; `DriverCuts` adds nothing to the data thread but two atomic stores.

Why its own count and not `clock.cycle`: the incomplete event carries no cycle number, so the listener must count cycles
itself to say which was cut, and a block naming its cycle by the same counter makes the two agree by construction.
`clock.cycle` (a `uint32_t` in the driver's position, which a filter's `process` reads too) moves with it exactly: S6
found it the listener's count plus a fixed offset in every cycle of every probe, and 1.4.2 writes it in one place only,
`node_ready`'s `clock.cycle++` just before `start` (`impl-node.c`), never resetting it [V]. The listener cannot read
`clock.cycle` itself: the server side's position is private to `impl-node.c`, and a filter's mapping of it goes away
with the filter (S6 crashed reading it after its probes were destroyed), so `DriverCuts` keeps its own count.

**`CycleAccount`** (`ear_core`, pure) replaces `CycleContinuity`: given one `DriverTick` per cycle (driver id, the
listener's cycle number, position, frames, raw start) and a `CutRecord`, it returns that cycle's `CycleFacts` and
whether the code runs it. It keeps `nextPosition()` for a drive's take-up. Its rule for the five causes `CycleContinuity`
has is the same, so every `discontinuity` stays where it is; `xruns` becomes a running sum of `missed`, kept until
step C4 removes it. It allocates nothing and runs in the block's `onProcess` where `CycleContinuity::next` runs today.

**The harness** runs the same `CycleAccount` against a `HarnessCuts` it owns, so a type's tests can lose cycles, cut or
not, change driver and reposition, and see exactly the facts the engine gives (section 6.3). One pure rule both call, as
R7 made the fade and the sink's cut.

### 4.3 How the facts reach code

`GraphCycle` gains a `CycleFacts facts` member; it is the one carrier, and the stamps a source's tap and a sink's tap
get are built from it (`chunkStamp`, `sourceCycle` in `block_rules.hpp` copy it). In step C4 `GraphCycle::xruns` and
`GraphCycle::discontinuity` go: `cycle.facts.discontinuity()` replaces the bit (four product sites, the harness, and the
tests that build a `GraphCycle` by hand), and nothing reads `xruns` outside the tests. A separate view was considered
and refused: every consumer already holds the `GraphCycle`, and a second object would be a second home.

Off the data loop the facts travel with what already crosses: a sink's stamp ring (`ChunkStamp` gains `causes` and
`lostFrames`), a source's `Played` (it moves by them), and the counters of section 8, which the main loop reads on a
timer.

### 4.4 The product consumers, moved

| Consumer | Today | On the facts |
|---|---|---|
| `CycleContinuity` | its own rule | deleted; `CycleAccount` |
| `GraphCycle::discontinuity`, `xruns` | two summaries | `facts` (C2 adds, C4 removes the two) |
| Sink chunk stamps | the same bit; a consumer compares positions | `Chunk` gains `causes` and `lostFrames` (the frames the graph lost before this chunk since the sink's last, `jumpFrames` on a `catchUp`); a wrapped chunk's second call has none, as its `discontinuity` is false today. `file` may then write the lost frames as silence or say them; that is its type's choice, not this page's |
| `SourceRing::keepTime` | a skip within capacity, else "a new clock" | `catchUp`: the stream moves on by `jumpFrames`, skipping what the ring holds and owing the rest (today's `debt`); `start`, `driverChange`, `reposition`: a new clock. A `catchUp` longer than the ring keeps in step too (Q-CF2, decided): the whole jump is counted played, what the ring holds is dropped, and the rest is owed, the writer's next frames dropped up to it; a clear forgets it |
| `Played` | moves by what was skipped | moves by `jumpFrames + frames` on every `catchUp`, however long (Q-CF2), so it stays on the graph's clock |
| `SendspinCode` skips and `PlayProgress` | `missedFrames` from its own position; progress adds only what the feed held | the feed skips `min(jumpFrames, held)`; progress moves by the whole `jumpFrames` on a `catchUp` (the library is told the time that went, not what the feed happened to hold); `lastSkipped()` is deleted. If R5c's open findings (the 256-frame step on a clean cycle, the short skips) are a product bug in progress, its fix is this row, not a patch of `missedFrames` (section 9, C4a). In 3c the feed becomes a source and this row merges into `SourceRing`'s |
| `StallWatch` | the main loop's heartbeat | **unchanged.** It watches the main loop, not cycles, so it is no consumer of cycle facts. NodeHost's "unfair" grace for a bound (`node_host.cpp:1798-1830`) is main-loop lateness too. What cycle facts add for an operator is section 8's counters, from the data side |
| Dating (`FrameClock::heardRawNs`, `enteredRawNs`) | `startRawNs` | unchanged: a date is still the cycle's start; the facts only say which frames never played |

### 4.5 What the facts are not

They are the block's account of the graph's schedule. They do not say a device sounded a frame (a card's own xruns
are its ALSA node's, `docs/engine.md#the-internal-clock`), nor what a library did with what ear gave it (sendspin-cpp's
re-sync is its own; section 5.4), nor whether an RTP sender was late. A cycle cut short after the block is a fact the
block learns one cycle later (`lastCut`); nothing can know it sooner.

## 5. One test judge

### 5.1 The rule

`CycleJudge` (`test/support/cycle_judge.hpp`, target `ear_cycle_judge`, pure: `ear_core` only) is the only place a
test may excuse a missed or cut cycle. It is fed the facts of the node whose output the test judges, the last of the
chain (a chain's tail loses every cycle lost anywhere before it: `docs/testing.md`'s "a chain is judged where its last
node says so"), and gives one verdict per cycle:

- **judge**: `whole()`. The test judges the boundary into this cycle.
- **excused**: `catchUp`, `confirmed()`, and the jump consistent with raw time (`jumpFrames == missed * frames`, and
  `FrameClock::ns(jumpFrames)` no more than `lostRawNs` plus the driver's 1 ms clamp and two cycles' wake room: the
  one home of S5's and part A's raw-clock rule). Exactly `missed` cycles are excused, counted.
- **expected**: a cause the test declared, up to the count it declared (`start` once, `drive` per drive it made,
  `driverChange` once per card it made a driver, `absent` per pause it made, `oversized` per forced quantum).
- **fail**: anything else, with the cycle, its facts and the rule it broke, for the failure's `INFO`.

There is no window. A disturbance excuses its own cycles and not one more; what a downstream library does after it is
judged separately (5.4).

### 5.2 Induced cuts are part of the tool

A test hands the judge its plan, and the plan must hold both counts:

```cpp
CycleJudge judge(JudgePlan{.judged = 32, .induce = StallPlan::every(50, 7, 4, StallLength::cycles(1.5))}, clock,
                 &engine.cuts());
```

`judge.stalls()` is the `Stalls` the test's upstream code calls in its `process()`; each planned 1.5-cycle stall cuts
that cycle straight after itself on any host (`docs/engine.md#the-one-data-loop-and-invokes`), so the judged node
misses it and the judge must excuse it. `done()` holds only when `judged` boundaries were judged **and** every planned
stall was made **and** at least that many cycles were excused as cut. A test whose excuse path is broken (the judge,
the facts, or the listener) fails every run, not on the first loaded host. The constructor refuses a plan with
`judged == 0`, and `done()` is false before the first judged cycle: nothing passes on nothing. A test that must not
stall (one that judges latency only) states `.induce = StallPlan{}` and `.judged` alone; the default is an induced plan.

### 5.3 The card case

A card runs no code of ear's, so its facts are not seen; the judge answers for it from the record itself:
`judge.blockMissing(graphCycle)` says whether a block the ramp wrote in `graphCycle` may be missing from the card's
file (that cycle cut: `CutRecord::cut`), and the test's own reading of the WAV supplies which blocks are missing. The
silence the card plays for a missed block is whole blocks (unchanged).

### 5.4 What lies past the graph

Three tests judge something that responds to the graph with its own dynamics. The judge covers the graph side exactly,
and each test splits off the rest:

- **sendspin's clock.** ear's side is judged frame for frame without the library: the code's progress moves by
  `jumpFrames + frames` on every excused cycle (a unit test on `SendspinCode` with scripted facts, in the harness's
  way). The library's re-lock after a disturbance is the library's behaviour; until its log reaches ear through a
  callback (the carried patch slice 3c owes, the `sendspin` skill), the timing test judges only cycles after which the
  library said nothing, and a disturbance's settle is declared in the judge as `librarySettle` with its bound in the
  library's units (chunks, from its own correction rule: two frames a chunk), counted in the report. It is the one
  declared consequence, named, not a window any cycle can use. The feed running dry is a writer fact: the feed counts
  its dry cycles itself (a product seam beside `waitsBegun`), and a dry cycle is excused only where that count moved.
- **RTP.** The graph side by the judge; the sender's late sends stay the sender's fact (`lateSends()`), and their reach
  is the receiver's buffer as configured (`latencyMs`, through `FrameClock`), not 600 ms. The half second "before" a
  tenth is that same reach.
- **System tests** (ear is another process). They judge from what ear says it lost: the `ear.graph-lost-cycles` and
  `ear.node-missed-cycles` records and the counters in `graph.get` (section 8), each skip in the WAV matched to a cut
  the records count, none left over.

### 5.5 Every judge today, mapped

| Test | Today | On the judge | Deleted |
|---|---|---|---|
| `port_block_test.cpp` inside ports ramp | `lostToCuts`, `Recorder::graphCycle`, `DriverCycles` | the recorder's facts into the judge; an induced plan | `lostToCuts`, the recorder's graph-cycle column, the `DriverCycles` include |
| `port_block_test.cpp` take-ups, fade-ins | "the listener heard consecutive positions" | the listener's facts: `whole()` at the take-up | the position comparisons |
| `sound_card_test.cpp` ramp onto channel 2 | `judgeRamp`'s `allCut` | `blockMissing` | `allCut`, `Code::graphCycle` |
| `sound_card_test.cpp` "joining moved no clock", "loses a link ... leaves the clock alone" | `Code::hostJumps` / `discontinuities` | `expected` none; every cycle judged or excused | `hostJumps`, the per-code excuse |
| `source_test.cpp` part A and the bound source | `judgeSource`'s raw-clock block, its own jump arithmetic | facts per cycle into the judge; the source-rule model stays (it is what is tested) and reads `causes` | the raw read in the code, `mostOverLost`, `unlost` |
| `spikes_test.cpp` S5 | its own raw-clock rule | stays a spike of PipeWire (it is not a block); it reads `DriverCuts` for the cut count | the duplicate raw rule |
| `engine_test.cpp` ramp, `sink_test.cpp` take-ups | `CycleLedger::clean`, `xruns` | `CycleLedger` records facts, `clean` is `whole()`; the judge | the ledger's own clean rule and `stalled` special case |
| `sendspin/tests/clock_test.cpp` | `Judge` with 1.5 s windows, `excusedSteps`, `dryCycles`, `lastSkipped` | 5.4 | `kSettleNs`, `lastSkipped()`, the window logic |
| `rtp_receiver_test.cpp`, `test/system/rtp_test.cpp` | clean cycles plus 0.5 s, late sends within 600 ms | 5.4 | the two fixed windows |
| `test/system/*` ramps and stamps | `isSkipStep`, the 200 µs step | the counters and records (section 8) | both |
| The branch's `DriverCycles` | the only reader of the driver's account | `DriverCuts`, in the product | `src/pipewire/tests/driver_cycles.hpp` |

## 6. Interfaces

### 6.1 Module map

| Header | Package | Holds | Threads |
|---|---|---|---|
| `ear/core/cycle_facts.hpp` (new, public) | `ear_core` | `CycleCause`, `CycleCauses`, `CycleFacts`, `CutRecord` (interface), `DriverTick`, `CycleAccount` | a `CycleAccount` belongs to the thread that runs its block's cycles |
| `ear/core/node_type.hpp` | `ear_core` | `GraphCycle::facts` | unchanged |
| `ear/core/block_rules.hpp` | `ear_core` | `CycleContinuity` deleted; `chunkStamp`, `sourceCycle` copy facts | unchanged |
| `ear/core/sink.hpp`, `source.hpp` | `ear_core` | `Chunk`/`ChunkStamp` gain `causes`, `lostFrames`; `SourceCycle` gains `facts` | unchanged |
| `ear/core/sample_rate.hpp` (new, public) | `ear_core` | `SampleRate` | value type |
| `ear/core/latency.hpp` | `ear_core` | `FrameClock(SampleRate)`, `framesCeil`, `framesNearest` | value type |
| `src/pipewire/driver_cuts.hpp` (new, private) | `ear_pipewire` | `DriverCuts final : CutRecord` | written on the data thread; read anywhere |
| `src/pipewire/callback_boundary.hpp` | `ear_pipewire` | `listenRealtime<>` for `pw_impl_node_rt_events` | registration on the main thread |
| `ear/pipewire/engine.hpp` | `ear_pipewire` | `const CutRecord& cuts() const noexcept` (this package's adapters and tests) | any |
| `src/pipewire/no_raw_callbacks.hpp` (new, private, forced include) | `ear_pipewire` | the poison list (7.2) | compile time |
| `ear/testing/node_harness.hpp` | `ear_type_harness` | `loseCycles`, `changeDriver`, `reposition` | the test's thread |
| `test/support/cycle_judge.hpp` (new) | `ear_cycle_judge` | `JudgePlan`, `CycleJudge`, `Verdict`, `JudgeReport` | the test's thread, reading ledgers |
| `test/support/inducers.hpp` | `ear_inducers` | `CycleLedger` records `CycleFacts` | data thread writes, test reads |
| `test/support/test_sleeps.hpp` (new, forced include in tests) | `ear_inducers` | `induce`, `negative`, `poll`, `pace` (7.3) | any |

The diagnostics counters' headers (`cycle_counters.hpp`, `counter_watch.hpp`) are in 8.4. No PipeWire, Boost, knobly or sendspin-cpp type appears in any public header here. `CutRecord` lives in core because
core's `CycleAccount` consumes it (the interface lives with its consumer); `ear_pipewire` and the harness implement it.

### 6.2 `ear/core/cycle_facts.hpp`

```cpp
// Pure, realtime-safe: nothing here locks, allocates or waits (docs/engine.md#graphcycle once built).
#pragma once
#include <cstdint>
#include <optional>
#include "ear/core/latency.hpp" // FrameClock
namespace ear {

enum class CycleCause : std::uint8_t {
	start = 1U << 0U,        // the block's first cycle
	drive = 1U << 1U,        // a new code's first cycle (any drive)
	driverChange = 1U << 2U, // another driver than the last cycle's
	catchUp = 1U << 3U,      // missed cycles, every one moving the position by its frames
	absent = 1U << 4U,       // as catchUp, but some missed cycle was not cut: the block was not scheduled
	oversized = 1U << 5U,    // the cycle after one too large for the block
	reposition = 1U << 6U,   // the position moved on the same driver by anything else
};
/// A set of causes; empty is a cycle that follows its last with nothing lost.
class CycleCauses {
public:
	constexpr CycleCauses() noexcept = default;
	constexpr CycleCauses(CycleCause cause) noexcept; // implicit: one cause
	constexpr bool has(CycleCause cause) const noexcept;
	constexpr bool empty() const noexcept;
	constexpr CycleCauses operator|(CycleCauses other) const noexcept;
	constexpr bool only(CycleCauses allowed) const noexcept; // every cause in this is in `allowed`
	friend constexpr bool operator==(CycleCauses, CycleCauses) noexcept = default;

private:
	std::uint8_t bits_ = 0;
};

/// What one cycle states, as the block that ran it accounts for it.
struct CycleFacts {
	std::uint64_t graphCycle = 0;   // the internal clock's cycle number; 0: not on it, or not told (a test's own cycle)
	std::uint32_t missed = 0;       // cycles started on the same driver since the block last ran one
	std::uint32_t missedCut = 0;    // of them, cut short by the driver's own account
	std::optional<bool> lastCut;    // the cycle the block last ran was cut short after it; none: nothing can answer
	std::int64_t jumpFrames = 0;    // position minus the last cycle's end, same driver (0 otherwise)
	std::int64_t lostRawNs = 0;     // raw time from the last cycle's end to this start, beyond its frames
	CycleCauses causes{};
	constexpr bool whole() const noexcept { return causes.empty(); }
	constexpr bool discontinuity() const noexcept { return !causes.empty(); }
	constexpr bool confirmed() const noexcept { return missed == missedCut; }
	friend constexpr bool operator==(const CycleFacts&, const CycleFacts&) noexcept = default;
};

/// Which graph cycles the driver cut short. Implemented by ear_pipewire's DriverCuts and the harness's HarnessCuts.
/// Written only on the thread that runs the cycles; read from any thread by the contract in 6.4 (as built: the
/// header's comment).
class CutRecord {
public:
	/// Cycles started so far; inside a cycle, the number of the one running.
	virtual std::uint64_t started() const noexcept = 0;
	/// Whether cycle `cycle` was cut short; none unless the ring keeps it (cutRecordKeeps).
	virtual std::optional<bool> cut(std::uint64_t cycle) const noexcept = 0;
	/// Cycles cut short so far, each once. No reach.
	virtual std::uint64_t cutsSoFar() const noexcept = 0;
	/// The newest cycle before the running one that ran whole (0: none). No reach.
	virtual std::uint64_t newestWhole() const noexcept = 0;

protected:
	~CutRecord() = default;
};

/// One cycle as the block's driver states it.
struct DriverTick {
	std::uint32_t driverId;
	std::uint64_t graphCycle; // CutRecord::started() at the cycle's entry; 0 when the block is not on the internal clock
	std::uint64_t position;
	std::uint32_t frames;
	std::int64_t startRawNs;
};

/// The one rule (replaces CycleContinuity): each cycle's facts, and whether the code runs it.
class CycleAccount {
public:
	/// `clock`: the graph's, for lostRawNs. `cuts` outlives the account; null: no cut is known (every missed cycle
	/// unconfirmed).
	CycleAccount(FrameClock clock, const CutRecord* cuts) noexcept;
	/// A drive installed a code: its first cycle has `drive`.
	void driven() noexcept;
	struct Step {
		CycleFacts facts;
		bool runs; // at most maxBlock frames
	};
	Step next(const DriverTick& tick, std::uint32_t maxBlock) noexcept;
	/// Where the next cycle starts on the same driver: a drive applied now takes effect from it.
	std::uint64_t nextPosition() const noexcept;
};
} // namespace ear
```

`CycleAccount` needs `FrameClock` (it includes `latency.hpp`). The causes' rule, in order: no last cycle → `start`;
driver id differs → `driverChange` (nothing else is worked out: another clock); else `jumpFrames` and `missed` from the
tick; `missed > 0` and a forward jump of exactly `missed` whole cycles (`jumpFrames > 0`, `% frames == 0`, `/ frames == missed`: 9.2) → `catchUp` if `confirmed()` else `absent`; any other non-zero
jump, or `missed > 0` with no jump → `reposition`; then `drive` and `oversized` added as flags. A block on another
driver than the internal clock (no shipped path makes one: the dummy always drives, `docs/engine.md#the-internal-clock`)
gets `graphCycle == 0`, so `missed` comes from the position alone and nothing is `confirmed`.

### 6.3 The harness

```cpp
// ear/testing/node_harness.hpp, NodeHarness, beside process():
/// How a lost cycle was lost, for loseCycles.
enum class Lost : std::uint8_t { cut, notScheduled };
/// `count` graph cycles start without the fake block running them (its clock moves on by their frames); `cut` marks
/// each cut short in the harness's CutRecord, `notScheduled` leaves them whole (the block was paused). The next
/// process() states them: catchUp or absent.
void loseCycles(std::uint32_t count, Lost how = Lost::cut);
/// The next process() runs on driver `id` (a card made driver): driverChange.
void changeDriver(std::uint32_t id);
/// The next process()'s position moves by `frames` on the same driver with no cycle missed: reposition.
void reposition(std::int64_t frames);
/// The CutRecord the fake block's account reads, for a CycleJudge.
const CutRecord& cuts() const noexcept;
```

The harness advances the raw start by each cycle's frames at the graph's rate, and by the lost ones' too, so
`lostRawNs` agrees with `jumpFrames` as on the tracking dummy.

**Conformance, as R2's table.** `test/support/cycle_table.hpp` (`ear_cycle_table`) holds tick scripts and the facts
each must give (first cycle; drive; drive across a stall; 1, 3 and 70,000 cuts, the last past the ring; a pause;
driver change; oversized then normal; reposition back and by a fraction of a block). `ear_core_tests` runs every row
through `CycleAccount`, `ear_type_harness`'s tests through `NodeHarness`, and `ear_pipewire_tests` the rows an engine
can make (start, drive, induced cut, pause by `PAUSED`) through a real block, each row requiring some fact that is not
empty, so no row passes for a block that states nothing.

### 6.4 `DriverCuts` (`ear_pipewire`, private)

```cpp
// src/pipewire/driver_cuts.hpp
namespace ear::pipewire {
class DriverCuts final : public CutRecord {
public:
	/// Main thread, in Engine::create after the context loaded engine.conf's objects. Finds the internal clock by
	/// node.name; none: EngineError. Allocates the ring (2^16 bits) here only.
	static std::expected<std::unique_ptr<DriverCuts>, EngineError> listen(pw_context* context);
	~DriverCuts(); // main thread: removes the listener through the boundary (leaked on a wedged loop)
	std::uint64_t started() const noexcept override;
	std::optional<bool> cut(std::uint64_t cycle) const noexcept override;
	std::uint64_t cutsSoFar() const noexcept override;
	std::uint64_t newestWhole() const noexcept override;

private:
	/// realtime<>: newestWhole_ = started_ if its bit is clear; started_ + 1 (release); a release fence; clear the new
	/// cycle's bit.
	void onStart() noexcept;
	/// realtime<>: set cycle started_'s bit; if it was clear, cutsSoFar_ + 1 (release). Idempotent.
	void onIncomplete() noexcept;
	std::vector<std::atomic<std::uint64_t>> bits_; // sized at listen, never resized
	std::atomic<std::uint64_t> started_{0};
	std::atomic<std::uint64_t> cutsSoFar_{0};
	std::atomic<std::uint64_t> newestWhole_{0};
	RegisteredRtListener hook_; // the boundary's handle
};
} // namespace ear::pipewire
```

Memory order (as built in `HarnessCuts`, the contract in `CutRecord`'s header comment): `started_`, `cutsSoFar_`
and `newestWhole_` are stored with release and loaded with acquire; a new cycle's slot is cleared only after
`started_` moved on and a release fence. A reader on another thread reads `cut(c)` like a seqlock: `started_`
(acquire), the bit, an acquire fence, `started_` again, and answers only when both readings keep `c`
(`cutRecordKeeps`: begun, not running, fewer than 2^16 behind), so a slot cleared for a newer cycle is never read as
the old one's. A reader on the data thread (the blocks) sees program order. Nothing walks the ring: the account
counts cuts over any span from `cutsSoFar` and `newestWhole` (9.2).

The port block takes `const CutRecord&` from the engine at make (`FilterPortBlocks` already holds the `Engine&`) and in
`onProcess` builds the `DriverTick` from `clock` and `cuts.started()` where it calls `CycleContinuity::next` today.

### 6.5 `test/support/cycle_judge.hpp`

```cpp
namespace ear::test {
struct JudgePlan {
	std::size_t judged;                                                   // boundaries to judge; 0 is refused
	StallPlan induce = StallPlan::every(50, 7, 4, StallLength::cycles(1.5)); // cuts the test makes; {} to make none
	struct Expected {
		std::uint32_t starts = 1, drives = 0, driverChanges = 0, absences = 0, oversized = 0;
	} expected{};
};
enum class Verdict : std::uint8_t { judge, excused, expected, fail };
struct JudgeReport {
	std::size_t judged, excusedCycles, excusedEvents, expected, failed;
	std::size_t stallsMade, stallsPlanned;
	std::string firstFailure; // the cycle, its facts and the rule broken
	bool ok() const noexcept;  // no failure, done
	friend std::ostream& operator<<(std::ostream&, const JudgeReport&); // for INFO
};
class CycleJudge {
public:
	/// Throws std::invalid_argument for plan.judged == 0. `cuts` may be null for a judge fed only facts.
	CycleJudge(JudgePlan plan, FrameClock clock, const CutRecord* cuts);
	/// The stalls the test's upstream code calls in its process() (its index: the code's own cycle count).
	Stalls& stalls() noexcept;
	/// One cycle of the judged node, in order (from its ledger, on the test's thread).
	Verdict at(const CycleFacts& facts, std::uint32_t frames);
	/// A card's block the ramp wrote in `graphCycle` is missing from its file: excused only if that cycle was cut.
	Verdict blockMissing(std::uint64_t graphCycle);
	/// sendspin only (5.4): the library's settle after an excused cycle, `chunks` long, counted.
	void librarySettle(std::uint32_t chunks);
	bool done() const noexcept;
	JudgeReport report() const;
};
} // namespace ear::test
```

A test waits with its `Waiter` for `judge.done() || ledger.full()`, then `INFO(judge.report())` and
`REQUIRE(judge.report().ok())`, as `docs/testing.md#the-tools` says a test waits for its planned count today.

## 7. The text guards

### 7.1 Frames: a `SampleRate` type

```cpp
// ear/core/sample_rate.hpp: a rate in hertz with no arithmetic. Its only exits: FrameClock, and hz() for a library or
// the wire.
namespace ear {
class SampleRate {
public:
	static constexpr std::uint32_t most = 768'000;
	/// None for 0 or above `most`: the boundary where a number becomes a rate (ear.conf, a flag, PipeWire's clock).
	static constexpr std::optional<SampleRate> of(std::uint32_t hz) noexcept;
	/// A rate known at compile time (tests, defaults): SampleRate::fixed<48'000>().
	template <std::uint32_t Hz>
		requires(Hz > 0 && Hz <= most)
	static consteval SampleRate fixed() noexcept;
	/// The exit to PipeWire's properties, libsndfile, sendspin-cpp and JSON. Never an operand (7.1's check).
	constexpr std::uint32_t hz() const noexcept;
	friend constexpr bool operator==(SampleRate, SampleRate) noexcept = default;
	friend constexpr auto operator<=>(SampleRate, SampleRate) noexcept = default;

private:
	explicit constexpr SampleRate(std::uint32_t hz) noexcept;
	std::uint32_t hz_;
};
} // namespace ear
```

- **`FrameClock` takes only a `SampleRate`** (`explicit constexpr FrameClock(SampleRate)`, the `std::uint32_t`
  constructor deleted; `rate()` returns `SampleRate`) and absorbs the frame guard's two exceptions:
  `framesNearest(std::chrono::nanoseconds)` for a peer's latency in ns (`latency_spa.hpp`) and
  `framesCeil(std::chrono::duration<Rep, Period>)`, exact for any `std::ratio` period, for the level window
  (`framesCeil(duration<int64_t, std::ratio<1, 30>>{1})` is `ceil(rate / 30)` exactly).
- **`AudioFormat::rate` becomes a `SampleRate`**, and so do the graph frequency `main.cpp` reads, `EngineSettings`,
  `FadeRamp`/`BlockFade`'s, `SourceRing`'s and the RTP receiver's sender rate. About 34 sites outside the tests read
  `.rate` today [V]; each becomes a `FrameClock` call, a comparison, or `.hz()` at a library or the wire. The wire does
  not change (the records still print a number).
- **What does not compile** (`src/core/tests/compile_fail/sample_rate_bad.cpp`, as `latency_bad.cpp`): `rate * 2`,
  `rate / 30`, `std::uint32_t r = rate`, `FrameClock(48000)`, `SampleRate{48000}`.
- **The check left** (`ear_sample_rate_exits`, replacing `ear_frames_through_frame_clock`): the token `.hz()` appears
  only in the files listed beside it, each a library's or the wire's edge (the property set and the filter's format in
  `ear_pipewire`, the WAV writer, sendspin's library adapter, `ear_control`'s encoder, `ear_config`'s parse is a
  constructor, not an exit). One exact token, a file list a reviewer reads: no operator parsing, no name heuristics. Its
  limit is what an allowed file does with the number, which the list keeps to a few edges.
- **`test/` is in scope.** `StallLength::at(frames, rate)` (`inducers.hpp`) and the test helpers that divide by `kRate`
  move onto `FrameClock`, so the tests and the product share the conversion too.

### 7.2 Callbacks: a compile-time refusal

`src/pipewire/no_raw_callbacks.hpp`, forced into every `ear_pipewire` source (`-include`, a target option), includes
every PipeWire and SPA header ear uses and `callback_boundary.hpp`, then poisons each name the text guard lists
(`#pragma GCC poison pw_loop_add_io pw_loop_add_event ... pw_impl_node_add_rt_listener spa_hook_list_append
SPA_INTERFACE_INIT ...`). GCC refuses any later use of a poisoned name, however it is spelled across lines, but not its
use inside a macro or template defined before the poison, so PipeWire's own inline code and the boundary's templates
compile and a raw registration anywhere else does not. A PipeWire header included only after the forced one fails to
compile (it names a poisoned function), which fails closed: add it to the forced header. The boundary's `.cpp` is the
one source compiled without the forced include. Configure checks that every target linking `libpipewire-0.3` takes the
option (only `ear_pipewire` and its tests do today), so a new target cannot leave it out. The guard script and its
snippet tests go; its refusals become compile-fail cases beside `callback_boundary_bad.cpp`. Tests that drive PipeWire
directly stay outside (their targets do not take the option), as they are exempt today.

### 7.3 Sleeps: roles as calls

A sleep's role becomes the function called: `ear::test::induce(duration, why)`, `negative(...)`, `poll(...)`,
`pace(...)` (`test/support/test_sleeps.hpp`), each taking a non-empty `why` and sleeping once. A forced include in
every test target poisons `sleep_for`, `sleep_until`, `usleep`, `nanosleep`, `clock_nanosleep`, `WARN`, `CATCH_WARN`
and `CHECK_NOFAIL` after the standard and Catch headers, so a raw sleep or a `WARN` does not compile. `luck` and `owed`
have no function: R5b to R5d left none (`test/sleeps.txt` on the branches). `sleeps.txt`, `sleep_guard.cmake` and its
snippets go. Python keeps one rule: `sendspin_server.py` sleeps only through its own `pace()` and `poll()`, and a
check refuses `time.sleep` and `asyncio.sleep` anywhere else in `test/`; one token pair, as 7.1's.

### 7.4 Recommendation, proportionately

`SampleRate` (7.1) and the callback poison (7.2) are worth doing: the first ends the last name-reading guard on product
code, the second guards the one rule whose breach is `std::terminate` in production and already misses the listener
this page adds. The sleep calls (7.3) are cheap but guard test code only, where the audit has already emptied the
inventory: do them last, or not, as review load allows. None changes behaviour.

## 8. Diagnostics counters, per graph and per node

The owner answered Q-CF1 (section 1): counters like the lost cycles belong in ear's core diagnostics, and asked "per
node? per graph?". **Recommended here, which the owner can still overturn: both, because they mean different things.**

- **The graph's counters** say what the host did to the whole graph. A cycle the driver cut is a dropout on every
  output at once (the host starved ear's audio thread); no node caused it alone or can avoid it. They live on the graph.
- **A node's counters** say what happened to that node beyond the graph's own losses: cycles its block missed that the
  driver did not cut (`absent`: the block was not scheduled), and cycles its own work ran past a whole cycle (an
  overrun, which is what makes the driver cut the next one). They live on each node.

Repeating the graph's cuts on every node would make each node look faulty for what the host did, and summing nodes'
misses into the graph would count one dropout once per node. So each fact is counted once, where it belongs.

### 8.1 What is counted, and where its one home is

Every counter is a monotonic total read from a fact that already exists; none is worked out again.

| Counter | Scope | Counts | Its source (never recomputed) |
|---|---|---|---|
| `cycles` | graph | graph cycles the internal clock started | `DriverCuts::started()` |
| `cutCycles` | graph | cycles the driver reported cut short | `DriverCuts`' cut count |
| `stalls` | graph | runs of consecutive cut cycles (one stall of the host, however many cycles it took) | `DriverCuts`: a cut whose previous cycle was not cut |
| `lostMs` | graph | time the graph lost | encoded from `cutCycles` (each cut is one block: `FrameClock::ns`), never kept |
| `missedCycles` | node | cycles its block missed that the driver did not cut | the block's `CycleFacts`: `missed - missedCut` |
| `overruns` | node | cycles in which its block's own work (sources, code, fade, taps) took longer than the cycle | the block: one raw clock read at the end of `processCycle`, against the entry read `onProcess` already takes |
| `repositions` | node | cycles with `reposition` (the position moved by what no missed cycle explains) | `CycleFacts::causes` |
| `driverChanges` | node | cycles with `driverChange` | `CycleFacts::causes` |
| a sink's `dropped`, a source's `shortfall` | the node's companion | as `companions.md` 3.7 designs them | `Sink::droppedFrames()`, `Source::shortfallFrames()`; they stay on the companion's entry |
| an RTP receiver's late or underrun packets | the node's companion | only if module-rtp-source ever gives a count (it gives none ear can read today) | the companion's entry |
| `sendspin`'s library re-locks, a type's own counts | the node | once slice 3c routes the library's log, and any count a type observes | a status field (`control-plane.md` 3.1: what a type observes is status, not a framework counter) |

The last three rows are not new counters: they name the homes other designs already give those facts, so a client finds
every count of a node on that node (its `counters`, its companions' entries, its status) and nothing twice. A legacy
handoff's drops stay in `node.gone`'s `droppedFrames` until 3c retires handoffs.

`repositions` and `driverChanges` are per node because each block sees its own; on the tracking dummy both stay zero
(S6, section 9.1: the 1127-frame jumps were the default `resync.ms`'s, and the driver restarts only when it drives no
node, so no running block sees a restart), and a non-zero value is a finding, not noise.

`overruns` measures the block's own work, from its entry to its end, not from the cycle's start: a late wake is the
host's and shows as the graph's `cutCycles`, not as the node's fault. It is the count behind the code
`ear.realtime-overrun` that `control-plane.md` 4.6 reserves; this page builds the count, and the code stays slice 5's
to put on the wire as a diagnostic.

### 8.2 Totals, scope and reset

- **Graph counters run from the engine's start** (ear's start): `DriverCuts` begins at `Engine::create`.
- **A node's counters run from the node's add** and survive its restarts, stops and changes of settings, as
  `control-plane.md` 3.3 decides for node diagnostics ("diagnostics belong to the node"). A remove drops them; a node
  added again under the same name starts from zero. If the node's block is made again within its life, `NodeHost`
  carries what the old block counted when it is destroyed and adds the new block's counts to it: the totals never step
  back.
- **Companion counters run per configuration**, as `companions.md` 3.7 already decides (a companion dies with its
  configuration). That is the one place two scopes meet, and the wire keeps them apart: node `counters`, companion
  entries' counts.
- **Nothing resets a counter** but its owner's end. A client that wants "since I looked" keeps the last value it saw
  and subtracts. No reset request is offered: a reset by one editor would hide the history from every other client and
  from standard output.
- **Width.** Every counter is a `std::uint64_t` in C++ and a JSON number on the wire; a cycle count stays below 2^53 for
  over a million years at 48 kHz / 256, so no client loses precision.

### 8.3 How they are reported

**On the control port.** A wire change, so `docs/control-port.md`, the fixtures in `test/protocol/`, `src/control/` and
`web/src/protocol.ts` change in one commit (`docs/control-port.md#changing-the-wire`). It is additive, so it is a
**feature, `counters`**, not a new `protocol`:

- The Graph gains `"counters": {"cycles", "cutCycles", "stalls", "lostMs"}`; each node gains
  `"counters": {"missedCycles", "overruns", "repositions", "driverChanges"}`, all zero for a node whose block was never
  made. Both are read in the one main-loop turn that makes the snapshot, so they agree with the rest of it.
- **`counters.changed`** `{"graph": {...}, "nodes": {"<name>": {...}, ...}}`: the graph's counters whole, and the whole
  counters of each node whose counters changed since the last one; sent at most once a second, and only when something
  changed. It is a sample, never a record of every change, and it orders with nothing (a client folds it by replacing
  the counters it names). A node it names that the client has not seen is ignored (its `node.added` is on the way).
- The counter names are an open set, as every enum is (`docs/control-port.md#protocol-and-features`): a client shows a
  counter it does not know by its name and number.
- Fixtures: `test/protocol/snapshot.json` gains both objects (under the feature), `counters-changed.json` is new, and
  `unknown-values.json` gains an unknown counter.

**On standard output** (`docs/output-records.md#diagnostic`, and its JSON schema):

- **`ear.graph-lost-cycles`**, no node: fields `cycles` (cut in the second it covers), `stalls`, `ms`, `totalCycles`
  (since start). At most one a second, and only for a second in which something was cut. Like the other node-less
  diagnostics it is standard output only; its numbers are on the wire as the graph's counters.
- **`ear.node-missed-cycles`**, with the node: fields `missed`, `overruns` (since its last such record) and their
  totals. At most once per 10 s per node, only when either moved: a node that misses cycles is a fault worth a line, but
  one that keeps missing must not flood the log (`companions.md`'s K2 paces its companions' drops the same way). It goes
  on the wire as a `node.diagnostic` once slice 5 builds that, as `ear.sink-dropped` does.
- The pace is the records', not the counters': the counters are exact whatever the records leave out.

### 8.4 Interfaces

```cpp
// ear/core/cycle_counters.hpp (ear_core, public): the counters' one home, filled from facts.
namespace ear {
/// A node's counters (8.1), a value.
struct NodeCounts {
	std::uint64_t missedCycles = 0, overruns = 0, repositions = 0, driverChanges = 0;
	friend constexpr NodeCounts operator+(NodeCounts, NodeCounts) noexcept; // NodeHost carries a destroyed block's
	friend bool operator==(const NodeCounts&, const NodeCounts&) = default;
};
/// The graph's counters (8.1), a value; lostMs is encoded from cutCycles, not kept.
struct GraphCounts {
	std::uint64_t cycles = 0, cutCycles = 0, stalls = 0;
	friend bool operator==(const GraphCounts&, const GraphCounts&) = default;
};
/// One block's counters as its cycles state them. One writer (the thread that runs the block's cycles), any reader:
/// each counter is an atomic stored relaxed by its one writer and loaded relaxed (counters are independent; no reader
/// needs two to agree). Allocates nothing; the port block and the harness's fake block each hold one.
class CycleCounters {
public:
	/// The writer, once per cycle the block runs, after its work: the cycle's facts, and whether its work overran.
	void add(const CycleFacts& facts, bool overran) noexcept;
	/// Any thread.
	NodeCounts read() const noexcept;
};

/// Where the graph's counters come from (ear_pipewire's Engine, over DriverCuts; the harness has none).
class GraphCounterSource {
public:
	/// Any thread.
	virtual GraphCounts graphCounts() const noexcept = 0;

protected:
	~GraphCounterSource() = default;
};
} // namespace ear

// ear/core/node_host.hpp: PortBlock gains
/// Any thread: the counts this block's cycles stated (zero before its first cycle).
virtual NodeCounts counts() const noexcept = 0;
// NodeHost gains (main thread)
/// The node's totals since its add (8.2): what its destroyed blocks counted plus its block's now; none for no node.
std::optional<NodeCounts> counts(std::string_view node) const;

// ear/core/counter_watch.hpp (ear_core, public): the one pacer of 8.3, on the main loop.
namespace ear {
/// Told the counters that changed, at most once a second. Implemented by ear_control (StatusLog's records and the
/// gateway's counters.changed); declared here, with its consumer.
class CounterListener {
public:
	struct NodeEntry {
		std::string name;
		NodeCounts total;
		NodeCounts sinceLast;
	};
	virtual void countersChanged(const GraphCounts& total, const GraphCounts& sinceLast,
	                             std::span<const NodeEntry> nodes) = 0;

protected:
	~CounterListener() = default;
};
/// Main thread. Each second (a Timers timer) reads the graph's counts and every node's (NodeHost::counts), compares
/// them with what it last told, and tells the listener only what changed. Keeps one entry per node, made at add and
/// dropped at remove (main thread; never on the data loop). StatusLog paces the per-node record to 10 s itself.
class CounterWatch {
public:
	CounterWatch(Timers& timers, const GraphCounterSource& graph, const NodeHost& nodes, CounterListener& listener);
};
} // namespace ear

// ear/pipewire/engine.hpp: Engine implements GraphCounterSource
GraphCounts graphCounts() const noexcept override; // from DriverCuts (started, cuts, stalls)
```

- **Threads.** The data thread writes a block's `CycleCounters` and `DriverCuts`' counts; the main loop reads them
  (`NodeHost::counts`, the snapshot, `CounterWatch`); the control thread never reads them, it gets the main loop's
  copies through the gateway as every other graph fact. Nothing on the data loop allocates, locks or formats; the
  per-node map lives on the main loop.
- **`DriverCuts`** gains a `stalls_` count (in `onIncomplete`, when the previous cycle's bit is clear): one more relaxed
  store. **The port block** reads the raw clock once more at the end of `processCycle` (vDSO, no system call) for
  `overran`, and calls `counters_.add(facts, overran)` there.
- **The harness** holds a `CycleCounters` too and gives `NodeHarness::counts()`, so a type's tests see a node's
  counters move under `loseCycles(n, Lost::notScheduled)` exactly as the engine's do; `Lost::cut` moves none of them
  (only the graph's would move, which the harness does not keep).
- **Packages.** `cycle_counters.hpp` and `counter_watch.hpp` are core (pure, no thread: `CounterWatch` uses core's
  `Timers`); `ear_pipewire` implements `GraphCounterSource` and `PortBlock::counts`; `ear_control` implements
  `CounterListener` and encodes both the records and the wire; `main.cpp` wires them. Dependencies keep pointing one
  way, and no PipeWire type reaches a public header.

### 8.5 The editor

Proposed, not over-built (`docs/editor.md` changes with it):

- **Nothing when nothing was lost.** A box with every counter at zero looks as today.
- **A node's mark.** When a node's `missedCycles` or `overruns` moved in the last minute (on the browser's clock, from
  `counters.changed`), a small warning mark sits beside its state word, the same mark companions' badges use for a
  counter above zero (`companions.md` 3.7); the tooltip and the side panel give the totals in words ("missed 12 cycles
  it was not scheduled for; its work overran 3 times; since it was added"). Not per port: these are the node's, and the
  per-port meters (the owner's R30) stay what they are, the audio itself.
- **The graph's badge.** Beside the "not realtime" badge, "dropouts" with the count of the last minute, shown while
  `cutCycles` moved in it, its tooltip the totals ("the host held up the audio thread: 12 dropouts, 64 ms, since ear
  started"). Hidden when the last minute had none.
- Gated on the `counters` feature; an unknown counter is listed in the panel by name.

Decided by the owner (Q-CF4, section 11): the side panel always holds the totals, and the canvas marks and badge
show only while something happened in the last minute.

## 9. Migration

Each step is one mergeable change, green on every test and `make format-check`, with its own review, changing the
library pages it makes stale. Assumed merged first: the engine-test branch (`worktree-agent-ac4b07a6145554fa8`:
`DriverCycles`, the chain judges) and then the sendspin and system branch (`worktree-agent-af4a96a389839d7b5`: R5c,
R5d).

| Step | What | Deletes | Behaviour |
|---|---|---|---|
| **S6** | **Done** (9.1). Spike (`spikes_test.cpp`, test only): on 1.4.2, with induced stalls and a busy thread per core, (a) every cycle a block misses is reported `incomplete` before the next `start`; (b) `clock.cycle` against the listener's count; (c) jumps are whole missed cycles on the dummy, and what makes the others (the 1127-frame jumps: a driver restart's re-anchoring?); (d) the `TRIGGERED` recovery path in `node_ready` loses no follower cycle. Findings go into this page | nothing | none |
| **C1** | **Done** (9.2). `cycle_facts.hpp`, `CycleAccount`, `cycle_table.hpp`; the harness on `CycleAccount` with `HarnessCuts` and `loseCycles`, `changeDriver`, `reposition`. `CycleContinuity` becomes a wrapper over `CycleAccount` with no cuts | nothing yet | none (the conformance rows pin today's discontinuities) |
| **C2** | `DriverCuts`; `listenRealtime<>` in the boundary and `pw_impl_node_add_rt_listener` added to the callback guard's list; the block on `CycleAccount` with the engine's cuts; `GraphCycle::facts` added (`discontinuity`, `xruns` derived from it); the conformance rows through a real block | `CycleContinuity` | none: `facts` is new, the two old members keep their values |
| **C3a** | `cycle_judge.hpp`; `CycleLedger` records facts; `ear_test_support_tests` on the judge (each verdict, a plan of 0 refused, done needing the stalls); `port_block_test.cpp` onto it | `DriverCycles` (`driver_cycles.hpp`), `lostToCuts`, the ledger's own clean rule | none |
| **C3b** | `sound_card_test.cpp`, `source_test.cpp`, `engine_test.cpp`, `sink_test.cpp`, S5's raw rule | `judgeRamp`'s `allCut`, `hostJumps`, `judgeSource`'s raw block, the code's own raw reads | none |
| **C4a** | `SendspinCode` on facts: progress moves by the whole jump on a `catchUp`; its unit test on scripted facts. **If R5c/R5d's open findings show a progress bug, its fix is this step** (not a patch of `missedFrames` first); C1 and C2 are small enough to land ahead of it | `missedFrames`, `nextPosition_`, `lastSkipped()` | yes: progress no longer lags after a short skip or a stall past the feed (Q-CF2: keep in step) |
| **C4b** | `SourceRing::keepTime` on facts; `Chunk`/`ChunkStamp` gain `causes`, `lostFrames`; `GraphCycle::xruns` and `discontinuity` removed (mechanical) | the source's position arithmetic, the two members | `Played` keeps in step across any stall (Q-CF2); `docs/writing-a-node-type.md#sources` and `#sinks` change |
| **C3c** | `clock_test.cpp`, the RTP tests onto the judge (5.4); the feed's dry count | `kSettleNs`, `excusedSteps`, the 600 ms and 0.5 s windows | none |
| **D1** | Counters kept (8.4): `cycle_counters.hpp`, `CycleCounters` in the block and the harness (`NodeHarness::counts`), `PortBlock::counts`, `NodeHost::counts` carrying a destroyed block's, `DriverCuts`' stalls, `Engine::graphCounts`; tests: each counter moved by its induced cause (an induced cut moves the graph's and no node's; a `PAUSED` block moves its `missedCycles`; a code that stalls past a cycle moves its `overruns`), in the harness and the engine | nothing | none visible |
| **D2** | Counters reported (8.3): `CounterWatch`, `CounterListener` in `ear_control`; the `counters` feature, `counters.changed`, the fixtures, `web/src/protocol.ts` and `docs/control-port.md` in one commit; `ear.graph-lost-cycles` and `ear.node-missed-cycles` in `docs/output-records.md` and its JSON; the system tests judging from them | `isSkipStep`, the 200 µs step rule | the `counters` feature and two records |
| **D3** | The editor (8.5): panel totals, recent marks (Q-CF4); `docs/editor.md` | nothing | the marks and badge |
| **C6a** | `SampleRate`, `FrameClock(SampleRate)` beside the old constructor, `framesCeil`, `framesNearest`; compile-fail cases | nothing | none |
| **C6b** | `AudioFormat::rate` and every rate to `SampleRate`; the `.hz()` check | `frame_guard.cmake`, `test/frame_guard/`, the `uint32_t` constructor | none |
| **C7** | The callback poison | `test/pipewire/callback_guard.cmake` and its snippets | none |
| **C8** | Sleep roles as calls (optional, 7.4) | `test/sleeps.txt`, `sleep_guard.cmake`, `test/sleep_guard/` | none |

Order: S6 first (it can change 4.1's causes), then C1, C2, C3a; C4a may jump ahead of C3a when sendspin's bug needs
it. C3c needs the sendspin branch merged and C4a (its judge reads progress). C6 and C7 are independent of the rest and
of each other and may land at any point; C7 before C2 would let C2 add its listener to the poison list instead of the
text guard. D1 follows C2 and C4b (it reads the facts and `absent`); D2 follows D1, D3 follows D2. C1 to C4 come before 3c, which
moves `sendspin` onto a source, so C4a's rule moves into `SourceRing` once, already on facts; the D steps may land
before or after 3c (3c's library log adds no framework counter: re-locks are status, 8.1).

### 9.1 Found in S6

Three test cases in `spikes_test.cpp` (tag `[s6]`; the first also `[s2]`, so serial), kept as regressions: they read
PipeWire through a test-only realtime listener on the internal clock (`S6Driver`, the shape `DriverCuts` will have,
plus the `complete` event and the driver's clock flags) and bare probes (`probes.hpp`, which gained `setActive`), with
no product change. PipeWire 1.4.2's `impl-node.c` (`node_ready`, `process_node`, `trigger_targets`), `private.h`
(`trigger_target_v1`, the activation states) and `node-driver.c` (`on_timeout`, `do_start`) were read beside them.

- **(a) Every cycle a node misses is reported cut short; no silent loss was found.** The chain u → m → d and a sibling s
  at a 64-frame block beside a busy thread per core, each probe stalling the data thread at its own planned runs (1.5
  to 150 cycles: 78 stalls a run), and the host's own stalls with them. Over 30 runs beside `yes` × 5 on 4 cores:
  180,044 graph cycles, 40,397 cut short, and 119,573 cycles missed by some probe between two it ran, **every one
  reported `incomplete`**; no probe ran a cycle twice or out of order. Idle runs agree. Why it holds, from the source:
  every follower has the driver as a target (`to_driver_peer`) and the driver counts them (`state->pending`), so the
  driver's activation is `FINISHED` only when every non-async follower finished the cycle; `node_ready` emits
  `incomplete` for any other state but `TRIGGERED`. The paths the design worried about:
  - *The `TRIGGERED` path* (the graph's last node finished, but the driver's own wake had not run when the next timeout
    came): no `incomplete`, and correctly so, since every follower finished. S6 makes it on purpose by stalling d, the
    last node: 606 such cycles over the 30 runs (about 20 a run, d's 16 planned and the host's), none missed by any
    probe. Every cycle got exactly one `complete` (a cut one from the recovery, with `XRUN_RECOVER` set).
  - *The driver's timer firing twice before the data thread wakes*: it cannot. The timer is a one-shot absolute
    timerfd (`set_timeout`, `SPA_FD_TIMER_ABSTIME`), re-armed at the last deadline plus a cycle, and `on_timeout`
    ignores the expiration count: one timeout is one `node_ready`, one `start` and one block of position. After a stall
    the deadline is in the past, so the timeouts fire back to back (the catch-up burst); during it the timer is ready
    before the second node of a chain is, so most burst cycles are cut after the head, and each is reported (S6's 150-
    cycle stalls).
  - *A follower still running when the next cycle starts*: not on one data loop. `node_ready` runs on the same thread,
    so it waits for the follower's `process` to return; a follower triggered but not yet woken is reset to
    `NOT_TRIGGERED` (counted in its own `xrun_count`), its stale eventfd later finds it not triggered and runs nothing,
    and the driver, not `FINISHED`, reports the cycle cut.
  - *A cut cycle a node still ran*: the driver's cut means some node did not finish, not that every node missed it (a
    run counted 1,148 cuts and the chain's tail missed 1,146 of them). `missed` counts only what the block missed, and
    `lastCut` (4.1) is what tells nodes after it may not have heard: no change.
- **The inside-ports test's rare case** (`root-classes.md` 5.5: an unclean recorder cycle across graph cycles not
  all cut, once in about 240 loaded runs) was not reproduced: the test passed 400 runs of 400 beside `yes` × 5 on 4 cores,
  and S6's probes, the same shape of chain, never lost a cycle silently. Its failure predates the message that names
  each cycle's cut and completes, so its cause is not known; nothing S6 saw points at PipeWire. If it recurs, the
  message now says what the driver saw.
- **(b) `clock.cycle` is the listener's count plus a fixed offset**, in every cycle the listener saw start and every run
  of every probe (offset 0 here: the listener was made before the driver's first cycle). Not diverging, not offset by
  a cut, a burst or the `TRIGGERED` path. It wraps at 2^32 cycles (66 days at 64 frames, 265 at 256). The design's
  choice of the listener's own count stands (4.2 says why it must).
- **(c) What makes a jump that is not whole cycles.** With `engine.conf`'s `resync.ms`, none on a running block: the 30
  loaded runs' 119,573 missed cycles each moved the position by exactly its frames, as `on_timeout` adds
  `clock.duration` per timeout. Two causes exist, and neither reaches a running block:
  - *The default `resync.ms`* (10 ms). The sendspin clock test's comment with 1127, 1156 and 1189 frames came in
    5bf75b4d1e, an hour before c1bb9d246b set `resync.ms` beyond any stall. S6 runs `engine.conf` without the setting:
    each 30 ms stall makes a jump of the raw time lost (1,450 to 1,460 frames), and the reset after the catch-up burst
    steps the position back (3 times in 3 stalls).
  - *The driver started again.* The dummy stops when it drives no node (all gone: state `IDLE`), and `do_start`
    clears `last_time`, so its first timeout puts the position on the raw clock (`position = current_position`). The
    next node's first run is the next cycle by count (none missed) with the position moved by the raw time between,
    which is less than a cycle when the gap is short: S6 measured jumps of -138 to -151 frames, a step back. A block
    keeps the driver running (every block is always-processing), so only a block made after every other one went sees
    it, and its first cycle is `start` anyway. `reposition` keeps its place for it.
  - *A node made inactive* (`pw_filter_set_active(false)`) does **not** stop the driver: node_ready skips an
    `INACTIVE` activation, the driver runs on, and the node, active again, missed every cycle started meanwhile, none of
    them cut, its position moved by exactly their frames: the design's `absent`, confirmed as a real shape (a paused
    block is the product's case of it).
- **(d) No mark a block can read on a lost cycle, reliable or not.** `SPA_IO_CLOCK_FLAG_XRUN_RECOVER` is set on the
  driver's clock only around the driver's own `process_node` in `node_ready`'s recovery and cleared before any follower
  is triggered: no probe ever read it in any run. `clock.xrun` stays 0 (only an ALSA driver sets it), and the
  `xrun_count` node_ready adds is on the late follower's own activation, which a filter does not see. What a block can
  read is `clock.cycle` and `clock.position`, which say that cycles were missed, not whether they were cut. So Q-CF3's
  option (b) does not exist on 1.4.2; since S6 found no silent loss, Q-CF3 is not asked (section 11).

**What changes in this design:** nothing in its interfaces. `CycleAccount`'s causes stand as 4.1 has them (`catchUp`
and `absent` both seen as designed; `reposition` only from a restart or the default `resync.ms`, so a non-zero
`repositions` counter is a finding); `DriverCuts` needs only `start` and `incomplete`, as 4.2 says, and must not read
the clock through a filter's mapping (above). Its listener should be made before the first block, as 4.2 already
orders, so blocks' first cycles have numbers.

### 9.2 Found building C1

What C1 built is described where it lives (`docs/writing-a-node-type.md#lost-cycles`, `docs/testing.md`'s
`ear_core_tests` row, the headers); this section keeps only where it departs from sections 4 and 6, and what the steps
after it must know.

- **`xruns` keeps today's count, not a sum of `missed`.** 4.2 says `xruns` becomes a running sum of `missed`; C1 and
  C2 both promise the old members keep their values, and the sum would count a three-cycle stall three times where
  today's block counts the jump once. `CycleAccount::xruns()` (an addition to 6.2) counts what `CycleContinuity` counted:
  one per forward jump on the same driver. C2 keeps it so; whether it ever becomes the sum is moot, since C4b removes it.
- **Cuts are counted for any span, not walked in the ring** (the review's root fix: as first built, a stall past the
  ring was `absent`, a pause to C3a's judge and D1's counters). `CutRecord` has no `count(from, to)`; it keeps two
  monotonic values written on the data thread, `cutsSoFar` (once per cut cycle, however often `incomplete` is told)
  and `newestWhole` (set at each start to the cycle before when that one was not cut). `CycleAccount` keeps, for the
  last cycle L it ran, `cutsSoFar` read at L's entry; at cycle N the difference D counts L's cut and the missed
  cycles'. Every missed cycle was cut exactly when `newestWhole` ≤ L, and then `missedCut = missed` and `lastCut = D -
  missed`, for any span. Only when some missed cycle ran whole is the ring asked whether L was cut (`missedCut = D -
  cut(L)`); past its reach, D = 0 and D = missed still decide it, and only 0 < D < missed leaves `lastCut` none and
  `missedCut` a lower bound (D - 1). Nothing walks the ring on the data thread. The ring stays for which-cycle
  questions (`cut(c)`, `blockMissing`). `cutRecordCycles` (2^16) and `cutRecordKeeps` are declared beside
  `CutRecord`: a ring of 2^16 slots answers for the 65,535 cycles before the running one (6.4's "older than
  `started_ - 2^16`" was one off). Rows pin it: 70,000 cuts are `catchUp` with `missedCut` 70,000; a pause past the
  ring is `absent` with `lastCut` false; past the ring with one cycle not cut, `lastCut` none and `missedCut` 69,998.
  C2's `DriverCuts` must give the same.
- **`lastCut` is `std::optional<bool>`**: `false` on a block's first cycle, none with no record, after an unnumbered
  cycle, or in the undecided case above. `CutRecord`'s cross-thread contract (seqlock reads of `cut`) is written in its
  header and `HarnessCuts` keeps it with atomics; a test reads a record from another thread while three ring-lengths of
  cycles are written and requires no wrong answer.
- **catchUp and absent need a forward jump of whole cycles.** The test is `jumpFrames > 0`, `jumpFrames % frames ==
  0` and `jumpFrames / frames == missed`, with no signed product, so a cycle of no frames (a row) or any jump cannot
  overflow into a false match. `jumpFrames` is worked out modulo 2^64: exact for any jump under 2^63 frames, which is
  also the limit of the claim that `CycleContinuity` decides as before (its old `position > nextPosition` and the
  wrapper's `jumpFrames > 0` agree below it).
- **The missed cycles' frames are this cycle's.** 6.2 leaves `missed * frames` unspecified: the rule takes each missed
  cycle to have the frames of the cycle that states them, for `catchUp`'s exactness and for `missed` off the numbered
  clock (whole cycles of a forward jump). ear forces one quantum (`docs/engine.md#the-quantum-guard`), so they agree on
  the engine; the harness's `loseCycles` moves the clock by the next `process()`'s frames to match.
- **What is worked out where.** `jumpFrames` and `lostRawNs` only on the same driver (0 on `start` and
  `driverChange`), `lastCut` whenever the last cycle had a number, across a driver change too (it is a fact of the last
  cycle, not of the clock).
- **`CycleContinuity` misses one cause.** With no cycle numbers it cannot see cycles missed with no move (the
  `reposition` 4.1 gives `missed > 0` and no jump), so the wrapper pins every row's discontinuity but that one, where
  it says whole. No shipped path makes it (S6: every missed cycle moved the position), and C2 gives the block numbers.
- **`HarnessCuts` is public** (`ear/testing/harness_cuts.hpp`, `ear_type_harness`), not private to the harness: the
  table's runner in `ear_core_tests` keeps one to script ticks, so the record the harness reads and the one the rule is
  tested against are one class. It is told as `DriverCuts` will be (`start()`, then `incomplete()` for the running
  cycle).
- **`NodeHarness::facts()`** (an addition to 6.3): the last `process()`'s facts. `GraphCycle::facts` is C2's (adding it
  now would hand every real code empty facts), so until then a type's test reads them here; the table's harness runner
  also checks that the code's `GraphCycle::discontinuity` agrees.
- **What the harness cannot make**: a cycle over `maxBlock` (`process()` refuses it, as before) or of no frames (0
  means `maxBlock` there), lost cycles of other frames than the next one's, a cut of the cycle the block ran
  (`lastCut`) and cycle numbers that move without the clock. The table marks no rows by hand: each step says whether
  the harness can make it (`CycleStep::inHarness()`); 14 of the 21 rows run in the harness, all through `CycleAccount`
  and `CycleContinuity`, each checking the `xruns` its cycles count. C2's engine runner should derive its rows the same
  way. `loseCycles` refuses (`std::logic_error`) more than 2^32 - 1 lost cycles before one `process()`.
- **Off the numbered clock** (after `changeDriver`) a loss is `absent` and a reposition by whole cycles reads as that
  many missed (`absent`): the harness's header and `docs/writing-a-node-type.md#lost-cycles` say `catchUp` and a pure
  `reposition` hold on the numbered clock only, and rows pin both. A driver change in the same step as a loss is
  `driverChange` alone.
- **A quantum change across a loss fails toward `reposition`**: the jump is not whole cycles of the new frames. ear
  forces one quantum, so the engine never makes it; a row pins the direction.
- **The harness's times follow time, not position.** Its `startNs` and `startRawNs` were the position's time; they now
  count the frames of time gone (cycles run and lost), which differ from the position only after `reposition` or on
  another driver. Before any of the new calls every cycle is as it was. Its driver id stays 0, the numbered clock.
- **Small additions to 6.2**: `CycleCause | CycleCause`, `CycleCauses::bits()` (for a test's message, never a rule),
  default member initializers on `DriverTick`. `CycleContinuity`'s account is made on a nominal
  `FrameClock`: it is given no raw time and reads only the causes.

## 10. Decided here (technical)

- One carrier (`GraphCycle::facts`), not a second view.
- The listener counts cycles itself; blocks name their cycle by its count, not `clock.cycle` (6.4).
- `absent` and `reposition` are causes of their own, so a pause and a re-anchoring are never excused as a stall.
- The judge excuses by cause and count, never by time; downstream dynamics are split off and named (5.4).
- An induced plan is the default, so every timing test proves its excuse path each run.
- `StallWatch` stays as it is (4.4).
- Counters: each fact counted once where it belongs (graph or node), monotonic, never reset but by its owner's end;
  a node's survive its restarts, companions' are per configuration as designed (8.2); on the wire as the additive
  feature `counters` with one paced notification, `counters.changed` (8.3).

## 11. Questions for the owner

**Q-CF1 (answered, 2026-11-10).** Should ear tell you when the audio thread fell behind and the graph lost cycles? The
owner: "Yes. We should include counters like this as part of the core diagnostics. Per node? Per graph?" Section 8
answers his question with both, each for what it means (graph: what the host did to every output; node: what happened
to that node beyond it), as a recommendation he can still overturn.

**Q-CF2 (answered, 2026-11-10).** After a stall longer than a node's buffer, should what ear reports as played keep in
step with the clock? The owner: keep in step (section 4.4).

**Q-CF3. If PipeWire turns out to lose a cycle without reporting it, what should the tests do?** *Not asked: S6 found
no such path (9.1). Kept for the day a PipeWire bump or a failure shows one; option (b) has no mark to read on 1.4.2.*

*Background.* The judge excuses only cycles PipeWire's driver says it cut. Spike S6 checks that PipeWire 1.4.2 always
says so. If it finds a path where a cycle goes missing silently, there are three ways on.

*Scenario* (hypothetical; S6 found the opposite: the timer cannot fire twice into one cycle, 9.1). A cycle is lost with
no report, on loaded CI runners about once a minute.

*Options.* (a) Carry a small patch to PipeWire: exact, but ear would build PipeWire itself instead of using Debian's
package (a longer image build, one more pin to bump). (b) Excuse such a cycle only where PipeWire leaves a mark a block can
read on it (S6 looks for one, such as the clock's recovery flag), counted and reported: nearly as strict, no patch,
and it holds only if the mark is reliable (S6: none exists on 1.4.2). (c) Report it upstream and let those tests fail on loaded runners until it is fixed.

*Recommendation: (b) if S6 shows the mark is reliable, else (a)*, with the upstream report either way. Only asked if
S6 finds such a path.

**Q-CF4. Should the editor show dropouts and a node's missed cycles without being asked?** *Answered: panel plus
recent — the totals in the side panel, and the canvas's marks and badge only while something happened in the last
minute (8.5).*

*Background.* With section 8 ear counts the graph's dropouts (the host held up ear's audio thread, every output went
quiet for a moment) and, per node, cycles that node missed on its own or in which its work ran too long. The editor can
show these on the canvas the moment they happen, or keep them in the side panel for when you go looking.

*Scenario.* You are routing FPP's stream to the MOTU and something clicks now and then. Shown unasked: a "dropouts: 3"
badge appears at the top for a minute after each click, and if the `rtp` node's own work was too slow its box gets a
small warning mark, so you see at once whether it was the host or the node. Panel only: the canvas stays as it is, and
you open a node's panel to read its counts; the dropout count shows only in a graph panel you open.

*Recommendation: shown unasked, but only while something happened in the last minute* (8.5), totals in the tooltip
and panel. A canvas that stays quiet when all is well and points at the trouble when there is some is the reason to
count at all.
