---
name: pipewire-engine
description: How EAR runs PipeWire's engine inside its own process (no daemon, no session manager, no socket), what src/pipewire/engine.conf sets and why, the dummy driver that clocks the graph, the traps found while building it, and how to bump the PipeWire version. Use when changing src/pipewire/ (engine, port blocks, links, live ports, handoffs) or engine.conf, when a port block never processes, a link never goes active, a stop signal kills the process, or the block size is not the one asked for, and before bumping the build image's PipeWire.
---

# Running PipeWire's engine inside EAR

EAR does not connect to a PipeWire daemon: it is the PipeWire instance. `ear::pipewire::Engine`
(`src/pipewire/engine.cpp`) makes PipeWire's main loop and context in EAR's own process and
connects to that context itself (`pw_context_connect_self`). Every other adapter in
`src/pipewire/` works through that one connection. The PipeWire sources behind the claims here,
with file and line numbers, are cited in `docs/design/m1-white-to-file.md` section 1 ("Engine
facts"); the claims were checked by `src/pipewire/tests/engine_test.cpp` against PipeWire 1.4.2,
the version Debian 13 (trixie) ships in the build image (`docker/Dockerfile`).

## What the engine is made of

| Piece | Where | Why |
|---|---|---|
| Main loop, context, own connection | `Engine::create` | One process, one loop; every core object and adapter runs on the main loop's thread |
| `engine.conf` | `src/pipewire/engine.conf`, copied beside `out/ear` | Read by `pw_context_new` (passed as `config.name`); loads the modules and the driver |
| Clock settings | `contextProperties()` in `engine.cpp` | The graph frequency and block size from `ear.json` |
| Port blocks | `port_blocks.cpp` | One `pw_filter` per node, one 32-bit float mono port per declared port |
| Live ports | `live_graph.cpp` | Registry globals joined into node name, port name, direction, port id |
| Links | `links.cpp` | `link-factory` links by port global id, owned by EAR's connection |
| Handoffs | `handoffs.cpp` | Realtime thread to an ordinary thread: `spa_ringbuffer` plus one `pw_thread_loop` each |

`main.cpp` passes `<directory of /proc/self/exe>/engine.conf`; the tests pass the source tree's
file (`EAR_ENGINE_CONF`). `Engine` does no lookup of its own. The ctest
`ear_engine_conf_beside_ear` fails if the copy beside the binary differs from the source.

## engine.conf, key by key

`context.properties`:
- `core.daemon = false`: no socket and no runtime directory. EAR's own connection is the only
  client in milestone 1. Serving the socket (so shairport-sync can connect) is parked.
- `core.name = ear`.
- `support.dbus = false`: there is no D-Bus in the container.
- Never `default.clock.*` or `clock.power-of-two-quantum`: `context.properties` from the file
  overwrite the properties `Engine` passes to `pw_context_new`, which carry the clock.

`context.spa-libs`: `support.* = support/libspa-support` (the loop, logging and the dummy driver).

`context.modules`, each for one reason:
- `libpipewire-module-rt` with `[ ifexists nofail ]`: realtime priority for the data thread
  where the container allows it; a container without the privilege still starts.
- `libpipewire-module-protocol-native`: the protocol EAR's own connection speaks, even with no
  socket.
- `libpipewire-module-client-node`: a `pw_filter` exports its node through it; without it no
  port block appears.
- `libpipewire-module-spa-node-factory`: makes the dummy driver below.
- `libpipewire-module-link-factory`: makes the route links.

`context.objects`: one `spa-node-factory` object, `factory.name = support.node.driver`,
`node.name = Dummy-Driver`, `priority.driver = 200000`. It is the driver that clocks the graph
while there is no sound card. It has no ports, so it never shows up as a live port.

## The clock

`Engine` sets, as context properties: `default.clock.rate` and `default.clock.allowed-rates`
(only the graph frequency), `default.clock.quantum`, `min-quantum` and `max-quantum` (all the
block size), `clock.power-of-two-quantum = false`, and widens `default.clock.quantum-floor`
(default 4) and `quantum-limit` (default 8192) so the block size fits. A port block sees
`position->clock.duration == blockSize` and `clock.rate.denom == graphFrequency`; the test
"Blocks process at the graph frequency and block size" checks it with 44100 Hz and 256.

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
- **Never tell a listener inside the call that caused it.** Refusals in `FilterPortBlocks::make`
  and `Links::createLink` are told later through `pw_loop_signal_event` on the main loop, as the
  core interfaces require.
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
   always-process nodes (from 1.7.0, load `libpipewire-module-scheduler-v1` in
   `engine.conf`), renamed clock keys, whether `context.properties` still overwrite
   `pw_context_new`'s properties, `pw_filter` and `link-factory` behaviour, and the registry's
   key whitelist.
3. Check each module named in `engine.conf` still exists:
   `scripts/dev.sh bash -c 'ls /usr/lib/*/pipewire-0.3/'`.
4. Run `scripts/dev.sh make test`: `ear_pipewire_tests` cover the engine facts and
   `ear_system_tests` the whole path.
5. Update the tested-against line in `engine.conf`'s header and the version named in this
   skill's first section and in `AGENTS.md`.
