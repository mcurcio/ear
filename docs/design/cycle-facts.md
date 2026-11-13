# Cycle facts, one test judge and a sample-rate type

Status: designed to module level (2026-11-10); Q-CF1 and Q-CF2 answered, diagnostics counters added (section 8);
spike S6 run (section 9.1: PipeWire reports every cycle a node misses, so Q-CF3 is not asked); step C1 built (section
9.2: the facts, `CycleAccount`, the table and the harness, described in `docs/writing-a-node-type.md#lost-cycles`
and `docs/testing.md`); step C2 built (section 9.3: `DriverCuts`, `listenRealtime`, the block on `CycleAccount` and
`GraphCycle::facts`, described in `docs/engine.md#graphcycle` and `docs/architecture.md#the-callback-boundary`); steps
C3a and C3b built (sections 9.4 to 9.6: `CycleJudge`, the ledger on facts, and the inside ports', card, source, engine
and sink tests and S5 on the judge, described in `docs/testing.md#the-cycle-judge`); steps C4a and C4b built
(sections 9.7 and 9.8: `SendspinCode` and `SourceRing` keeping in step on the facts, the sink's stamps carrying them,
`GraphCycle::xruns` and `discontinuity` gone, described in `docs/node-types.md#clock`, `docs/engine.md#graphcycle`
and `docs/writing-a-node-type.md#sources` and `#sinks`); step C3c built (section 9.11: the `sendspin` clock and RTP
tests on the judge, library settles, the feed's dry count, described in `docs/testing.md#the-cycle-judge`); step D1 built
(section 9.12: the diagnostics counters kept, per graph and per node, described in `docs/engine.md#counters`); step D2
built (section 9.13: the counters reported on the control port and standard output, described in
`docs/engine.md#reporting-the-counters`, `docs/control-port.md#counters` and `docs/output-records.md#diagnostic`), the
rest not. Its steps
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
| `sendspin/tests/clock_test.cpp` | `Judge` with 1.5 s windows, the library's lines read from standard error, `lossSteps`, `dryCycles`, `lastSkipped`, `lastHeld`, `lastGot` | 5.4 | `kSettleNs`, `lastSkipped()`, `lastHeld()`, the window logic (below) |
| `rtp_receiver_test.cpp`, `test/system/rtp_test.cpp` | clean cycles plus 0.5 s, late sends within 600 ms | 5.4 | the two fixed windows |
| `test/system/*` ramps and stamps | `isSkipStep`, the 200 µs step | the counters and records (section 8). Built in D2 (9.13): the excuse is the records' count; the shapes stay as classifiers | the excuse by shape |
| The branch's `DriverCycles` | the only reader of the driver's account | `DriverCuts`, in the product | `src/pipewire/tests/driver_cycles.hpp` |

The windows the R5 review left in those tests, each with what it becomes and the step that does it:

| Window | Where | Why it is there | Becomes | Step |
|---|---|---|---|---|
| `kLogReach` (a line matched within the feed and a chunk of cycles from where it was read) and `kLogLag` (32 cycles of reading lag) | `clock_test.cpp` | a library line is stamped with the cycle count when the reading thread got it, not where its audio plays | deleted: the library's log through a callback (the carried patch slice 3c owes) stamped with the feed's write position, so a line names the frame it acts on | 3c's patch (C3c kept it: 9.11) |
| `inLoss` (a loss from its line less `kLogLag` to its end plus `kLogLag`) | `clock_test.cpp` | the same stamping | the loss's frames, from the stamped lines; timing inside it stays unjudged, declared as `librarySettle` (C3c declares it so already) | 3c's patch (C3c kept the lag: 9.11) |
| `heldBy` (a tenth's time as when the WAV was first seen to hold it, looked at every 50 ms) | `test/system/rtp_test.cpp` | ear is another process, with no ledger the test can read | stays: it times the test sender's own late sends, which no counter of ear's counts (9.13) | none |
| A `Lost sync` line matched by size to any step it covers | `test/system/sendspin_system.hpp` | ear prints the library's warning with no position | the line stamped with the frames written when ear printed it, its drop exact | D2, C3c |

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
| `cycles` | graph | graph cycles the internal clock started | `DriverCuts::started()` (its `CutRing`'s) |
| `cutCycles` | graph | cycles the driver reported cut short | `DriverCuts`' cut count (`cutsSoFar`) |
| `stalls` | graph | times the graph fell behind and lost cycles (one stall of the host, however many cycles it cut) | `DriverCuts`' `CutRing`: a cut while no stall is in progress; a stall lasts until a cycle starts on time (9.12) |
| `lostMs` | graph | time the graph lost | encoded from `cutCycles` (each cut is one block: `FrameClock::ns`), never kept |
| `missedCycles` | node | cycles its block missed that the driver did not cut | the block's `CycleFacts`: `missed - missedCut` |
| `overruns` | node | cycles in which its block's own work (sources, code, fade, taps) took longer than the cycle | the block: one monotonic clock read at the end of `onProcess`, against the entry read it already takes (`cycleOverran`) |
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

Built in D2 and corrected in place (9.13 says what moved and why): the library pages hold it as built
(`docs/engine.md#reporting-the-counters`, `docs/control-port.md#counters`, `docs/output-records.md#diagnostic`). What
D2 settled beyond the first writing: the counters are read at the main loop's 100 ms heartbeat; a *loss* (`cutCycles`,
`stalls`, a node's counters) is a change and `cycles` alone never is; `node.added` carries counters too (zero, or what a
port-caused replace carries on); a gone node's last counts reach standard output only; the records wait for the cuts to
hold still for a beat, so a long stall is one record; and every record held back is written at a stop.

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

- **`ear.graph-lost-cycles`**, no node: fields `cycles` (cut since the last such record), `stalls`, `ms`,
  `totalCycles` (since start). At most one a second, once the cuts held still for a beat (or kept moving 10 s). Like the other node-less
  diagnostics it is standard output only; its numbers are on the wire as the graph's counters.
- **`ear.node-missed-cycles`**, with the node: fields `missed`, `overruns` (since its last such record) and their
  totals, `totalMissed` and `totalOverruns`. At most once per 10 s per node, only when either moved, and at once as the
  node goes: a node that misses cycles is a fault worth a line, but
  one that keeps missing must not flood the log (`companions.md`'s K2 paces its companions' drops the same way). It goes
  on the wire as a `node.diagnostic` once slice 5 builds that, as `ear.sink-dropped` does.
- The pace is the records', not the counters': the counters are exact whatever the records leave out.

### 8.4 Interfaces

Corrected before D1 against the code as C2 to C4 left it (9.12 says what moved and why): the stall count lives in
`CutRing`, the one record class `DriverCuts` forwards to (9.3); `GraphCounterSource` waits for D2 with its consumer;
the block counts every cycle its `onProcess` sees, not only those `processCycle` runs; the harness keeps the graph's
counts too, since its record is the same class.

```cpp
// ear/core/cycle_counters.hpp (ear_core, public): the counters' one home, filled from facts.
namespace ear {
/// A node's counters (8.1), a value.
struct NodeCounts {
	std::uint64_t missedCycles = 0, overruns = 0, repositions = 0, driverChanges = 0;
	friend constexpr NodeCounts operator+(NodeCounts, NodeCounts) noexcept; // NodeHost carries a destroyed block's
	friend constexpr bool operator==(const NodeCounts&, const NodeCounts&) noexcept = default;
};
/// The graph's counters (8.1), a value; lostMs is encoded from cutCycles, not kept.
struct GraphCounts {
	std::uint64_t cycles = 0, cutCycles = 0, stalls = 0;
	/// Each cut cycle lost one block of `quantum` frames (ear forces one quantum).
	constexpr std::uint64_t lostMs(FrameClock clock, std::uint32_t quantum) const noexcept;
	friend constexpr bool operator==(const GraphCounts&, const GraphCounts&) noexcept = default;
};
/// The one rule for an overrun, which the port block and the harness's fake block both call: the block's own work,
/// from its entry to its end, took longer than the cycle's frames last.
constexpr bool cycleOverran(std::int64_t workNs, FrameClock clock, std::uint32_t frames) noexcept;
/// One block's counters as its cycles state them. One writer (the thread that runs the block's cycles), any reader:
/// each counter an atomic its writer loads and stores relaxed (no read-modify-write: it is the only writer), read
/// relaxed (a node's counters are independent; no reader needs two to agree). Allocates nothing; the port block and
/// the harness's fake block each hold one.
class CycleCounters {
public:
	/// The writer, once per cycle the block's process sees (one too large to run included), after its work.
	void add(const CycleFacts& facts, bool overran) noexcept;
	/// Any thread.
	NodeCounts read() const noexcept;
};
} // namespace ear

// ear/core/cut_ring.hpp: CutRing counts stalls in incomplete(), ended by onTime() (data thread: the running cycle
// started on time; DriverCuts forwards it, and Engine::cycleOnTime() for the port blocks), and gains
/// Any thread: the graph's counters from this record, read newest-written first (stalls, cuts, cycles, each acquire),
/// so any reading has stalls <= cutCycles <= cycles.
GraphCounts counts() const noexcept;
// src/pipewire/driver_cuts.hpp: DriverCuts forwards it. ear/pipewire/engine.hpp: Engine gains (any thread)
GraphCounts graphCounts() const noexcept; // the internal clock's, from the engine's start

// ear/core/node_host.hpp: PortBlock gains
/// Any thread: the counts this block's cycles stated (zero before its first cycle).
virtual NodeCounts counts() const noexcept = 0;
// NodeHost gains (main thread)
/// The node's totals since its add (8.2): what its destroyed blocks counted plus its block's now; none for no node.
std::optional<NodeCounts> counts(const NodeName& node) const;

// ear/testing/node_harness.hpp: NodeHarness gains
NodeCounts counts() const;       // NodeHost::counts of the harness's node: its fake block's CycleCounters
GraphCounts graphCounts() const; // its HarnessCuts' counts: what loseCycles(n, Lost::cut) moves
```

D2 added, with their consumer, as built (9.13; `ear/core/counter_watch.hpp` is their home):

```cpp
// ear/core/counter_watch.hpp (ear_core, public): the one pacer of 8.3, on the main loop.
namespace ear {
class GraphCounterSource { virtual GraphCounts graphCounts() const noexcept = 0; };            // Engine; any thread
class NodeCounterSource { virtual std::optional<NodeCounts> counts(const NodeName&) const = 0; }; // NodeHost
/// Told by NodeHost (NodeHost::subscribeCounts): where each name's span of counts begins and ends.
class NodeCountsListener {
	virtual void countsBegan(const NodeName& node) = 0;                          // not for a port-caused replace
	virtual void countsEnded(const NodeName& node, const NodeCounts& total) = 0; // its final counts, before nodeGone
};
struct NamedCounts { NodeName name; NodeCounts counts; };
/// Each call at its own pace; a listener overrides what it reports (the gateway the first, StatusLog the others).
class CounterListener {
	virtual void countersChanged(const GraphCounts& graph, std::uint64_t lostMs, std::span<const NamedCounts> nodes);
	virtual void graphLostCycles(const GraphCounts& total, const GraphCounts& since, std::uint64_t sinceMs);
	virtual void nodeMissedCycles(const NodeName& node, const NodeCounts& total, const NodeCounts& since);
};
/// Main thread: tick() at the heartbeat, flush() at a stop.
class CounterWatch final : public NodeCountsListener {
	CounterWatch(const Timers& timers, const GraphCounterSource& graph, const NodeCounterSource& nodes,
	             AudioFormat format);
	void subscribe(CounterListener& listener);
	void tick();
	void flush();
};
} // namespace ear
// Engine implements GraphCounterSource; NodeHost implements NodeCounterSource and tells NodeCountsListener;
// GraphStatus reads both into the snapshot; MainLoopGateway::readCounts gives node.added a port-caused replace's counts.
```

- **Threads.** The data thread writes a block's `CycleCounters` and the record's counts; the main loop reads them
  (`NodeHost::counts`, the snapshot, `CounterWatch`); the control thread never reads them, it gets the main loop's
  copies through the gateway as every other graph fact. Nothing on the data loop allocates, locks or formats; the
  per-node totals live on the main loop.
- **The stall count is `CutRing`'s** (so the harness's record keeps it too): in `incomplete()`, when the running cycle
  is marked cut for the first time and no stall is in progress, one relaxed load and a release store more, and a
  stall is then in progress until a cycle starts on time. The port block tells `Engine::cycleOnTime()` when a cycle
  of the internal clock is not overdue (`GraphCycle::overdueNs` 0), a plain store on the data thread, before that
  cycle's cut can be told; the harness tells its record at each `process()`.
- **The port block** reads the monotonic clock once more at the end of `onProcess` (vDSO, no system call), after
  everything the block does in the cycle, the oversized branch's zeros included, and calls
  `counters_.add(step.facts, cycleOverran(end - entry, FrameClock(rate), frames))`, the entry being the monotonic
  reading `onProcess` already takes for `startRawNs`. So a late wake (the host's, `GraphCycle::overdueNs`) is never
  the node's overrun, and a cycle too large to run still counts what it missed.
- **NodeHost** keeps per node what its destroyed block counted (read just before the block goes, freed or leaked) and
  what a port-caused replace's older node counted (read live until that node is gone, then carried), so `counts()`
  never steps back. A node's block is made once in its life, so the port-caused replace is the one case a name's
  counts span two blocks. `counts(name)` answers for the newest node of the name not gone.
- **The harness** holds a `CycleCounters` in its fake block and times its `process()` work (sources, code, fade, taps)
  on `std::chrono::steady_clock`, so a type's tests see a node's counters move under `loseCycles(n,
  Lost::notScheduled)`, `changeDriver`, `reposition` and a code that stalls past its cycle, as the engine's do;
  `Lost::cut` moves none of them, only `graphCounts()`. `NodeHarness::counts()` is `NodeHost::counts`, so the
  harness answers by NodeHost's rule.
- **Packages.** `cycle_counters.hpp` is core (pure, no thread), and so in D2 is `counter_watch.hpp` (`CounterWatch`
  uses core's `Timers`); `ear_pipewire` gives `Engine::graphCounts` and `PortBlock::counts`; in D2 `ear_control`
  implements `CounterListener` and encodes both the records and the wire, and `main.cpp` wires them. Dependencies
  keep pointing one way, and no PipeWire type reaches a public header.

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
| **C2** | **Done** (9.3). `DriverCuts`; `listenRealtime<>` in the boundary and `pw_impl_node_add_rt_listener` added to the callback guard's list; the block on `CycleAccount` with the engine's cuts; `GraphCycle::facts` added (`discontinuity`, `xruns` derived from it); the conformance rows through a real block | `CycleContinuity` | none: `facts` is new, the two old members keep their values |
| **C3a** | **Done** (9.4). `cycle_judge.hpp`; `CycleLedger` records facts; `ear_test_support_tests` on the judge (each verdict, a plan of 0 refused, done needing the stalls); `port_block_test.cpp` onto it | `DriverCycles` (`driver_cycles.hpp`), `lostToCuts`, the ledger's own clean rule | none |
| **C3b** | **Done** (9.5). `sound_card_test.cpp`, `source_test.cpp`, `engine_test.cpp`, `sink_test.cpp`, S5's raw rule | `judgeRamp`'s `allCut`, `hostJumps`, `judgeSource`'s raw block, the code's own raw reads | none |
| **C4a** | **Done** (9.7, which departs from the progress half). `SendspinCode` on facts: progress moves by the whole jump on a `catchUp`; its unit test on scripted facts. **If R5c/R5d's open findings show a progress bug, its fix is this step** (not a patch of `missedFrames` first); C1 and C2 are small enough to land ahead of it | `missedFrames`, `nextPosition_`, `lastSkipped()` | yes: progress no longer lags after a short skip or a stall past the feed (Q-CF2: keep in step) |
| **C4b** | **Done** (9.8). `SourceRing::keepTime` on facts; `Chunk`/`ChunkStamp` gain `causes`, `lostFrames`; `GraphCycle::xruns` and `discontinuity` removed (mechanical) | the source's position arithmetic, the two members | `Played` keeps in step across any stall (Q-CF2); `docs/writing-a-node-type.md#sources` and `#sinks` change |
| **C3c** | **Done** (9.11, which keeps `kLogReach`, `kLogLag` and `inLoss`'s lag for 3c's patch). `clock_test.cpp`, the RTP tests onto the judge (5.4); the feed's dry count | `kSettleNs`, `kSettleCycles`, `lossSteps`, `dryCycles` from `lastGot()` (and `lastGot()`), the 600 ms and 0.5 s windows (5.5) | none |
| **D1** | **Done** (9.12). Counters kept (8.4): `cycle_counters.hpp`, `CycleCounters` in the block and the harness (`NodeHarness::counts`), `PortBlock::counts`, `NodeHost::counts` carrying a destroyed block's, `DriverCuts`' stalls, `Engine::graphCounts`; tests: each counter moved by its induced cause (an induced cut moves the graph's and no node's; a `PAUSED` block moves its `missedCycles`; a code that stalls past a cycle moves its `overruns`), in the harness and the engine | nothing | none visible |
| **D2** | **Done** (9.13). Counters reported (8.3): `CounterWatch`, `CounterListener` in `ear_control`; the `counters` feature, `counters.changed`, the fixtures, `web/src/protocol.ts` and `docs/control-port.md` in one commit; `ear.graph-lost-cycles` and `ear.node-missed-cycles` in `docs/output-records.md` and its JSON; the system tests judging from them | `isSkipStep`, the 200 µs step rule | the `counters` feature and two records |
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

### 9.3 Found building C2

What C2 built is described where it lives (`docs/engine.md#graphcycle`, `docs/architecture.md#the-callback-boundary`,
`docs/writing-a-node-type.md#lost-cycles`, `docs/testing.md`'s `ear_pipewire_tests` and callback guard rows, the
`pipewire-engine` skill); this section keeps only where it departs from sections 4 and 6, and what the steps after it
must know.

- **One record class, `CutRing`** (`ear/core/cut_ring.hpp`, `ear_core`). 6.4 gives `DriverCuts` its own bits and
  counts; C2 moved C1's `HarnessCuts` into core as `CutRing` (`HarnessCuts` is now an alias of it) and `DriverCuts`
  holds one and forwards, telling it from `onStart` and `onIncomplete`. So the writer's memory order and the seqlock
  read have one home, and C1's cross-thread test covers the engine's record too. `DriverCuts` stays a `CutRecord` of
  its own, private to `ear_pipewire`, and adds `clockId()`.
- **The block needs the internal clock's id**, which 6.4's `const CutRecord&` does not carry (a tick's `graphCycle` is
  0 off the internal clock). `Engine` gains `internalClockId()` beside `cuts()`; the block numbers a cycle only when
  `clock.id` is that id.
- **The handle is `RealtimeListener<Target>`** (6.4's `RegisteredRtListener`), and it holds the target its callbacks
  reach (`DriverCuts`' is its `CutRing`, whose `start` and `incomplete` are the realtime members) as well as the hook.
  On a wedged loop `remove()` (`[[nodiscard]]`, true when left) hands both to the boundary (`keptForever`), which
  never frees them, so the leak is structural: whoever owns the listener may go at once and none can forget, and a
  listener that left one refuses to attach again. `DriverCuts` is destroyed as usual on a wedged loop. Adding is
  PipeWire's asynchronous invoke, so `listen` does not wait: the data loop runs it before any block's node reaches the
  driver (posting order); a test checks `started()` is 0 before the first block, and that the block's facts number
  its first cycle above 0 and every later one higher.
- **The node is found through the boundary.** `pw_context_for_each_global` takes a C function, so the lookup by
  `node.name` (`engineNode`) lives in `callback_boundary.cpp`; the guard does not list that call (it registers
  nothing that outlives it). The name is `engine_names.hpp`'s `internalClockNode`, checked at compile time against the
  property table's row.
- **The block reads both clocks every cycle**, not only on cycles it runs: `CycleAccount` needs the raw start for a
  cycle too large to run as well. Three vDSO reads on such a cycle; nothing else changes.
- **Which rows an engine makes** is `CycleStep::onEngine()`, derived from the steps as C1's `inHarness()` is: the
  table's quantum (the runner's engine runs at the table's 48 kHz / 240), a drive, one cycle cut (a head's
  1.5-cycle stall: a longer one leaves the catch-up's cuts to the loop's order), a cut after the block ran (its own
  stall, a tail after it), or a pause of at most 16 cycles, one of those a step. Four rows qualify: start and drive,
  one cut, a pause, a cut after. Three things the engine cannot make literally, so the runner says how it judges them:
  the cycle numbers (the engine's count since its start) are checked against the driver's own position, which the
  tracking dummy moves on by one quantum each cycle it starts, so the position less the number's quanta is the same
  in every cycle; a pause stands for as many cycles as the engine took to make the block active again (at least the row's),
  its `missed`, `jumpFrames` and `lostRawNs` scaled to them; `lostRawNs` is the driver's planned starts apart, checked
  within its 1 ms clamp. Between steps the block runs cycles the runner does not judge (links settling, a drive or a
  pause on its way): each step's facts are against the block's cycle before it, so only `graphCycle` differs from the
  row. Each planned cut must be recorded on the cycle it was made in (a 1.5-cycle stall cuts on any host, so a missing
  one is the record's fault and fails at once, and a host's cut elsewhere cannot stand in for it), and the record
  must count at least the planned cuts. An attempt the host spoiled (more cuts than planned, or a cycle run before the
  first drive was applied, so the first step is no `start`) is made again, up to eight times, and the test fails
  naming why if none was clean.
- **The pause is made with `pw_impl_node_set_active(false)`** on the block's engine node (S6's shape), on a block linked
  to nothing: made inactive, a linked block would hold its peers back. A test's `EngineNodes::suspend` would not do:
  nothing brings a suspended node back.
- **`stalls_` (D1) is not added**; nothing reads it before D1.

### 9.4 Found building C3a

What C3a built is described where it lives (`docs/testing.md#the-cycle-judge`, `#making-the-rare-event`, its
`ear_test_support_tests` and `ear_pipewire_tests` rows, the header); this section keeps only where it departs from
sections 5 and 6, and what the steps after it must know.

- **A card's judged boundaries are counted by `blockHeard()`**, an addition to 6.5: a card states no facts, so a
  boundary the test heard whole in the card's file is counted as judged through it, and `blockMissing` counts each
  excused block as an excused cycle. `done()` is then the same for a card as for a chain.
- **`librarySettle` is not built**: C3c, its only user, adds it with its rule (5.4).
- **`JudgeReport` carries `judgedPlanned` and `done()`**, so `ok()` (no failure, done) needs nothing but the report.
- **A declared cause with a catch-up** (a drive across a stall) is `expected`, its cut cycles counted excused as well;
  a failed verdict uses up no declared count, so the next cycle is judged as if it had not come.
- **The judge names a cycle's facts with `describe`** from `cycle_table.hpp`, so `ear_cycle_judge` links
  `ear_cycle_table` rather than keeping a second spelling of the causes.
- **`DriverCycles` is gone, and with it the second listener of the driver's events.** The test that compared the
  engine's record with it cycle for cycle now checks the record against what the driver and the chain show
  independently: each cycle's number against the driver's position, each of the head's 1.5-cycle stalls recorded
  cut, and over the tail's span a cycle recorded cut exactly when the tail missed it (a cycle the last node ran was
  finished by every node, so the driver cannot have cut it; S6, 9.1). The tail's last cycle is left out: its cut, if
  any, is told only as the next cycle starts.
- **The ledger keeps no clean rule and no `stalled` flag**: a planned stall no longer marks the cycle after it, since
  the facts say what that cycle lost. `StallingCode` records and stalls as before.
- **What C3b moves was kept compiling mechanically in C3a**: `sound_card_test.cpp` reads `Engine::cuts()` where it read
  `DriverCycles`, `engine_test.cpp` judges over `whole()` cycles where it read `clean`, and `sink_test.cpp`'s take-up
  is judged where its only cause is the drive, where it compared `xruns`. The sendspin clock test and the RTP
  receiver's engine test (C3c's) read a ledger cycle's `whole()` where they read `clean`: neither marked a stall, so
  the two say the same.
- **The take-ups and fade-ins of `port_block_test.cpp` stay on the probe's consecutive positions** (5.5's second row):
  their listener is a bare probe, not a block, so it states no facts, and a take-up is never excused, only made again;
  on the tracking dummy consecutive positions are what `whole()` says (S6). Moving them needs a probe that runs
  `CycleAccount` itself, as C3b's S5 does.

### 9.5 Found building C3b

What C3b moved is described in `docs/testing.md#the-cycle-judge`; this section keeps only where it departs from 5.3 and
5.5, and what C3c must know.

- **The card's clock tests make no stall** (`.induce = StallPlan{}`, `.judged = 16`, the code's drive declared): they
  judge that a card's join and loss leave the blocks' clock alone, and the judged codes are siblings whose order in a
  cycle nothing fixes, so a planned stall in one cuts nothing the other surely misses. The excuse path is proven every
  run by the ramp onto channel 2 in the same file, through the same `Code`.
- **The card's ramp reads the file incrementally** (`RampReader`): the judge counts, so each block is judged once
  however often the growing file is read. `Graph` owns the ramp's judge, made from the engine's record, since the
  codes call its stalls and the record exists only once the engine does. The test still requires each stalled ramp
  cycle among the excused blocks (stronger than `done()`'s count) and waits for the file to pass the last of them.
- **The sink's block under test gains an input bank**, fed by the block that counts the graph's cycles, which makes
  the judge's four 1.5-cycle stalls one at a time while the code runs: a stall in the ramp's own block would cut only
  what follows it, and the ramp's ledger is the judged node. The storm's re-drives each make a `drive`, so the plan
  declares at most `kMostRounds * 202` of them, the most the test asks.
- **The bound source gains an upstream probe** making the default stalls, and S5's part A's unbounded plan becomes 30
  stalls of 12 ms (a plan `done()` can reach). The source-rule model reads the stated `jumpFrames` and stays the
  model; `entryRawNs`, `rawNsNow`, `unlost` and `mostOverLost` are deleted.
- **S5's B states its own facts.** B is a probe, so it runs `CycleAccount` over the engine's record from the tick a
  block builds (the driver's start mapped to the raw clock as `port_blocks.cpp` maps it). U's overruns are the
  judge's stalls (200), and the judge fails what the deleted `notWhole`, `back` and raw-clock (`kJumpSlack`) rules
  failed: a jump not whole cycles or backwards is a `reposition`, one past the raw clock fails the catch-up's check.
  U's own raw read stays: it is S2's (`tooFarAhead`), not a judge of lost cycles.
- **The card's ramp also judges its own block's facts** (`judgeClock` on the ramp's code): a block that did not run a
  cycle leaves the card a whole block of silence, which the file alone excuses as a late block, so without its own
  facts the test passed such a bug (found by the injection below).
- **Proven to fail a real bug**, each injected, the moved tests run and the injection reverted: every block skipping
  the same uncut cycle every 29 (`onProcess` returning early) fails all eight block tests (the judge's `absent`, or
  breaks first); a block's outputs zeroed on such a cycle after its code ran fails the ramps (inside ports, engine,
  card, both sources); a catch-up's `lostRawNs` four cycles short (a wrong date) fails every test that excuses a cut,
  S5 and the sink included. Untouched, as they should be: by the zeroed outputs, the card's clock tests (no audio
  judged) and the sink (its tap is fed before the outputs); by the wrong date, the card's clock tests (no catch-up
  needed on an idle run).

### 9.6 Found in the C3 review

The review found the judge still held windows; each is closed where its rule lives (`docs/testing.md#the-cycle-judge`).

- **The raw-clock rule is the 1 ms clamp alone.** 5.1's two cycles' wake room came from the tests that read the raw
  clock at their own entry; a cycle's `startRawNs` is the driver's deadline mapped to the raw clock, so a late wake
  cancels out, as the cycle table's conformance already judged. `JudgeReport::mostOverLostNs` keeps the evidence the
  deleted `mostOverLost` gave (257 ns in a run that printed it, against the 1 ms allowed). A date one cycle short fails
  every test that excuses a catch-up.
- **A planned stall counts only where it was excused.** `Stalls` keeps the graph cycle of each stall (given the
  judge's record), and `done()` needs each inside an excused verdict's missed cycles (or a card's missing block). A
  judge that plans stalls refuses a null record.
- **A cut's audio is checked**: across an excused boundary the ramp moved on by whole blocks, at least none and at
  most the jump. "At least none", not "at least one": the driver can cut a cycle before the ramp, the chain's head,
  ran it (a catch-up burst cuts before the second node and sometimes the first: 9.1), and a run under load showed it.
- **A card's whole-block silence needs a fact**: the ramp's next cycle must state a catch-up over at least as many cut
  cycles. No declared bound was needed: the card's ramp passed 30 loaded runs and 10 idle with the rule.
- **The card's clock tests count their judged boundaries from the event** (the ledger's index at ready, at the loss),
  with the cycles before judged by a judge of their own.
- **The sink's drives are exact**: the plan bounds them by the most the test asks; the report's `expected` must equal
  the drives taken at distinct positions, since drives the data loop takes before one cycle state one `drive`.
- **Deny by default**: a cause outside the judge's known set fails.
- **The source model checks the position** against the last end moved by the stated jump.
- **One tick helper**: `src/pipewire/driver_tick.hpp` builds a cycle's `DriverTick` from its clock for the port block
  and S5's B alike.

### 9.7 Found building C4a

What C4a built is described in `docs/node-types.md#clock` and the `sendspin` skill; this section keeps only where it
departs from 4.4's row, and what C4b and 3c must know.

- **Progress does not move by the whole jump; the library keeps its own timeline in step.** 4.4's row has the feed
  skip `min(jumpFrames, held)` and progress move by the whole jump. The library takes what it is told played from what
  it wrote (`sync_task.cpp`, `process_playback_progress`: `buffered_frames -= frames_played`, clamped at zero with a
  `Buffered frames underflow` warning) and puts its next audio at the told finish plus what it thinks is still
  buffered. Told the whole jump with only what the feed held skipped, its count of what is buffered falls short of the
  feed by the rest, so it puts its next audio that much early for good, while the audio is really that much late. A
  debt (the rest dropped from what the library writes next, `SourceRing`'s rule) mends that only when the library had
  written the whole jump by the time it reads the report: told more than it wrote, it clamps, and then counts the frames
  the debt drops from its next writes as still buffered, off for the rest of the stream. Told the debt as it is paid
  instead (the first build), the library reads the first report, sees itself late by the rest at its next chunk and
  catches up itself (a late chunk skipped, or a hard sync), and the debt drops the same time again. So the code skips
  what the feed holds of a catch-up's jump, however long, and tells exactly that with the cycle's real finish: the
  library is never told a frame it did not write, and catches the rest up itself, as it always did for a stall within
  the feed. What changed in behaviour: a stall past the feed, which the position rule took for a change of driver
  (nothing skipped, the feed's stale audio played late), is a catch-up like any other, and only a `catchUp` skips (a
  reposition or a pause forward no longer does). Q-CF2's "keep in step" holds for the stream's timing, which the
  library keeps from the finish times; the frames ear reports stay what it took of what the library wrote. **A
  question for the owner** if that reading of Q-CF2 is not what was meant: the frames count can move with the clock
  only if the library is patched to take a skip apart from its buffer.
- **Under heavy load the clock tests fail with or without this step.** 20 runs beside `yes` x 5 on 4 cores, with two
  other agents' full builds and suites on the same host (load 10 to 12), failed 10 times, every failure in the clock
  tests: the worker not connecting within 20 s (4), the library's interpolated frames past 1% at a steady +95 us offset
  (5; not whole frames, so a timing offset, not ear's count), and the library's own losses of sync (+90 ms of its
  silence after skipping a late chunk, a ramp step of a chunk and 257; a 2 s stall with 18% dry cycles). The same
  signature came with the debt's first build and without it. The source and sink tests passed all 20. The step before
  C4a, its clock tests run alternately with this step's under the same load (which other agents raised to 18 to 46 as
  it went), failed 6 of 7 runs where this step failed 3 of 6, in the same ways (too few cycles judged, interpolated
  frames past 1%, steps no rule explained, the clock never tracking): the host, not the step. They are timing tests,
  rerun alone before the code is suspected (`docs/testing.md`).
- **Only `catchUp` skips.** `absent` (the block not scheduled), `reposition`, `driverChange`, `start` and `oversized`
  alone are a new clock, as `SourceRing`'s row says; a `catchUp` with a `drive` (a re-drive across a stall) skips too,
  since the feed and the library's stream outlive the code.
- **The clock test works the skips out from the facts.** `lastSkipped()` is gone; the test's judge skips each
  catch-up's `jumpFrames` as far as `lastHeld()` says the feed held (the code's own rule, modelled), holds the ramp's
  steps and each `PlayProgress` against it, and now also requires that a cycle that skipped and read nothing told
  nothing. What the feed did not hold of a catch-up is lost to the library, which may step the ramp by exactly it
  with a line of its own near (the rule a jump past the feed already had, now for every short skip).
- **`code_test.cpp` states its cycles through `CycleAccount` over a `CutRing`** (a small `Driver` in the test: lose
  cycles cut or not, change driver, reposition), not `NodeHarness`: the code is the old interface's, made by the
  worker, not a type the harness hosts. A catch-up past 2^32 frames is a scripted fact (`Feed::skip` takes the jump
  whole and skips what it holds).
- **For C4b and 3c**: `SourceRing` keeps 4.4's rule (the whole jump in `Played` at once, the rest a debt). When
  `sendspin` moves onto a source in 3c, the library must not be told `Played` as it stands: it would count frames it
  never wrote (the clamp) and, with the debt, catch the same time up twice, as above. 3c must tell the library what the
  source took of what it wrote, or give the source this feed's rule for a writer that keeps its own timeline.
- **Decided (owner, C4 review): the library is patched** (its floor, and its skip of late chunks: 9.9), and in the
  second review the remainder of a stall the feed did not hold is ear's to pay, not the library's to catch up: the code
  owes it and skips it from what the library writes next before telling the library anything (9.9), so a stall past
  the feed is back on the server's stamps within a few blocks of the catch-up's end with no correction of the
  library's. The 4.4 deviation above stands in what it was for: the library is never told a frame ear did not take.

### 9.8 Found building C4b

What C4b built is described in `docs/engine.md` (`#graphcycle`, `#sinks`, `#sources`) and
`docs/writing-a-node-type.md` (`#sources`, `#sinks`, `#lost-cycles`); this section keeps only where it departs from
4.3, 4.4 and 6.1, and what the steps after it must know.

- **Every consumer now reads the facts; none works a loss out from the position.** Searched for in `src/` (position
  arithmetic, an expected or next position, `xruns`, `discontinuity`): `SendspinCode` (C4a), `SourceRing::keepTime`
  (its `expected` deleted), the sink's stamp (`chunkStamp`) and a source's cycle (`sourceCycle`), both copied from
  `GraphCycle::facts`, and the two makers of `GraphCycle` (the port block, the harness). Nothing else in `src/` derived a
  loss: the RTP receivers and cards are PipeWire's, `file` writes what it is handed, and `pass_through` reads no
  cycle. `CycleAccount::xruns()` went with `GraphCycle::xruns`, and with it the cycle table's `xruns` column and its
  checks in the harness's and the engine's runners; the port block's clock test now holds each cycle's position
  against the last end and its stated `jumpFrames`.
- **`Chunk` and `ChunkStamp` hold `causes` in place of `discontinuity`**, not beside it (one home), with
  `discontinuity()` as a member; `lostFrames` is a `catchUp`'s `jumpFrames` and 0 for every other cause (`absent` and
  `reposition` moved the position too, but lost nothing a sink could write as the graph's: `block_rules.hpp`'s
  `lostFrames`). A split chunk's second call has neither. `SourceCycle` holds `facts` in place of `discontinuity`.
- **A source owes only while its writer is not idle.** 4.4 lifts the debt's cap (Q-CF2), which would let catch-ups
  while a source idles between streams pile up a debt that drops the start of the next. An idle writer owes nothing
  (its next frames begin a stream with no place on the clock); `Played` still counts every catch-up's whole jump, so
  it keeps to the clock either way.
- **The first cycle after a stop keeps time afresh** even when it states a `catchUp` with its `drive` (the ring was
  dropped at the stop); otherwise a `drive` with a `catchUp` keeps in step, as before (the stream outlives the code).
- **A cycle too large to run is a new clock to a source.** Its next cycle states `oversized` with no jump (the account
  ran the oversized tick), so the facts carry no frames to skip, where the position arithmetic skipped them. The quantum
  guard means the engine never makes one (`docs/engine.md#the-quantum-guard`); were it to, the facts would need the
  lost cycle's frames, an addition to 4.1.
- **The debt has no width limit**: `std::uint64_t`, paid at most a ring's capacity a cycle; a row drives a catch-up of
  2^32 + 8 frames through the ring (scripted facts: the harness cannot lose that many cycles in a test's time).
- **`source_ring_test.cpp` states its cycles through `CycleAccount` over a `CutRing`** (a small `Clock` in the test),
  so a row's jump is a cause, not a position: catch-ups are whole cycles cut, and every other cause (back, part of a
  cycle, a pause, another driver) is made and shown to skip nothing. The test of positions past 2^32 is gone with the
  arithmetic it tested.
- **The source model** in `source_test.cpp` moves `Played` by the jump on a `catchUp` of any length and counts any
  other jump as a new clock, which must never come (`clocks == 0`).

### 9.9 Found in the C4 review

- **An idle writer's debt is forgotten** (`SourceRing::play`): 9.8 stopped a debt growing while the writer idles, but
  one owed before it went idle survived and, uncapped, dropped the start of the next stream. Now the idle is latched by a
  count `markIdle` moves on, and the next cycle forgets the debt when the count moved, even if the writer wrote again
  before any cycle saw it idle.
- **`sendspin` skips a pause too** (`absent`): the feed's audio of that time is stale, and the library re-anchors from
  the finish it is told. A source does not: a pause leaves `Played` behind the clock by its frames, on purpose
  (`docs/writing-a-node-type.md#sources`).
- **A stall never reaches a cycle as one jump past the feed.** The driver catches a 200 ms stall up in a burst that runs
  the block every other timeout (`docs/engine.md#the-internal-clock`), so the block states a run of one-block
  catch-ups (18 of 256 frames); the feed absorbs them unless the library could not refill it between them. So C4a's
  change for a jump past the feed's capacity never happens on the engine; what keeps the stream in step is the per-cycle
  skip, which was the old rule's too.
- **After a stall the library went off the timeline, six ways, now each fixed at its root.** Measured with the stall
  test (a 200 ms stall of the data thread, the real server) before the fixes: 11 of 14 runs took two corrections or
  left silence, and every run played late what the feed had not held.
  1. *Its floor fired during the catch-up burst, not inside the stall.* `starved_playtime_floor` (`sync_task.cpp`, top
     of `handle_synchronize_audio`) moved the playtime up to now whenever it trailed now by over 5 ms. During the burst
     the burst cycles' finish (from their start, the driver's nominal time) trails now until the burst ends, so a chunk
     synced mid-burst floored a playtime that was only behind, with frames still buffered; the library took that chunk
     for about 67 ms late, dropped it, and added the silence back at the next. Fixed by
     `cmake/sendspin-starved-floor.patch`: floor only when `buffered_frames == 0`, the spec's underrun recovery.
  2. *The burst's reports were dated behind now.* Fixed in ear: nothing is told from a cycle that caught lost time up.
  3. *It skipped whole chunks past the timeline.* `decode_chunk` skipped every chunk whose start was over 5 ms late;
     late by 154 ms it skipped two 96 ms chunks, landed 37 ms early and filled that with silence. Fixed by
     `cmake/sendspin-late-chunk.patch` (`SyncTask::chunk_too_late`): decode first and skip only a chunk whose last
     frame is late.
  4. *Ear skipped too little* (the second review). The burst drains the feed faster than the library refills it (it
     ran dry for 14 of the burst's 18 cycles), so `min(jump, held)` left 2560 to 7168 frames of each stall unskipped,
     which played late until the library re-synced (4352 to 5888 frames after the burst's end). With the reports
     deferred the debt 4.4 gives a source is race-free here: the library is told nothing until it is paid. The code owes
     each catch-up's and pause's jump, and while the catch-up is untold each short cycle's zeros (the library cannot see
     them either); pays it from the feed as the library refills it, before reading; and tells the library from the
     first cycle that lost nothing once nothing is owed, the finish the end of that cycle, where the library's next
     frame plays. A clear forgets the debt.
  5. *A starved library was caught up twice* (the third review). With the floor patched, the library realigns itself
     exactly when it sees itself starved: everything it wrote told played, so its next chunk starts at now. A catch-up
     with the feed dry and every frame told owed its jump all the same, and ear dropped the first frames of the refill
     that the library had already placed at now. The root: ear owed with no model of what the library would correct
     itself. Fixed by `LibraryView` (`library_view.hpp`): the library holds the feed's frames plus those consumed and
     not told, and is starved when that is 0; nothing is owed while it is starved. One invariant, written in
     `docs/node-types.md#clock`: ear and the library never both correct the same lost time. (A second check, forgetting
     the debt at a cycle that ends with the library starved, was dropped in the follow-up as dead: a debt is made only
     while the library holds untold frames, and it is told nothing until the debt is paid.)
  6. *The cap fired mid-burst* (the third review). The untold limit, a feed and the longest chunk of cycles, counted
     executed cycles; a burst's cycles are short, so it ran out inside a long stall's burst, forgot the debt and told
     the library from a catch-up cycle dated behind raw time, and the burst's next catch-ups were owed again, caught
     up by ear and the library both. Fixed: the bound is `untoldBound`, 2 s of the graph's nominal time (the cycles'
     `startRawNs`, as the driver dates them), not cycles; when due the debt is forgotten and nothing more is owed until
     the library is told, which happens from that cycle even if it lost frames (the follow-up: a host with no lossless
     cycle for longer would never tell, and at a stream's start the library would never leave its priming; with nothing
     owed no told frame is dropped). 2 s leaves room to pay a 1 s stall's remainder at a feed less a block a cycle.
  With all six the stall tests (200 ms and 1 s) require every run, with no gate: a remainder above 0 (the feed's writer
  is held from the arming through the burst by `Feed::holdWriter()`, so the burst always drains the feed), back on the
  server's stamps within 2 + ceil(remainder / (feed - block)) blocks of the burst's end (one block for the library to
  refill the feed the burst drained, one per feed less a block of the remainder paid, since a cycle that skips all the
  feed holds plays a block of zeros that is owed too, and one for the first frame on time to reach the outputs), no
  correction of the library's own (its soft-sync blends excused as checkStep counts them: a step of at most 3, at most
  once a chunk), no silence after, no `Buffered frames underflow`, every report exact, no debt forgotten by the bound
  while still owed, and 750 cycles within 1 ms. Measured on the final code: ear's payment reverted fails 5 runs of 5 at
  200 ms and 5 of 5 at 1 s; the floor patch's change reverted fails its library test and every stall run (3 of 3 at
  each length); the late-chunk patch's change reverted fails its two library tests, while the stall tests pass (ear
  pays the remainder, so no chunk reaches the library late); the starved rule (5) reverted alone fails its unit row
  (feed dry and told, a catch-up, a refill: nothing may be dropped); the bound (6) reverted alone fails its unit row.
  The library's two patches remain right for any sink, and the upstream report is drafted in the `sendspin` skill.
