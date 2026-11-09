---
name: pipewire-engine
description: How EAR runs PipeWire's engine inside its own process (no daemon, no session manager, no socket), what src/pipewire/engine.conf sets and why, the internal clock (the dummy driver, which always drives and tracks the raw clock) and the sound cards that follow it, the graph group, GraphCycle's clock facts, latency carried per bank pair, denormals, outside nodes (sound cards and RTP receivers loaded from module-rtp-source), drive(), level metering, realtime priority (CAP_SYS_NICE), the traps found while building it, telling xruns under host load from bugs, and how to bump the PipeWire version. Use when changing src/pipewire/ (engine, port blocks, links, live ports, handoffs, outside nodes, sound cards, RTP receivers, the main-loop executor) or engine.conf, when a port block never processes, a link never goes active, a card never follows or never becomes ready, latency never arrives, an rtp node stays starting or is silent, levels stop, engine or RTP tests fail only on a loaded host, the data thread is not realtime, a stop signal kills the process, or the block size is not the one asked for, and before bumping the build image's PipeWire.
---

# Running PipeWire's engine inside EAR

EAR does not connect to a PipeWire daemon: it is the PipeWire instance. `ear::pipewire::Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in EAR's own process and
connects to that context itself (`pw_context_connect_self`). Every other adapter in
`src/pipewire/` works through that one connection. The PipeWire sources behind the claims here,
with file and line numbers, are cited in `docs/design/m1-white-to-file.md` section 1 ("Engine
facts"), `docs/design/batch2.md` sections 4.3 and 5 (cards, levels, the graph group) and
`docs/design/rtp.md` sections 4 and 13 (the rtp-source spike and errata); the claims were checked by the tests in `src/pipewire/tests/` against PipeWire 1.4.2, the version
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
| Outside nodes | `outside_nodes.cpp` (`joinOutsideNode`) | The one lifecycle of every engine node a node's insides ask for: runs alone, then linked to the block (the outside node by global id, the block by node name, ports by name), ready, lost |
| Sound cards | `sound_cards.cpp` | One kind of outside node: one ALSA adapter node per card and direction |
| RTP receivers | `rtp_receivers.cpp` | The other kind: `libpipewire-module-rtp-source` loaded into the context, one per `rtp` node |
| Handoffs | `handoffs.cpp` | Realtime thread to an ordinary thread: `spa_ringbuffer` plus one `pw_thread_loop` each |
| Main-loop executor | `main_loop_executor.cpp` | Work posted from any thread (the control port's graph calls) runs on the main loop: a mutex-guarded queue plus a `pw_loop` event source |
| Shared names | `engine_names.hpp` | The graph group's name, a card's driver priority and the internal clock's name |

`main.cpp` passes `<directory of /proc/self/exe>/engine.conf`; the tests pass the source tree's
file (`EAR_ENGINE_CONF`). `Engine` does no lookup of its own. The ctest
`ear_engine_conf_beside_ear` fails if the copy beside the binary differs from the source.

## engine.conf, key by key

`context.properties`:
- `core.daemon = false`: no socket and no runtime directory. EAR's own connection is the only
  client. Serving the socket (so shairport-sync can connect) is parked.
- `core.name = ear`.
- `support.dbus = false`: there is no D-Bus in the container.
- Never `default.clock.*` or `clock.power-of-two-quantum`: `context.properties` from the file
  overwrite the properties `Engine` passes to `pw_context_new`, which carry the clock.

`context.spa-libs`: `support.* = support/libspa-support` (the loop, logging and the dummy driver);
`api.alsa.* = alsa/libspa-alsa` (a sound card's ALSA PCM node); `audio.convert.* =
audioconvert/libspa-audioconvert` (the converter the adapter wraps around it). Debian's
`libspa-0.2-modules` holds all three.

`context.modules`, each for one reason:
- `libpipewire-module-rt` with `[ ifexists nofail ]`: realtime priority for the data thread
  where the container allows it; a container without the privilege still starts.
- `libpipewire-module-protocol-native`: the protocol EAR's own connection speaks, even with no
  socket.
- `libpipewire-module-client-node`: a `pw_filter` exports its node through it; without it no
  port block appears.
- `libpipewire-module-spa-node-factory`: makes the dummy driver below.
- `libpipewire-module-link-factory`: makes the route links (by port global id) and each
  outside node's own links (by node and port).
- `libpipewire-module-adapter`: makes a sound card, an ALSA PCM node wrapped in a converter
  with one port per channel.

`libpipewire-module-rtp-source` is not in `engine.conf`: `RtpReceivers` loads one per `rtp`
node with `pw_context_load_module` (section "RTP receivers"). Debian's `libpipewire-0.3-modules`
holds it.

`context.objects`: one `spa-node-factory` object, `factory.name = support.node.driver`,
`node.name = Dummy-Driver`, `priority.driver = 200000`, `node.group = ear.graph`, `clock.id =
monotonic-raw`. It is ear's internal clock, the graph's one driver whatever links or fails
(`docs/design/node-framework.md` section 10, next section but one). It has no ports, so it never
shows up as a live port. A card's `priority.driver` (`cardPriority` in `engine_names.hpp`, 1) must
stay below it: that is what keeps the dummy the driver once a card is linked.

Never `cpu.zero.denormals` in `context.properties`: on 1.4.2 it lands on the main thread in this
engine (the data loop's setup runs inline there before the data thread enters) and leaks to every
thread made later. `Engine::create` sets FTZ and DAZ (FZ on arm64) with a blocking invoke on the
data loop instead (`flushDenormals`); "process() runs with denormals flushed to zero" reads the
MXCSR from `process`. That invoke races the data thread too: `pw_data_loop_start` returns before
the thread enters its loop, and until it does `loop_invoke` runs the function inline on the caller
(about 4% of boots). So `flushDenormals` does nothing on the thread that invoked it, and
`Engine::create` invokes again until it ran on the data thread (1 ms apart, failing after 5 s). Any
blocking invoke made right after `pw_context_new` meets the same race.

## The clock

`Engine` sets, as context properties: `default.clock.rate` and `default.clock.allowed-rates`
(only the graph frequency), `default.clock.quantum`, `min-quantum` and `max-quantum` (all the
block size), `clock.power-of-two-quantum = false`, and widens `default.clock.quantum-floor`
(default 4) and `quantum-limit` (default 8192) so the block size fits. A port block sees
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
dummy, and no card ever moves it (the slice 7 spike, `docs/design/node-framework.md` section 10).
The test "Every block is in the ear.graph group and processes on the dummy driver alone" checks the
group, the dummy's group and its clock name. A card's adapter node has no `node.group` (next
section).

**It tracks `CLOCK_MONOTONIC_RAW`.** `CLOCK_MONOTONIC` is slewed by NTP (chrony may slew it by up
to 83,333 ppm), and timerfd cannot arm `CLOCK_MONOTONIC_RAW` (`EINVAL`). With `clock.id =
monotonic-raw` the driver runs in its tracking mode (`node-driver.c`): its timer stays on
`CLOCK_MONOTONIC`, a delay-locked loop steers it so `clock.position` follows raw time, and
`rate_diff` is the loop's correction. `clock.nsec` and `next_nsec` stay `CLOCK_MONOTONIC`. Its
error is clamped at 1 ms a cycle; past `resync.ms` (10 ms, e.g. after a data-thread stall) the
position jumps to raw time. It can jump *back*: a starved data loop reads the timer late, the
reset puts the position forward on raw time, the next timeouts (computed from the late one) fire
back to back, a block each, until the position is `resync.ms` ahead, and the next reset steps it
back by up to about 440 frames. The whole graph then runs positions it already ran, so a test or
a type must never treat `clock.position` as unique or tell cycles apart by it (node-lifecycle.md
10.5; S2 in `spikes_test.cpp` tells them apart by entry order). Its `clock.name` is
`clock.system.monotonic-raw` (`internalClockName`).
Whether it follows a host slewed at chrony's worst case is untested; the fallback is chrony's
`maxslewrate` (the `ear-on-talos` skill).

**`GraphCycle`** is made by the block once a cycle (`port_blocks.cpp`, `onProcess`), so no type reads
a clock: `startNs` (`clock.nsec`), `position`, `startRawNs` (both clocks sampled together, the
difference added to `clock.nsec`, so a late wake cancels out), `rateDiff` (`clock.rate_diff`),
`driverId` (`clock.id`, the driver's global id), `nextNs` (`clock.next_nsec`), `xruns` and
`discontinuity`. PipeWire gives a filter no xrun count (`clock.xrun` is set only by a driving ALSA
node), so the block counts cycles that start ahead of where the last one ended on the same driver
(a backward resync is no xrun); `discontinuity` marks the block's first cycle, a new code's first
cycle, a new driver, any position jump (the dummy's resync), and the cycle after one too large for
the block. Both clocks are read mono, raw, mono, and the offset taken from the midpoint, as
`node-driver.c` does. `downstream` is the latency downstream of each output (below). The dummy's
`clock.delay` is always 0, followers or not: a following ALSA node writes its delay into its own
clock, never into the driver's position.

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

The block's rule (`retell`), from the run's `LatencyPlan` (`carryLatency`, given by `NodeHost`
before the run's code drives the block, cleared at its teardown): each output bank's downstream
latency is its ports' peers combined (only ports whose peers reported one: an unrouted port is no
path and does not pull the least to 0), published to the realtime side (`GraphCycle::downstream`, one
per output port); an output bank tells upstream (capture) latency = what reaches the input banks
paired with it, plus what the run declared on it; an input bank tells downstream (playback)
latency = what lies beyond the output banks paired with it, plus what the run declared. The pairs
are the pass-through's, so a card block forwards the card's playback latency from `to_card` to
`play`, and the card's capture latency from `from_card` to `capture`. A run declares its own with
`StartContext::declareLatency(bank, Latency)`: `rtp` its `latencyMs` on `stream` (rtp-source's own
stream reports none), `sendspin` its feed (0 to its size) on `from`. Frames: a quantum is the block
size and nanoseconds are at the graph frequency. ALSA's playback node reports one quantum plus its
headroom (`alsa-pcm-sink.c`, `alsa-pcm.c`): 256 frames on the test card at a 256 quantum.

## Sound cards and the clock

A `card` node is a port block (its public ports, metered) plus one ALSA adapter node per
direction made by `AlsaSoundCards` (`sound_cards.cpp`), joined to the block's inside ports by
the card's own links. A card is one kind of outside node: the state machine below is
`joinOutsideNode` (`outside_nodes.cpp`), shared with RTP receivers; the card's body says only
how its node is made and heard from (its own proxy) and what fails it:

1. **Opening.** The adapter is made with `factory.name = api.alsa.pcm.sink` (playback) or
   `.source` (capture), `api.alsa.path = hw:CARD=<card id>,DEV=0` (built in one place,
   `alsaPath`; the card id's grammar, `CardId` in ear_core, keeps every other ALSA device form
   out, because ALSA's `file` plugin can run commands), `audio.rate` = the graph frequency,
   `audio.channels`, `audio.position = [ AUX0 AUX1 ... ]`, `node.always-process = true`,
   `priority.driver = 1` and no `node.group`. So it runs alone, on its own clock, and holds no
   port block while it opens. In the group it would follow the dummy from creation and never
   start before it is linked, which this lifecycle (link once RUNNING) cannot do. Never mark it
   `node.driver = false` either: the ALSA node publishes `node.driver = true` again (the spike).
2. **Linking.** When the node reports RUNNING (its PCM runs), the card makes one link per
   channel through `link-factory` (`link.output.node`, `link.output.port`, `link.input.node`,
   `link.input.port`): the adapter named by its global id, the block by its node name, ports by
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
engine node name); `docs/design/rtp.md` section 5 has the full string. What each part is for:

- **`remote.name = "internal"`.** The module connects its own client to a core. After
  `pw_context_connect_self` nothing registers the core object in the context, so the module calls
  `pw_context_connect` with its `remote.name`; left unset that is the daemon's socket, which EAR
  does not serve. `internal` is protocol-native's connection to the local
  context. Each receiver is one more client in the engine.
- **`stream.props = { adapter.auto-port-config = { mode = dsp ... } }`.** Without it the stream's
  converter has no format and so **no ports at all**: the node appears, nothing can link. With it
  the ports are `receive_AUX0`..`receive_AUX<N-1>` (a `node.network` stream's outputs are named
  `receive_<position>`; ear passes `audio.position = [ AUX0 ... ]`). Unlike the card's adapter
  (Traps), auto port config works here, because a stream's format is fixed.
- **`node.always-process = true` in `stream.props`.** The receiver runs on the graph's driver
  before it is linked, so its RUNNING proves the socket is bound: the module opens the socket when
  the stream starts. The lifecycle links on that RUNNING, as it waits for a card's PCM.
- **`sess.ignore-ssrc = true`.** By default the receiver locks onto the first packet's SSRC and
  drops every other one for good, so a sender that restarts with a new SSRC (FPP restarting) is
  never heard again.
- **`source.ip`.** `0.0.0.0` binds any address and hears every sender. A multicast group is
  joined on the default interface (`local.ifname` unset). Any other address does not mean "this
  host's address": PipeWire binds any address and `connect()`s to `source.ip:source.port`, so it
  hears only that sender, and only packets whose source port equals the receiving port.
  `RtpAddress` therefore accepts only `0.0.0.0` and 224.0.0.0-239.255.255.255. A host name would
  block the main loop in a name lookup (15 s for `fpp.local` in the spike) before the load fails.
- **`audio.rate`** is the sender's rate (the graph frequency by default); another rate is
  resampled. The receiver steers its resampler with a delay-locked loop from packet arrival, so
  its rate wobbles by parts in ten thousand: a sine fit over a whole second fails while every
  tenth of a second fits (rtp.md erratum 1). Check a received tone window by window.
- **`node.description` and `media.name`** = the engine node name, replacing the host name the
  module would use (`link-factory` also matches by description, Traps).
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
  With always-process the node then never reaches RUNNING, so the `rtp` node stays `starting`
  with silent outputs. The module sets `SO_REUSEADDR`, so two receivers on one port both bind
  (unicast then reaches only one); a port held by a program without it fails.
- **No packets is not a failure.** The module never destroys itself. After `cleanup.sec` without
  packets it sets the node property `rtp.receiving = false` (true again on the next packet); a
  bound node proxy sees it, the registry does not. ear does not read it: the node stays
  `running` and silent (the owner's choice, rtp.md section 1, Q1).
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

## drive(): swapping a block's code

`PortBlock::drive(request, done)` (`docs/design/node-lifecycle.md` section 5.3) returns at once:
the request goes into the block's ring of eight, one non-blocking `pw_loop_invoke` per block in
flight applies it on the block's own data loop between two cycles, and `done(P)` runs later on the
main loop, from the block's completion event (or `settle()`), never inside the call. Once `done`
ran, no `process()` of the old code is running or will run, so the node framework can release the
old insides. A request with no code makes every output silent; the block keeps processing. A
block with no data loop (refused, never connected, or its local node destroyed: `filter.c` clears
the pointer) applies it on the main thread and still completes by signal. A block whose engine
node was destroyed through the registry keeps its data loop (E1 found this), so the invoke runs
there as usual. The blocking `drive(code)` went with step L-3a; an engine test that needs a code
running before it goes on uses `ear::testing::driveNow` (`ear/testing/drive_now.hpp`).

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
  by port ids. `port_block_test.cpp` carries a ramp and levels over dotted names. Read again at a PipeWire bump. Only the outside nodes'
  links go by name, and those name inside ports and `playback_AUX<i>`, with no dot.
- **Stop signals kill the process unless blocked first.** `pw_context_new` starts the data-loop
  thread. A SIGTERM sent to the process goes to any thread that does not block it, where its
  default action ends EAR. `Engine::create` blocks SIGINT and SIGTERM before `pw_init`, so every
  thread inherits the mask, and `onStopSignal` reads them through `pw_loop_add_signal` (a
  signalfd, which also reports a signal that arrived earlier).
- **Block size silently rounded.** Without `clock.power-of-two-quantum = false` a block size of
  480 runs as 256.
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
  longer relies on it (since step L-3a a node is silenced by an asynchronous drive before its block
  is destroyed), but a block destroyed with a code still driven is safe.
- **Link lifetime.** A link made through `link-factory` belongs to the connection that asked for
  it (no `object.linger`), so destroying its proxy removes it. The factory destroys a link that
  goes to ERROR. A proxy removed before ACTIVE is a failure; after ACTIVE it is gone. Never
  destroy a proxy inside its own event: `links.cpp` silences it and destroys it later from a
  main-loop event.
- **Existing objects arrive later.** A new registry binding replays every existing global as
  events after the call that made it returns, never inside it. `LiveGraph` gives each
  subscriber its own binding and caches nothing; a port can arrive before or after its node.
- **Never tell a listener inside the call that caused it.** Refusals in `FilterPortBlocks::make`,
  `Links::createLink` and `joinOutsideNode` (every outside node's `make`) are told later through `pw_loop_signal_event`
  on the main loop, as the core interfaces require.
- **The adapter's `adapter.auto-port-config` fails on 1.4.2.** It left one unconfigured port
  whose PCM failed to start (EINVAL). The card sends the port configuration itself, as a
  session manager would: `pw_node_set_param(SPA_PARAM_PortConfig)` in dsp mode with 32-bit float
  planar and AUX positions, which names the ports `playback_AUX<n>` and `capture_AUX<n>`. On
  an RTP receiver's stream the same key works and is what gives it ports.
- **`cpu.zero.denormals` sets the wrong thread.** See `engine.conf` above: the engine flushes
  denormals with an invoke on the data loop.
- **Custom latency is never told a lost last link.** See "Latency, per bank pair".
- **`node.driver-id` changes reach no client on 1.4.2.** The engine sets a node's
  `node.driver-id` when its driver changes but sends no info event for it. A test that reads it
  binds the node again (`sound_card_test.cpp`).
- **link-factory finds nodes by more than `node.name`.** By name it also matches nick,
  description and object path (`module-link-factory.c`). Outside-node links therefore name the
  outside node by global id; the block side is still a name, so a node a user names like another
  engine node (only `Dummy-Driver` today) gets outside-node links that fail, and the outside node
  is lost with the factory's error.
- **Main-loop work from another thread.** `MainLoopExecutor::post` queues the function and
  signals a main-loop event; posted from the loop thread itself it still runs later, never
  inline. Work left when the executor goes is destroyed unrun. `pw_loop_invoke` would do the
  same crossing, but the queue owns each function (no leak at stop) and never reports full.
- **Threads started later inherit the signal mask.** The control port's thread is started after
  `Engine::create`, so it too blocks SIGINT and SIGTERM.
- **Waking from the realtime thread.** The handoff writer wakes its thread with
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
  (`docs/design/rtp.md` erratum 3).
- **Tests and the environment.** `engine_test.cpp` unsets `XDG_RUNTIME_DIR`,
  `PIPEWIRE_RUNTIME_DIR`, `PIPEWIRE_REMOTE` and the D-Bus addresses, so it proves the engine
  needs none of them. Its `Waiter` runs the main loop until the awaited events have arrived; a
  timer only fails the test. Copy that pattern for new engine tests.

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
   SSRC lock, the resync on a sequence mismatch, and the `rtp.receiving` property.
3. Check each module named in `engine.conf`, and `libpipewire-module-rtp-source`, still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover the engine facts (`[rtp]` the
   receiver) and `ear_system_tests` the whole path.
5. Update the tested-against line in `engine.conf`'s header and the version named in this
   skill's first section and in `AGENTS.md`.
