---
name: pipewire-engine
description: How EAR runs PipeWire's engine inside its own process (no daemon, no session manager, no socket), the property table of every PipeWire property ear sets (engine.conf and code) and its four checks, the internal clock (the dummy driver, which always drives and tracks the raw clock) and the sound cards that follow it, the graph group, GraphCycle's clock facts, latency carried per bank pair, denormals, engine companions (sound cards and RTP receivers loaded from module-rtp-source), the asynchronous drive() and fadeOut() with their bounds and the wedged data loop, the interim sinks' taps, what the slice 3a spikes proved about invokes and the one data loop, level metering, realtime priority (CAP_SYS_NICE), the traps found while building it, telling xruns under host load from bugs, and how to bump the PipeWire version. Use when changing src/pipewire/ (engine, port blocks, links, live ports, handoffs, engine companions, sound cards, RTP receivers, the main-loop executor) or engine.conf, when adding or changing a PipeWire property, when a port block never processes, a link never goes active, a card never follows or never becomes ready, latency never arrives, an rtp node stays configuring, fails "the RTP receiver never ran" or is silent, a card fails "never started", a node never reaches running or a fade or barrier is overdue, ear says the data loop is wedged, a sink drops or misses cycles, levels stop, engine or RTP tests fail only on a loaded host, the data thread is not realtime, a stop signal kills the process, or the block size is not the one asked for, and before bumping the build image's PipeWire.
---

# Running PipeWire's engine inside EAR

EAR does not connect to a PipeWire daemon: it is the PipeWire instance. `ear::pipewire::Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in EAR's own process and
connects to that context itself (`pw_context_connect_self`). Every other adapter in
`src/pipewire/` works through that one connection. The PipeWire sources behind the claims here,
with file and line numbers, are cited in `docs/engine.md` (its property table, sound cards,
levels, the graph group and RTP receivers); the claims were checked by the tests in `src/pipewire/tests/` against PipeWire 1.4.2, the version
Debian 13 (trixie) ships in the build image (`docker/Dockerfile`).

## What the engine is made of

| Piece | Where | Why |
|---|---|---|
| Main loop, context, own connection | `Engine::create` | One process, one loop; every core object and adapter runs on the main loop's thread; denormals flushed on the data thread |
| `engine.conf` | `src/pipewire/engine.conf`, copied beside `out/ear` | Read by `pw_context_new` (passed as `config.name`); loads the modules and the driver |
| Clock settings | `contextProperties()` in `engine.cpp` | The graph frequency and block size, from `ear.conf`'s `graph_frequency` and `block_size` at boot (else `--graph-frequency` and `--block-size`) |
| Port blocks | `port_blocks.cpp` | One `pw_filter` per node, one 32-bit float mono port per declared port (public and inside); every block in the graph group; `drive()`; level metering; each cycle's `GraphCycle`; latency per bank pair (`carryLatency`) |
| Level mailbox | `level_mailbox.hpp` | One triple buffer per block: the realtime thread publishes a level window, an ordinary thread reads the latest |
| Live ports | `live_graph.cpp` | Registry globals joined into node name, port name, direction, port id |
| Links | `links.cpp`, `link_proxy.hpp` | `link-factory` links by port global id, owned by EAR's connection |
| Engine companions | `engine_companions.cpp` (`joinEngineCompanion`) | The one lifecycle of every engine node a node's insides ask for: runs alone, then linked to the block (the engine companion by global id, the block by node name, ports by name), ready, lost |
| Sound cards | `sound_cards.cpp` | One kind of engine companion: one ALSA adapter node per card and direction |
| RTP receivers | `rtp_receivers.cpp` | The other kind: `libpipewire-module-rtp-source` loaded into the context, one per `rtp` node |
| Sinks and handoffs | `handoffs.cpp` | Realtime thread to an ordinary thread: `ThreadSinks` (the interim sink: a frame ring and a stamp ring, written by the block's taps, and one `pw_thread_loop` handing chunks to its consumer) and `ThreadHandoffs` (the legacy handoff `sendspin` alone uses until slice 3c): `spa_ringbuffer` plus one `pw_thread_loop` each |
| The wedge mark | `data_loop_wedge.hpp` | Once ear says the data loop is wedged, `main.cpp` marks the engine and the adapters stop calling into it (section "drive()") |
| Main-loop executor | `main_loop_executor.cpp` | Work posted from any thread (the control port's graph calls) runs on the main loop: a mutex-guarded queue plus a `pw_loop` event source |
| Shared names | `engine_names.hpp` | The graph group's name, a card's driver priority and the internal clock's name |

`main.cpp` passes `<directory of /proc/self/exe>/engine.conf`; the tests pass the source tree's
file (`EAR_ENGINE_CONF`). `Engine` does no lookup of its own. The ctest
`ear_engine_conf_beside_ear` fails if the copy beside the binary differs from the source.

## Properties: one table

Every PipeWire property EAR sets, in `engine.conf` or in code, is one row of
`src/pipewire/engine_properties.hpp`. Why each is set, what it couples to and how the table is
checked: `docs/engine.md#properties-ear-sets`; what EAR never sets:
`docs/engine.md#properties-ear-never-sets`. `engine.conf`'s comments only point there. To add,
change or remove a property:

1. Change its row in `engine_properties.hpp` (home, key, value, heading, `pinned::` test) and its
   row in `docs/engine.md`'s table in the same change.
2. Build it with `property<Home::<home>>("<key>")`, a `PropertyKey<home>`, into that home's
   `PropertySet` or `ModuleArguments` (`engine_property_set.hpp`; a core object through
   `createObject`); a key of another home does not compile, and the property guard refuses any
   other way to
   set a key.