- **The late-chunk patch is proven through ear by a network gap** (the C4 follow-up). A stall of ear's never reaches
  it now that ear pays, so the stall tests could not show it. The real server never sends a chunk already late (its
  `_check_late_binary`), so a gap in what it sends never delivers one; the test server's `gap` holds the audio frames
  past that check until the library has starved for about 300 ms, then sends them at once, the release placed a third
  of the way into a 96 ms chunk (5 ms in from either end, the hard-sync threshold), so which chunk is late at its front
  is certain. The library floors to now, skips the wholly late chunks, decodes the late one and drops its front: one
  correction of the lost time, no silence. Measured: 5 of 5 pass; with the patch's change reverted it skips that chunk
  whole and fills the overshoot with silence, failing 5 of 5. What the test allows besides: the floor puts the next
  frame at now, but the block plays it from the first cycle to start after the library wrote it, so the stream comes
  back late by up to a block, or two when its decode and write outlast the cycle it floored in (95 us, 2.08 ms and
  5.47 ms seen), which the next chunk's sync (its settle threshold 500 us) drops once more: a settle step of under two
  blocks, back on the stamps within a chunk and two blocks of the recovery. Telling the library
  on each whole cycle while it is starved (the finish the end of that cycle) would remove that step, at the cost of a
  tell every cycle while no stream plays; not done.
