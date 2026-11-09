# The engine: PipeWire inside ear

ear does not connect to a PipeWire daemon: it **is** the PipeWire instance. `Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in ear's own process and
connects to that context itself (`pw_context_connect_self`); every adapter in `src/pipewire/` works
through that one connection. There is no daemon, no socket, no runtime directory, no D-Bus and no
session manager. *Why:* the owner wanted PipeWire to "handle low-level audio concerns so that `ear`
can focus on the graph and UX", inside one container.

Every claim here was checked against **PipeWire 1.4.2** (Debian 13 "trixie", the build image) by
the tests in `src/pipewire/tests/`. Source citations are to PipeWire's tree at that version unless
marked otherwise; read them again at a PipeWire bump ([below](#bumping-pipewire)). The
`pipewire-engine` skill (`.claude/skills/pipewire-engine/SKILL.md`) is the working guide for
changing this code.

## What the engine is made of

| Piece | Where | What |
|---|---|---|
| Main loop, context, own connection | `Engine::create` | One process, one loop; every core object and adapter runs on the main loop's thread |
| `engine.conf` | `src/pipewire/engine.conf`, copied beside `out/ear` | Read by `pw_context_new`; loads the modules and the internal clock |
| Clock settings | `contextProperties()` in `engine.cpp` | The graph frequency and block size, fixed at boot |
| Port blocks | `port_blocks.cpp` | One `pw_filter` per node; `drive`, fades, levels, `GraphCycle`, latency |
| Level mailbox | `level_mailbox.hpp` | One triple buffer per block |
| Live ports | `live_graph.cpp` | Registry globals joined into node name, port name, direction, port id |
| Links | `links.cpp`, `link_proxy.hpp` | `link-factory` links by port global id |
| Engine companions | `engine_companions.cpp` (`joinEngineCompanion`) | The one lifecycle of every engine node a node's configuration asks for |
| Sound cards | `sound_cards.cpp` | One ALSA adapter node per card and direction |
| RTP receivers | `rtp_receivers.cpp` | `libpipewire-module-rtp-source`, one per `rtp` node |
| Sinks and handoffs | `handoffs.cpp` | `ThreadSinks` (interim sinks) and `ThreadHandoffs` (the legacy handoff `sendspin` uses) |
| The engine's log | `engine_log.cpp` | ear's own `pw_log` and alsa-lib's error handler ([output-records.md](output-records.md#how-output-is-written)), both `noexcept` and registered through the callback boundary as realtime functions |
| The wedge mark | `data_loop_wedge.hpp` | Once the data loop is judged wedged, the adapters stop calling into it |
| Main-loop executor | `main_loop_executor.cpp` | Work posted from any thread runs on the main loop |
| The callback boundary | `callback_boundary.hpp`, `faults.hpp` | Every C function ear gives PipeWire or alsa-lib, with no exception: `guarded<>` (caught, reported to the engine's `Faults`), `realtime<>` on the data loop (a `DataLoop` takes nothing else), `realtimeFunction<>` for the log and alsa-lib's handler ([architecture.md](architecture.md#the-callback-boundary)) |
| Shared names | `engine_names.hpp` | The graph group's name, a card's driver priority, the internal clock's name |

`main.cpp` passes `<directory of /proc/self/exe>/engine.conf`; the tests pass the source tree's
file. The test `ear_engine_conf_beside_ear` fails if the copy beside the binary differs.

## `engine.conf`, key by key

`context.properties`:

- `core.daemon = false`: no socket and no runtime directory; ear's own connection is the only
  client.
- `core.name = ear`.
- `support.dbus = false`: there is no D-Bus in the container (`context.c:315`).
- `context.num-data-loops = 1`: one data loop runs every engine node's `process`. It is 1.4.2's
  default (`context.c`, `DEFAULT_DATA_LOOPS`), pinned because the framework relies on it: a block
  and the engine nodes beside it see one `clock.position` per cycle, and a non-blocking invoke on
  that loop runs between any two of their cycles.
- **Never** `default.clock.*` or `clock.power-of-two-quantum` here: `context.properties` from the
  file overwrite the properties `Engine` passes to `pw_context_new`, which carry the clock
  (`context.c:245`, `conf.c:986-990`).
- **Never** `cpu.zero.denormals` ([denormals](#denormals)).

`context.spa-libs`: `support.*` (the loop, logging, the dummy driver), `api.alsa.*` (a card's ALSA
PCM node), `audio.convert.*` (the converter the adapter wraps around it). Debian's
`libspa-0.2-modules` holds all three.

`context.modules`, each for one reason:

| Module | Why |
|---|---|
| `libpipewire-module-rt` (`ifexists nofail`) | Realtime priority for the data thread where allowed; a container without the privilege still starts |
| `libpipewire-module-protocol-native` | The protocol ear's own connection speaks, even with no socket |
| `libpipewire-module-client-node` | A `pw_filter` exports its node through it (`filter.c:1682-1713`); without it no port block appears |
| `libpipewire-module-spa-node-factory` | Makes the internal clock |
| `libpipewire-module-link-factory` | Makes route links (by port global id) and engine companions' links |
| `libpipewire-module-adapter` | Makes a sound card: an ALSA PCM node wrapped in a converter |

`libpipewire-module-rtp-source` is not listed: `RtpReceivers` loads one per `rtp` node with
`pw_context_load_module`. Debian's `libpipewire-0.3-modules` holds it.

`context.objects`: one `spa-node-factory` object, the internal clock: `factory.name =
support.node.driver`, `node.name = Dummy-Driver`, `priority.driver = 200000`, `node.group =
ear.graph`, `clock.id = monotonic-raw`, `resync.ms = 1000000000`. It has no ports, so it never shows
as a live port.

## The clock

`Engine` sets, as context properties: `default.clock.rate` and `default.clock.allowed-rates` (only
the graph frequency); `default.clock.quantum`, `min-quantum` and `max-quantum` (all the block size);
`clock.power-of-two-quantum = false`; and sets `default.clock.quantum-floor` (default 4) and
`default.clock.quantum-limit` (default 8192) to the block size itself
([the quantum guard](#the-quantum-guard)). A block sees `clock.duration == block size` and
`clock.rate.denom == graph frequency`. Both are fixed for the engine's life: `main.cpp` reads
`ear.conf` once before `Engine::create` and takes both values when both lines read, else the flags
([graph-file.md](graph-file.md#settings-applied-only-at-restart)).

### The quantum guard

The graph has one quantum and one rate, the driver's, and every block is made for at most the block
size (`maxBlock`): a cycle longer than that the block cannot run, so it writes zeros. A node that
carries `node.force-quantum` (or `node.force-rate`, `node.lock-quantum`, `node.lock-rate`) asks the
driver for its own, and PipeWire clamps a forced quantum to `[quantum-floor, quantum-limit]`
(`context.c`), not to `min-quantum`/`max-quantum`. So two guards keep every cycle at the block size:

- **`quantum-floor` and `quantum-limit` are the block size**, so even a forced quantum is clamped
  back to it (`engine_test.cpp` forces four blocks, and a quarter block, on a node in the group: with
  the old limit of 8192 every block stopped running at four blocks; with both at the block size every
  cycle stays one block).
- **No engine companion carries a clock property.** ear builds every property it gives a card or a
  receiver from closed grammars and sets none of these; `AlsaSoundCards`' test seam (its `extra`
  properties) refuses each with `std::invalid_argument` rather than stripping it after the fact, and
  refuses as well what would take driving from the internal clock: `priority.driver` (ear sets it,
  1), `node.driver`, `node.group` and `node.link-group`.

**A card's ALSA buffer does not follow the limit.** `module-adapter` hands every node it makes the
graph's limit as `clock.quantum-limit`, and an ALSA node with no period count caps its buffer at
`min(hardware maximum, 4 x clock.quantum-limit)` (`alsa-pcm.c`, `spa_alsa_set_format`): at a
256-frame block that is 1024 frames (about 21 ms, with `max_delay` half of it), where the old limit
gave 32768. So `AlsaSoundCards` makes every card with `api.alsa.period-size` = the block size and
`api.alsa.period-num` = `cardPeriods(block)`, which takes ALSA's period-count path instead. ALSA
halves a batch card's period (a USB card such as the MOTU: block / 2, as before), so a batch card's
buffer is again `min(hardware maximum, 4 x max(block, 8192))` frames, its period and headroom (and
so its latency) unchanged; a card that is not batch gets periods of one block and twice that buffer,
capped by the hardware (the test card: 65536 frames, 256 periods of 256, where it had 32768). ALSA's
info log shows it (`PIPEWIRE_DEBUG="2,spa.alsa:4"`: `buffer frames ..., period frames ...,
periods ...`); `sound_card_test.cpp` pins the properties.

Cards (the test card) and RTP receivers run at that limit, a receiver whose sender is at 44.1 kHz
or 96 kHz into a 48 kHz graph included (`sound_card_test.cpp`, `rtp_receiver_test.cpp`). A real
card at that limit is checked only on the hardware (the `ear-on-talos` skill).

### The internal clock

**The graph is always driven by ear's internal clock, the dummy driver, whatever links or fails.**
Every port block sets `node.group = ear.graph`, and so does the dummy. PipeWire puts every node of
one group on the same driver and walks drivers in `priority.driver` order: the dummy (200000) comes
first, so it collects the group and everything linked to it, and a linked card (priority 1,
`cardPriority`) is a follower. So no card ever moves the clock: a card's failure costs only that
card's audio.

**It tracks `CLOCK_MONOTONIC_RAW`.** `CLOCK_MONOTONIC` is slewed by NTP (chrony may slew it by up to
83,333 ppm), which every card's follower loop, every RTP receiver and `sendspin` would have to
chase; and a timerfd cannot be armed on `CLOCK_MONOTONIC_RAW` (`EINVAL`). With `clock.id =
monotonic-raw` the driver runs in its tracking mode (`node-driver.c`): its timer stays on
`CLOCK_MONOTONIC`, a delay-locked loop steers it so `clock.position` follows raw time, and
`rate_diff` is the loop's correction, its error clamped at 1 ms a cycle. `clock.nsec` and
`next_nsec` stay `CLOCK_MONOTONIC`. Its `clock.name` is `clock.system.monotonic-raw`. Whether it
follows a host slewed at chrony's worst case is untested; the fallback is chrony's `maxslewrate`.

**`resync.ms` is set beyond any stall, so the driver never resets.** With the default (10 ms), past
it after a data-thread stall, `on_timeout` puts the position on raw time but leaves its timer where
it was. The timeouts it then fires back to back (the timer catching up) each add a block while raw
time barely moves, so the next reset steps the position **back** by up to about 440 frames, and the
burst leaves it up to 10 ms ahead of raw time; the loop, at its least bandwidth, then slews the whole
graph by hundreds of ppm for tens of seconds (measured: +190 frames and -230 ppm 9 s after one 20 ms
stall). Without the reset the driver catches up on its own timer: the blocks run about every other
timeout of the burst, each cycle they run skips one block, the position never steps back, and the
phase to the raw clock is kept within a frame after stalls of up to a few seconds.

**The limit.** The catch-up burst after any pause is inherent to `node-driver.c` whatever
`resync.ms` is (about 187 timeouts per second of pause at 48000/256), and each feeds the loop its
clamped 1 ms error, so a pause of tens of seconds or more winds it up and slews the graph by 0.1 to
0.6% for minutes (simulated: 30 s, +287/-429 ppm for about 60 s; 120 s, 1700 ppm at peak). A 3 s
pause stays within 150 ppm from 5 s on. The default `resync.ms` is somewhat better only for pauses
of minutes; a large finite value is worse than both (it resets mid-burst and steps back again and
again). The real fix is upstream: a reset that re-arms `next_time` from now and calls
`spa_dll_init`. A `spa.driver ... err ... > max_resync ..., resetting` warning means `resync.ms` was
lost.

### `GraphCycle`

The block makes one `GraphCycle` a cycle (`port_blocks.cpp`, `onProcess`), so no type reads a clock:

| Member | From |
|---|---|
| `startNs` | `clock.nsec` (`CLOCK_MONOTONIC`) |
| `startRawNs` | The same start on `CLOCK_MONOTONIC_RAW`: both clocks read (mono, raw, mono, the offset taken from the midpoint, as `node-driver.c` does) and the difference added to `clock.nsec`, so a late wake cancels out |
| `position` | The driver's frames at the cycle's start |
| `rateDiff` | `clock.rate_diff` |
| `driverId` | `clock.id`, the driver's global id: another driver is another clock |
| `nextNs` | `clock.next_nsec` |
| `xruns` | Cycles that started ahead of where the last ended on the same driver. PipeWire gives a filter no xrun count (`clock.xrun` is set only by a driving ALSA node), so the block counts these; each cycle run during a stall's catch-up skips one block |
| `discontinuity` | The block's first cycle, a new code's first cycle, a new driver, any position jump, and the cycle after one too large for the block |
| `downstream` | The latency downstream of each output port ([below](#latency-per-bank-pair)) |

The dummy's `clock.delay` is always 0, followers or not: a following ALSA node writes its delay into
its own clock, never into the driver's position.

### Denormals

The engine flushes denormals to zero on the data thread (FTZ and DAZ; FZ on arm64) with a blocking
invoke on the data loop (`flushDenormals`). *Why not `cpu.zero.denormals`:* on 1.4.2 it lands on the
main thread in this engine (the data loop's setup runs inline there before the data thread enters)
and leaks to every thread made later. The invoke races the data thread too: `pw_data_loop_start`
returns before the thread enters its loop, and until it does `loop_invoke` runs the function inline
on the caller (about 4% of boots), so `Engine::create` invokes again until it ran on the data thread
(1 ms apart, failing after 5 s). Any blocking invoke made right after `pw_context_new` meets the same
race.

## Port blocks

Each node has one port block, a `pw_filter` named by the node's name (`node.name` set explicitly:
`pw_filter_new`'s name becomes `media.name`), with one 32-bit float mono port per declared port,
public and inside banks alike, every one in the graph group, with `node.always-process = true`.

- **The graph always runs.** An unlinked always-processing node is assigned to the first active
  driver by the context itself (up to PipeWire 1.6; `context.c` 1.0.5:1222,1236-1311). From 1.7.0
  that moved into `module-scheduler-v1`, which `engine.conf` would then have to load.
- **No buffer this cycle.** `pw_filter_get_dsp_buffer` returns null when a port has no buffer
  (`filter.c:2038-2045`): an input then reads zeros, an output writes to scratch.
- **A name added back waits for its older block.** Two blocks of one `node.name` would confuse
  `LiveGraph` and `link-factory`, so a node's block is made only once the older block of its name was
  destroyed, and an engine companion links its block by the block's global id (`pw_filter_get_node_id`,
  read at its first STREAMING), never by name.
- **Destroying a filter is a barrier**: `pw_filter_destroy` removes the node from the data loop with
  a blocking invoke. It is never called once the data loop is wedged (the block is leaked), and
  `NodeHost` batches every destroy and make of a turn into one flush, destroys first, so the main
  loop waits on the data loop at most once a turn.

### Drive and fade

`PortBlock::drive(request, done)` swaps a block's code asynchronously: the request goes into the
block's ring of eight, one non-blocking `pw_loop_invoke` per block in flight applies it on the data
loop between two cycles, and `done(position)` runs later on the main loop, never inside the call,
with the position it takes effect from. Once `done` ran, no `process()` of the old code is running or
will run, so the framework can release the old insides. A request with no code makes every output
silent; the block keeps processing. A block with no data loop applies it on the main thread and
still completes by signal; a block PAUSED or whose driver went still still runs the loop's invokes,
so a drive or fade never waits for a cycle that will not come.

- **Fades.** A drive names which outputs it fades (public outputs, then inside outputs, so a card's
  playback bank fades too); the new code ramps in linearly over 5 ms (`rate * 5 / 1000` frames,
  exact at both ends). `fadeOut(done)` ramps the faded outputs to zero over 5 ms from the next cycle,
  the code still running, and completes once a cycle ended at zero. A sink's bank is never faded.
- **Bounds and the wedge.** A fade not acknowledged in 100 ms is `ear.fade-unacknowledged`; a drive
  or silence not completed in 5 s is `ear.barrier-overdue` and then, once, `ear.data-loop-wedged`.
  Each bound first runs every `done` the data loop already completed, and is judged after a grace
  when the whole process stalled, so a stalled main loop never blames the data loop. Once wedged,
  `Links` makes no link (each refused "the data loop is wedged") and leaks the proxies it would
  destroy, an engine companion leaks its links and body, and a block tells its peers no latency (it
  still publishes it to its own realtime side, only atomics), since each of those reaches the data
  loop through a blocking invoke (`impl-port.c`'s mix release, `port_set_io`) that would hang the
  main loop, SIGTERM included.
- **One invoke in flight per block is a requirement.** An unbounded producer of invokes wedges the
  data loop ([below](#the-one-data-loop-and-invokes)).

### Sinks

A drive carries the sink bindings of its configuration; the invoke copies them into storage sized
when the block was made (nothing allocates on the data loop) and unbinds them in the invoke that
applies the next request. Each cycle the code runs, after the code and the fade, the block writes
each bound inside bank's frames into its sink's frame ring with a stamp (position, both clocks,
`discontinuity` on the first cycle after any drive, `upstreamFrames`) in the stamp ring beside it,
and wakes the sink's thread (`pw_loop_signal_event`, an eventfd write; tested to allocate nothing).
So a sink takes exactly the cycles its code drove. The two rings drop together; a full one counts
dropped frames, which `Sink::droppedFrames()` reads at any time (an atomic) and the close reports. A
sink's first chunk after a block's first drive carries the clock's real position, which may differ
from that drive's take-up position when no cycle ran before it.

`upstreamFrames` is the most capture latency the block last told on the sink's bank
([below](#latency-per-bank-pair)): what reaches the input banks paired with it plus what the
configuration declared on it. Each time the main loop works the block's latency out afresh it
publishes every port's told latency, in frames, as one packed atomic per port, and the tap reads
the bank's at the cycle the chunk was taken. So a consumer dates a chunk without a mailbox: its
first frame entered the graph at `startRawNs - upstreamFrames * 1e9 / rate`. A chunk that wraps the
ring's end is handed in two calls, and the second's `position`, `startNs` and `startRawNs` are
advanced by the first's frames at the graph's rate (the adapter is given it), so the rule holds for
each call. The block converts each peer's latency to frames at its port before it combines any
([below](#latency-per-bank-pair)), so when a bank's paths differ the value is the exact most over
them, not PipeWire's field-by-field bound (quanta, frames and ns apart). A wedged data loop stops
the telling to peers, but the block still works its latency out and publishes it, so chunks follow
a new plan.

### Sources

A drive carries the source bindings of its configuration too, bound and unbound in the same invoke
as the code and the sinks, into storage sized when the block was made (one source per inside input
bank). Each cycle the block runs a code:

1. Each port's input span: PipeWire's buffer, or zeros for a port with none.
2. Each bound source plays into scratch of the block's own (one `maxBlock` buffer per inside input
   port, made with the block; the bank's engine ports stay, unlinked), and the code reads that
   scratch for the bank. Its `SourceCycle` is the cycle's position, frames, raw start,
   `discontinuity` and the playback latency the block last told on the bank (published per input
   port as a packed atomic, as each output's is for sinks).
3. The code (or zeros, when none is driven).
4. The fade on the faded outputs.
5. Each bound sink's tap ([above](#sinks)).
6. Metering of the public ports.

A cycle too large to run plays no source and taps no sink; the next cycle's position jump is a skip
the source keeps time over. The source, the code and its sinks run in one `process`, at one
position, swapped together between two cycles, so a cycle the block does not run none of them runs.
`SourceTap::play` makes no system call but its wakes, each at most one eventfd write and only when
someone waits (a writer for room; the played wake once until answered); tested to allocate nothing
with a writer waiting and the wake ringing. Once a stop's silence completed, `NodeHost` tells each
source `stopped()` (its ring dropped); at a teardown's first step it closes each writer, and frees
the sources after the instance. The rules a source plays by are
[writing-a-node-type.md](writing-a-node-type.md#sources)'s; `EventSources` adds only the writer's
wait (`poll` on an eventfd) and the played wake's eventfd, and three test seams:
`EventSources::waitsBegun(source)` counts the waits a writer began, each past its last look at the
ring, `waitsWoken(source)` how many of them the room descriptor ended (a play or the close, not the
patience), and `waitDeadline(patience)` is when a wait of that patience ends, so a
test waits for "the writer is waiting" as an event ([testing.md](testing.md#a-writers-own-event)).

## Latency, per bank pair

A port block connects with `PW_FILTER_FLAG_CUSTOM_LATENCY`. *Why:* without it `pw_filter` merges
every output's downstream latency into every input, and every input's upstream into every output
(`filter.c`, `default_latency`), which mixes paths that never meet (a card's playback and capture;
`sendspin`'s `to` and `from`).

What 1.4.2 does under the flag (`filter.c`, `impl-port.c`): the engine recomputes a port's latency
from its peers whenever a link or a peer's latency changes (`pw_impl_port_recalc_latency`) and sets
it on the port; the filter stores it and emits `param_changed` and nothing else (`handle_latency`).
The block then tells its ports' latency itself with `pw_filter_update_params`, which the engine hands
to the peers.

The rules are written once, as a pure model in `ear_core` (`ear/core/latency.hpp`), in frames at
the graph's rate. The block works its latency out by it and nothing else: at `carryLatency` (the
configuration's plan, given before its code drives, cleared at its teardown) and at each peer's
report, it hands the plan and each port's peer to `workOut`, publishes what comes back to the
realtime side and tells each port's latency to its peers. The type harness calls the same
`workOut` with no peer on any port ([writing-a-node-type.md](writing-a-node-type.md#testing-a-type)).
By the plan, which names roles:

- What lies **beyond** a bank is its ports' peers combined (only ports whose peers reported one),
  plus the plan's `beyond` entry for the bank **added in series** (to the least and to the most):
  what a companion adds beyond an inside bank, such as a receiver's buffer or a source's ring
  (`[0, capacity]`), which no engine node reports. *Why in series:* the buffer lies behind the
  companion's own ports, and rtp-source's output ports report a latency of zero once linked;
  combined as one more peer (least of the leasts) every path would read a least of zero. A bank with
  neither comes to zero.
- Each output bank's **downstream** latency is what lies beyond it, published to the realtime side
  as `GraphCycle::downstream`.
- An output bank tells upstream (**capture**) latency: over the pairs that end at it, what lies
  beyond the pair's input bank plus the pair's `onPair` delay, combined; plus its own `onBank`. A
  sink on the bank stamps its chunks with the most of it ([sinks](#sinks)).
- An input bank tells downstream (**playback**) latency: over the pairs that start at it, what lies
  beyond the pair's output bank plus the pair's `onPair` delay, combined; plus its own `onBank`. A
  source on the bank dates its `Played` with the most of it ([sources](#sources)).
- A bank in no pair tells its `onBank` alone. A paired bank with no peer is a zero path through its
  pair (an empty combination finishes at zero), so it lowers the least of what the pair's other side
  tells.

`NodeHost` makes the plan at configure's return and carries it with each drive: `pairs` are the
pass-through's; `beyond` holds each source's ring and each RTP receiver's `latencyMs`. A typed
`declareLatency(from, to, delay)` makes `from` to `to` a path, with `delay` as its `onPair`, so
the delay counts both ways on it (as PipeWire adds a node's process latency to both), in what `to`
tells and in what `from` tells. For a code of the type's own the pair is added to `pairs`; a
pass-through must already copy it, or the node fails at configure's return (a declared path must
exist). Typed declarations are keyed by their path, so a mixer declares each input's path into one
output. A legacy declaration on one bank (`sendspin`'s) is `onPair` on the
pass-through's pair that ends at the bank or, on a bank no pair ends at, `onBank`. NodeHost works
the plan out with no peer before configure's return is accepted, so a plan that does not fit the
ports fails the node `start-failed` (`configure gave a latency plan that does not fit: <why>`)
instead of reaching a drive.

- **Series and parallel are different types.** `Latency` adds only in series (`+`, saturating).
  Side by side, paths combine only through `Paths`, PipeWire's rule (`spa_latency_info_combine`:
  the least of the leasts, the most of the mosts); an empty `Paths` is **no path**, which is not
  zero, and only `orZero()` (what `spa_latency_info_combine_finish` does) makes it zero. What lies
  in series on every path (a `beyond` entry, a code's delay) is a `Delay`, which `Paths` refuses at
  compile time, so a buffer combined as one more peer does not compile.
- **One unit.** A peer's `{quantum, rate, ns}` converts to frames at the port, once, as the block
  hears it, in `ear_pipewire`'s private `latency_spa.hpp` (`fromSpa`: a quantum is the block size,
  ns are at the graph frequency, rounded to the nearest frame; an unfinished empty combination is no
  path; a negative total is zero), and what a block tells goes back in rate units only (`toSpa`).
  Combined in frames, peers in mixed units give the exact least and most, inside PipeWire's per-unit
  bound; in one unit the two agree, which `ear_pipewire_tests` checks against PipeWire's own
  functions. A negative peer is clamped at the port, before anything in series is added to it: a
  peer reporting -100 frames behind a receiver's 480 tells 480 (summed in PipeWire's units first, as
  the block did before the model, it told 380).
- **A plan that does not fit** the block's ports (a bank it lacks, a pair backwards, a pair delay
  off the plan's pairs, an entry twice) is `workOut`'s `std::logic_error`. NodeHost meets it at
  configure's return (above); the block's `carryLatency` still throws it, on the main loop, so a
  plan from anywhere else is refused before it is kept. After it only the peers change, so no
  peer's report meets a bad plan inside a callback.
- **Dating.** `FrameClock` turns frames into ns at the graph's rate, by one division of the whole
  count, truncated toward zero; `heardRawNs` (a frame heard once its downstream's most has played)
  and `enteredRawNs` (a frame entered its upstream's most before) are the two dating rules. The four
  dating sites (a sink chunk's split, `SourceRing`'s `Played`, `sendspin`'s progress) still carry
  their own copies of the formula, which the model's tests pin; they move onto it next
  ([root-classes.md](design/root-classes.md), step R3).

The model's table of bank shapes (`test/support/latency_table.hpp`) is run through `workOut` in
`ear_core_tests` and through a real block in `ear_pipewire_tests`, every port linked to a real
engine node (a peer telling in mixed units where the row gives one, a listener that is no path
elsewhere) and every inside bank read through a sink or a source, so the block cannot drift from
the model ([testing.md](testing.md#what-each-test-program-proves)).

The pairs are the pass-through's, so a card block forwards the card's playback latency from
`to_card` to `play`, and its capture latency from `from_card` to `capture`. An RTP receiver's
`latencyMs` is its `beyond` entry on `from_rtp` (rtp-source's ports report zero, not the buffer),
which the framework adds and `rtp`'s pass-through carries to `stream`. A configuration declares its
own with `declareLatency`: `sendspin` its feed on `from` (in no pair: `onBank`). Frames are at the
graph frequency. ALSA's playback node reports one quantum plus its headroom (`alsa-pcm-sink.c`,
`alsa-pcm.c`): 256 frames on the test card at a 256 quantum.

**Trap: the last link going is never told.** When a port loses its last peer the engine sets a null
Latency param, and `handle_latency` returns on null before it emits anything, so a block keeps the
latency its last peers reported until another peer reports. A lost card leaves its latency on the
card block's inside ports (harmless while nothing hears what goes there). A test checks the trap, so
a PipeWire that fixes it fails it.

## Engine companions

An engine companion is a PipeWire node a node's configuration asks for, joined channel for channel
to the ports of one of its block's inside banks and torn down with the configuration. Sound cards
and RTP receivers are the two kinds. A type asks with its bank and a shape (`companions.hpp`:
`SoundCardShape{card}`, `RtpReceiverShape{...}`); a card's direction is its bank's (playback on an
inside output bank, capture on an inside input bank), and an inside bank has at most one companion
of any kind ([writing-a-node-type.md](writing-a-node-type.md#engine-companions)). `NodeHost` works
out each one's `CompanionTarget` (its engine name, the block, the bank's direction and ports) and,
once the block is ready, hands it with the shape to the kind's `EngineCompanionAdapter`
(`AlsaSoundCards`, `RtpReceivers`), which tells `ready` and `lost` through `EngineCompanionEvents`.
Both kinds share one lifecycle (`NodeHost` in `ear_core`, `joinEngineCompanion` in `ear_pipewire`),
and a kind says only how its node is made, how ear hears from it, and what fails it:

1. **Alone.** The engine companion is made and runs on its own until it reports RUNNING.
2. **Linked.** One link per channel through `link-factory` (`link.output.node`, `link.output.port`,
   `link.input.node`, `link.input.port`): the engine companion and the block each by its **global id**
   (the block's is `PortBlock::blockId`), ports by name. *Why by id:* a node of the same name still
   going (a restart's old one, or an older block of a name added back) would otherwise match first, and `link-factory` matches a name against nick, description and
   object path too (`module-link-factory.c`).
3. **Ready** once every link is active; the node is `running` only then.
4. **Lost.** Any failure first destroys the links and the engine companion, waits for a `pw_core_sync`
   round trip, and only then tells `lost(detail)`: the node fails `engine-refused: <detail>`, its
   block and routes stay.

Engine companions are named `<node>.playback`, `<node>.capture` and `<node>.rtp`. An RTP
receiver's `latencyMs` enters the configuration's latency plan as `beyond` its bank
([latency](#latency-per-bank-pair)); a card reports its own through its links.

### Readiness deadlines

A node never waits for good on something that will never be ready. Each engine companion and each
sink's consumer has a deadline from its request (configure's applied return), on the main loop's
timers (`companions.hpp`: `cardReadinessBound` and friends, `readinessBound(kind)`); a source is
ready when made and has none. The wait counts from the request, so time the block took to be ready
counts too.

| Kind | Deadline | Past it, the node fails |
|---|---|---|
| Sound card (either direction) | 10 s | `engine-refused: the card never started: <card id>` |
| RTP receiver | 5 s | `engine-refused: the RTP receiver never ran (UDP port <port> held?)` |
| A sink's consumer | 5 s | `start-failed: the sink <bank>'s consumer did not begin within 5 s` (its `begin` is the type's code) |