3. Give the live test the instance's expected value if the row's value is prose, or say where
   PipeWire holds it (another key, consumed before the node) and where its effect is checked.

`engine.conf`'s `context.spa-libs` and `context.modules` (not properties) are in
`docs/engine.md#modules-and-libraries`; `libpipewire-module-rtp-source` is not in `engine.conf`:
`RtpReceivers` loads one per `rtp` node with `pw_context_load_module`. The internal clock is
`engine.conf`'s one `context.objects` entry (its rows: home "internal clock"); it has no ports, so
it never shows up as a live port.

`cpu.zero.denormals` is forbidden in `engine.conf`: on 1.4.2 it lands on the main thread in this
engine (the data loop's setup runs inline there before the data thread enters) and leaks to every
thread made later. `Engine::create` sets FTZ and DAZ (FZ on arm64) with a blocking invoke on the
data loop instead (`flushDenormals`); "process() runs with denormals flushed to zero" reads the
MXCSR from `process`. That invoke races the data thread too: `pw_data_loop_start` returns before
the thread enters its loop, and until it does `loop_invoke` runs the function inline on the caller
(about 4% of boots). So `flushDenormals` does nothing on the thread that invoked it, and
`Engine::create` invokes again until it ran on the data thread (1 ms apart, failing after 5 s). Any
blocking invoke made right after `pw_context_new` meets the same race.

## The clock

`Engine` sets the clock as context properties (the table's `context` rows; why each, and the
trap a card's ALSA buffer fell into: `docs/engine.md#the-quantum-guard`). A port block sees
`position->clock.duration == blockSize` and `clock.rate.denom == graphFrequency`; the test
"Blocks process at the graph frequency and block size" checks it with 44100 Hz and 256.

Both are fixed for the engine's life (changing them live is deferred). `main.cpp` reads
`ear.conf` once before `Engine::create` and takes the two values when both lines read; when
the file is missing, cannot be read or lacks either line, the engine takes `--graph-frequency`
and `--block-size` (default 48000 and 256). A later edit or hand edit that changes either value
is accepted for everything else and reported as pending restart (`pendingRestart` in
`graphFile.*`, `graph-file pending-restart <keys>` on standard output, PostgreSQL's
`pending_restart` pattern): it applies at the next start. Nodes always see the engine's
format (`NodeGrant::format()`), never the file's pending one.

## The graph group and the internal clock

Every port block sets `node.group = ear.graph` (`graphGroup` in `engine_names.hpp`), and so does
the dummy driver. PipeWire puts every node of one group on the same driver, and it walks drivers in
`priority.driver` order: the dummy (200000) comes first, so it collects the group and everything
linked to it, and a linked card (1) is a follower. So one clock always drives every block, the
dummy, and no card ever moves it (the slice 7 spike, `docs/engine.md#the-internal-clock`).
The test "Every block is in the ear.graph group and processes on the dummy driver alone" checks the
group, the dummy's group and its clock name. A card's adapter node has no `node.group` (next
section).

**It tracks `CLOCK_MONOTONIC_RAW`.** `CLOCK_MONOTONIC` is slewed by NTP (chrony may slew it by up
to 83,333 ppm), and timerfd cannot arm `CLOCK_MONOTONIC_RAW` (`EINVAL`). With `clock.id =
monotonic-raw` the driver runs in its tracking mode (`node-driver.c`): its timer stays on
`CLOCK_MONOTONIC`, a delay-locked loop steers it so `clock.position` follows raw time, and
`rate_diff` is the loop's correction. `clock.nsec` and `next_nsec` stay `CLOCK_MONOTONIC`. Its
error is clamped at 1 ms a cycle. **`engine.conf` sets `resync.ms` beyond any stall (1e9 ms), so
the driver never resets.** With the default (10 ms), past it after a data-thread stall,
`on_timeout` puts the position on raw time but leaves its timer where it was: the timeouts it
then fires back to back (the timer catching up) each add a block while raw time barely moves, so
the next reset steps the position *back* by up to about 440 frames, and the burst leaves it up to
10 ms ahead of raw time. There the error sits in the clamp, and the loop, at its least
bandwidth, slews the whole graph by hundreds of ppm for tens of seconds to work it off (measured:
+190 frames and -230 ppm 9 s after one 20 ms stall; a sendspin stream then interpolates frames,
and a reset's forward jump is any number of frames, not whole blocks). Without the reset the
driver catches up on its own timer: the blocks run about every other timeout of the burst, and
each cycle they run skips one block; after stalls of up to a few seconds the position never
steps back and the phase to the raw clock is kept to within a frame (the test "After data-thread
stalls past 10 ms ...", and S2's `stepsBack == 0`). PipeWire's non-tracking driver never resets
either. **The limit:** the catch-up burst after any pause is inherent to `node-driver.c` whatever
`resync.ms` is (about 187 timeouts a second of pause at 48000/256), and each feeds the loop its
clamped 1 ms error, so a pause of tens of seconds or more winds it up and slews the graph by 0.1
to 0.6 % for minutes (simulated: 30 s, +287/-429 ppm for about 60 s; 120 s, 1700 ppm at peak).
A 3 s one stays within 150 ppm from 5 s on ("After a 3 s data-thread stall ..."). The default is
somewhat better only for pauses of minutes; a large finite value is worse than both (it resets
mid-burst and steps back again and again). The real fix is upstream: a reset that re-arms
`next_time` from now and calls `spa_dll_init` (`docs/engine.md#the-internal-clock`). A cycle is still told
apart by entry order where it matters, not by position (docs/engine.md#the-one-data-loop-and-invokes; S2 in
`spikes_test.cpp`). Its `clock.name` is
`clock.system.monotonic-raw` (`internalClockName`).
Whether it follows a host slewed at chrony's worst case is untested; the fallback is chrony's
`maxslewrate` (the `ear-on-talos` skill).

**`GraphCycle`** is made by the block once a cycle (`port_blocks.cpp`, `onProcess`), so no type reads
a clock: `startNs` (`clock.nsec`), `position`, `startRawNs` (both clocks sampled together, the
difference added to `clock.nsec`, so a late wake cancels out), `rateDiff` (`clock.rate_diff`),
`driverId` (`clock.id`, the driver's global id), `nextNs` (`clock.next_nsec`) and `facts` (below).
PipeWire gives a filter no xrun count (`clock.xrun` is set only by a driving ALSA node), so the
block states each loss in its facts; `facts.discontinuity()` marks the block's first cycle, a new
code's first cycle, a new driver, any position jump (a stall's catching up), and the cycle after
one too large for the block. Both clocks are read mono, raw, mono, and the offset taken from the midpoint, as
`node-driver.c` does. `downstream` is the latency downstream of each output (below). The dummy's
`clock.delay` is always 0, followers or not: a following ALSA node writes its delay into its own
clock, never into the driver's position.

**`GraphCycle::facts`** (`docs/engine.md#graphcycle`, `docs/writing-a-node-type.md#lost-cycles`) is
core's `CycleAccount` over the engine's record of the internal clock's cuts: `DriverCuts`
(`driver_cuts.hpp`, private), made in `Engine::create` before any block, found by `node.name`
(`internalClockNode`; none is an `EngineError`), listening to the dummy's realtime `start` and
`incomplete` through the boundary's `listenRealtime` (realtime entries only; the guard refuses a raw
`pw_impl_node_add_rt_listener`). Traps: number a block's cycle by the listener's own count
(`Engine::cuts().started()` inside `process`), never `clock.cycle`; never read the clock through a
filter's mapping from the listener (it goes with the filter); the listener must be added before the
first block (an async invoke the data loop runs before any block's node reaches the driver);
removing it is a blocking invoke, so on a wedged loop the hook is left and the boundary keeps it and the
ring it tells (the `RealtimeListener` holds its target, so no owner has to leak itself). The facts
are the one home of what was lost: a source's `SourceCycle` and a sink's `ChunkStamp` carry them
(the stamp as `causes` and `lostFrames`), and nothing in ear works a loss out from the position
(`GraphCycle::xruns` and `discontinuity` are gone).

## Latency, per bank pair

A port block connects with `PW_FILTER_FLAG_CUSTOM_LATENCY`. Without it `pw_filter` merges every
output's downstream latency into every input, and every input's upstream into every output
(`filter.c`, `default_latency`, plus the filter's process latency), which mixes paths that never
meet (a card's playback and capture; sendspin's `to` and `from`). What 1.4.2 does under the flag
(read in `filter.c` and `impl-port.c`):

- The engine recomputes a port's latency from its peers whenever a link or a peer's latency
  changes (`pw_impl_port_recalc_latency`): an output gets the combined playback latency of the
  inputs it feeds (direction INPUT), an input the combined capture latency of what feeds it
  (direction OUTPUT), and sets it on the port. The filter stores it and, under the flag, emits
  `param_changed(port, SPA_PARAM_Latency)` and does nothing else (`handle_latency`). A port's
  own-direction latency it is told is ignored.
- The block then tells its ports' latency itself with `pw_filter_update_params` (a Latency param
  of the port's own direction), which the engine hands to the peers.
- **Trap: the last link going is never told.** When a port loses its last peer the engine sets a
  null Latency param, and `handle_latency` returns on null before it emits anything. So a block
  keeps the latency its last peers reported until another peer reports. A lost card leaves its
  latency on the card block's inside ports (and, through the pass-through, upstream): harmless
  while nothing hears what goes there, and a card made again reports afresh. "A following card's
  latency reaches its block's inside bank" checks this trap, so a PipeWire that fixes it fails.

The block's rule is the latency model's, `workOut` in `ear/core/latency.hpp`; the rule, the plan's
roles and the units are in `docs/engine.md#latency-per-bank-pair`. The block hands the plan and each
port's peer (in frames, as heard) to `workOut` at `carryLatency` and at each peer's report, and tells
its peers what comes back. Never combine latency by hand in the block, the harness or a type: change
the model, whose table the conformance test in `port_block_test.cpp` runs through a real block.

## Sound cards and the clock

A `card` node is a port block (its public ports, metered) plus one ALSA adapter node per
direction made by `AlsaSoundCards` (`sound_cards.cpp`), joined to the block's inside ports by
the card's own links. A card is one kind of engine companion: the state machine below is
`joinEngineCompanion` (`engine_companions.cpp`), shared with RTP receivers; the card's body says only
how its node is made and heard from (its own proxy) and what fails it:

1. **Opening.** The adapter is made with the table's `card` rows and no `node.group`
   (`docs/engine.md#properties-ear-sets` and `#properties-ear-never-sets` say why each). So it runs
   alone, on its own clock, and holds no port block while it opens. `alsaPath` is the one home of
   the device name; the card id's grammar (`CardId` in ear_core) keeps every other ALSA device form
   out, because ALSA's `file` plugin can run commands.
2. **Linking.** When the node reports RUNNING (its PCM runs), the card makes one link per
   channel through `link-factory` (`link.output.node`, `link.output.port`, `link.input.node`,
   `link.input.port`): the adapter and the block each named by its global id, ports by
   name: block port `to_card.<i>` to the adapter's `playback_AUX<i-1>`, or the adapter's
   `capture_AUX<j-1>` to `from_card.<j>`. By id, because a node of the same name that is still
   going (a restart's old node) would otherwise match first.
   Linked to its block, the card joins the blocks' graph, where the dummy outranks it: it
   becomes a follower (one repeated sample at the switch on the test card) and every block's
   and the card's `node.driver-id` is the dummy's. As a follower the ALSA node runs a
   delay-locked loop on its buffer against the graph clock and steers the adapter's resampler,
   so every card is resampled all the time (no output is bit-perfect) and its rate takes a few
   seconds to settle.
3. **Ready** once every link is active. Its health (PCM running, every link active) is the
   node's insides readiness, so the node is `running` only then.
4. **Lost.** Any failure (refused at open, the node going to ERROR, a rate the kernel refuses,
   the card unplugged, a link that fails or goes) first destroys the links and the adapter, then
   waits for a `pw_core_sync` round trip, and only then tells `lost(detail)`. A lost card never
   held the clock, so the blocks keep the dummy and their position without a discontinuity. The node becomes
   `failed engine-refused: <detail>`; its block and routes stay. No retry: a `node.update`
   with changed settings restarts it, and one with the same settings changes nothing
   (`graph_file_model.cpp`, `desired_graph.cpp`), so with none to change, restart ear.

CI has no sound card. `src/pipewire/tests/alsa/alsa.conf`, set as `ALSA_CONFIG_PATH` in the
test process, defines `hw` so `hw:CARD=Test` is alsa-lib's `file` PCM over its `null` PCM
(what the card is sent lands in `$EAR_ALSA_OUT/card.raw`); any other card id cannot open. It has
no oscillator, so as a follower its rate match never settles (it always reports a delay of 0 and
the correction climbs: the mean step fell 6.6% in 20 s, with breaks). Tests therefore make it with
`AlsaSoundCards(engine, clocklessCardProperties())`, a test seam of extra adapter properties that
sets its `clock.name` to the dummy's: an ALSA follower whose clock name equals the driver's turns
matching off (`alsa-pcm.c`, `setup_matching`), so it is bit-exact. `main.cpp` never passes it: a
real card keeps matching on. CI checks ready, following, the channel mapping sample for sample,
the card's latency carried through the pass-through, a card that cannot open and a lost card
leaving the clock alone. The system test "A sound card node runs and its route is linked" runs the
test card as `main.cpp` makes it, with matching on, so it is a follower that never settles: it
proves the wiring (running, linked) and nothing about following. Drift tracking, follower resyncs, the ALSA-error path on a running PCM,
the rate a real card refuses and a real card's latency are checked only on the hardware (the
`ear-on-talos` skill).

## RTP receivers (module-rtp-source in-process)

An `rtp` node is a port block (public outputs `stream.1`..`stream.N`, inside inputs
`from_rtp.1`..`from_rtp.N`, passed straight through) plus one RTP receiver, engine node
`<node>.rtp`, made by `RtpReceivers` (`rtp_receivers.cpp`). Its module arguments are built in one
place, `moduleArguments`, from parsed values only (`RtpAddress`, an enum, ranged numbers, the
engine node name), each key from the table: the `RTP receiver` and `RTP stream` rows of
`docs/engine.md#properties-ear-sets` say what each is for (`remote.name = "internal"`,
`adapter.auto-port-config`, `node.always-process`, `sess.ignore-ssrc`, `source.ip`, ...). Beyond
the table:

- **`source.ip`'s grammar.** `RtpAddress` accepts only `0.0.0.0` and 224.0.0.0-239.255.255.255,
  because any other address makes PipeWire `connect()` to that sender. A host name would block the
  main loop in a name lookup (15 s for `fpp.local` in the spike) before the load fails.
- **`audio.rate`** is the sender's rate; another rate is resampled. The receiver steers its
  resampler with a delay-locked loop from packet arrival, so its rate wobbles by parts in ten
  thousand: a sine fit over a whole second fails while every tenth of a second fits
  (docs/engine.md#rtp-receivers). Check a received tone window by window.
- **Left at PipeWire's defaults:** `cleanup.sec` (60), `net.mtu` (1280), the packet times.

How the body finds its node: the module makes the node through its own connection, so there is
no proxy to watch. The body binds a registry on EAR's connection and sends a `pw_core_sync`.
Node globals named `<node>.rtp` seen before its done are **stale** (a restart's old receiver,
still being removed); after it the module is loaded, and the first node of that name that is not
stale is bound for its info. A stale id is forgotten when its global goes, because the engine may
reuse the id for the new node. RUNNING gives the global id the links name; ERROR, the node's
global removed or the module destroyed is `failed`. The restart test ("A receiver made again at
once for the same names") checks this race.

What PipeWire 1.4.2 does that ear cannot see or change:

- **A port that cannot be bound is only logged** (`bind() failed: Address already in use`). The
  stream's error is set, but the node is not put in ERROR, not removed, the module not destroyed.
  With always-process the node then never reaches RUNNING, so the `rtp` node stays `configuring`
  (state `starting`) with silent outputs, waiting for its companion: after 2 s ear says `node <n> still
  configuring after 2s: waiting for <n>.rtp`, and at the receiver's 5 s readiness deadline the node
  fails `engine-refused: the RTP receiver never ran (UDP port <port> held?)`
  (`docs/engine.md#readiness-deadlines`; `lifecycle_test.cpp` holds the port to check both). The module
  sets `SO_REUSEADDR`, so two receivers on one port both bind (unicast then reaches only one); a port
  held by a program without it fails.
- **No packets is not a failure.** The module never destroys itself. After `cleanup.sec` without
  packets it sets the node property `rtp.receiving = false` (true again on the next packet); a
  bound node proxy sees it, the registry does not. ear does not read it: the node stays
  `running` and silent (the owner's choice, `docs/node-types.md#rtp`).
- **Datagram size.** The receiver reads into a buffer of `net.mtu` bytes (1280); a longer
  datagram is cut short and its samples garbled. 1 ms packets at 48 kHz fit up to 8 channels of
  L24 or 13 of L16. A sender must not use a larger `net.mtu`.
- **Any sequence mismatch resyncs.** A packet lost or out of order (`unexpected seq (1898 !=
  1897)` at info level) drops the sync: the ring is zeroed and refilled to the target latency,
  an audible gap of about `latencyMs` (`module-rtp/audio.c:131-166`). So does a ring overrun.
  Even one sending thread over loopback reordered packets under load.
- **Latency rounding.** `sess.latency.msec` is rounded down to whole packet times (100 became
  96.9 ms for 2-channel L16). The ring holds 4 MiB, so 64 channels of L24 hold about 455 ms; a
  `latencyMs` near that overruns.

CI's tests use a sender thread on 127.0.0.1 (`test/support/rtp_tone.hpp`), never PipeWire's
`module-rtp-sink`, and never multicast.

## drive() and fadeOut(): swapping a block's code

`PortBlock::drive(request, done)` (`docs/engine.md#drive-and-fade`) returns at once:
the request goes into the block's ring of eight, one non-blocking `pw_loop_invoke` per block in
flight applies it on the block's own data loop between two cycles, and `done(P)` runs later on the
main loop, from the block's completion event (or `settle()`), never inside the call, with `P` the
position it takes effect from (`takenAt`: the next cycle's expected position; a stall's catch-up
skips whole blocks forward, so it can be late by a block there, never early; with the driver's
default `resync.ms` it was up to 10 ms plus a block across a reset). Once `done` ran, no `process()` of the
old code is running or will run, so the node framework can release the old insides. A request with
no code makes every output silent; the block keeps processing. A block with no data loop (refused,
never connected, or its local node destroyed: `filter.c` clears the pointer) applies it on the main
thread and still completes by signal. A request is applied whatever the block's state: a block
PAUSED (suspended) or whose driver went still runs the loop's invokes, so a drive or fade never waits
for a cycle that will not come (a test suspends a block). A block whose engine node was destroyed
through the registry keeps its data loop (E1 found this), so the invoke runs there as usual. There
is no blocking `drive(code)`; an engine test that needs a code running before it goes
on uses `ear::testing::driveNow` (`ear/testing/drive_now.hpp`).

- **Fades.** A drive names which outputs it fades (`DriveRequest::faded`, one flag per output in
  `process()`'s order: public outputs, then inside outputs, so a card's playback bank fades too); the
  invoke resets their gain to the fade-in's start, so the new code ramps in linearly over 5 ms (rate *
  5 / 1000 frames, exact at both ends) and any pending fade-out is cancelled (its `done` dropped).
  `fadeOut(done)` ramps the faded outputs to zero over 5 ms from the next cycle, the code still
  running, and completes once a cycle ended at zero (a cycle too large to run counts as one); a block
  that is not processing, or leaves STREAMING mid-fade, acknowledges it by signal. `NodeHost` fades
  before every planned silence (a stop, restart, disable, removal, failure, `stopAll`) and then
  drives no code. A sink's bank is never faded. A node with no output but sink banks (`file`) still
  posts a fade, acknowledged at the next cycle's end.
- **Bounds and the wedge** (docs/architecture.md#bounds-the-wedged-data-loop-and-the-watchdog; the timers are `NodeHost`'s). A fade not
  acknowledged in 100 ms is `ear.fade-unacknowledged` and the silence goes ahead unfaded. A drive or
  silence not completed in 5 s is `ear.barrier-overdue`, and then, once, `ear.data-loop-wedged`: the
  loop runs no invoke. Every bound first calls the block's `settle()` (it runs every `done` the data
  loop already completed), and is judged after a grace when the whole process stalled, so a stalled
  main loop never blames the data loop. Once wedged, `NodeHost` makes no PipeWire call and leaks what
  it would end, and `main.cpp` marks the engine (`markDataLoopWedged`): `Links` makes no link (each
  refused "the data loop is wedged") and leaks the proxies it would destroy, an engine companion leaks
  its links and body, and a block tells no latency, since each of those reaches the data loop through
  a blocking invoke (`impl-port.c`'s mix release, `port_set_io`) that would hang the main loop, SIGTERM
  included. A new node fails `engine-refused: the data loop is wedged` without a block, and ear ends
  through `std::_Exit(0)` after writing `ear.conf` and every record.
- **The interim sinks' taps** (docs/engine.md#sinks). A drive carries `SinkBinding`s; the
  invoke copies them into storage sized when the block was made (at most one tap per inside output
  bank; nothing allocates on the data loop) and unbinds them in the invoke that applies the next
  request. Each cycle the code runs, after the code and the fade, the block writes each bound bank's
  frames into its sink's frame ring with a stamp (position, both clocks, the cycle's `causes`, `drive`
  on the first cycle after any drive, and a catch-up's `lostFrames`) in the stamp ring beside it, and wakes the sink's thread
  (`pw_loop_signal_event`). So a sink takes exactly the cycles its code drove: a stop's silence ends
  them, a start begins them again at its first cycle. The two rings drop together; a full one counts
  dropped frames, which reach the node's `removed dropped=<n>`. A sink's first chunk after a block's
  first drive carries the clock's real position, which may differ from that drive's `takenAt` when no
  cycle ran before it (`takenAt` is 0 until one did). In 3b the realtime side moves behind a ring
  companion's own `pw_filter`.
- **One invoke in flight per block is a requirement** (S1 below): an unbounded producer of
  invokes wedges the data loop. Never post invokes to the data loop in a loop of your own.

## What the spikes proved (S1 to S3 in slice 3a, S5 in 3b; `spikes_test.cpp`)

Recorded in `docs/engine.md#the-one-data-loop-and-invokes` with PipeWire's sources (`loop.c`, `context.c`,
`filter.c`):

- **S1: asynchronous `pw_loop_invoke` from the main loop** runs on the data thread in posting
  order, never inside any `process`, and the post returns the async code, never the function's
  result. A full invoke queue (32 KiB, about 455 items) is never refused: the *posting* thread
  `calloc`s an overflow queue and goes on, and posting order holds across queues; the data thread
  allocates nothing. A loop whose thread is not running (not started, or stopped) runs the function
  inline on the caller and returns its result, which is why completions are posted, never run inline.
  A producer posting as fast as it can outruns the flush: the data loop spins in
  `flush_all_queues`, never runs `process` again, and memory grows.
- **S2: take-up by position, under an invoke storm** (`RUN_SERIAL`, tag `[s2]`, a 64-frame block).
  Every request is taken up once, in order, one cycle after publication, and nodes enter in link
  order in every cycle they all run. But a cycle an xrun cuts short is run by an upstream prefix of
  the graph only (U, or U and B), never by a downstream node without its upstream. On a loaded
  host `clock.position` stepped back too, the whole graph running positions again, until
  `engine.conf` set `resync.ms` (section "The clock"); S2 now checks it never does. Cycles are
  still told apart by the order they ran in, never by position. For 3a this changes nothing
  (the sink is inside the block); 3b's source companions must count a chunk as played only once
  their block took it up, or keep their realtime side inside the block (`docs/design/companions.md`).
- **S3: one data loop.** `context.num-data-loops` reads 1, the process has one `data-loop.0`
  thread, and a filter's `pw_filter_get_data_loop` is that loop. Adapters and the rtp module name
  `data-loop.0` in their info, while `pw_filter`s name no loop (`node.loop.name` is set only by a
  node's maker).
- **S5 (slice 3b.0, tag `[s2]`, so serial)**: a source served by its block, on S2's harness
  beside a busy thread per core, about 5 s. It cuts its own cycles short, 200 after U and 200
  after B, by planned overruns of 1.5 cycles inside the probe's `process` (`StallPlan` and
  `Stalls`, `docs/testing.md#making-the-rare-event`; never wait for a host to cut cycles short: a
  GitHub runner cut 9 in 60 s under the same load). Its played count keeps
  to the clock across cut-short cycles (a cycle that states a catch-up skips its whole jump and
  counts it, however long: one beyond the ring's capacity, which only a host's stall past 42 ms
  makes, owes the rest from what is written next; each jump must be time the graph really lost,
  never an engine miscount), and after
  a cycle its upstream ran and it did not, a node reads the upstream's frames of the cycle it runs,
  never the cut one's (`docs/engine.md#the-one-data-loop-and-invokes`).
- **S6 (cycle facts, tag `[s6]`, its first case also `[s2]`)**: the driver reports every cycle a node
  misses (`incomplete`, before the next `start`), across planned stalls of 1.5 to 150 cycles in each
  node of a chain and a sibling beside a busy thread per core; the `TRIGGERED` recovery loses nothing;
  `clock.cycle` is the listener's count plus a fixed offset; a running node's position jumps by whole
  missed cycles only. Non-whole jumps come from the default `resync.ms` or the driver restarting (it
  stops only when it drives no node; an inactive node misses uncut cycles instead). A follower can
  read no mark of the recovery (`XRUN_RECOVER` is cleared before followers run). A listener must not
  read the driver's clock through a filter's mapping once that filter is gone (it crashed).
- After a 2 s data-loop stall the dummy driver resyncs (an error of about -96,000 frames) and then
  resets about 70 times within 7 ms; the bounds treat it as no wedge while the loop answers invokes.

## Levels

Each block meters its public ports (never inside ports) every cycle on the realtime thread:
sample peak and RMS, linear (`LevelMeter`, `src/core/levels.cpp`). A window is
`levelWindowFrames(rate)` = ceil(rate / 30) frames (1600 at 48 kHz), a stated display rate.
A block publishes in the cycle that crosses a multiple of the window on the graph clock
(`position->clock.position`), so every block's windows end at the same positions whenever the
block was made. Publishing writes the block's triple-buffer mailbox (no lock, no allocation)
and rings the level bell (an eventfd) at most once per window: `rungFor` holds the window last
rung for and only the realtime side touches it, so a bell answered without reading the levels
never silences later windows. `latestLevels()` (any ordinary thread) returns one window for
every block that has one: blocks publish one after another within a cycle, so it takes the
oldest latest window, using the previous window each publish carries.

## Realtime priority (CAP_SYS_NICE)

`libpipewire-module-rt` raises the data thread to SCHED_FIFO only if the process may. The run
image gives `/opt/ear/ear` CAP_SYS_NICE as a permitted-only file capability (`setcap
cap_sys_nice=p`); a binary marked effective (`=ep`) would fail to start with EPERM wherever the
capability is not allowed. `Engine::create` makes it effective (raw `capget`/`capset`, no
libcap) before `pw_init` and before any engine thread exists, so module-rt can use it. Not
permitted: nothing changes, nothing fails. A capability added by `docker run --cap-add` or a pod
spec alone never reaches a non-root process (execve clears it), and `no_new_privs` (Kubernetes'
`allowPrivilegeEscalation: false`) ignores file capabilities.

`Engine::dataThreadScheduling()` asks the data thread itself (an invoke on the data loop runs
`sched_getscheduler(0)`): `realtime` true for SCHED_FIFO or SCHED_RR, with the policy and
priority as detail. `ear` prints `engine: realtime SCHED_FIFO <n>` or `engine: not realtime
(SCHED_OTHER)` at boot, and `graph.get` carries `realtime` and `scheduling`. In the dev
container it is not realtime; the image smoke checks both cases.

## No session manager

Nothing like WirePlumber runs. Two things make that work:
- Every port block sets `node.always-process = true`. Up to PipeWire 1.6 the context itself
  assigns an unlinked always-processing node to the first active driver, so an unrouted block
  runs (design rule 3: the graph always runs). The design page records that from 1.7.0 this
  moved into `module-scheduler-v1`, which `engine.conf` would then have to load.
- Links are made by EAR (the reconciler, through `Links`), never by a policy daemon.

## Traps

- **A dot in a public port's name is safe.** Ports are `<bank>.<member>` since 2026-11-06. In PipeWire 1.4.2 `pw_impl_port_add`
  (`impl-port.c`) only copies `port.name` and joins it into `port.alias` with a `:`; it never parses the name, and a route's link goes
  by port ids. `port_block_test.cpp` carries a ramp and levels over dotted names. Read again at a PipeWire bump. Only the engine companions'
  links go by name, and those name inside ports and `playback_AUX<i>`, with no dot.
- **Stop signals kill the process unless blocked first.** `pw_context_new` starts the data-loop
  thread. A SIGTERM sent to the process goes to any thread that does not block it, where its
  default action ends EAR. `Engine::create` blocks SIGINT and SIGTERM before `pw_init`, so every
  thread inherits the mask, and `onStopSignal` reads them through `pw_loop_add_signal` (a
  signalfd, which also reports a signal that arrived earlier).
- **Block size silently rounded.** Without `clock.power-of-two-quantum = false` a block size of
  480 runs as 256 (its row in `docs/engine.md#properties-ear-sets`).
- **`node.name` is not the filter's name.** `pw_filter_new`'s name becomes `media.name`;
  `node.name` defaults to the program's name. `port_blocks.cpp` sets `node.name` explicitly,
  because the live graph names nodes by it.
- **Custom properties never reach the registry.** Registry globals carry only PipeWire's
  whitelisted keys, so a mark key set on a node is invisible to `LiveGraph`. EAR recognises its
  nodes by `node.name`, which is safe only while no other client can connect (no socket).
- **No buffer this cycle.** `pw_filter_get_dsp_buffer` returns null when a port has no buffer:
  an input then reads zeros, an output writes to scratch (`port_blocks.cpp`).
- **Destroying a filter is a barrier.** `pw_filter_destroy` removes the node from the data loop
  with a blocking invoke, so `process` is never called after it returns. The node framework no
  longer relies on it (a node is silenced by an asynchronous drive before its block is
  destroyed), but a block destroyed with a code still driven is safe. Because it blocks, it is
  never called once the data loop is wedged (the block is leaked instead), and `NodeHost` batches
  every destroy and make of a turn into one flush posted to the main-loop executor, destroys first
  (docs/architecture.md#isolation), so the main loop waits on the data loop at most once a turn.
- **A name added back waits for its older block.** Two blocks of one `node.name` would confuse
  `LiveGraph` and link-factory, so a node's block is made only in the flush that destroyed the older
  block of its name, and an engine companion links its block by the block's global id
  (`pw_filter_get_node_id`, read at its first STREAMING), never by name.
- **Link lifetime.** A link made through `link-factory` belongs to the connection that asked for
  it (no `object.linger`), so destroying its proxy removes it. The factory destroys a link that
  goes to ERROR. A proxy removed before ACTIVE is a failure; after ACTIVE it is gone. Never
  destroy a proxy inside its own event: `links.cpp` silences it and destroys it later from a
  main-loop event.
- **Existing objects arrive later.** A new registry binding replays every existing global as
  events after the call that made it returns, never inside it. `LiveGraph` gives each
  subscriber its own binding and caches nothing; a port can arrive before or after its node.
- **Never tell a listener inside the call that caused it.** Refusals in `FilterPortBlocks::make`,
  `Links::createLink` and `joinEngineCompanion` (every engine companion's `make`) are told later through `pw_loop_signal_event`
  on the main loop, as the core interfaces require.
- **The adapter's `adapter.auto-port-config` fails on 1.4.2.** It left one unconfigured port
  whose PCM failed to start (EINVAL). The card sends the port configuration itself, as a
  session manager would: `pw_node_set_param(SPA_PARAM_PortConfig)` in dsp mode with 32-bit float
  planar and AUX positions, which names the ports `playback_AUX<n>` and `capture_AUX<n>`. On
  an RTP receiver's stream the same key works and is what gives it ports.
- **`cpu.zero.denormals` sets the wrong thread.** See "Properties: one table": the engine flushes
  denormals with an invoke on the data loop.
- **Custom latency is never told a lost last link.** See "Latency, per bank pair".
- **`node.driver-id` changes reach no client on 1.4.2.** The engine sets a node's
  `node.driver-id` when its driver changes but sends no info event for it. A test that reads it
  binds the node again (`sound_card_test.cpp`).
- **link-factory finds nodes by more than `node.name`.** By name it also matches nick,
  description and object path (`module-link-factory.c`). Engine-companion links therefore name the
  engine companion by global id; the block side is still a name, so a node a user names like another
  engine node (only `Dummy-Driver` today) gets engine-companion links that fail, and the engine companion
  is lost with the factory's error.
- **Main-loop work from another thread.** `MainLoopExecutor::post` queues the function and
  signals a main-loop event; posted from the loop thread itself it still runs later, never
  inline. Work left when the executor goes is destroyed unrun. `pw_loop_invoke` would do the
  same crossing, but the queue owns each function (no leak at stop) and never reports full. Each
  item runs through the callback boundary on its own, so one that throws drops none behind it.
- **Every callback goes through the boundary.** A new event source, timer, signal, invoke,
  listener, log method or handler in `src/pipewire/` is registered with `addEvent`, `addTimer`,
  `addSignal`, `invoke`, `listen`, `installLog` or `setAlsaErrorHandler` from
  `callback_boundary.hpp`: a member function wrapped as `guarded<&C::m>` (caught and reported to the
  engine's `Faults`, today ending the process with a line naming the callback) on any loop but the
  data loop; `realtime<&C::m>` (the member `noexcept`, nothing added) on the data loop, invoked
  through a `DataLoop`, which takes nothing else; a `noexcept` C function as `realtimeFunction<&f>`
  for the log and alsa-lib. A raw function pointer, or the wrong kind for the loop, does not
  compile, and `ear_callbacks_through_the_boundary` fails on any raw `pw_loop_add_*`,
  `pw_loop_invoke`, `*_add_listener`, `pw_log_set`, interface table and the rest it lists outside
  the boundary's two files and tests. One engine holds the boundary at a time: a second
  `Engine::create` fails while another lives. A listener table PipeWire has that the boundary lacks (say `pw_stream_events`)
  is added there as one more `*Callbacks` table and `listen` overload
  (`docs/architecture.md#the-callback-boundary`).
- **Threads started later inherit the signal mask.** The control port's thread is started after
  `Engine::create`, so it too blocks SIGINT and SIGTERM.
- **Waking from the realtime thread.** The sink's tap and the handoff writer wake their thread with
  `pw_loop_signal_event` (an eventfd write). The S4 test checks it allocates nothing; the
  design page lists its realtime safety as otherwise unverified.
- **Xruns on a loaded host are not bugs.** When the host is busy (other containers in Docker's
  VM, a load average far above the CPU count) PipeWire's data thread can stall for tens to
  hundreds of milliseconds without a wake; each stall is a graph xrun. A ramp test then sees a
  break, and an RTP test a gap (an underrun past `latencyMs`, or a resync after reordered
  packets). To tell it from a bug, rerun with `PIPEWIRE_DEBUG="2,pw.node:4"`: an xrun shows the
  dummy driver's missed cycles back to back while the late nodes' own process time is
  microseconds, so nothing ear runs is slow. Run the same test on the unchanged code in the same
  conditions; if that fails too, the host cannot judge it and CI's runner is the gate
  (`docs/engine.md#xruns-on-a-loaded-host-are-not-bugs`). A stall never resets the internal clock (`resync.ms`,
  above): a `spa.driver ... err ... > max_resync ..., resetting` warning in a test's output means
  `engine.conf`'s `resync.ms` was lost, and the graph is then slewed off the raw clock for tens of
  seconds after it.
- **Tests and the environment.** `engine_test.cpp` unsets `XDG_RUNTIME_DIR`,
  `PIPEWIRE_RUNTIME_DIR`, `PIPEWIRE_REMOTE` and the D-Bus addresses, so it proves the engine
  needs none of them. Its `Waiter` runs the main loop until the awaited events have arrived; a
  timer only fails the test. Copy that pattern for new engine tests. A test that needs a stall or a
  cut-short cycle makes it (`StallingCode`, `Stalls`), excuses a lost cycle only through the one judge
  (`CycleJudge`, `docs/testing.md#the-cycle-judge`), and reads a retold latency after
  `Quiescence::quiesce`; a new sleep or `WARN` must be
  listed in `test/sleeps.txt` (`docs/testing.md#the-tools`).

## Bumping PipeWire

The version is whatever Debian trixie's archive holds when the image is built: the Dockerfile
pins the base image by digest, not the packages. To move to another PipeWire:

1. Change the source in `docker/Dockerfile` (a new base digest or Debian release, or a backports
   line), rebuild through `scripts/dev.sh`, and read the version with
   `scripts/dev.sh dpkg-query -W libpipewire-0.3-0t64` (the library package's name may change
   with the release).
2. Read PipeWire's release notes between the two versions for: scheduling of unlinked
   always-process nodes and of node groups (from 1.7.0, load `libpipewire-module-scheduler-v1`
   in `engine.conf`, and re-check how a card joins the group's clock), renamed clock keys,
   whether `context.properties` still overwrite `pw_context_new`'s properties, `pw_filter` and
   `link-factory` behaviour (link by name and by id), the adapter's port configuration (whether
   `adapter.auto-port-config` now works), `node.driver-id` events, the registry's key
   whitelist, and module-rtp-source: its arguments, whether a failed bind is now reported, the
   SSRC lock, the resync on a sequence mismatch, and the `rtp.receiving` property. And
   `node-driver.c`'s tracking mode: whether `resync.ms` is still read, and whether a reset now
   re-arms `next_time` from now and calls `spa_dll_init` (then the default could come back, and
   long pauses would stop slewing the graph; the two stall tests say either way).
3. Check each module named in `engine.conf`, and `libpipewire-module-rtp-source`, still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover the engine facts (`[rtp]` the
   receiver) and `ear_system_tests` the whole path.
5. Update the tested-against line in `engine.conf`'s header and the version named in this
   skill's first section and in `AGENTS.md` and `docs/engine.md`.