- **"Report the whole jump" is caught deterministically by the code's unit tests** (eight checks fail in two of them)
  and by the stall test only when a burst cycle found the feed short (one run in three): with the feed holding every
  one-block catch-up, the two rules tell the same.

### 9.10 Found after the C4 follow-up (a GitHub runner's burst)

- **The burst's shape is the host's.** Both stall tests failed on a GitHub runner: after the stall the block's next
  cycle was whole, and no cycle of the run was disturbed, skipped or dry. `on_timeout` triggers the graph and only then
  re-arms its timer, already due, so whether a block runs a burst's cycle depends on whether its wake is dispatched
  before that timer's: the dev host misses every other one (9.9's 18 one-block catch-ups), the runner ran every one and
  lost nothing. Only the stalled cycle's own cut is certain (`docs/engine.md#the-one-data-loop-and-invokes`). 9.9's
  "a stall never reaches a cycle as one jump past the feed" holds on the dev host only.
- **A burst that loses nothing was told from cycles behind raw time.** Deferral keyed on loss, and in the runner's
  shape every cycle is whole, so ear told the library from each burst cycle, its finish on the driver's nominal time,
  behind now by up to the stall: 9.9's first finding again, by the other shape. In the runner's run the library kept the
  feed full (no dry cycle), so it never starved and its floor never fired (no hard sync, no lost sync and no ramp step
  after the stall, over the 750 judged cycles); a library slower to refill would have starved mid-burst and floored at
  now. The condition is "this cycle is dated behind raw time", not "this cycle lost frames". Built: the framework states
  `GraphCycle::overdueNs` (how long the next cycle had been due as the block began this one, from the clock it already
  samples for `startRawNs`; `cycleOverdueNs` in `cycle_facts.hpp`), and `SendspinCode` tells nothing from an overdue
  cycle, as from a lossy one, under the same `untoldBound`. It is a field, not a cause: an overdue cycle loses nothing,
  and every other consumer (a source's tap, a sink's stamp, the judges) keeps reading it as whole.
