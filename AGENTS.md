# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine
inside its own process and builds an audio graph from a JSON file (`ear.json`): it starts the
nodes the file names, links their ports as the file's routes say, prints a status line for every
change, and stops cleanly on SIGINT or SIGTERM. While it runs, its control port (7777) serves a
graph editor in the browser and a JSON-RPC interface that reads and edits the graph live and
streams every change and per-port audio levels. The built-in node types are `white` (white
noise), `file` (records a stereo WAV file) and `card` (a sound card through ALSA). It ships as a
container image published from GitHub Actions. The system design is
`docs/design/pipewire-graph.md`; milestone 1's module design is `docs/design/m1-white-to-file.md`
and batch 2's (control port, graph editor, sound card, image) is `docs/design/batch2.md`.

## Build and run

EAR builds and runs on Linux only (it links PipeWire); configure stops on any other platform.
Build and test in the project's image, `docker/Dockerfile` (Debian 13 "trixie": PipeWire 1.4.2,
GCC 14, CMake 3.31), through `scripts/dev.sh`, which builds the image when the Dockerfile
changed, mounts the checkout at the same path and runs the command as you (its header says
how). On a Mac this is the only way to build; it needs Docker.

```bash
scripts/dev.sh make test          # configure Debug with -DBUILD_TESTS=ON, build everything, run ctest
scripts/dev.sh make debug         # configure Debug with -DBUILD_TESTS=OFF and build ear
scripts/dev.sh make release       # same with Release
scripts/dev.sh make JOBS=4 test   # JOBS sets build and ctest parallelism (default: the CPU count)
scripts/dev.sh make clean         # rm -rf out
scripts/dev.sh make test CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_BOOST=<path>/.cache/deps/boost-src
scripts/dev.sh make web           # npm ci, then build the graph editor into web/dist
scripts/dev.sh make web-test      # npm ci, typecheck, vitest, build
scripts/dev.sh bash               # a shell in the image
```

- Needs CMake 3.28 or newer (the `$<COMPILE_ONLY:...>` link and `FetchContent`'s
  `EXCLUDE_FROM_ALL`), pkg-config, `libpipewire-0.3`, `libspa-0.2` and their run-time modules,
  and `sndfile`; the Dockerfile's comments say why each package is there. Configure names every
  pkg-config package it did not find.
- Build output goes to `out/` (gitignored). A post-build step copies the binary to `out/ear`,
  and configure copies `src/pipewire/engine.conf` beside it to `out/engine.conf`.
- Boost 1.92.0 (pinned by SHA-256) is the only fetched C++ dependency; `program_options` is its
  only compiled library, and Beast and Asio (header-only) serve the control port; configure fails
  naming Beast if it is not there. Its source tree is kept in `.cache/deps/boost-src/` (about 800 MB,
  gitignored), which `make clean` keeps; `cmake/KeptFetchContent.cmake` says when a kept tree is
  reused and when it is fetched again. A worktree points `CMAKE_ARGS` at the main checkout's tree
  (as above) instead of fetching its own. Bump Boost by changing `boost_url` and `boost_sha256`
  in `CMakeLists.txt` together.
- ccache: the image's compiler launcher; `scripts/dev.sh` points `CCACHE_DIR` at the main
  checkout's `.cache/ccache`, shared by every worktree.
- The graph editor (`web/`: React, React Flow, Vite, TypeScript, Vitest) builds with the image's
  Node.js 24 (`web/.nvmrc`); `web/package-lock.json` pins its packages and `npm ci` installs
  exactly those. npm's cache is the main checkout's `.cache/npm`.
- `JOBS`, not `make -jN`, sets the build's parallelism, and configure runs only when
  `out/CMakeCache.txt` lacks the target's build type and `BUILD_TESTS` value or `CMAKE_ARGS` is
  given (the `Makefile` says why).

Running it (inside the image, or on a Linux host with the packages):

```bash
./out/ear --config ear.json   # the default; --help lists the options; file nodes write into ./recordings
./out/ear --web-root web/dist # serve the editor built by make web
```

| Flag | Default | What |
|---|---|---|
| `--config` | `ear.json` | the graph file |
| `--control-address` | `0.0.0.0` | the control port's address |
| `--control-port` | `7777` | its port; `0` takes any free one (tests) |
| `--web-root` | `<directory of ear>/web` | the editor's built files |
| `--allowed-host` | none | a DNS name the Host header may carry (repeatable) |
| `--recordings-dir` | `recordings` | the folder `file` nodes write into; made at boot if missing (not its parents), and a folder it cannot make or open exits 1 |