Each deadline is an input of its own when its timer fires, and judges then: if its configuration is
still the current one, still readying (not torn down, not failed) and the companion not ready, the
node fails. Readiness heard first, even in the same main-loop turn, wins, and a companion's
readiness cancels its timer. A timer that fires more than 250 ms late (the main loop stalled) posts
its judgement to the main loop once more, so a ready the stall held back, dispatched behind the
timer, lands first. An engine companion never made, because its block was not ready by the
deadline, fails the node `engine-refused: <node>.<suffix> was never made: the node's block was not
ready within <n> s` rather than blaming the card or the port. The failure tears the configuration
down as any other: the flush destroys an engine companion made or not (destroying one tells nothing,
so its `lost` never comes), and a sink is closed (a `begin` still running finishes first; the close deadline covers one that
never returns). Meanwhile `waitingFor` shows `companion` or `consumer` after 2 s. A failed node is
retried only by `node.start`, `node.restart` or a change until recovery is built; a USB card that
takes longer than 10 s to start fails.

### Sound cards

`AlsaSoundCards` makes one ALSA adapter node per direction:

- **Made** with `factory.name = api.alsa.pcm.sink` (playback) or `.source` (capture),
  `api.alsa.path = hw:CARD=<card id>,DEV=0` (built in one place from a `CardId`, whose grammar keeps
  every other ALSA device form out, because ALSA's `file` plugin can run commands), `audio.rate` =
  the graph frequency, `audio.channels`, `audio.position = [ AUX0 AUX1 ... ]`,
  `node.always-process = true`, `priority.driver = 1`, `api.alsa.period-size` = the block size and
  `api.alsa.period-num` = `cardPeriods(block)` (its buffer, [the quantum guard](#the-quantum-guard)),
  and **no `node.group`**. So it runs alone, on
  its own clock, and holds no port block while it opens. In the group it would follow the dummy from
  creation and never start before it is linked, which a lifecycle that links once RUNNING cannot do.
  Never mark it `node.driver = false` either: the ALSA node publishes `node.driver = true` again.
- **Ports.** `adapter.auto-port-config` fails on 1.4.2 for the card's adapter (one unconfigured port
  whose PCM fails to start, EINVAL), so the card sends the port configuration itself, as a session
  manager would: `pw_node_set_param(SPA_PARAM_PortConfig)` in dsp mode with 32-bit float planar and
  AUX positions, naming the ports `playback_AUX<n>` and `capture_AUX<n>`. Block port `to_card.<i>`
  links to `playback_AUX<i-1>`, `capture_AUX<j-1>` to `from_card.<j>`.
- **Following.** Linked to its block, the card joins the blocks' graph, where the dummy outranks it:
  it becomes a follower (one repeated sample at the switch on the test card). As a follower the ALSA
  node runs a delay-locked loop on its buffer against the graph clock and steers the adapter's
  resampler, so every card is resampled all the time (no output is bit-perfect), its rate takes a
  few seconds to settle, and a card's playback and capture each follow with their own loop (their
  relative latency jitters by tens of samples).
- **Lost.** Refused at open, ERROR, a rate the kernel refuses, unplugged, a link that fails or goes.
  A lost card never held the clock, so the blocks keep the dummy and their position.
- **ALSA calls run on the main loop.** The engine runs in ear's process, so a card's adapter opens,
  prepares, drops and closes its PCM on ear's main loop, synchronously. A USB card that wedges in the
  kernel can block the whole main loop until ear restarts; the main-loop watchdog reports it.
- `node.driver-id` changes reach no client on 1.4.2: the engine sets it but sends no info event. A
  test that reads it binds the node again.

**CI's test card.** `src/pipewire/tests/alsa/alsa.conf`, set as `ALSA_CONFIG_PATH`, defines `hw` so
`hw:CARD=Test` is alsa-lib's `file` PCM over its `null` PCM (what the card is sent lands in a file);
any other id cannot open. It has no oscillator, so as a follower its rate match never settles (the
correction climbs). Engine tests therefore make it with `clocklessCardProperties()`, which sets its
`clock.name` to the dummy's: an ALSA follower whose clock name equals the driver's turns matching off
(`alsa-pcm.c`, `setup_matching`), so it is bit-exact. `main.cpp` never passes it: a real card keeps
matching on. Drift tracking, follower resyncs, a real card's latency and its errors on a running PCM
are checked only on the hardware (the `ear-on-talos` skill).

### RTP receivers

`RtpReceivers` loads `libpipewire-module-rtp-source` into the engine, one per `rtp` node. Its
arguments are built in one place from parsed values only (an `RtpAddress`, an enum, ranged numbers,
the engine node name):

```
{ remote.name = "internal"
  source.ip = "<address>"  source.port = <port>
  sess.media = "audio"  sess.latency.msec = <latencyMs>  sess.ignore-ssrc = true
  audio.format = "S16BE" | "S24BE"  audio.rate = <rate>  audio.channels = <N>
  audio.position = [ AUX0 AUX1 ... AUX<N-1> ]
  node.name = "<node>.rtp"  node.description = "<node>.rtp"  media.name = "<node>.rtp"
  stream.props = { node.always-process = true
                   adapter.auto-port-config = { mode = dsp monitor = false control = false position = preserve } } }
```

| Argument | Why |
|---|---|
| `remote.name = "internal"` | After `pw_context_connect_self` nothing registers the core object, so the module calls `pw_context_connect` with its `remote.name`; unset, that is the daemon's socket, which ear does not serve. `internal` is protocol-native's connection to the local context (`module-rtp-source.c:671-679`, `module-protocol-native.c:1336-1350`) |
| `adapter.auto-port-config` in `stream.props` | Without it the stream's converter has no format and so **no ports at all**. With it the ports are `receive_AUX0`.. (`audioadapter.c:2164-2171`, `audioconvert.c:1606-1617`). It works here, unlike on a card's adapter, because a stream's format is fixed |
| `node.always-process = true` in `stream.props` | The receiver runs on the graph's driver before it is linked, so its RUNNING proves its socket is bound (the module opens it when the stream starts); the lifecycle links on that RUNNING |
| `sess.ignore-ssrc = true` | By default the receiver locks onto the first packet's SSRC and drops every other for good, so a sender that restarts (FPP) is never heard again (`module-rtp/audio.c:131-136, 199-206`) |
| `source.ip` | `0.0.0.0` binds any address. A multicast group is joined on the default interface. Any other address binds any address and `connect()`s to `source.ip:source.port`, so it hears only that sender, and only from a source port equal to the receiving port (`module-rtp-source.c:277-283, 314-319`) |
| `node.description`, `media.name` | Replace the host name the module would use (`link-factory` matches descriptions too) |
| left at defaults | `cleanup.sec` (60), `net.mtu` (1280), packet times, `local.ifname` |

**How ear finds its node.** The module makes the node through its own connection, so there is no
proxy to watch. ear binds a registry on its own connection and sends a `pw_core_sync`; node globals
of the name seen before its done are stale (a restart's old receiver still going), and the first
node of the name after it that is not stale is bound. A stale id is forgotten when its global goes,
because the engine may reuse the id.

**What PipeWire 1.4.2 does that ear cannot see or change:**

- **A port that cannot be bound is only logged** (`bind() failed: Address already in use`): the node
  is not put in ERROR, not removed, the module not destroyed (`module-rtp-source.c:369-386`,
  `module-rtp/stream.c:228-251`). With always-process the node never reaches RUNNING, so the `rtp`
  node stays `configuring`, waiting for its companion, until the receiver's 5 s deadline fails it
  `engine-refused: the RTP receiver never ran (UDP port <port> held?)`
  ([readiness deadlines](#readiness-deadlines)). The module sets `SO_REUSEADDR`, so two receivers on
  one port both bind.
- **No packets is not a failure.** The module never destroys itself; after `cleanup.sec` without
  packets it sets the node property `rtp.receiving = false` (a bound node proxy sees it, the registry
  does not; `module-rtp-source.c:456-481`). ear does not read it.
- **Datagram size.** The receiver reads into a buffer of `net.mtu` bytes (1280,
  `module-rtp-source.c:715-716`); a longer datagram is cut short and garbled.
- **Any sequence mismatch resyncs**: a packet lost or out of order (`unexpected seq`) zeroes the
  ring and refills it to the target latency, an audible gap of about `latencyMs`
  (`module-rtp/audio.c:131-166`). So does a ring overrun.
- **Rate.** The receiver steers its resampler with a delay-locked loop from packet arrival, so its
  rate wobbles by parts in ten thousand: a sine fit over a whole second fails while every tenth of a
  second fits. Check a received tone window by window.
- **Latency rounding.** `sess.latency.msec` is rounded down to whole packet times (100 became 96.9 ms
  for 2-channel L16). The ring holds 4 MiB.
- **A host name blocks the main loop** in `pw_net_parse_address` (15 s for `fpp.local`) before the
  load fails: hence the closed address grammar.

CI's tests use a sender thread on 127.0.0.1 (`test/support/rtp_tone.hpp`), never PipeWire's
`module-rtp-sink`, and never multicast.

## The one data loop and invokes

Verified by `spikes_test.cpp` (S1 to S3, and S5's part B) against 1.4.2's `loop.c`, `context.c` and `filter.c`:

- **Asynchronous `pw_loop_invoke` from the main loop** runs on the data thread in posting order,
  never inside any `process`, and the post returns the async code, never the function's result. A
  full invoke queue (32 KiB, about 455 items) is never refused: the *posting* thread `calloc`s an
  overflow queue and goes on, and posting order holds across queues; the data thread allocates
  nothing. A loop whose thread is not running runs the function inline on the caller, which is why
  completions are posted, never run inline. **A producer posting as fast as it can outruns the
  flush**: the data loop spins in `flush_all_queues`, never runs `process` again, and memory grows.
- **Take-up by position.** Every request is taken up once, in order, one cycle after publication,
  and nodes enter in link order in every cycle they all run. But a cycle an xrun cuts short is run by
  an upstream prefix of the graph only, never by a downstream node without its upstream; so cycles
  are told apart by the order they ran in, never by position alone.
- **What a node reads after a cut-short cycle** (S5). In the first cycle a node runs after its
  upstream ran one it did not, it reads the upstream's frames of the cycle it runs, never those of
  the cut one: no stale buffer, no lasting shift. The cut cycle's frames are lost downstream, and
  a route between blocks keeps in step across an xrun. Measured with the upstream stamping every
  frame with its cycle's position: 3,491 such cycles the host cut short over ten idle and ten
  loaded runs, and 2,833 over ten runs that cut them on purpose, every one current.
- **A cycle can be cut short on purpose.** A node that sleeps past the cycle inside its `process`
  cuts that cycle short straight after itself, on any host: the driver's timer became ready
  during the sleep, before the eventfd the node writes as it finishes, the loop dispatches them
  in that order, and the next cycle's `node_ready` (`impl-node.c`) resets the woken node, whose
  wake then finds it no longer triggered. S5 makes both shapes this way (U overruns, and B), since
  a host cuts cycles short only by chance (a GitHub runner: 9 in 60 s beside a busy thread per
  core). The driver may cut a cycle or two more catching up.
- **A source keeps to the clock across them** (S5's part A, on a test filter, then on the real block
  in `source_test.cpp`). An upstream probe sleeping 12 ms past the 5.3 ms quantum every 50th cycle
  cuts that cycle short after itself, as above: the block does not run it, and the next cycle's
  position has jumped by whole blocks within the source's capacity. Across every cycle the block
  ran, the source's `Played` count equals the frames since its first position, and the node after
  the block hears each position's frame of the stream. So the source keeps time by summing what it
  played and skipped.
- **One data loop.** `context.num-data-loops` reads 1, the process has one `data-loop.0` thread, and
  a filter's `pw_filter_get_data_loop` is that loop. Adapters and the RTP module name `data-loop.0`
  in their info; `pw_filter`s name no loop.
- After a 2 s data-loop stall the driver catches up as [above](#the-internal-clock); the bounds treat
  it as no wedge while the loop answers invokes.

**Data-loop callbacks never throw.** A filter's `process` and every invoke that runs on the data loop
(a block's requests and its blocking flush, the denormal flush, the scheduling read) are registered
through the callback boundary as `realtime<>` on a `DataLoop`, which takes nothing else, whose member
must be `noexcept`, and nothing is added around them; so are the engine log's methods and alsa-lib's
error handler, which may run there too (`realtimeFunction<>`): nothing on the data loop catches, allocates or formats (the scheduling read takes the
policy and priority there and says them on the caller's thread). A throw there is `std::terminate`
([architecture.md](architecture.md#the-callback-boundary)).

## Levels

Each block meters its public ports (never inside ports) every cycle on the realtime thread: sample
peak and RMS, linear (`LevelMeter`, `src/core/levels.cpp`). A window is ceil(rate / 30) frames
(1600 at 48 kHz). A block publishes in the cycle that crosses a multiple of the window on the graph
clock, so every block's windows end at the same positions whenever it was made. Publishing writes
the block's triple-buffer mailbox (no lock, no allocation) and rings the level bell (an eventfd) at
most once per window; only the realtime side remembers which window it rang for, so a bell answered
without reading never silences later windows. `latestLevels()` returns one window for every block
that has one, the oldest latest window, since blocks publish one after another within a cycle.

## Realtime priority

`libpipewire-module-rt` raises the data thread to SCHED_FIFO only if the process may. The run image
gives `/opt/ear/ear` CAP_SYS_NICE as a **permitted-only** file capability (`setcap cap_sys_nice=p`);
a binary marked effective (`=ep`) would fail to start with EPERM wherever the capability is not
allowed. `Engine::create` makes it effective (raw `capget`/`capset`, no libcap) before `pw_init` and
before any engine thread exists. Not permitted: nothing changes, nothing fails. A capability added
by `docker run --cap-add SYS_NICE` or a pod spec alone never reaches a non-root process (execve
clears it), and `no_new_privs` (Kubernetes' `allowPrivilegeEscalation: false`) ignores file
capabilities. `Engine::dataThreadScheduling()` asks the data thread itself, and ear prints the
answer (`engine` record) and `graph.get` carries it.

## No session manager

Nothing like WirePlumber runs. Every block is always-processing (so an unrouted one runs), and ear
makes every link itself (the reconciler, through `Links`). A `link-factory` link belongs to the
connection that asked for it (no `object.linger`), so destroying its proxy removes it; the factory
destroys a link that goes to ERROR; a proxy removed before ACTIVE is a failure, after ACTIVE it is
gone (`module-link-factory.c` 1.0.5:263-277,365-400). ear never destroys a proxy inside its own event.

## Traps

- **A dot in a port's name is safe.** `pw_impl_port_add` (`impl-port.c`) only copies `port.name` and
  joins it into `port.alias`; it never parses it, and a route's link goes by port ids.
- **Stop signals kill the process unless blocked first.** `pw_context_new` starts the data-loop
  thread, and a SIGTERM goes to any thread that does not block it. `Engine::create` blocks SIGINT and
  SIGTERM before `pw_init`, so every thread inherits the mask, and reads them through a signal
  source (`pw_loop_add_signal`, a signalfd, registered through the callback boundary). Threads
  started later (the control port's) inherit it too.
- **Block size silently rounded.** Without `clock.power-of-two-quantum = false` a block size of 480
  runs as 256.
- **Custom properties never reach the registry.** Registry globals carry only whitelisted keys
  (`impl-node.c:59-78`, `impl-port.c:1016-1033`), so ear recognises its nodes by `node.name`, which is
  safe only while no other client can connect (no socket).
- **Existing objects arrive later.** A new registry binding replays every existing global as events
  after the call that made it returns. `LiveGraph` gives each subscriber its own binding and caches
  nothing; a port can arrive before or after its node.
- **Never tell a listener inside the call that caused it.** Refusals in making a block, a link or an
  engine companion are told later through `pw_loop_signal_event` on the main loop.
- **Main-loop work from another thread.** `MainLoopExecutor::post` queues the function and signals
  a main-loop event; posted from the loop thread itself it still runs later. Work left when it goes
  is destroyed unrun. Each item runs through the callback boundary on its own, so one that throws
  never drops the items behind it.
- **An exception through a C callback** unwinds through PipeWire's C frames, which is
  `std::terminate` with no word of where. Every callback is registered through the callback
  boundary instead (`addEvent`, `addTimer`, `addSignal`, `invoke`, `listen`, `installLog`,
  `setAlsaErrorHandler`), never with `pw_loop_add_*`, `pw_loop_invoke`, an `*_add_listener`,
  `pw_log_set` or an interface table of its own; a CTest test refuses those outside the boundary ([architecture.md](architecture.md#the-callback-boundary)).
- **A user node named like another engine node** (only `Dummy-Driver` today) gets engine-companion links
  that fail, since the block side of those links is matched by name.

## Xruns on a loaded host are not bugs

When the host is busy (other containers in Docker's VM, a load average far above the CPU count),
PipeWire's data thread can stall for tens to hundreds of milliseconds without a wake; each stall is a
graph xrun. A ramp test then sees a break, and an RTP test a gap (an underrun past `latencyMs`, or a
resync after reordered packets). To tell it from a bug, rerun with `PIPEWIRE_DEBUG="2,pw.node:4"`: an
xrun shows the dummy driver's missed cycles back to back while the late nodes' own process time is
microseconds. Run the same test on the unchanged code in the same conditions; if that fails too, the
host cannot judge it, and CI's runner is the gate. The S2 spike test is `RUN_SERIAL` for this reason.

## Known limits

- A card's ALSA calls run on the main loop ([sound cards](#sound-cards)).
- A pause of tens of seconds or more on the data thread slews the graph off the raw clock for minutes
  ([the internal clock](#the-internal-clock)).
- A cycle an xrun cuts short is run by an upstream prefix of the graph only.
- Latency is only declared at start, never reported while a configuration runs.
- Not built: an internal clock anchored to one card (that card bit-perfect), a follower watchdog for
  a frozen card, cards opened off the main loop, serving PipeWire's socket.

## Bumping PipeWire

The version is whatever Debian trixie's archive holds when the image is built (the Dockerfile pins
the base image by digest, not the packages).

1. Change the source in `docker/Dockerfile`, rebuild through `scripts/dev.sh`, and read the version
   with `scripts/dev.sh dpkg-query -W libpipewire-0.3-0t64`.
2. Read PipeWire's release notes between the two versions for: scheduling of unlinked always-process
   nodes and node groups (from 1.7.0, load `libpipewire-module-scheduler-v1`, and re-check how a card
   joins the group's clock); renamed clock keys; whether `context.properties` still overwrite
   `pw_context_new`'s; `pw_filter` and `link-factory` behaviour; the adapter's port configuration;
   `node.driver-id` events; the registry's key whitelist; module-rtp-source's arguments, failed binds,
   the SSRC lock, the resync and `rtp.receiving`; custom latency's null param; and `node-driver.c`'s
   tracking mode (whether `resync.ms` is still read, and whether a reset now re-arms its timer and
   calls `spa_dll_init`, in which case the default could come back).
3. Check every module `engine.conf` names, and `libpipewire-module-rtp-source`, still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover these facts and `ear_system_tests` the
   whole path.
5. Update the version named in `engine.conf`'s header, the `pipewire-engine` skill, `AGENTS.md` and
   this page.
