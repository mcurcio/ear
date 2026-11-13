# Testing

How ear is tested: the rules every test follows, and what each test program proves. The commands
that build and run them are in [`AGENTS.md`](../AGENTS.md#build-run-and-test).

## The rules

- **Tests are event-gated.** A test waits for the event it needs (a record ear prints, a
  notification, a link going active, a thread's answer); a deadline only fails a test, never
  decides when something happens. A sleep that stands in for an event is a bug, and so is a
  `WARN` that tells and goes on where the test should fail naming the event it waited for:
  every sleep and `WARN` is listed with its role, and a guard fails on one that is not
  ([the sleep guard](#the-sleep-guard)).
- **Audio is verified here, never on real hardware.** The engine runs in-process with no daemon,
  so the tests read samples, stamps and WAV files exactly. Hardware checks (the MOTU 24Ao, a real
  Music Assistant) are listed as owed in the skills, never run in CI.
- **A failing test fails; nothing is skipped.** The all-type tests fail naming a type that lacks
  what they need, a CTest test that matches no test case fails, and a guard that would check no
  file fails. A case that needs a folder it cannot write runs as an ordinary user under root too
  ([an ordinary user's folder](#an-ordinary-users-folder)).
- **Timing tests need a data thread that keeps time.** The engine, ramp, RTP and `sendspin` clock
  tests fail without a bug when a heavily loaded host stalls PipeWire's data thread for hundreds of
  milliseconds (graph xruns): rerun such a failure alone before suspecting the code
  ([engine.md](engine.md#xruns-on-a-loaded-host-are-not-bugs)).
- **A test makes the rare event it needs; it never waits for the host to.** What a host does by
  chance (a graph xrun, a cut-short cycle) comes at very different rates on different machines, so
  a test that needs it brings it about itself and requires the count it planned; what the host adds
  is counted with it, never required ([the inducers](#making-the-rare-event)). A load of its own,
  as S5's busy thread per core, only adds evidence.
- **Judge what the test controls, and only that.** Exactness is judged over the cycles nothing
  broke, to a planned number of them, and a lost cycle is excused by one judge only, exactly as
  the driver cut it ([the cycle judge](#the-cycle-judge)); a value that
  propagates is read once the engine has settled ([a settled engine](#a-settled-engine)); a
  statistical bound comes from the count measured, never a margin sized to the host
  ([statistics from the count](#statistics-from-the-count)). A loaded host then only makes a test
  take longer.

## The tools

### Making the rare event

`test/support/inducers.hpp` (the target `ear_inducers`; namespace `ear::test`):

- **`StallPlan`** says which cycles stall the data thread and for how long, by an index the caller
  counts: the cycles listed (`StallPlan::at`), or every n-th from a phase up to a limit
  (`StallPlan::every`). A `StallLength` is a fixed time or a number of the stalled cycle's own
  durations: `StallLength::cycles(1.5)` lets the driver's next timeout expire before the node
  returns, so that cycle is cut short straight after it, on any host
  ([engine.md](engine.md#the-one-data-loop-and-invokes)).
- **`Stalls`** carries a plan out from inside a `process()` or a probe's cycle and counts what it
  made; it holds the one sleep that stalls a data thread on purpose. A judge's planned stalls, S5's
  overruns and the clock's stall tests all stall through it.
- **`CycleLedger`** records each cycle a code ran (position, frames, raw start, rate and the facts
  its block stated, `GraphCycle::facts`) into storage made beforehand. It works nothing out: whether
  anything was lost before a cycle is its block's own word (`whole()`), and what is excused is the
  judge's.
- **`StallingCode`** is an `AudioCode` that writes silence, records into its ledger and stalls at
  its plan's cycles (by ledger index).
- **Judged to a planned count.** A check that holds only where nothing was lost (a take-up's
  first cycle at the published position) is judged where the cycles themselves say so: a take-up
  whose only cause is its drive, or a probe downstream that heard consecutive positions there. The
  test repeats until it has judged the number it planned; a take-up the host disturbed only makes
  another round, and what holds across a lost cycle too (a start never later than its first cycle,
  a sink's stretches exactly the code's) is checked every time. A model of a rule the design
  defines over jumps applies the rule to the jump the block stated (`CycleFacts::jumpFrames`): a
  source's test clock asks to skip a catch-up's whole jump, however long, and the model holds the
  ring to have dropped exactly min(what the clock asked, what it held) and nothing else, as the play
  rule says, instead of counting a drift. Whether a cycle may be lost at all is never the model's:
  it is the judge's, below.

### The cycle judge

`test/support/cycle_judge.hpp` (the target `ear_cycle_judge`; namespace `ear::test`) is the one
place a test excuses a cycle its judged node missed. A chain is judged where its last node says
so: each code in a chain counts only the cycles it runs, and a cycle cut short is run by an upstream
prefix only ([engine.md](engine.md#the-one-data-loop-and-invokes)), so a ramp at its head moves on
by a block its tail never hears. The judge is fed the facts of that last node
([engine.md](engine.md#graphcycle)), one cycle at a time in the order they ran (from its ledger, on
the test's thread), and gives each a verdict:

- **judge**: nothing was lost before it (`whole()`). The test judges the boundary into it.
- **excused**: a `catchUp`: the driver cut every cycle the node missed (`confirmed()`), the position
  moved by exactly those cycles' frames, and no further than the raw clock went:
  `FrameClock::ns(jumpFrames)` at most `lostRawNs` plus the driver's 1 ms error clamp, and nothing
  more, since a cycle's start is the driver's deadline mapped to the raw clock, so a late wake
  cancels out ([engine.md](engine.md#graphcycle)). Exactly the missed cycles are excused, counted,
  and the most any catch-up's jump outran the raw time lost is reported (`mostOverLostNs`).
- **expected**: a cause the test declared in its plan, up to the count it declared (`start`, a
  `drive` per drive it made, a `driverChange` per card it made a driver, an `absent` per pause, an
  `oversized` per cycle too large for the block); a cut it follows is excused as well.
- **fail**: anything else: a `reposition` always, and a cause the judge has no rule for (deny by
  default), with the cycle, its facts and the rule it broke.

There is no window: a disturbance excuses its own cycles and not one more. A test hands the judge
its plan, `JudgePlan{.judged, .induce, .expected}`: the boundaries it judges (0 is refused: nothing
passes on nothing), and the stalls it makes itself, by default four of one and a half cycles
(`StallPlan::every(50, 7, 4, StallLength::cycles(1.5))`). `judge.stalls()` is the `Stalls` the
test's upstream code calls in its `process()`; each planned stall cuts its cycle straight after
it, on any host, so the judged node misses it and the judge must excuse it. Each stall keeps the
graph cycle it was made in (the record's count, read in that cycle), and `done()` holds only when
the planned boundaries were judged and every planned stall was made in a cycle an excused verdict
covers (the cycles a catch-up missed, or a card's missing block), so a test whose excuse path is
broken (the judge, the facts or the record) fails every run, not on the first loaded host, and a
stall made in the judged node itself (which cuts nothing it misses) never counts: stalls are made
upstream of it. A test waits with its `Waiter` until
`judge.done()` (or a failure, or its ledger full), then `INFO(judge.report())` and requires
`judge.report().ok()`. A test that must not stall says why and plans `StallPlan{}`.

**A card** runs no code of ear's, so its facts are not seen; the judge answers for it from the
engine's record (`Engine::cuts()`). The test reads the card's file as it grows: a block boundary
heard whole on both sides is judged (`blockHeard()`), and each block of the ramp missing from the
file is excused only if the driver cut the cycle the ramp wrote it in (`blockMissing(graphCycle)`,
the cycle from the ramp's own ledger). The card plays a whole block of silence for a cycle the
ramp's block did not run (the driver cut it before the block) and the ramp goes on where it stopped:
such silence is allowed only where the ramp's next cycle states a catch-up over at least that many
cut cycles, and the ramp's block is judged from its own facts as well.

Every test that excuses a lost cycle does it through the judge:

- The ramp through a block's inside ports (`port_block_test.cpp`) and from one block to another
  (`engine_test.cpp`): within every cycle the recorder heard, the ramp is whole; a boundary is
  judged where the judge says so, to 32 of them; across an excused one the ramp moved on by whole
  blocks it wrote unheard, never more than the jump (none where the driver cut the cycle before the
  ramp too), so a repeated, stale or over-skipped block fails; the ramp makes the judge's four
  stalls once it is heard.
- The ramp onto a card's channel 2 (`sound_card_test.cpp`): as above, read from the card's file,
  each stalled block among those excused. The card's join and its loss judge every cycle each block's
  code ran (its drive expected, nothing else), counting the planned boundaries only from the event
  (the ledger's index at ready, at the loss), and make no stall: they judge the clock the card
  leaves the blocks on, and the ramp proves the excuse path in the same file through the same code.
- A sink's take-ups across stops, starts and a storm of drives (`sink_test.cpp`): every cycle of the
  code through the judge; the block counting the graph's cycles feeds the block under test and makes
  the judge's four stalls while the code runs. The `drive`s expected must be exactly the drives taken
  at distinct positions (drives the data loop takes before one cycle state one).
- A bound source and S5's part A on the real block (`source_test.cpp`): every cycle of the source's
  block through the judge, its drive declared; an upstream probe makes the stalls (the default four,
  and S5's 30 of 12 ms), and the play rule's model reads each stated jump, checking the position
  is the last cycle's end moved by it, and what the source's test clock asked and was told.
- S5 (`spikes_test.cpp`): B is a probe, not a block, so it states its own facts by the rule a block
  runs (`CycleAccount` over the engine's record, from the tick a block builds, by the same helper,
  `src/pipewire/driver_tick.hpp`); U's 200 overruns are the judge's stalls, and every cycle B ran is
  judged.
- A `sendspin` node's clock (`clock_test.cpp`): every cycle of the node's block through the judge,
  its drive declared. A block linked into the node's `to` (an `ArmedStall`, `inducers.hpp`, the
  counting block the sink test uses too) makes the judge's four stalls once the timing is judged,
  one every 150 cycles, and the timing is judged on the next whole cycle after each: the feed holds
  the cycle lost and ear skips it exactly ([node-types.md](node-types.md#clock)), so nothing after
  it is excused. The stall and
  network-gap tests make their own disturbance and plan none; the card's loss is the event its test
  judges.
- An RTP receiver's tone (`rtp_receiver_test.cpp`): every cycle of the recorder through the judge,
  its drive declared; a block linked into the recorder's block makes the judge's four stalls while the
  tone plays, one every three tenths, and a tenth is judged only when every cycle of it and of one
  receiver buffer before it was judged whole (below, "The sender's own counter").

**Past the graph.** Where a test's output goes through something with dynamics of its own, the
judge covers the graph side exactly and the rest is declared apart. A `sendspin` node's library
realigns itself only where it lost time ear did not skip ([node-types.md](node-types.md#clock)), and
the clock tests declare a **library settle** there, with the evidence that makes it
(`CycleJudge::librarySettle(LibrarySettle, chunks)`, counted per cause in the report): a dry cycle
(the feed's own count, `Feed::dryReads()`, moved), a catch-up's jump the feed did not hold whole
(`unskipped`: the frames the code could not skip), or a hard sync or loss of sync the library
logged. A settle lasts two of the library's chunks, by the server's own chunks, not by time: the
library corrects at the next chunk it syncs (a floor puts that chunk at now, which the block plays
late by up to two blocks, and the chunk after drops that once more), so from the second chunk
boundary past the first frame played after the loss the stream is on the stamps. Inside it the timing is not judged, and two forward steps of the
library's own are excused, in order, each with a line of the library's near and one soft-sync frame
either way: one realignment of what the evidence says was lost and up to the floor's two blocks
more, then one settle step of up to two blocks. A step short of what was lost, or a second
realignment, fails; anywhere else a step must be the code's skip, a blend or a logged drop. After a
settle the library's soft sync interpolates one frame a chunk while it is off by more than 100 us,
and its hard sync left it within 500 us (`sync_task.cpp`'s thresholds), so up to 20 blended cycles
in the 20 chunks after a settle are its own and counted apart; any other blended cycle counts
against the 1% the clock tests allow. A catch-up the feed held whole declares nothing: ear skips
it exactly, and the next whole cycle is judged.

### A writer's own event

A test that needs a thread of ear's to be waiting waits for that thread's own signal, never a sleep
that guesses it has started: `EventSources::waitsBegun(source)` counts the waits a source's writer
began, each past its last look at the ring, so only a play, the close or its patience ends it, and
`waitsWoken(source)` how many of them a play or the close ended rather than the patience
([engine.md](engine.md#sources)). `source_test.cpp` waits for it before it plays or closes, and
checks `waitDeadline(milliseconds::max())` is a year away. The `sendspin` node's feed has the same
two, `Feed::waitsBegun()` and `Feed::waitsWoken()`, counted on the writer's side: `feed_test.cpp`
waits for the first before it reads or clears, and judges by the second that the wake ended the
wait, not its patience. The feed counts its own short reads on the reading side,
`Feed::dryReads()` and the frames they lacked, `Feed::dryFrames()`: a dry cycle is the feed's word,
not a guess from the output. Its audio code says what the feed held as its last cycle began and
what it skipped of that (`SendspinCode::lastHeldAtStart()` and `lastSkipped()`, read on the data
thread after `process()`); the clock tests work out from each cycle's facts, what the feed held and
its counts, by the code's rule, what it skips for the graph's lost time and when it tells and with
what finish ([node-types.md](node-types.md#clock)), and hold each skip, each ramp step and each
report of what played against that, and each report's finish against the last one's. A stall test makes its catch-up certain rather than likely: the stall is made by a
probe block linked upstream of the node's (`StallProbe`), whose stalled cycle is cut short before
the node's block runs, and which holds each cycle of the driver's catch-up burst 1 ms while its next
timeout is already due, so each of those is cut short before the block too; the block then states
one catch-up of the stall and the burst, where the burst's own cycles would be run or lost as the
host's timing fell ([engine.md](engine.md#the-internal-clock)); a 200 ms or 1 s stall is then past
the feed, so the library's one correction is certain, and the same probe's 1.5-cycle hiccup is held
by the feed, so the code's exact skip is. Two more stall tests stall inside the node's own block and
judge the burst the host gives: the block missing some of its cycles (each a catch-up, frames lost),
or running every one while the next is due (`GraphCycle::overdueNs`, nothing lost). Each passes
only its own shape's rule, and each appends the shape it judged, pass or fail, to
`out/Testing/sendspin-stall-shapes.txt` (`sendspin stall judged: ...`), which CI's `test` job prints
after the suite: a passing test's output reaches no log, a node's tests may not write standard
output (`ear_nodes_never_write_standard_output`), and no `WARN` is added
([the sleep guard](#the-sleep-guard)). What the
library does after a planned event (a stall, the network gap) is judged as its answer until the
first cycle after the stream's return that the host disturbed in a way the library could see (one
the ledger did not call clean, other than a catch-up the feed held whole, which ear skips from audio
the library already wrote and tells exactly; a dry cycle is the product's), and never short of a floor of a chunk and two cycles past the return:
a host disturbance inside it fails the run naming the cycle, to be rerun alone, and a window it
shortened says the host event that ended it (`responseReach` in `clock_test.cpp`). The network gap
test makes its late chunk certain the same way: the test server's `gap` holds the audio it sends
until the library has starved, and places the release a fixed way into a chunk by the chunks'
stamps (`test/support/sendspin_server.py`), so the chunk the library is then playing is late at its
front and not at its end, whatever the host's timing.

### The library's own word

What the `sendspin` library does it says only on standard error. The clock tests route standard
error through a pipe for the rig's life (`StandardErrorLines`, `test/support/standard_error_lines.hpp`:
outside the node's folder, which never names standard error), pass on all but the lines they asked
for, and, once the worker has set the library's level, raise it to verbose, so each hard sync says
its frames and each loss of sync its start and end, each stamped with the cycles recorded when it
was read. A step or a stretch of silence the cycles do not explain is judged against those lines:
excused exactly as far as they say, never further, and counted. A line is stamped when it was read,
not where its audio plays, so it is matched within a reach (`kLogReach`: the feed and a chunk of
cycles from where it was read, give or take `kLogLag`, 32 cycles of reading lag) and a loss of sync
covers the cycles from its line less that lag to its end plus it: interim, until the library's log
reaches ear through a callback stamped with the feed's write position (slice 3c's carried patch),
and used for the library's own lines and nothing else. The system tests read what ear
prints (its warnings, as log records): each `Lost sync (<n>us off)` accounts for one forward step of
whole chunks (skipped as too late) and a drop of at most `|n|` us in whole frames, as the library
works it out; a line above zero (silence added) is counted apart, and a forward step past what any
line says takes a line left and is counted apart too (`stepsInLoss`): interim bounds, as the
library's later hard syncs in a loss reach ear only at verbose.

### Waiting for what ear wrote

A system test that needs a recording waits for the WAV to hold it, never for a window of wall
time: `waitForWav(path, enough, patience)` (`test/system/ear_system.hpp`) follows what the file node
appends (`WavFollower`: the data chunk found once by its RIFF chunks, as the header's sizes are
filled in only at the end, then only the new whole frames each look) every 50 ms, telling `enough`
the new frames, until it holds; the finished file is what the test judges. `waitForNoiseSecond` waits for a second of a route's
noise; the `sendspin` tests play a long stream (`playLong`) until `waitForRamp` sees the exact
ramp frames they need in it, then end it (`endStream`), so a server that joined its ramp late on
a loaded host only plays longer. The source's audio is judged chunk by chunk as the test server
says each one arrived (`source-chunk`), until a second of frames came on clean stamp steps.

### A stop by ear's own word

A system test that stops ear judges the stop by ear's last record, `ear: stopped`
([output-records.md](output-records.md#exit)), waited for under the shared deadline, and waits for
the process's end only for its exit status, under `exitDeadline` (60 s), which catches a hang and
judges nothing: what is left after the record is the kernel's teardown, which a starved host
stretches to seconds ([architecture.md](architecture.md#stop-sigint-or-sigterm)). `waitForStop(ear)`
(`test/system/ear_system.hpp`) is both, and `Ear::stop` uses it. A bound on how long a stop takes
(`stopBound`, the `sendspin` tests' 5 s) is judged on ear's own records, by their order and `t`
(UTC wall time, the only time a record carries): `stopTook`, from the first `stopping: removing` to
`ear: stopped` (every teardown, the control port, the control thread and the keeper's join), and
`removalTook`, from a node's `stopping: removing` to its `removed`; a negative span fails.

### The counters' own word

A system test judges the cycles ear lost by ear's own count of them, its counter records
([output-records.md](output-records.md#diagnostic)), never by the shape of what the WAV or the
stamps show, and deny by default: each skip must be paid by a record that covers it in time. Each
cycle the graph cut and each one a node missed uncut skips at most one block of what reaches a
file. `unpaidSkips(ear, skips)` (`test/system/ear_system.hpp`) takes each skip with the window of
wall time its cycles were lost in, and pays it from the records, read after ear's stop when every
record held back has been written ([engine.md](engine.md#reporting-the-counters)). A graph record
covers every cut since the graph record before it, and a node's record every cycle it missed since
its record before, so a skip is payable only by the first record of each kind written after its
loss: with the loss somewhere in the window, the records written in it and the first after it.
Each pays from its own cycles (a graph record's cut cycles, a node record's missed ones), at most
its own. A first record after the window later than its kind's lag (the longest a record can wait
for its losses to hold still: 10.3 s for the graph's, 20.3 s for a node's) cannot be the record of
that loss and pays nothing, named. A skip no record pays fails, named by where it was seen. A
ramp's skip (`RampCheck::skips`, each step's blocks from `skippedBlocks(step)`) was lost from when
the WAV frame it lands on was due (from the file node's `running` record, its frames and the
blocks the skips before it took out) for as long as its blocks last, from 100 ms before to 100 ms
after (`unpaidRampSkips`). The skips it counts are the `sendspin` node's own clock's
([node-types.md](node-types.md#clock)): it skips at most the graph's jump, and a cycle cut after its
block shows as the block it lost, so the skips before a frame never add up to more than the cycles
the file node lost, and no loss moves a skip's due time later than its frames were due (a stall
past the node's feed only moves it earlier). The framework skips nothing for a node
([architecture.md](architecture.md#the-timing-model)), so the reckoning holds behind a node whose
own clock skips the graph's lost time, not behind one with none. A source stamp's
(`SourceStamps::seen()`) was lost while its chunk or the one before was captured: from the stamp of
the chunk before (earlier by the skip's own excess, in case the loss fell inside that chunk),
read onto the wall clock by the quickest arrival measured (each chunk's lag and when the test read
it), less 100 ms, to its own arrival. The shape still says which step is a skip (whole blocks, exactly; within
`stampSlackUs` for a stamp, which is rounded to the microsecond), but no longer excuses it, and a
host cut elsewhere in the run pays for nothing. `lostCycles(ear)` sums the records, the totals.

`counters_test.cpp` makes the stall itself: the whole process stopped (SIGSTOP) for 50 ms. Which
cycles a stall cuts is the host's: the burst after it is all cut on the dev host and runs whole on a
GitHub runner ([engine.md](engine.md#the-internal-clock)), where a stop while the data thread waits
cuts nothing. The cut that is certain is of a cycle held in progress, so the stop is aimed at a
moment `/proc` shows the data thread running, and the test stalls again until one stall cut a
cycle, judging each by the graph's counters once its cycles caught up (one that cut nothing lost
nothing). It then proves the wiring end to end, deny by default: a graph record covers the stall's
cuts within the records' pacing (its cycles at least the stall's, at least one stall), every
record's total is the sum of the records up to it, `counters.changed` and `graph.get` agree with
them, and after the stop the records add up to at least what the control port counted (the flush);
no record, or totals that disagree, fails, named. How many stalls a host's stall is counted as is
proven in `counter_watch_test.cpp` and the engine's in-process stall tests, where the stall is made
inside a cycle; a whole process signalled on a loaded host cannot pin it, so this test does not
claim it.

### The sender's own counter

A receiver's output can be judged only where its input came in time. The RTP test sender
(`test/support/rtp_tone.hpp`) counts each packet it sent later than due by more than
`RtpSender::lateSendBound` (`lateSends()`: when, and how late), as module-rtp-source counts
nothing a test can read. Both RTP tone tests take a late send's reach from one rule,
`lateSendReaches` (`rtp_tone.hpp`), and the receiver's configured latency (an rtp node's
`latencyMs`) through `FrameClock` (`receiverReach`): a send late by no more than the buffer reaches
nothing; one later left the buffer dry from when its packet was due until it came, and the buffer
refills over one buffer after, so it reaches from when it was due to be sent to one buffer after it
was. A send can reach back, so a tenth is judged only once the sender has sent what was due by its
end (`sentDue()`), and only when no late send reaches it and, in `ear_pipewire_tests`, every cycle
of it and of one buffer before it was judged whole by the cycle judge (a lost cycle starves or
floods the buffer, which plays it out within that buffer). The receiver's start is the one span left
unjudged for its own sake, once per start, while its rate control steers to the packets' arrival:
each test keeps the span it always had, 0.5 s after the first sound in `ear_pipewire_tests` and
0.2 s in the system test, and a dropout 0.1 s past either fails it. The system test knows a tenth's
time only as when the WAV was first seen to hold it (`heldBy`), so it measures every look against
when the frames it saw first were due to be sent (`RtpSender::dueAt`) and fails if a look came
later than `heldByLag` (300 ms; measured at most 188 ms: the receiver's 100 ms buffer, the file
node's write and the 50 ms between looks), an interim bound until ear reports its own lost cycles. They judge a planned ten tenths, the sender
sending on until then. `holdOnce` holds the sender on purpose, and a hold inside the headroom must
leave the tone clean: the tenths it reaches are judged, the stream played again until the host
leaves them judgeable.

### An ordinary user's folder

`ear::test::WithoutFilePrivileges` (`test/support/without_file_privileges.hpp`, on `ear_temp_folder`'s
path) clears `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH` and `CAP_FOWNER` from the calling thread's
effective set for its lifetime, with the raw `capset` (the calling thread alone; a thread begun
meanwhile starts without them too), and restores them from the permitted set. A case that needs a
folder it cannot write holds one first, so under root it runs as an ordinary user would instead
of skipping; it fails naming why when the capabilities cannot be changed.

### A fault on purpose

The callback boundary's own seams (`src/pipewire/callback_boundary.hpp`, namespace
`ear::pipewire::boundary`; never armed outside tests) make ear's own bugs on purpose, inside the
real code, so each owner of a fault is proven ([architecture.md](architecture.md#faults-by-owner)):

- **`throwInCallback(name)`** makes the next guarded callback of that member
  (`FilterPortBlock::onStateChanged`, `RtpReceiverBody::onCoreDone`, `LinkProxy::onInfo`) throw
  inside the boundary instead of running, once, as that callback's own bug would.
- **`throwAfterRegistration(n)`** makes the `n`th registration from now throw once its owner holds
  it, and **`registrationsHeld()`** counts what is held, so a test makes an adapter, counts its
  registrations, then throws after each in turn and checks the count is back where it was: a
  constructor that throws leaves nothing registered.

### A settled engine

`ear::pipewire::testing::Quiescence` (`src/pipewire/tests/quiescence.hpp`) counts every port info
with changed params the engine's connection hears through the registry (a block's retell, the
latency PipeWire sets on its peers, a port that came or went). `quiesce({cycle counts})` makes
`pw_core_sync` round trips until two in a row complete with no change, then waits until each named
code has run two more cycles: one whole cycle begun after the engine settled. A test that reads what
a plan or a link propagated (a latency told hop by hop) quiesces first, instead of waiting for one
probe's sighting, which can come before the retell reached the next block.

### Statistics from the count

`test/support/statistics.hpp` (the target `ear_statistics`): `noiseRmsWithin(rms, frames, sigmas)`
holds when an RMS of the white node's noise (uniform on [-1, 1): sqrt(1/3)) measured over `frames`
samples lies within `sigmas` standard deviations for that count, `noiseRmsSigma(frames)` = 0.258 /
sqrt(frames); no frames is never within. A check states its sigmas and the frames it measured.

### The sleep guard

`test/sleeps.txt` lists every sleep in the tests and their support (`sleep_for`, `sleep_until`,
`sleep`, `usleep`, `nanosleep`, `clock_nanosleep`, Python's `time.sleep` and `asyncio.sleep`, a
`poll` or `select` on nothing) and every Catch `WARN`, `CATCH_WARN` or `CHECK_NOFAIL`, one line each:
its role, its file, the line as it stands and why. A sleep's role is `induce` (the sleep is the
event), `negative` (a bounded look for what must not happen, taken after the event that would cause
it), `poll` (a pause between looks at the event, which decides), `pace` (the clock of a stand-in
that keeps real time: a sender, a heartbeat) or `luck`; a `WARN`'s is `luck` or `owed`. `luck` and
`owed` entries are the audit's inventory (`docs/design/root-classes.md`, steps R5b to R5d): each
names the step that removes it, and none is added.

`ear_sleeps_are_listed` (`test/sleep_guard.cmake`) reads every C, C++ and Python file under `test/`
and under `src/` in a `tests` or `testing` folder, and fails naming the place of a sleep or `WARN`
not listed (as often as a line occurs, so often it is listed), an entry that matches no line, a
`WARN` listed by a sleep's role, a sleep listed `owed`, an unknown role, a `luck` or `owed` entry
naming no step, and a check of nothing; strings and comments are not code. Its own tests
(`ear_sleep_guard_*`, on `test/sleep_guard/`) refuse each spelling, let through what only looks
like one, and refuse each of those faults. Editing a listed line, its comment included, means
editing its entry.

## How the suites are built

Catch2 v2 (`vendor/catch`). Each package has one test executable, and `ear_catch_tests`
(`test/CMakeLists.txt`) makes each test case its own CTest test with `catch_discover_tests` and
Catch's `--warn NoTests`, so `ctest --parallel` runs them side by side. Keep `;` out of test names:
CMake splits a name at it. A test case whose load would starve the realtime-timed tests beside it
is named by tag in `ear_catch_tests(<target> SERIAL <tag>)`, which gives it CTest's `RUN_SERIAL`
(only the `[s2]` spikes in `ear_pipewire_tests`: S2, S5 and S6's first). `test/main.cpp` turns off Catch's
POSIX signal handlers, which do not compile against glibc 2.34 and newer (Catch 2.13.5 fixes it).
It is Catch's runner, with one check before any test runs: Catch 2.12's console reporter indents a
failing test's header after the name's first `": "`, in a column 79 wide: an indent of 79 or more
aborts the process (an `assert`) and one of 78 loops forever printing `-`, either way before the
failing check is printed. So a test program holding a name whose first `": "` ends past column 69
(less than 10 columns left) fails its build: `ear_catch_tests` runs each executable with
`--ear-check-test-names` after it links, which exits 3 naming each such name (a run by hand refuses
the same way). Keep a test name's first `": "` early, or leave it out. A `SECTION`'s name is printed
the same way but is known only once it runs, so it is not checked; keep its `": "` early too.
Discovery is checked on its own as well: Catch.cmake takes the listing's exit code as a count of
tests and goes on past one that listed nothing, which would drop the whole executable from ctest
unseen, so `test/catch_tests_guard.cmake` fails the build unless every file discovery wrote names
a test.

Beside the executables, `test/CMakeLists.txt` runs the sleep guard and its own tests
([above](#the-sleep-guard)), the frame guard and its own (in the table below), `ear --help` (the binary loads its shared libraries), `ear_carried_patch_check` runs what configure asks of a tree that must carry a patch,
`ear_engine_conf_beside_ear` checks that the copy of `engine.conf` beside the binary matches
the source, and `ear_doc_example_*` take the worked example out of
[writing-a-node-type.md](writing-a-node-type.md#worked-example-tone) and compile it against today's
headers.

## What each test program proves

| Executable | What it proves |
|---|---|
| `ear_core_tests` | Port banks and whole port names; the desired graph's checks, plan and commit (event order, a port change as a replacement, stale plans), observer order, closing; the node lifecycle's machines by table (every event in every state, every operation in every phase); `NodeHost` over fake adapters: restart, removal, sound cards and RTP receivers in either order, a lost block, the two-phase start, one name per engine object, the pass-through, a class's `start()` and `stop()` paired, a type's throw from any of its code (each hook, its code's `prepare`, a legacy recipe and close, a sink's or handoff's consumer, a throw during a teardown or from an old configuration a `log` record only) a fact about its node; ear's own faults by owner ([architecture.md](architecture.md#faults-by-owner)): an adapter's `std::logic_error` inside a context call, caught by the type or not, and a factory's or companion adapter's throw in the flush, each `ear.internal-error` and its node `lost` with the rest running on, an adapter's refusal still `start-failed` through a nested context call too (a recording through the legacy start's `acquire`) and `std::bad_alloc` with it, an adapter that makes nothing or opens nothing ear's bug, a block or companion told faulted failing its node `lost`, and NodeHost's own throw still leaving `NodeHost` with its place; a type that throws at parse refusing the edit, `ear.hook-slow`, every rule a type breaks failing its node `start-failed` even when the type catches it, the interim sink (begun before the node is configured, never faded, bound by every drive and no silence), each companion's readiness deadline on the fake clock (its kind's text and code, a ready first wins, a restart's own), latency plans by role (a declaration on the bank a pair ends at counted on that pair, any other on its bank alone), runs with threads, and the teardown rules (the barrier first, a consumer blocked at teardown, a late write from an old run, a run leaked past the close deadline); `FrameRing`; the latency model by table (series and `Paths` apart, no path apart from zero, one row per bank shape and per past regression: a wrapped chunk's second call, a pair's declared delay reaching its input's told latency, a companion's `beyond` in series with peers reporting zero, parallel peers' least and most, rate conversion; a plan that does not fit its ports refused); `FrameClock` by table (frames as ns and a duration as frames, a row per site that converts or dates, large counts past where `frames * 1e9` overflows, and the exact quotient worked out in 128 bits at every rate from 1 Hz to 768 kHz and each edge count: zero, one frame, whole seconds and a frame either side, where `frames * 1e9` leaves int64, the largest count that fits, negative and sub-millisecond durations); the block's rules the engine and the harness share (`ear/core/block_rules.hpp`, `sink_ring.hpp`): `cutChunk` by table, a `SinkRing`'s order, cut and drops, `FadeRamp` in, out from mid-ramp and silenced, the stamps; cycle facts by table (`test/support/cycle_table.hpp`, the target `ear_cycle_table`: scripts of cycles, one row per cause and per shape of loss, 1, 3 and 70,000 cuts, the last past the cut record's ring, a pause short and past the ring, past the ring with one cycle not cut (the one undecided `lastCut`), a driver change, alone and with a loss, losses and repositions off the numbered clock, a cycle too large to run, with and without a drive, repositions, a cut after the block ran, alone and before a stall, a cycle of no frames, a quantum changed across a loss; each row expects a fact after its first cycle): every row through `CycleAccount` over a `CutRing`, and every row the type harness can make through `loseCycles`, `changeDriver` and `reposition`, its facts and its code's `GraphCycle::facts` checked ([writing-a-node-type.md](writing-a-node-type.md#lost-cycles)); the causes as a set, the cut record's reach and counts, and the record read on another thread while cycles wrap its ring, never answering wrongly; an account with no record and no cycle numbers by table (the discontinuities the real block counted before it had either); the reconciler; levels; the graph read; card ids and RTP addresses; the module-run converters' latency rule (`moduleRunDelay`) by table, and which shapes are module-run; settings tables; the diagnostics counters ([engine.md](engine.md#counters)): each node counter moved by its own cause by table, an overrun as work past the cycle, the graph's lost time a block per cut, the cut record counting cycles, cuts once each and a stall per run of consecutive cuts (past its ring too), read from another thread never more stalls than cuts or cuts than cycles, and through the type harness an induced cut moving the graph's counters and no node's, a pause the missed cycles, a reposition and a driver change their own, a code that stalls past its cycle its overruns, and `NodeHost` keeping a node's counters when its block goes and carrying them across a port-caused replace, never stepping back, from zero after a replace by another type; the type interface through the harness (its cycles' discontinuities and its fade-out acknowledged by the cycle that ends it, as the real block's). Pure, milliseconds |
| Compile-fail tests (`src/core/tests/compile_fail/`) | What must not compile is refused for its reason: `settings_table_bad.cpp` (a bad row, a misspelt or unlisted row, an unchecked read of an optional row, a type that disagrees with its table or folder) and `type_spec_bad.cpp` (a bank the spec lacks, a write into an input, a class of the wrong shape, a call `CreateOf` does not offer, a resource on the wrong bank, a spec in an unnamed namespace, and more) and `latency_bad.cpp` (a `Delay`, such as a companion's buffer, added to `Paths` as a peer, or passed off as a path). 89 cases, each running the compiler: the slow part of the suite (about 230 s summed at `JOBS=4`). They match GCC 14's wording, so a compiler bump may need their regexes edited |
| `ear_test_support_tests` | The test tools themselves on cycles and samples made by hand: which cycles a `StallPlan` names and how long a `StallLength` is, `Stalls` sleeping and counting, a `CycleLedger` keeping each cycle's facts as stated, `StallingCode`; the cycle judge: each verdict and its rule (a whole cycle judged; a catch-up excused only when every missed cycle was cut, its jump their frames and within the raw clock, its missed cycles counted; a catch-up over an uncut cycle, of the wrong jump or past the raw clock, and every reposition failed; each declared cause expected up to its count and failed past it, a failed verdict using none up; a card's missing block excused only where its cycle was cut), a plan of nothing judged refused, and `done()` needing the planned boundaries and every planned stall made and excused; `noiseRmsWithin` against real uniform noise from fixed seeds and a gain off by 2%; `WithoutFilePrivileges` binding its own thread to a folder's mode and no thread already running |
| Compile-fail tests of the callback boundary (`src/pipewire/tests/compile_fail/callback_boundary_bad.cpp`) | What the boundary refuses at compile time, each for its reason, beside a control that must compile: a raw function pointer to a source, an invoke or a table entry; a log method that is a raw pointer or can throw; `guarded<>` as a filter's `process` or invoked on the data loop; `realtime<>` that can throw, in a guarded table entry, as a timer, or invoked on a loop that is not the data loop; an object of another class than the member's; a wrong signature ([architecture.md](architecture.md#the-callback-boundary)) |
| Compile-fail tests of the property table (`src/pipewire/tests/compile_fail/engine_properties_bad.cpp`) | `property<home>()` refuses a key the table does not list for that home (an unlisted key, a key listed for another home: `notInThePropertyTable`) and a key it forbids there (`forbiddenOnThisHome`), each by the function the error names; a `PropertySet<home>` refuses a string, a `PropertyKey` made from a pointer (the constructor is private), a key of another home and the card seam's key on anything but a card; `seamChecked` refuses a temporary; only `stream.props` nests the RTP stream's arguments; beside a control that must compile ([engine.md](engine.md#properties-ear-sets)) |
| The property guard (`test/pipewire/property_guard.cmake`, CTest) | No file under `src/` but `src/pipewire/engine_property_set.{hpp,cpp}` and tests names PipeWire's property writers (every `pw_properties_new*`, `_set*`, `_update*`, `_add*`, `pw_*_update_properties`), `pw_core_create_object`, module loading (`pw_context_load_module`, `pw_impl_module_load*`), `SPA_DICT_ITEM*`/`SPA_DICT_INIT*`, `spa_dict_item` but as `const spa_dict_item*`, or a `spa_dict` made in place (`ear_properties_from_the_table`); and the guard itself: refusing each spelling it knows (`property_guard/<spelling>.snippet`, a `//` inside a string, a call split across lines and a function pointer among them), letting the table's functions, reads, pointers to read through and look-alikes in strings and comments through, skipping exactly the exempt path and tests but not a namesake elsewhere, and failing when it would check nothing |
| The callback guard (`test/pipewire/`, CTest) | No file under `src/` but the boundary's two and tests hands PipeWire or alsa-lib a callback itself (`callback_guard.cmake`), and the guard itself: refusing each spelling it knows (`callback_guard/<spelling>.snippet`, one listener split across lines and an engine node's realtime listener among them), letting look-alikes through, skipping exactly the exempt path and tests but not a file merely named like the boundary, and failing when it would check nothing |
| The frame guard (`test/frame_guard.cmake`, CTest) | No file under `src/` but `FrameClock`'s (`latency.hpp`, `latency.cpp`), its two named exceptions (`latency_spa.hpp`, `levels.cpp`) and tests does arithmetic with a rate: an identifier spelled as one (`rate`, `…_rate`, `…Rate`, `frequency`, `…_frequency`, `…Frequency`) as an operand of `*`, `/` or `%`, through members, casts and braces and across a line break (`ear_frames_through_frame_clock`, [engine.md](engine.md#dating)); and the guard itself: refusing each spelling it knows (`frame_guard/<spelling>.snippet`, a `//` inside a string, a digit separator before a character literal, code between two block comments and an expression split before or after its operator among them), letting a dereference, calls, other names, comments, strings and ns-to-`timespec` and ms conversions through, skipping exactly the exempt path and tests but not a namesake elsewhere, and failing when it would check nothing |
| `ear_folder_tests` | The confined folder on real temporary folders: made if missing (never its parents), `create()` beneath only, `read()` and `replaceIf()` refusing symlinks, directories, FIFOs, large files and paths out, replace-if-unchanged keeping the file's mode, a new file's mode under any umask, `restrictMode()`, a folder it cannot write, an inotify watch, `requireApart()` |
| `ear_node_white_tests`, `ear_node_file_tests`, `ear_node_card_tests`, `ear_node_rtp_tests` | Each type through `TypeHarness`, by its spec's bank handles: settings and each refusal's text, ports, the resources its `configure` asks for, readiness and loss, stop, start and restart, and what its code or the pass-through carries. `file` runs over a real recordings folder: exact samples, every frame in order through chunks split at the ring's end, every cycle the code drove and none while stopped, a restart finishing one WAV and opening the next, paths that lead out refused |
| `ear_node_sendspin_tests` | The feed on real threads (a writer's wait ended by a read or a clear, by its own counts), PCM, the audio code's clock on scripted cycle facts (`CycleAccount` over a `CutRing`: a catch-up or a pause skipped as far as the feed holds it, past the feed and past 2^32 frames, and nothing written after it taken; every other cause, and a clearing cycle, skipping nothing; nothing told from an overdue cycle and all of a burst told from the first on time, dated nominally; a lossy cycle on time told; the bound forcing one report dated by when its cycle ran, the next clamped to it, and the clamp idle otherwise), the connection watch on fake time, settings, key files on a real folder (a read-only one under root too), port claims and the probe, the transport's share per address and its bound of twice the room in threads, exactly, the mDNS name and on-link check, peer text on a status line, the notes' rate; through a real engine and the real Sendspin server, the clock within 1 ms on the dummy driver and through a following test card, judged over a planned 750 cycles the cycle judge judged whole and outside any library settle, the judge's four stalls made upstream meanwhile and the next whole cycle after each judged, after the internal clock has reached the raw clock's rate over its whole cycles; a stall past the feed (200 ms and 1 s, upstream and in the block, in the burst shape the host gives, which each run names) corrected once by the library by what the feed did not hold (and at most one settle step of two blocks after it, with a dry or empty-feed cycle between as its evidence), or not at all when the burst lost nothing, a 1.5-cycle hiccup skipped exactly, and a starving network gap corrected once; a lost card's recovery within 3 s of graph cycles from the cut or the host's last disturbance after it |
| `ear_node_types_tests` | What holds for every type, taken from the generated list as `main.cpp` takes it: a folder makes one type of its name, each type's settings table and every refusal's text and order (`test/nodes/settings_refusals_test.cpp`), and every type but those in `notYetInHarness` run through the harness (configured, running once its companions are ready, every output sample written, stopped, started, restarted, disabled, enabled, removed). Also, as CTest tests: configure refuses one node type linking another in each spelling, and an empty list of types; and no file under `src/nodes/` writes standard output or error (`test/nodes/standard_output_guard.cmake`, itself tested) |
| `ear_output_tests` | The output thread on real pipes (order, a reader that does not read refused at once, everything written at stop); the capture of descriptors 1 and 2 in its own test program; a crash's last words in forked children (a dying process's own last line, after the queue and the pipes and before terminate's message, once); the main-loop watchdog on a clock stepped by hand and on a real stall |
| `ear_control_tests` | The JSON-RPC envelope (the specification's examples), commands, events, the graph file's types, every standard-output record field for field against `output-records.json` (drops, pacing, bad UTF-8, one order from many threads), the lifecycle's wire, gateway and sessions, all against `test/protocol` |
| `ear_config_tests` | The graph file model (each of ear's codes at its place, `@include`, missing and unreadable files), places in UTF-16 columns, edits at the source, `disabled`, the lifecycle verbs' mapping to the wire; the keeper's sequencer stepped by hand over a real folder (accepted and refused edits, both hand-edit collisions, `graphFile.set`, batches, write failures, pending restart, stop); the keeper's thread and the whole keeper with inotify (a stopped keeper's folder watch removed and its descriptor closed while its thread runs, as the kernel counts them in `/proc/self`) |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample, the clock and each cycle's `GraphCycle`; the cycle facts ([engine.md](engine.md#graphcycle)): the engine's record of the internal clock's cuts numbering a block's cycles from its first (above 0, increasing), numbering each cycle by the driver's position and, over a chain's tail's span, holding a cycle cut exactly when the tail missed it (the chain's head stalling four times, each stalled cycle recorded cut, its tail stating each cut as `catchUp` and nothing else), `Engine::create` failing with no internal clock and leaving nothing registered when its listener's registration throws, a realtime listener heard while held and silent once removed, and on a wedged loop left on its node with its target kept by the boundary (still counting once the loop runs again, its handle refusing to attach again); and the cycle table's conformance: every row an engine can make (`CycleStep::onEngine`: start and drive, one cycle cut by a head's 1.5-cycle stall, a pause by making the block inactive, a cut after the block ran by its own stall before a tail) through a real block at the table's format, each step's facts the row's (a pause's counts scaled to the cycles the engine took, the raw time within the driver's 1 ms clamp), every cycle's number against the driver's position (the position less the number's quanta constant), each planned cut recorded on its own cycle and never fewer cuts than planned, an attempt the host spoiled (more cuts than planned, a cycle before the first drive) made again up to eight times; the diagnostics counters ([engine.md](engine.md#counters)): a chain's head stalling four times moving the graph's cut cycles and stalls by at least four and its tail, which missed each cut cycle, counting no missed cycle, a block made inactive counting exactly the missed cycles not cut its cycles state, and a code stalling one and a half cycles three times moving its node's overruns by at least three; latency per bank pair and the latency model's conformance: every row of its table (`test/support/latency_table.hpp`, which `ear_core_tests` runs through `workOut`; each row expects some latency that is not zero) through a real block, every port linked to a bare `pw_filter` (a peer telling the row's latency in ns, quanta and frames, or, where the row gives none, a listener with `port.ignore-latency` that is no path), checking what every port tells against `workOut`'s told latency, in rate units only, as its linked node hears it; each output's downstream as the code reads it in `GraphCycle`; each inside input bank's told latency as a source's clock on it is told it (`SourceTick::playback`) and each inside output bank's most as a sink's stamp carries it (`upstreamFrames`); and a peer's negative latency clamped before a receiver's buffer is added; the harness's side of that conformance: every row again with no peer through a real block against `workOut` with none (the harness's whole latency rule), and the plan a type builds in the type harness carried on a real block with no peer, its sink's stamp and the playback latency its source's clock is told compared with the harness's, and a scripted clock's skip, a clear (with the skip it asks on the clearing tick, ignored) and the underrun after, each tick's held, `SourcePlayed` and told playback compared tick for tick between the harness and a real block; denormals, teardown, SIGTERM; the legacy handoff and the interim sink (order, stamps, both rings dropping together, the dropped count read while a block writes, no allocation on the realtime side, a ramp through a real block across a stop and a start, each chunk stamped with the capture latency its block tells, a chunk split at the ring's end dated by its own first frame, the same cycles through `ThreadSink` and through the type harness's sink compared call by call, split in the same places, a consumer that throws on its real thread failing only its node through `NodeHost`); the latency model's units (`latency_spa.hpp`: a peer's triple to frames, what a block tells in rate units) and its parallel combine checked against PipeWire's own `spa_latency_info_combine` and `_finish` on the same peers, unit by unit, mixed units inside PipeWire's bound; a companion's latency beyond an inside bank added in series to its peers' (a real receiver's buffer told whole on `stream`); level windows; the main-loop executor and the watchdog; the PipeWire spikes kept as regressions; the property table ([engine.md](engine.md#properties-ear-sets): every row read back from the object PipeWire holds and no object holding a forbidden key, `engine.conf` against its rows, its four sections and its forbidden keys, each row's test existing, and the page's tables against the code); a block size that is not a power of two running as itself; the quantum floor and limit at the block size (a node forcing a larger or a smaller quantum changes no cycle), no clock property on a card or a receiver, and a card's ALSA period size and count; sound cards through a test ALSA card (alsa-lib's file and null PCMs, made clockless); the module-run converters' shared body apart from RTP, through a test-only shape on PipeWire's pipe-tunnel module ([engine.md](engine.md#module-run-converters): ready and channel order, the stale-name rule, a module that will not load, a node that never runs, a node removed, a module destroyed and a fault named by the module's body, each in the module's own words); RTP receivers fed by a test sender on 127.0.0.1 (senders at 44.1 and 96 kHz resampled); ear's own engine log; the callback boundary (a guarded callback's throw reported with its member and the place the main loop was in, through NodeHost's rethrow and past a callback made during an unwinding; a FaultOwner's fault reported with its node, the owner told after and only once however often it throws, and an owner that throws as it is told reported again with no owner; a throw that came out of NodeHost through an owner's callback reported with no owner, the owner not told, and in a forked child, under the Faults ear runs with, a NodeHost `std::logic_error` out of a block's callback recorded `fatal` and ending the process with the line; that Faults returning for an owned fault; each owner by a throw injected in its real callback (a port block told faulted, leaving the graph and still completing its requests; an RTP receiver's body, its lifecycle with a sync in flight, and its link; a card's body; a route link failing its route, and a route link listener's throw taken as core's); and each adapter's making (a block, a card, a receiver) thrown after each registration in turn, leaving nothing registered; a realtime call plain; the executor's batch running on past an item that throws, and its timers; a second holder of the boundary, or a second engine, refused; with no `Faults`, a forked child ending with the line before terminate's message) |
| `ear_server_tests` | Port 7777 on real sockets with a fake main loop and gateway: files, Host and Origin, round trips, the 1008 bound, a client that resets, stop with 1001, levels |
| `ear_system_tests` | The real `out/ear` on an `ear.conf`, reading its records: white noise recorded to the routed channel only (a second of it waited for in the WAV, its RMS from the count), every line a record with `seq` unbroken; old port names refused; folders it cannot make; the graph file end to end (editor edits written back, hand edits winning, typos, symlinks, `@include`, boots that run nothing); an `rtp` node's tone recorded channel for channel, over ten tenths the sender sent in time; `sendspin` against the real server (pairing, a ramp frame for frame until the WAV holds a second of it, its skips no more than ear's records say it lost, identity across restarts, floods with holders renewing on the transport's close, mDNS, a lost server, the source's stamps step by step); the control port (edits heard in the WAV, levels from the count, errors, stop); a stall of the whole process as one `ear.graph-lost-cycles` record, its counts in the snapshot and `counters.changed`; the node lifecycle across restarts of ear, on a shared `disabled` value, on the test card and on a held UDP port (a still record, then the receiver's deadline failing the node) |
| web (Vitest, `make web-test`) | The editor's logic against every `test/protocol` fixture ([editor.md](editor.md#developing-it)) |
