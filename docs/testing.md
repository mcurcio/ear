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
  file fails.
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
  broke, to a planned number of them ([the cycle ledger](#making-the-rare-event)); a value that
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
  made; it holds the one sleep that stalls a data thread on purpose. S5's overruns, the source's
  cut-short cycles and the clock's stall tests all stall through it.
- **`CycleLedger`** records each cycle a code ran (position, frames, both clocks' start, rate,
  `xruns`, `discontinuity`, whether it stalled on purpose) into storage made beforehand, and calls a
  cycle clean when it follows the last one with nothing lost: no discontinuity, no new xrun, its
  position the last one's end, the last one not stalled. A test judges exactness over clean cycles
  and waits until `clean()` reaches the number it planned.
- **`StallingCode`** is an `AudioCode` that writes silence, records into its ledger and stalls at
  its plan's cycles (by ledger index).
- **Judged to a planned count.** A check that holds only where the clock did not jump (a take-up's
  first cycle at the published position, a ramp continuing across a cycle boundary) is judged where
  the cycles themselves say so: a ledger's clean cycles, the block's own `xruns` unchanged since its
  last cycle before the take-up, or a probe downstream that heard consecutive positions there. The
  test repeats until it has judged the number it planned; a jump the host made is recorded and only
  makes another round, and what holds across a jump too (a start never later than its first cycle,
  a sink's stretches exactly the code's) is checked every time. A model of a rule the design defines
  over jumps applies the rule: S5's source takes a jump beyond its capacity as a new clock and
  re-anchors, as the source rule says, instead of counting a drift. What excuses a jump is checked
  against the engine, never taken from it: each jump must be time the raw clock shows was lost (within
  two cycles and the driver's 1 ms clamp), so a host's stall is excused and an engine that jumps
  further than time went fails.

### A writer's own event

A test that needs a thread of ear's to be waiting waits for that thread's own signal, never a sleep
that guesses it has started: `EventSources::waitsBegun(source)` counts the waits a source's writer
began, each past its last look at the ring, so only a play, the close or its patience ends it, and
`waitsWoken(source)` how many of them a play or the close ended rather than the patience
([engine.md](engine.md#sources)). `source_test.cpp` waits for it before it plays or closes, and
checks `waitDeadline(milliseconds::max())` is a year away.

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
(only the `[s2]` spikes in `ear_pipewire_tests`: S2 and S5). `test/main.cpp` turns off Catch's
POSIX signal handlers, which do not compile against glibc 2.34 and newer (Catch 2.13.5 fixes it).

Beside the executables, `test/CMakeLists.txt` runs the sleep guard and its own tests
([above](#the-sleep-guard)), `ear --help` (the binary loads its shared libraries), `ear_carried_patch_check` runs what configure asks of a tree that must carry a patch,
`ear_engine_conf_beside_ear` checks that the copy of `engine.conf` beside the binary matches
the source, and `ear_doc_example_*` take the worked example out of
[writing-a-node-type.md](writing-a-node-type.md#worked-example-tone) and compile it against today's
headers.

## What each test program proves

| Executable | What it proves |
|---|---|
| `ear_core_tests` | Port banks and whole port names; the desired graph's checks, plan and commit (event order, a port change as a replacement, stale plans), observer order, closing; the node lifecycle's machines by table (every event in every state, every operation in every phase); `NodeHost` over fake adapters: restart, removal, sound cards and RTP receivers in either order, a lost block, the two-phase start, one name per engine object, the pass-through, a class's `start()` and `stop()` paired, a hook that throws, `ear.hook-slow`, every rule a type breaks failing its node `start-failed` even when the type catches it, the interim sink (begun before the node is configured, never faded, bound by every drive and no silence), each companion's readiness deadline on the fake clock (its kind's text and code, a ready first wins, a restart's own), latency plans, runs with threads, and the teardown rules (the barrier first, a consumer blocked at teardown, a late write from an old run, a run leaked past the close deadline); `FrameRing`; the latency model by table (series and `Paths` apart, no path apart from zero, one row per bank shape and per past regression: a wrapped chunk's second call, a pair's declared delay reaching its input's told latency, a companion's `beyond` in series with peers reporting zero, parallel peers' least and most, rate conversion; a plan that does not fit its ports refused); the reconciler; levels; the graph read; card ids and RTP addresses; settings tables; the type interface through the harness. Pure, milliseconds |
| Compile-fail tests (`src/core/tests/compile_fail/`) | What must not compile is refused for its reason: `settings_table_bad.cpp` (a bad row, a misspelt or unlisted row, an unchecked read of an optional row, a type that disagrees with its table or folder) and `type_spec_bad.cpp` (a bank the spec lacks, a write into an input, a class of the wrong shape, a call `CreateOf` does not offer, a resource on the wrong bank, a spec in an unnamed namespace, and more) and `latency_bad.cpp` (a `Delay`, such as a companion's buffer, added to `Paths` as a peer, or passed off as a path). 89 cases, each running the compiler: the slow part of the suite (about 230 s summed at `JOBS=4`). They match GCC 14's wording, so a compiler bump may need their regexes edited |
| `ear_test_support_tests` | The test tools themselves on cycles and samples made by hand: which cycles a `StallPlan` names and how long a `StallLength` is, `Stalls` sleeping and counting, which cycles a `CycleLedger` calls clean, `StallingCode`; `noiseRmsWithin` against real uniform noise from fixed seeds and a gain off by 2% |
| Compile-fail tests of the callback boundary (`src/pipewire/tests/compile_fail/callback_boundary_bad.cpp`) | What the boundary refuses at compile time, each for its reason, beside a control that must compile: a raw function pointer to a source, an invoke or a table entry; a log method that is a raw pointer or can throw; `guarded<>` as a filter's `process` or invoked on the data loop; `realtime<>` that can throw, in a guarded table entry, as a timer, or invoked on a loop that is not the data loop; an object of another class than the member's; a wrong signature ([architecture.md](architecture.md#the-callback-boundary)) |
| The callback guard (`test/pipewire/`, CTest) | No file under `src/` but the boundary's two and tests hands PipeWire or alsa-lib a callback itself (`callback_guard.cmake`), and the guard itself: refusing each spelling it knows (`callback_guard/<spelling>.snippet`, one listener split across lines among them), letting look-alikes through, skipping exactly the exempt path and tests but not a file merely named like the boundary, and failing when it would check nothing |
| `ear_folder_tests` | The confined folder on real temporary folders: made if missing (never its parents), `create()` beneath only, `read()` and `replaceIf()` refusing symlinks, directories, FIFOs, large files and paths out, replace-if-unchanged keeping the file's mode, a new file's mode under any umask, `restrictMode()`, a folder it cannot write, an inotify watch, `requireApart()` |
| `ear_node_white_tests`, `ear_node_file_tests`, `ear_node_card_tests`, `ear_node_rtp_tests` | Each type through `TypeHarness`, by its spec's bank handles: settings and each refusal's text, ports, the resources its `configure` asks for, readiness and loss, stop, start and restart, and what its code or the pass-through carries. `file` runs over a real recordings folder: exact samples, every cycle the code drove and none while stopped, a restart finishing one WAV and opening the next, paths that lead out refused |
| `ear_node_sendspin_tests` | The feed on real threads, PCM, the audio code's clock and skips, the connection watch on fake time, settings, key files on a real folder, port claims and the probe, the transport's share per address, the mDNS name and on-link check, peer text on a status line, the notes' rate; through a real engine and the real Sendspin server, the clock within 1 ms on the dummy driver and through a following test card |
| `ear_node_types_tests` | What holds for every type, taken from the generated list as `main.cpp` takes it: a folder makes one type of its name, each type's settings table and every refusal's text and order (`test/nodes/settings_refusals_test.cpp`), and every type but those in `notYetInHarness` run through the harness (configured, running once its companions are ready, every output sample written, stopped, started, restarted, disabled, enabled, removed). Also, as CTest tests: configure refuses one node type linking another in each spelling, and an empty list of types; and no file under `src/nodes/` writes standard output or error (`test/nodes/standard_output_guard.cmake`, itself tested) |
| `ear_output_tests` | The output thread on real pipes (order, a reader that does not read refused at once, everything written at stop); the capture of descriptors 1 and 2 in its own test program; a crash's last words in forked children (a dying process's own last line, after the queue and the pipes and before terminate's message, once); the main-loop watchdog on a clock stepped by hand and on a real stall |
| `ear_control_tests` | The JSON-RPC envelope (the specification's examples), commands, events, the graph file's types, every standard-output record field for field against `output-records.json` (drops, pacing, bad UTF-8, one order from many threads), the lifecycle's wire, gateway and sessions, all against `test/protocol` |
| `ear_config_tests` | The graph file model (each of ear's codes at its place, `@include`, missing and unreadable files), places in UTF-16 columns, edits at the source, `disabled`, the lifecycle verbs' mapping to the wire; the keeper's sequencer stepped by hand over a real folder (accepted and refused edits, both hand-edit collisions, `graphFile.set`, batches, write failures, pending restart, stop); the keeper's thread and the whole keeper with inotify |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample, the clock and each cycle's `GraphCycle`, latency per bank pair, denormals, teardown, SIGTERM; the legacy handoff and the interim sink (order, stamps, both rings dropping together, the dropped count read while a block writes, no allocation on the realtime side, a ramp through a real block across a stop and a start, each chunk stamped with the capture latency its block tells, a chunk split at the ring's end dated by its own first frame); the latency model's units (`latency_spa.hpp`: a peer's triple to frames, what a block tells in rate units) and its parallel combine checked against PipeWire's own `spa_latency_info_combine` and `_finish` on the same peers, unit by unit, mixed units inside PipeWire's bound; a companion's latency beyond an inside bank added in series to its peers' (a real receiver's buffer told whole on `stream`); level windows; the main-loop executor and the watchdog; the PipeWire spikes kept as regressions; the quantum floor and limit at the block size (a node forcing a larger or a smaller quantum changes no cycle), no clock property on a card or a receiver, and a card's ALSA period size and count; sound cards through a test ALSA card (alsa-lib's file and null PCMs, made clockless); RTP receivers fed by a test sender on 127.0.0.1 (senders at 44.1 and 96 kHz resampled); ear's own engine log; the callback boundary (a guarded callback's throw reported with its member and the place the main loop was in, through NodeHost's rethrow and past a callback made during an unwinding; a FaultOwner told after; a realtime call plain; the executor's batch running on past an item that throws, and its timers; a second holder of the boundary, or a second engine, refused; with no `Faults`, a forked child ending with the line before terminate's message) |
| `ear_server_tests` | Port 7777 on real sockets with a fake main loop and gateway: files, Host and Origin, round trips, the 1008 bound, a client that resets, stop with 1001, levels |
| `ear_system_tests` | The real `out/ear` on an `ear.conf`, reading its records: white noise recorded to the routed channel only, every line a record with `seq` unbroken; old port names refused; folders it cannot make; the graph file end to end (editor edits written back, hand edits winning, typos, symlinks, `@include`, boots that run nothing); an `rtp` node's tone recorded channel for channel; `sendspin` against the real server (pairing, a ramp frame for frame, identity across restarts, floods, mDNS, a lost server); the control port (edits heard in the WAV, levels, errors, stop); the node lifecycle across restarts of ear, on a shared `disabled` value, on the test card and on a held UDP port (a still record, then the receiver's deadline failing the node) |
| web (Vitest, `make web-test`) | The editor's logic against every `test/protocol` fixture ([editor.md](editor.md#developing-it)) |