- **The debt's payment raced the writer.** The code skipped the debt and then read, two feed calls; the skip woke the
  writer, and frames it wrote between the two were read with the debt unpaid, played behind the clock. Fixed in the
  feed: `Feed::skipThenRead` pays the debt from what the feed holds and, if some is still owed, once more from what
  the writer added meanwhile (two passes: the data thread's work stays bounded by twice the capacity, where a loop
  until paid could spin for most of a quantum behind a fast writer on a 1 s stall's 46000 frames), and reads nothing
  while any is owed. Reading frames during the payment paid
  nothing with them, so the payment ran slow: a 1 s stall under load came back in 31 blocks against a bound of 28.
- **The stall tests made the burst they judged.** `StallProbe` stalls a block upstream of the node's and holds each
  burst cycle 1 ms while its next timeout is due, so the node's block misses the whole burst on any host; two more
  tests stall inside the block and judge whichever shape the host gives (with cycles lost, 9.9's rules; with none,
  nothing owed, no silence, at most one correction). A planned event's answer is judged until the host disturbs the
  graph again in a way the library could see (a cycle the ledger did not call clean, other than a catch-up the feed
  held whole; never a dry one), never short of a floor of a chunk and two
  cycles past the return, inside which a host disturbance fails the run naming it.

### 9.11 Found building C3c

What C3c built is described in `docs/testing.md` (`#the-cycle-judge`, `#a-writers-own-event`, `#the-librarys-own-word`,
`#the-senders-own-counter`); this section keeps the plan it was built from (written before the code, from the tests
as they stood after C4, a96f841750, before 9.10's follow-up), and then what building it found.

**Inventory: the windows still there, and what each still excuses after C4.** C4 made ear pay a catch-up's lost
time itself while the library is not starved (`LibraryView`, 9.9), so most of what the windows were for no longer
happens on a quiet host; each is still there and still wide:

| Window | Still there | What it excuses in practice now |
|---|---|---|
| `kSettleNs` (1.5 s after any unclean cycle, dry cycle, library silence, logged hard-sync step or loss) | yes, `clock_test.cpp` `Judge::one` | every catch-up, even one ear paid exactly and the library never saw: 1.5 s of timing unjudged after each. A paid catch-up needs nothing; only a library that realigns itself does (a dry feed with nothing owed, a catch-up while starved, a debt forgotten at `untoldBound`, a hard sync it logged) |
| `kSettleCycles` | defined, unused | nothing |
| `lossSteps` | yes, `checkStep` | a forward step of `lost_` ± 1 with a line near, `lost_` counting every short skip (which ear now pays, so the library loses nothing), every dry frame (owed ones too) and the raw clock's time across any non-catch-up jump (a pause or reposition, which the judge now fails anyway) |
| `dryCycles` from `lastGot() < frames` | yes | a dry cycle starts the 1.5 s, owed or not, and its frames go into `lost_` |
| `kLogReach`, `kLogLag` | yes, `takeDrops`, `takeLogged`, `loggedNear`, `Judge::read` | a library line stamped with the cycle count when standard error was read, matched within the feed and a chunk of cycles |
| `inLoss` with `kLogLag` | yes | timing unjudged from a `Lost sync` line less 32 cycles to its `Regained` plus 32 |
| `kLateHeardWithin` 600 ms, `kSettleFrames` 0.5 s | yes, `rtp_receiver_test.cpp` | a tenth with a late send up to 600 ms before it; a tenth with any unclean cycle in the half second before it |
| 600 ms and 0.2 s | yes, `test/system/rtp_test.cpp` | a late send up to 600 ms before a tenth's `heldBy` time (its buffer plus the WAV's coarse looks); the first 0.2 s of sound |
| `stepsInLoss` | `test/system/sendspin_system.hpp` only, not the clock test | a step past a `Lost sync` line's frames in a system test: D2's row (ear is another process, and the library's later hard syncs reach it only at verbose), so not this step's |

**Decided: `kLogReach`, `kLogLag` and `inLoss`'s lag stay** until slice 3c's log patch. C4 changed what makes the
library correct itself, not how its lines reach the test: they are still a bare `fprintf` read on another thread,
stamped with the tap's count when read, and the hard syncs and losses they report are the only evidence of what the
library did on its own (a time-filter step, its worker held by the host, a network gap). Deleting them would fail a
loaded run on the library's own corrections, or excuse them by a window again. They stay, each with a comment pointing
here, and nothing else in the test may use them.

**The interfaces added.**

- `Feed::dryReads()` and `Feed::dryFrames()`: the reads that found the feed short, and the frames they lacked, counted
  by the feed on the reading thread (relaxed atomics, realtime-safe), read by a test after the cycle is published. The
  feed's own word for a dry cycle; `SendspinCode::lastGot()` goes (the tap works what was read out of the count).
- `CycleJudge::librarySettle(LibrarySettle cause, std::uint32_t chunks)`: a settle of the library past the graph,
  declared by the test with the evidence that makes it (`dry` with nothing owed, `starvedCatchUp`, `forgottenDebt`,
  `logged`), in the library's own units (chunks of its audio), counted per cause in `JudgeReport`. The judge only
  counts; where the settle ends is the test's, by the server's chunks.
- `ear::test::ArmedStall` (`inducers.hpp`): an `AudioCode` that writes silence and makes the next stall of a
  `Stalls` when armed, the counting block `sink_test.cpp` had; the clock tests and the RTP receiver tests put it
  upstream of the judged node, so the judge's stalls cut a cycle it misses.
- `ear::test::lateSendReaches` and `receiverStartSettle` (`rtp_tone.hpp`): the one rule both RTP tests use for a late
  send.

**What each deleted window becomes.**

- `kSettleNs`: the judge's verdict per cycle (a catch-up excuses its own cycles, nothing after), and a `librarySettle`
  of two chunks only where the library realigns itself: a dry cycle the code does not owe (the feed's count moved; the
  code tells it from that cycle, so it sees itself starved and floors at now), a catch-up while the model says it is
  starved, a debt forgotten at `untoldBound`, or a hard sync or loss it logged. Two chunks: the floor acts at the next
  chunk it syncs, which plays late by up to two blocks (`docs/node-types.md#library-facts-and-carried-patches`), and
  the chunk after drops that once more; from the first frame of the second chunk past the one playing at the
  disturbance the stream is on the stamps. A settle ends at that frame, by the server's own chunks, not by time.
- `lossSteps`: a forward step inside an open settle, at most the settle's evidence (the feed's dry frames, the starved
  catch-up's `jumpFrames`, the forgotten debt) and two blocks for the floor, give or take one soft-sync frame, with a
  line of the library's near. Outside a settle a step must be a skip, a blend or a logged drop.
- `dryCycles` from `lastGot()`: the feed's `dryReads()`; a dry cycle opens a settle only when the code did not owe its
  zeros (the model's `catchingUp`), and the code's read is checked against the count.
- `kSettleCycles`: deleted.
- `kLateHeardWithin` and `kSettleFrames` (both RTP tests): a late send reaches from when its packet was due to one
  receiver buffer after it was sent (the configured `latencyMs` through `FrameClock`); in the engine test a tenth is
  judged when every recorder cycle of it and of one buffer before it is `judge` in the judge (the same reach), and an
  upstream `ArmedStall` makes the judge's four stalls while the tone plays. The first sound's settling is `expected`
  once per start (`receiverStartSettle`, 0.5 s, the same in both). The system test's `heldBy` lag is D2's and stays,
  named apart from the buffer.

**Also:** the clock test feeds every cycle of its block to a `CycleJudge`, so a cycle the judge fails (an absence, a
reposition, an unconfirmed catch-up) fails the test, which it did not before; the steady tests make the judge's four
1.5-cycle stalls during the judged span and must judge timing straight after each; the stall and gap tests make their
own disturbance and plan none. A step of the code's reports between two whole cycles prints both cycles' `startRawNs`,
facts and `rateDiff`.

**Found building it**, where it departs from the plan above:

- **A settle's steps are two, in order, not a budget.** The plan's budget (everything the evidence lost plus two blocks,
  however split) excused an ear fault: a real dry read that also lost 128 frames of the stream went through as a step
  of 128 inside a settle that lost 256. Built: one realignment of what the evidence says was lost and up to the floor's
  two blocks more (one soft-sync frame either way), then at most one settle step of up to two blocks; a step short of
  what was lost, or a second realignment, fails. Both need a line of the library's near.
- **A settle's soft sync is counted, not timed away.** Under load (`yes` x 4 beside the suite, load 8 to 10) the gap
  and 1 s stall tests went past the 1% of blended cycles: after a settle the library is within its 500 us settle
  threshold and interpolates one frame a chunk while it is off by over 100 us (`sync_task.cpp`), up to 20 chunks of
  one blended cycle each, which the 1.5 s window had hidden for 15. Built: those, at most 20 per settle in the 20
  chunks after it, are counted apart (`settleBlends`); their timing is judged.
- **The review (M2 and its should-fix):** each RTP test keeps its own start settle, 0.5 s in the engine test and 0.2 s
  in the system test (one shared 0.5 s had widened the system test's for no evidence); a dropout 0.1 s past each
  (240 frames of silence from the sender at 0.6 s and 0.3 s) fails 3 of 3, and one inside it passes 2 of 2.
  `heldByLag` is measured every look and checked: from a frame's due time to the look that first saw it, 147 to
  158 ms over 20 runs at load 2 to 3, at most 188 ms over 10 beside six busy threads; it is 300 ms, from 500.
- **The payment's cycles needed an allowance, until 9.10's fix.** Judging timing from the first whole cycle after a
  1 s stall's burst failed the 1 s stall test (828 of 850 within 1 ms, p99 544 ms): the code read, while it still
  owed, what the library wrote between its skip and its read, which played behind the clock. Built first as an
  interim allowance (those cycles untimed, capped per payment at ceil(remainder / (feed - block)) + 2), and deleted on
  the rebase onto 9.10's `Feed::skipThenRead`, which reads nothing while a debt is owed: a cycle of the payment now
  plays zeros the feed counts dry and the code owes, which opens no settle and has no ramp frame to time.
- **Rebased onto 9.10.** `Feed::skipThenRead` counts its short reads with `read` (one `countDry`), so a payment's zeros
  are the feed's word too. `StallProbe` and the in-block stall tests plan no judge stalls (their stall is what they
  judge) and run through the same judge; a burst the block ran every cycle of, overdue, states nothing a judge excuses,
  and the response window (`responseReach`) is kept as 9.10 built it, its host disturbances now the cycles the judge
  did not judge whole, less a catch-up the feed held whole. A library thread the host holds late is a `dry` settle:
  an unowed dry read of 256 frames injected every 400th cycle made the library lose sync by 5.3 ms and log a drop of
  254 to 256 frames at its next chunk, inside the settle, 3 runs of 3 passing. Measured on the rebase: the full suite
  1151 of 1151; the clock tests, all eleven a run, beside `yes` x 4 at load 2.6 to 8.7, 10 of 10.
- **A late send reaches back, so a tenth is judged only once the sender has sent what was due by its end**
  (`RtpSender::sentDue()`). A 150 ms hold made on purpose failed the engine tone test 2 runs of 3: the receiver's
  buffer ran dry from the late packet's due time, about 50 ms before the send, and the tenth there was judged before
  the late send existed. With the gate a 150 ms hold passes 8 of 8; the old rule (a send within 600 ms before a tenth,
  none after) would have failed it too, as no test held the sender that long before.
- **The receiver's buffer is the reach both ways, and that is enough.** With the judge's four 1.5-cycle stalls made
  upstream of the recorder during the tone, tenths whose cycles and one buffer before them were judged whole fit the
  tone in every run (the RTP repeat counts below); no settle was needed after a lost cycle beyond the buffer.
- **The induced stalls in the clock tests cost nothing after them.** On an idle host the dummy test judged 750 of 755
  cycles after the ramp's third second, four stalls made and excused, worst 19 us: the timing of the whole cycle after
  each paid catch-up is within 1 ms, which the 1.5 s window never let a test see.
- **Proven to fail a real fault**, each injected, run and reverted: a blend ear makes every 50th cycle (as a soft
  sync would, but with no settle before it) fails the dummy test 3 of 3 (15 blended cycles against 7.5); zeros the
  code made in a cycle the feed never
  counted short fail the dummy test 3 of 3 (unexplained silence and a step of 257: no settle without the feed's word);
  a dry read that also lost 128 frames fails 3 of 3 (a step short of what the settle lost), while the same dry read
  alone passes 3 of 3; a dropout in the receiver's output 60 cycles (320 ms) after a catch-up fails the L24 tone test 6
  of 6 (the deleted 0.5 s settle would have excused it), and one 10 cycles after passes 3 of 3 (inside the buffer); a
  dropout 450 ms after a 150 ms late send fails 5 of 5 on an idle host (the deleted 600 ms reach would have excused it;
  under load 12, 5 of 8, the passes inside the reach of the host's own catch-ups).
- **`stepsInLoss` was never the clock test's**: it is the system tests' (`sendspin_system.hpp`), D2's row, and stays.
- **Measured** (4 cores, other agents' builds beside it). The clock tests, all seven a run: before the soft-sync tail,
  20 runs at load 1.4 to 12.3, 19 passed (the 1 s stall test's own `corrections == 0`: the library lost sync 640
  cycles after the burst, `Lost sync -15898us`, under load 8.3; 5 of 5 alone), and 10 beside `yes` x 4 at load 8 to
  10, 6 passed (all four failures in the stall and gap tests: their own bounds, `blocksToBack`, `corrections`,
  `silenceAfter`, `back`, and the 1% of blended cycles the tail now counts apart); as built, 20 runs at load 1.2 to
  6.2, 19 passed (the 1 s stall test's own `blocksToBack`, 31 against 28), and 6 beside `yes` x 4 at load 2 to 7.2
  (the run was cut at its time limit), 5 passed (the card test's 1% of blended cycles at a steady -95 us offset with
  118 host catch-ups, 9.7's signature under load). The steady tests failed nothing else in 56 runs. The RTP receiver's
  five tone tests: 20 of 20 at load 2.5 to 9.7, 10 of 10 beside `yes` x 4 at load 3.6 to 7.9; the system RTP test 20
  of 20 at load 3.5 to 3.8.

### 9.12 Found building D1

What D1 built is described in `docs/engine.md#counters` and `docs/writing-a-node-type.md#lost-cycles`; this section
keeps only where it departs from 8.1, 8.2 and 8.4 as first written (8.4 is corrected in place).

- **The stall count is `CutRing`'s, not `DriverCuts`'.** Since C2 `DriverCuts` holds no bits or counts of its own,
  only a `CutRing` it forwards to (9.3), so the stall count went where the cuts are counted, and the harness's
  record keeps it by the same code. `CutRing::counts()` reads stalls, cuts and cycles in that order, each acquire,
  against the writer's order (cycles, then cuts, then stalls, each release), so no reading shows more stalls than
  cuts or more cuts than cycles; 8.4 as first written read them relaxed, with no reader needing two to agree, which
  a reader that subtracts would.
- **A stall is defined by the graph being behind, not by adjacent cuts** (the D1 review). As first built, a stall was a
  cut whose cycle before was not cut. After a stall the driver catches up in a burst whose cycles are cut or run whole
  as the host dispatches them (every one cut, every other, or none but the stalled cycle: 9.9, 9.10), so a run of
  consecutive cuts made one stall of the host one stall or many by the host's shape. Every burst cycle starts
  overdue, its next timeout already due, which is `GraphCycle::overdueNs`; so a cut starts a stall only when none
  is in progress, and the first cycle that starts on time ends it. The fact is the blocks': the driver's `start`
  event carries no time, and the server side's position, whose `next_nsec` would say it, is private to
  `impl-node.c` (S6, 9.1). So a port block whose cycle of the internal clock is not overdue tells the record
  (`Engine::cycleOnTime`), inside that cycle, on the data thread, the record's one writer thread. A cycle no block ran
  on time keeps a stall going: a burst's cuts before the chain's head ran stay one stall. With no block at all no
  cycle is cut. Measured on the dev host, a 200 ms stall's burst cut 37 to 39 of its about 40 cycles, every other one
  held cut on purpose or not, so the adjacency rule counted 1 there too; the record's table holds the shapes that
  tell the two rules apart.
- **`GraphCounterSource` waits for D2.** Its one consumer is `CounterWatch` (D2), and an interface lives with its
  consumer, so D1 gives `Engine::graphCounts()` as a plain member, which D2's interface then names.
- **Every cycle the block's `onProcess` sees is counted**, not only those `processCycle` runs: a cycle too large to
  run is written as zeros outside `processCycle`, and its facts (what it missed) would otherwise go uncounted. The
  overrun is timed from the monotonic reading `onProcess` already takes to one at its very end, metering and the
  window's publish included, so a late wake (`GraphCycle::overdueNs`, the host's) is never the node's.
- **`cycleOverran` is one rule** (`cycle_counters.hpp`) the block and the harness both call, as `CycleAccount` is for
  the facts; the harness times its work on `steady_clock` (CLOCK_MONOTONIC, as the block), so a code that stalls
  past its cycle in a type's test moves `overruns` as it would in the engine.
- **The harness keeps the graph's counts** (`NodeHarness::graphCounts()`): 8.4 said it keeps none, but its record is
  the engine's class, so `loseCycles(n, Lost::cut)` moving the graph's counters and no node's is checked in the
  harness as in the engine. `NodeHarness::counts()` goes through `NodeHost::counts`, so the harness answers by
  NodeHost's rule, not a copy.
- **A node's counts span two blocks only across a port-caused replace.** 8.2's "if the node's block is made again
  within its life" does not happen: NodeHost makes a node's block once and destroys it only at its removal. A
  port-caused replace carries one health stream and the older node's dropped frames, so it carries its counts too:
  the newer node reads the older's live until it is gone, then adds its total. A replace by another type is a
  removal and an add, and starts from zero (8.2).
- **`missedCycles` is `missed - missedCut`, so the one undecided case counts one more.** Past the ring's reach with
  some but not all of the cycles since the last cut, `missedCut` is a lower bound by one (9.2); the counter then may
  count one cycle more than the block missed uncut. Only a pause of over 2^16 cycles with cuts in it makes it.
- **The pause is made with `pw_impl_node_set_active(false)`**, as C2's runner makes it (9.3): the step table's
  "a `PAUSED` block" is a block made inactive. A suspended node is never brought back. Another block runs beside it,
  so the driver has a node to drive and the test's waiter is woken while the paused block runs nothing.
- **Proven to fail a broken count**, each injected, run and reverted: no stall counted fails the record's table, the
  harness's cut test and the engine's induced cut (0 stalls against at least 4); every cut a stall fails the table,
  the harness and the engine's long stall (about 39 stalls against 1); stalls by adjacent cuts (the rule first built)
  fails the table's burst shapes, but not the engine's long stall on the dev host, whose burst cuts nearly every cycle
  (adjacency counts 1 there as well); no block telling the record a cycle on time fails the engine's induced cut (one
  stall against at least 4: the first never ends); cycles read as cuts fails all three; cut cycles read as stalls fails the table and the
  harness; no missed cycle counted fails the table, the harness's pause and the engine's pause (0 against 33); the
  graph's cut cycles counted as a node's fails the table, the harness's cut test and the engine's tail (4 against 0);
  no overrun counted fails the table, the harness's stalling code and the engine's (the wait for three never ends);
  the block's work timed as nothing fails the engine's; no reposition or driver change counted fails the table and
  the harness (the engine makes neither: S6); a destroyed block's counts dropped, or a port-caused replace's not
  carried, fails NodeHost's test.

### 9.13 Found building D2

What D2 built is described in `docs/engine.md#reporting-the-counters`, `docs/control-port.md#counters`,
`docs/output-records.md#diagnostic` and `docs/testing.md#the-counters-own-word`; this section keeps only where it departs
from 8.3 and 8.4 as first written (both are corrected in place).

- **Read at the heartbeat, not on a timer of its own.** 8.4 armed a 1 s `Timers` timer. The main loop already wakes
  every 100 ms for the watchdog's heartbeat (`main.cpp`'s `Heartbeat`), which now also calls `CounterWatch::tick()`; a
  tick is a few relaxed and acquire loads and a NodeHost lookup per node. An event from the data thread was the other
  way, and was refused: a cut or a miss would then cost the data thread a system call (an eventfd write) inside a
  stall's catch-up burst, the moment it is furthest behind. It is not a sleep in a test, so `test/sleeps.txt` has no
  role for it; the one sleep D2 adds is `counters_test.cpp`'s induced stall.
- **A loss is the change, never `cycles`.** 8.3's "only when something changed" would have sent `counters.changed`
  every second, as `cycles` moves every cycle. A change is `cutCycles`, `stalls` or any node counter; `cycles` and
  `lostMs` ride along.
- **The records wait for the cuts to hold still.** 8.3's "one a second, for a second in which something was cut" would
  split a stall whose catch-up burst crosses a second's edge into two records, and a host that cuts every few cycles
  into one a second. A record is written once its counter held still over a beat (100 ms), at most once per its pace
  (1 s graph, 10 s a node), and at least once every 10 s while it never holds still. Measured on the dev host: a 500 ms
  SIGSTOP of the whole process is one record of 93 or 94 cycles in 1 stall, every run (`counters_test.cpp` asks for one
  record with three quarters of the stall's 93.75 cycles).
- **All the pacing is CounterWatch's.** 8.4 had StatusLog pace the node record itself. The hold needs the beats, which
  only CounterWatch has, so it paces all three and `CounterListener` has a call for each (defaults do nothing; the
  gateway overrides the wire's, StatusLog the two records). `ms` is the difference of the totals' times, so the
  records' `ms` add up to `lostMs`.
- **Spans, told by NodeHost.** 8.4's "one entry per node, made at add and dropped at remove" needs to tell a newer node
  of a name from the older (a remove and an add in one commit leaves the name's counts stepping back to zero, or, by
  chance, equal) and needs the final counts, which `NodeHost::counts` stops answering once a node is gone. NodeHost
  alone knows both, so it tells `countsBegan` (a node that carries no older node's counts) and `countsEnded` (with the
  final counts, before `nodeGone`), the latter not for a superseded node nor a port-caused replace's older node.
  `NodeCounterSource` is the read CounterWatch needs of NodeHost, so its tests run on a fake.
- **A gone node.** On the wire a client drops a node at `node.removed`; a `counters.changed` naming it while it closes
  is ignored, as any event naming a node the client does not hold. On standard output its end writes what it moved
  since its last record at once, pace and hold waived, so a node removed within 10 s of its last record still says it.
  A superseded node's last beat or less of movement is lost (its name's span is the newer node's from the add): a
  replace by another type of a node that was missing cycles in the 100 ms before it.
- **`node.added` carries counters.** 8.3 gave counters to the snapshot alone. A port-caused replace is `node.removed`
  and `node.added` on the wire but one node to NodeHost, whose counts carry on; a client holding zero until the next
  sample would see the carried counts as a sudden loss (D3's mark). The gateway hears the replace before NodeHost does
  (observers first), so it reads the older node's counts then (`MainLoopGateway::readCounts`); any other add carries
  zero.
- **A flush at the stop.** So the records' sums are the totals, the graph's record held back is written after the main
  loop ends, before `exit` (each node's went with it). The system tests judge from that.
- **The system tests' excuse moved, their shapes stayed.** `isSkipStep` became `skippedBlocks`, which says which steps
  are skips (whole blocks, exactly) and how many blocks; the ramp's and the source stamps' skipped blocks may not pass
  ear's own count (`LostCycles::budget`: cut cycles and every node's uncut misses). The 200 µs `stampSlackUs` stays: it
  is the stamps' rounding to the microsecond and the clock's loop, read on clean steps as on skips, not an excuse.
  Deleting the shapes would let a drop of a few frames pass whenever the budget had room.
- **The RTP test's `heldBy` stays** (5.5 gave it to D2): it times the test's own sender, whose late sends no counter of
  ear's counts.
- **`snapshot.json`'s counters and the reducer.** Every fixture's snapshots and `node.added` carry `counters` (zero
  where nothing was lost); the editor's reducer keeps them whole, an unknown counter among them, and folds
  `counters.changed`, so D3 has totals per node and graph from the first snapshot on.
- **Proven to fail a broken report**, each injected, run and reverted: `cycles` counted as a loss, no wire pace, no
  hold, no hold bound, no record pace, a node's end or the flush writing nothing, a newer node keeping the older's span
  (each fails `counter_watch_test.cpp`); a port-caused replace beginning a span, a superseded node's end told (NodeHost's
  counters tests); the snapshot reading no node's counters (`graph_status_test.cpp`); the graph's counters not encoded
  (`snapshot.json`); `counters.changed` not posted, or a port-caused replace's `node.added` carrying zero
  (`counters-changed.json`); a record's field missing or wrong (`output-records.json`); the heartbeat not ticking the
  pacer, or no flush at the stop (`counters_test.cpp`: no record of the stall; the second stall's cuts unwritten); a
  ramp's skipped blocks not counted (`sendspin_ramp_test.cpp`).

## 10. Decided here (technical)

- One carrier (`GraphCycle::facts`), not a second view.
- The listener counts cycles itself; blocks name their cycle by its count, not `clock.cycle` (6.4). Built in C2.
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