`ear` reads `engine.conf` from beside its own binary. It prints how its audio thread is
scheduled (`engine: realtime SCHED_FIFO <n>` or `engine: not realtime (SCHED_OTHER)`), then
`control listening on <address>:<port>`, then status lines on standard output:

```
node <name> starting|running|stopping
node <name> failed <engine-refused|start-failed>: <detail>
node <name> stopped dropped=<frames>
route <node>:<port> -> <node>:<port> pending|linked|refused[: <detail>]
```

SIGINT or SIGTERM first refuses graph edits (clients get `stopping`), stops every node (a
`file` node finishes its WAV file), closes every client with WebSocket code 1001 and exits 0. A
graph file that does not parse, an engine that does not start, a control port it cannot bind,
or a node or route the graph refuses at boot (printed as an error code and what it names, for
example `unknown-port rec:FX`) exits 1. There is no lint config.

## Control port and graph editor

Port 7777 serves the editor at `/` and JSON-RPC 2.0 over WebSocket at `/rpc`: `graph.get`,
`graph.subscribe` (the graph, then every change), `levels.subscribe` (per-port peak and RMS 30
times a second), `node.add`, `node.update`, `node.remove`, `route.add` and `route.remove`. The
wire format's one home is `docs/design/batch2.md` section 6 and the fixtures in
`test/protocol/`. Live edits are not saved; `ear.json` loads at boot.

```bash
node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get   # one call from a script
```

There is no login: anyone who reaches the port can edit the graph. It answers only requests
whose Host is an IP address, `localhost` or an `--allowed-host` name, and refuses WebSocket
upgrades from another site's page. The port runs on its own thread, never on PipeWire's; graph
edits cross to PipeWire's main loop. The `control-port` skill covers talking to it, the rules,
the threads and the traps.

The editor (`web/src/`) draws ear's nodes as boxes with port handles and live meters and its
routes as wires coloured by status and drawn by level; dragging from an output to an input adds
a route, the delete key removes the selected wire or node, a panel adds nodes (type and JSON
settings) and edits settings, and a badge shows when the audio thread is not realtime. It draws
only what ear announced; node positions live in the browser. On a lost socket it reconnects
(1 s doubling to 30 s) and subscribes again. In development, `npm run dev` (Vite, hot reload)
forwards `/rpc` to an ear on `localhost:7777`; `scripts/dev.sh`'s header shows how to publish
its port.

## Graph file (`ear.json`, version 2)

Not committed; read by `ear::control::parseGraphFile` (`src/control/graph_file.cpp`):

```json
{
  "graphFrequency": 48000,
  "blockSize": 256,
  "nodes": [
    { "name": "noise", "type": "white" },
    { "name": "rec", "type": "file", "settings": { "path": "/tmp/rec.wav" } }
  ],
  "routes": [
    { "from": { "node": "noise", "port": "MONO" }, "to": { "node": "rec", "port": "FL" } }
  ]
}
```

- `graphFrequency` (Hz) and `blockSize` (samples per cycle) are required whole numbers above
  zero and fixed at boot; the engine runs at exactly these.
- `nodes` and `routes` may be left out (read as empty). Unknown keys are errors at every level,
  and an error names the file and the JSON pointer of the bad member.
- Each node has a unique `name` (1 to 64 letters, digits, `-` and `_`), a `type` and optional
  `settings`, which only the type checks.
  `white` has one output, `MONO`, uniform noise in [-1, 1), and takes no settings. `file` has
  inputs `FL` and `FR` and needs `{"path": "<path>"}`, relative to the recordings folder
  (`--recordings-dir`): non-empty, not absolute, no `..` component, else invalid settings. It is
  opened beneath the folder only (`openat2` with `RESOLVE_BENEATH` on the folder's descriptor,
  held from boot), so a symlink that leads out fails the node's start; a missing subfolder fails
  it too (none is made). It truncates the file at start and writes 32-bit float stereo WAV at the
  graph frequency, complete once the node is gone.
- `card` is a sound card: `{"card": "<card id>", "playback": <channels>, "capture":
  <channels>}`. The card id is the ALSA name in `/proc/asound/cards` (letters, digits, `-`,
  `_`, at most 15), never an index or an ALSA device string; each count is 0 to 64 (default 0),
  at least one above 0. Inputs `playback_1`..`playback_N`, outputs `capture_1`..`capture_M`. Its
  rate is the graph frequency. The card drives the graph's clock only while it works; a card that
  cannot open or is lost makes the node `failed engine-refused: <detail>`, its routes stay, and
  the rest of the graph keeps running on the dummy driver. `node.update` retries it.
