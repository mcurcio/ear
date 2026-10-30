---
name: pipewire-engine
description: How EAR runs PipeWire's engine inside its own process (no daemon, no session manager, no socket), what src/pipewire/engine.conf sets and why, the dummy driver and sound cards that clock the graph, the graph group, drive(), level metering, realtime priority (CAP_SYS_NICE), the traps found while building it, and how to bump the PipeWire version. Use when changing src/pipewire/ (engine, port blocks, links, live ports, handoffs, sound cards, the main-loop executor) or engine.conf, when a port block never processes, a link never goes active, a card never drives or never becomes ready, levels stop, the data thread is not realtime, a stop signal kills the process, or the block size is not the one asked for, and before bumping the build image's PipeWire.
---

# Running PipeWire's engine inside EAR

EAR does not connect to a PipeWire daemon: it is the PipeWire instance. `ear::pipewire::Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in EAR's own process and
connects to that context itself (`pw_context_connect_self`). Every other adapter in
`src/pipewire/` works through that one connection. The PipeWire sources behind the claims here,
with file and line numbers, are cited in `docs/design/m1-white-to-file.md` section 1 ("Engine
facts") and `docs/design/batch2.md` sections 4.3 and 5 (cards, levels, the graph group); the
claims were checked by the tests in `src/pipewire/tests/` against PipeWire 1.4.2, the version
Debian 13 (trixie) ships in the build image (`docker/Dockerfile`).

## What the engine is made of

| Piece | Where | Why |
|---|---|---|
| Main loop, context, own connection | `Engine::create` | One process, one loop; every core object and adapter runs on the main loop's thread |
| `engine.conf` | `src/pipewire/engine.conf`, copied beside `out/ear` | Read by `pw_context_new` (passed as `config.name`); loads the modules and the driver |
| Clock settings | `contextProperties()` in `engine.cpp` | The graph frequency and block size from `ear.json` |
| Port blocks | `port_blocks.cpp` | One `pw_filter` per node, one 32-bit float mono port per declared port (public and inside); every block in the graph group; `drive()`; level metering |
| Level mailbox | `level_mailbox.hpp` | One triple buffer per block: the realtime thread publishes a level window, an ordinary thread reads the latest |
| Live ports | `live_graph.cpp` | Registry globals joined into node name, port name, direction, port id |
| Links | `links.cpp`, `link_proxy.hpp` | `link-factory` links by port global id, owned by EAR's connection |
| Sound cards | `sound_cards.cpp` | One ALSA adapter node per card and direction, with its own links by node and port name |
| Handoffs | `handoffs.cpp` | Realtime thread to an ordinary thread: `spa_ringbuffer` plus one `pw_thread_loop` each |
| Main-loop executor | `main_loop_executor.cpp` | Work posted from any thread (the control port's graph calls) runs on the main loop: a mutex-guarded queue plus a `pw_loop` event source |
| Shared names | `engine_names.hpp` | The graph group's name and a card's driver priority |

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
- `libpipewire-module-link-factory`: makes the route links (by port global id) and each sound
  card's own links (by node and port name).
- `libpipewire-module-adapter`: makes a sound card, an ALSA PCM node wrapped in a converter
  with one port per channel.

`context.objects`: one `spa-node-factory` object, `factory.name = support.node.driver`,
`node.name = Dummy-Driver`, `priority.driver = 200000`. It is the driver that clocks the graph
while no sound card does. It has no ports, so it never shows up as a live port. A card's
`priority.driver` (`cardPriority` in `engine_names.hpp`, 1) must stay below it, so a card that
runs alone is never the driver the port blocks fall back to.

## The clock

`Engine` sets, as context properties: `default.clock.rate` and `default.clock.allowed-rates`
(only the graph frequency), `default.clock.quantum`, `min-quantum` and `max-quantum` (all the
block size), `clock.power-of-two-quantum = false`, and widens `default.clock.quantum-floor`
(default 4) and `quantum-limit` (default 8192) so the block size fits. A port block sees
`position->clock.duration == blockSize` and `clock.rate.denom == graphFrequency`; the test
"Blocks process at the graph frequency and block size" checks it with 44100 Hz and 256.

## The graph group

Every port block sets `node.group = ear.graph` (`graphGroup` in `engine_names.hpp`). PipeWire
puts every node of one group on the same driver, so one clock drives every block: the dummy
while no card drives, else the card. The test "Every block is in the ear.graph group and
processes on the dummy driver alone" checks it. A card's adapter node has no `node.group`
(next section).

## Sound cards and the clock

A `card` node is a port block (its public ports, metered) plus one ALSA adapter node per
direction made by `AlsaSoundCards` (`sound_cards.cpp`), joined to the block's inside ports by
the card's own links. One state machine per card owns "is this card in the clock":

1. **Opening.** The adapter is made with `factory.name = api.alsa.pcm.sink` (playback) or
   `.source` (capture), `api.alsa.path = hw:CARD=<card id>,DEV=0` (built in one place,
   `alsaPath`; the card id's grammar, `CardId` in ear_core, keeps every other ALSA device form
   out, because ALSA's `file` plugin can run commands), `audio.rate` = the graph frequency,
   `audio.channels`, `audio.position = [ AUX0 AUX1 ... ]`, `node.always-process = true`,
   `priority.driver = 1` and no `node.group`. So it runs alone, on its own clock, and holds no
   port block while it opens.
2. **Linking.** When the node reports RUNNING (its PCM runs), the card makes one link per
   channel through `link-factory`, by node and port name (`link.output.node`,
   `link.output.port`, `link.input.node`, `link.input.port`): block port `to_card_<i>` to the
   adapter's `playback_AUX<i-1>`, or the adapter's `capture_AUX<j-1>` to `from_card_<j>`.
   Linked to its block, the card joins the blocks' clock and, as the only driver among them
   (the dummy is linked to nothing and in no group), drives every block: every block's
   `node.driver-id` becomes the card's (E2's test).
3. **Ready** once every link is active. Its health (PCM running, every link active) is the
   node's insides readiness, so the node is `running` only then.
4. **Lost.** Any failure (refused at open, the node going to ERROR, a rate the kernel refuses,
   the card unplugged, a link that fails or goes) first destroys the links and the adapter, then
   waits for a `pw_core_sync` round trip, and only then tells `lost(detail)`. So a lost card
   never holds the graph: the blocks go back to the dummy and keep processing. The node becomes
   `failed engine-refused: <detail>`; its block and routes stay. No retry: `node.update`
   restarts it.

CI has no sound card. `src/pipewire/tests/alsa/alsa.conf`, set as `ALSA_CONFIG_PATH` in the
test process, defines `hw` so `hw:CARD=Test` is alsa-lib's `file` PCM over its `null` PCM
(what the card is sent lands in `$EAR_ALSA_OUT/card.raw`); any other card id cannot open. E2's
spike showed this card runs and drives on PipeWire 1.4.2, so CI checks ready, driving, the
channel mapping sample for sample, a card that cannot open and a lost card. The ALSA-error path
on a running PCM, the rate a real card refuses, and latency are checked only on the hardware
(the `ear-on-talos` skill).

## drive(): swapping a block's code

`PortBlock::drive(code)` stores the new `AudioCode*` on the block's own data loop with a
blocking `pw_loop_invoke`, between two cycles: when it returns, no `process()` of the old code
is running or will run, so the node framework can release the old insides. `drive(nullptr)`
makes every output silent; the block keeps processing. A block with no data loop (refused,
never connected, or its local node destroyed: `filter.c` clears the pointer) is not processing,
so storing the pointer is enough. A block whose engine node was destroyed through the registry
keeps its data loop (E1 found this), so the invoke runs there as usual.

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
  with a blocking invoke, so `process` is never called after it returns; the node framework
  relies on that before `AudioCode::release`.
- **Link lifetime.** A link made through `link-factory` belongs to the connection that asked for
  it (no `object.linger`), so destroying its proxy removes it. The factory destroys a link that
  goes to ERROR. A proxy removed before ACTIVE is a failure; after ACTIVE it is gone. Never
  destroy a proxy inside its own event: `links.cpp` silences it and destroys it later from a
  main-loop event.
- **Existing objects arrive later.** A new registry binding replays every existing global as
  events after the call that made it returns, never inside it. `LiveGraph` gives each
  subscriber its own binding and caches nothing; a port can arrive before or after its node.
- **Never tell a listener inside the call that caused it.** Refusals in `FilterPortBlocks::make`,
  `Links::createLink` and `AlsaSoundCards::make` are told later through `pw_loop_signal_event`
  on the main loop, as the core interfaces require.
- **The adapter's `adapter.auto-port-config` fails on 1.4.2.** It left one unconfigured port
  whose PCM failed to start (EINVAL). The card sends the port configuration itself, as a
  session manager would: `pw_node_set_param(SPA_PARAM_PortConfig)` in dsp mode with 32-bit float
  planar and AUX positions, which names the ports `playback_AUX<n>` and `capture_AUX<n>`.
- **`node.driver-id` changes reach no client on 1.4.2.** The engine sets a node's
  `node.driver-id` when its driver changes but sends no info event for it. A test that reads it
  binds the node again (`sound_card_test.cpp`).
- **link-factory finds nodes by more than `node.name`.** By name it also matches nick,
  description and object path (`module-link-factory.c`). A node a user names like another
  engine node (only `Dummy-Driver` today) gets card links that fail, so its card is lost with
  the factory's error.
- **Main-loop work from another thread.** `MainLoopExecutor::post` queues the function and
  signals a main-loop event; posted from the loop thread itself it still runs later, never
  inline. Work left when the executor goes is destroyed unrun. `pw_loop_invoke` would do the
  same crossing, but the queue owns each function (no leak at stop) and never reports full.
- **Threads started later inherit the signal mask.** The control port's thread is started after
  `Engine::create`, so it too blocks SIGINT and SIGTERM.
- **Waking from the realtime thread.** The handoff writer wakes its thread with
  `pw_loop_signal_event` (an eventfd write). The S4 test checks it allocates nothing; the
  design page lists its realtime safety as otherwise unverified.
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
   `link-factory` behaviour (link by name included), the adapter's port configuration (whether
   `adapter.auto-port-config` now works), `node.driver-id` events, and the registry's key
   whitelist.
3. Check each module named in `engine.conf` still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover the engine facts and
   `ear_system_tests` the whole path.
5. Update the tested-against line in `engine.conf`'s header and the version named in this
   skill's first section and in `AGENTS.md`.