- A route joins one output port to one input port. Nodes are added in file order, then routes;
  the first error stops EAR.

Version 1 (`device`, `airplay`, `connect` pairs) is gone with the JUCE runtime; there is no
compatibility with it.

## Architecture

Five libraries, the binary and the editor, each C++ package in `src/<package>/` with its own
`CMakeLists.txt`, public headers in `include/ear/<package>/` and tests in `tests/`:

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | Desired graph (`DesiredGraph`: wanted nodes and routes, checked against declared ports; edits, observers told before subscribers, `close()` when stopping; `CommandError`), node catalogue contract (`NodeType`, `NodeConfig`, `AudioCode`, handoff ends, sound card requests, `CardId`), node framework (`NodeLifecycle`, the one node state machine; `NodeHost`: a node's port block lives as long as the node, its insides start, restart and fail behind it), reconciler (route links and route status), levels (`LevelMeter`), the graph read (`GraphStatus`), the executor interface | Any PipeWire call, socket, thread or file access; nlohmann/json is its only dependency |
| `ear_nodes` (`src/nodes`) | The built-in types `white`, `file` and `card` | PipeWire and ALSA (`file` uses libsndfile; `card` asks the framework for its cards) |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement core's interfaces: port blocks (`pw_filter`, with level metering), links (`link-factory`), live ports (the registry), handoffs (`pw_thread_loop`), sound cards (ALSA adapter nodes with their own links), the main-loop executor, the scheduling report | Policy |
| `ear_control` (`src/control`) | Reads `ear.json` (one decoder of node and route JSON, shared with the control port) and replays it; status lines; the JSON-RPC protocol (envelope, commands, events, sessions and subscriptions); `MainLoopGateway`, the one crossing between the main loop and the control thread | Sockets; graph rules; holding the graph on the control thread |
| `ear_server` (`src/server`) | The control thread and port 7777 (Boost.Beast): the editor's files, `/rpc`, Host and Origin checks, per-client queues, the level bell | Graph state; PipeWire |
| `ear` (`src/main.cpp`) | The composition root: wiring, boot and stop order | Computing anything |
| `web/` | The graph editor; shares no code with C++, only the wire format and `test/protocol/` | Graph rules (ear is the one source of truth) |

Dependencies point one way: `ear_core` <- {`ear_nodes`, `ear_pipewire`, `ear_control`},
`ear_control` <- `ear_server`, and all of them <- `ear`. No Boost type is in a public header.
An interface lives with the package that consumes it: `node_host.hpp` declares
`PortBlockFactory` and `HandoffFactory`, `reconciler.hpp` declares `LinkMaker` and the
live-port and link listeners, and `ear_pipewire` implements them. Each package's headers form a
CMake header set that is compiled on its own (`VERIFY_INTERFACE_HEADER_SETS`); `ear_pipewire`'s
headers only forward-declare PipeWire's types, so PipeWire's headers stay private to it.

Events, not polling, move everything. `main.cpp` wires: `DesiredGraph` tells its observer (the
gateway) of each change first, then `NodeHost` about nodes and the `Reconciler` about routes;
`LiveGraph` tells the `Reconciler` which ports exist; `Links` tells it when a link is active,
failed or gone; `NodeHost` and the `Reconciler` tell `StatusLog` and the gateway. So a change's
notification always comes before the statuses it causes.

Threads (the owner, 2026-10-30: "The control port shouldn't run in the PipeWire context, because
it will not be real time"):

| Thread | Runs |
|---|---|
| PipeWire's data loop (realtime) | every port block's `process`, metering and level windows; the handoff writers |
| PipeWire's main loop (the main thread) | every piece of graph state: the engine, desired graph, node framework, reconciler, adapters, the gateway's graph side |
| The control thread | port 7777: sockets, JSON-RPC, sessions, queues; it holds only `GraphGateway`, which posts commands to the main loop (`MainLoopExecutor`) and gets replies and events back in order |
| Handoff threads | the `file` node's writer |

How audio moves:

1. `Engine` (`src/pipewire/engine.cpp`) runs PipeWire's engine in-process: its main loop, a
   context loaded from `engine.conf`, and EAR's own connection to it. There is no daemon, no
   socket and no session manager; a dummy driver clocks the graph at `graphFrequency` with
   `blockSize` frames a cycle. The `pipewire-engine` skill covers the configuration and traps.
2. A node starts (`NodeHost`) block first: `FilterPortBlocks` makes its port block, once for
   the node's life: one always-processing `pw_filter` named by the node's name, with one 32-bit
   float mono port per declared port. Then its type's `NodeConfig::start` makes the type's
   insides, `AudioCode::prepare` runs and the block is driven with that code; new settings
   restart the insides behind the same block. The node is `running` once the block is ready
   (its first STREAMING state) and the insides are ready, every sound card they asked for
   included (`NodeLifecycle`). Every block processes whether routed or not; an unlinked input
   reads zeros.
3. On PipeWire's realtime thread each cycle the block calls `AudioCode::process` with one span
   per port. `white` fills its output with noise.
4. `LiveGraph` reports each port as the engine's registry announces it (node name, port name,
   direction, port id). When both ends of a wanted route are live, the `Reconciler` asks `Links`
   for a link by port ids; the route is `linked` once the link is active, and `pending` again
   if an end goes, relinked when it comes back.
5. A `card` node's port block has inside ports no route can name. Once the block is ready the
   framework asks `AlsaSoundCards` for one ALSA adapter node per direction; it runs alone until
   its PCM runs, then links its channels to the inside ports, and through the blocks' graph
   group (`node.group = ear.graph`) it drives every block. A card that fails tears down its node
   and links before it is reported lost, so the dummy driver takes over at once. The
   `pipewire-engine` skill has the details and traps.
6. Levels: each block meters its public ports every cycle and publishes a window (1/30 s of the
   graph clock, on the same clock positions for every block) into a lock-free mailbox, then
   rings an eventfd; the control thread reads every block's latest window and sends it to levels
   subscribers.
7. `file` cannot write a file on the realtime thread. Its audio code writes into a handoff
   (`ThreadHandoffs`, `src/pipewire/handoffs.cpp`): a lock-free ring of interleaved floats (two
   seconds) that wakes the handoff's own thread, where libsndfile writes the frames. A full ring
   drops frames and counts them; the count is the `dropped=` of the node's `stopped` line.
8. Stopping: `DesiredGraph::close()` refuses every later edit, then `NodeHost::stopAll` takes
   each node through `stopping` and tears it down in one order: port block destroyed (no
   `process` after that), `release`, sound cards destroyed, handoffs drained and finished,
   insides destroyed; then `stopped`. A restart (`node.update`) runs the same teardown but keeps
   the block, only silencing it (`drive(nullptr)`) first. After the last node, the control port
   closes every client (1001), the control thread stops and EAR's main loop quits.

Not built yet: serving PipeWire's socket, the program supervisor and the `airplay` type, Music
Assistant's Sendspin (next batch), a login on port 7777, saving live edits, card retry and
hotplug, arm64 images.

## Tests

Catch2 v2; each package has one test executable, and `catch_discover_tests` makes each test
case its own CTest test, so `ctest --parallel` runs them side by side.

| Executable | What it proves |
|---|---|
| `ear_core_tests` | The desired graph's checks, edits and errors, observer order, closing; the node state machine (every state against every fact); `NodeHost` with fake factories (restart, removal, sound cards, a lost block); the reconciler; levels; the graph read; card ids. Pure, milliseconds |
| `ear_nodes_tests` | `white`'s ports and noise statistics; `file` writing exact samples through a fake grant, refusing an unopenable path, absolute and `..` paths (invalid settings) and a symlink out of the recordings folder (at start); the recordings folder made at boot, never its parents; `card`'s settings, ports and pass-through |
| `ear_control_tests` | The graph-file parser (error pointers, unknown keys) and replay order; the JSON-RPC envelope (the spec's examples), commands, events, gateway and sessions, all against `test/protocol` |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample over a link, the clock, teardown, SIGTERM; the handoff (order, drops, close, no allocation on the writer); the graph group, `drive`, level windows on the clock, the main-loop executor, the scheduling report; sound cards through a test ALSA card (alsa-lib's file and null PCMs): driving, channels sample for sample, a card that cannot open, a lost card |
| `ear_server_tests` | Port 7777 on real sockets with a fake main loop and gateway: files, Host and Origin, round trips, the 1008 bound, a client that resets, stop with 1001, levels |
| `ear_system_tests` | The owner's end-to-end tests, through the real `out/ear` on a graph file: white noise recorded to the routed channel only (exact zeros on the other, header finished, `dropped=0`); a route to an undeclared port exits 1, as does a recordings folder it cannot make; and through the control port: the scheduling and listening lines and the editor's files, a client's edits heard in the WAV in event order, levels, removal and errors, file paths outside the recordings folder refused with nothing written, edits during stop refused as `stopping`, a sound card node linked |
| web (vitest, `make web-test`) | The editor's reducer against every `test/protocol` fixture, the reconnecting client with a fake socket, connection rules, levels and layout. No browser; rendering is checked by eye |

`test/CMakeLists.txt` also runs `ear --help` (the binary loads its shared libraries), and
`ear_engine_conf_beside_ear` checks the copy beside the binary matches the source. Tests are
event-gated: they wait for the event they need, and a deadline only fails a test, never decides
when something happens. Verify audio here, never on real hardware. Run one, inside the image:

```bash
scripts/dev.sh ctest --test-dir out -R "lands on FL" --output-on-failure   # by test name regex
scripts/dev.sh out/src/nodes/ear_nodes_tests "[white]"                     # by Catch tag
```

## CI

`.github/workflows/test.yml` runs on every pull request and every push to `master`, three
`ubuntu-24.04` jobs in parallel: `test` builds the dev image with buildx (layers cached in
GitHub's cache), restores the Boost tree and ccache, and runs `scripts/dev.sh make test` with
`EAR_DEV_IMAGE=ear-dev` (on failure it uploads ctest's logs as `ctest-output`); `web` runs `make
web-test` in the same image; `image` checks `scripts/version-tags.sh`, builds the run image and
runs `scripts/image-smoke.sh` on it. Its comments give the reasons for each step and cache.

## Run image and releases

`docker/Dockerfile` has three targets: `dev` (the build and test image: Debian 13, PipeWire,
GCC 14, CMake, Node.js 24), `build` (`make release` and the editor's build) and `run`, what
ships: `debian:trixie-slim` with only the run-time libraries, no compiler; `/opt/ear/ear`,
`engine.conf` and `web/`; user `ear` (uid 10001); port 7777; default `--config
/etc/ear/ear.json --recordings-dir /var/lib/ear/recordings`, a volume owned by `ear` (arguments
given to the container replace both, so pass them again). The binary carries CAP_SYS_NICE as a permitted-only file capability that
EAR makes effective at boot, so the audio thread is realtime where the container is given
SYS_NICE (`docker run --cap-add SYS_NICE`) and plain elsewhere; EAR prints which, and
`graph.get` says it. Built for amd64 only until the Talos nodes' CPU is known.

```bash
docker build --target run --tag ear:local --file docker/Dockerfile .
scripts/image-smoke.sh ear:local   # what CI checks: user, no compiler, files, a graph, graph.get
```

`.github/workflows/image.yml` runs on every push to `master` (and by hand), never cancelled:
release-please keeps a release pull request open from master's Conventional Commits, and
merging it makes the release, its `v<version>` tag and the version in `CMakeLists.txt` (the
`x-release-please-version` line). Then the run image is pushed to `ghcr.io/mcurcio/ear` tagged
`master`, `sha-<short commit>` and one version per `v*` tag on the commit
(`scripts/version-tags.sh` reads them from git, so a rerun of a failed publish still pushes the
version). Never tag or version by hand. After the first merge the owner must let GitHub Actions
create pull requests (Settings, Actions) for release-please. The ghcr package is public
(anonymous pulls work). Running it on Talos with the MOTU: the `ear-on-talos` skill.

## Vendored and fetched code

`vendor/json` (nlohmann) and `vendor/catch` (Catch 2.12.2) are used; `vendor/jsonrpccxx`,
`vendor/jsonrpc` and `vendor/libjsonrpc` are not built (the control port has its own JSON-RPC
code in `ear_control`; removing them is deferred). All were
added with `git subtree`; leave them alone unless you are updating a dependency.
`test/main.cpp` turns off Catch's POSIX signal handlers, which do not compile against glibc 2.34
and newer (its comment says why; Catch 2.13.5 fixes it). Boost is fetched, not vendored (see
Build and run). PipeWire, alsa-lib and libsndfile come from the image's Debian packages; the
editor's npm packages are pinned by `web/package-lock.json`.

## Skills and references

Repo skills in `.claude/skills/`:

- `pipewire-engine`: running the engine in-process: what `engine.conf` sets and why, the
  modules, the dummy driver, the graph group, sound cards and the clock, `drive`, levels,
  realtime priority, running without a session manager, the traps, and how to bump PipeWire.
  The PipeWire source behind each engine fact is cited in `docs/design/m1-white-to-file.md`
  section 1 and `docs/design/batch2.md` sections 4.3 and 5.
- `control-port`: talking to port 7777, its Host and Origin rules, threads, queues, stopping
  and traps.
- `ear-on-talos`: the run image on a Talos node with the MOTU 24Ao (kernel module, privileged
  namespace, `/dev/snd`, SYS_NICE, the card's own settings, checks on the hardware).
