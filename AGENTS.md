# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine
inside its own process and builds an audio graph from a JSON file (`ear.json`): it starts the
nodes the file names, links their ports as the file's routes say, prints a status line for every
change, and stops cleanly on SIGINT or SIGTERM. Milestone 1 has two built-in node types, `white`
(white noise) and `file` (records a stereo WAV file). The system design is
`docs/design/pipewire-graph.md`; milestone 1's module design, frozen interfaces and slices are
`docs/design/m1-white-to-file.md`.

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
scripts/dev.sh bash               # a shell in the image
```

- Needs CMake 3.28 or newer (the `$<COMPILE_ONLY:...>` link and `FetchContent`'s
  `EXCLUDE_FROM_ALL`), pkg-config, `libpipewire-0.3`, `libspa-0.2` and their run-time modules,
  and `sndfile`; the Dockerfile's comments say why each package is there. Configure names every
  pkg-config package it did not find.
- Build output goes to `out/` (gitignored). A post-build step copies the binary to `out/ear`,
  and configure copies `src/pipewire/engine.conf` beside it to `out/engine.conf`.
- Boost 1.92.0 (pinned by SHA-256) is the only fetched dependency; `program_options` is its only
  compiled library. Its source tree is kept in `.cache/deps/boost-src/` (about 800 MB,
  gitignored), which `make clean` keeps; `cmake/KeptFetchContent.cmake` says when a kept tree is
  reused and when it is fetched again. A worktree points `CMAKE_ARGS` at the main checkout's tree
  (as above) instead of fetching its own. Bump Boost by changing `boost_url` and `boost_sha256`
  in `CMakeLists.txt` together.
- ccache: the image's compiler launcher; `scripts/dev.sh` points `CCACHE_DIR` at the main
  checkout's `.cache/ccache`, shared by every worktree.
- `JOBS`, not `make -jN`, sets the build's parallelism, and configure runs only when
  `out/CMakeCache.txt` lacks the target's build type and `BUILD_TESTS` value or `CMAKE_ARGS` is
  given (the `Makefile` says why).

Running it (inside the image, or on a Linux host with the packages):

```bash
./out/ear --config ear.json   # the default; --help lists the options
```

`ear` reads `engine.conf` from beside its own binary. It prints status lines on standard output:

```
node <name> starting|running|stopping
node <name> failed <engine-refused|start-failed>: <detail>
node <name> stopped dropped=<frames>
route <node>:<port> -> <node>:<port> pending|linked|refused[: <detail>]
```

SIGINT or SIGTERM stops every node (a `file` node finishes its WAV file) and exits 0. A graph
file that does not parse, an engine that does not start, or a node or route the graph refuses
(printed as an error code and what it names, for example `unknown-port rec:FX`) exits 1. There
is no lint config.

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
- Each node has a unique `name`, a `type` and optional `settings`, which only the type checks.
  `white` has one output, `MONO`, uniform noise in [-1, 1), and takes no settings. `file` has
  inputs `FL` and `FR` and needs `{"path": "<non-empty string>"}`; it truncates the file at start
  and writes 32-bit float stereo WAV at the graph frequency, complete once the node is gone.
- A route joins one output port to one input port. Nodes are added in file order, then routes;
  the first error stops EAR.

Version 1 (`device`, `airplay`, `connect` pairs) is gone with the JUCE runtime; there is no
compatibility with it.

## Architecture

Four libraries and the binary, each package in `src/<package>/` with its own `CMakeLists.txt`,
public headers in `include/ear/<package>/` and tests in `tests/`:

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | Desired graph (`DesiredGraph`: wanted nodes and routes, checked against declared ports; `CommandError`), node catalogue contract (`NodeType`, `NodeConfig`, `AudioCode`, handoff ends), node framework (`NodeLifecycle`, the one node state machine; `NodeHost`), reconciler (route links and route status) | Any PipeWire call or file access; nlohmann/json is its only dependency |
| `ear_nodes` (`src/nodes`) | The built-in types `white` and `file` | PipeWire (`file` uses libsndfile) |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement core's interfaces: port blocks (`pw_filter`), links (`link-factory`), live ports (the registry), handoffs (`pw_thread_loop`) | Policy |
| `ear_control` (`src/control`) | Reads `ear.json`, replays it as `addNode` and `addRoute` calls; status lines | Graph rules; engine calls |
| `ear` (`src/main.cpp`) | The composition root: wiring only | Computing anything |

Dependencies point one way: `ear_core` <- {`ear_nodes`, `ear_pipewire`, `ear_control`} <- `ear`.
An interface lives with the package that consumes it: `node_host.hpp` declares
`PortBlockFactory` and `HandoffFactory`, `reconciler.hpp` declares `LinkMaker` and the
live-port and link listeners, and `ear_pipewire` implements them. Each package's headers form a
CMake header set that is compiled on its own (`VERIFY_INTERFACE_HEADER_SETS`); `ear_pipewire`'s
headers only forward-declare PipeWire's types, so PipeWire's headers stay private to it.

Events, not polling, move everything. `main.cpp` wires: `DesiredGraph` tells `NodeHost` about
each added node and the `Reconciler` about each added route; `LiveGraph` tells the `Reconciler`
which ports exist; `Links` tells it when a link is active, failed or gone; `NodeHost` and the
`Reconciler` tell `StatusLog`. All of these run on the engine's main-loop thread.

How audio moves:

1. `Engine` (`src/pipewire/engine.cpp`) runs PipeWire's engine in-process: its main loop, a
   context loaded from `engine.conf`, and EAR's own connection to it. There is no daemon, no
   socket and no session manager; a dummy driver clocks the graph at `graphFrequency` with
   `blockSize` frames a cycle. The `pipewire-engine` skill covers the configuration and traps.
2. A node starts (`NodeHost`): its type's `NodeConfig::start` makes the type's insides, then
   `AudioCode::prepare` runs, then `FilterPortBlocks` makes its port block: one
   always-processing `pw_filter` named by the node's name, with one 32-bit float mono port per
   declared port. Its first STREAMING state makes the node `running`. Every block processes
   whether routed or not; an unlinked input reads zeros.
3. On PipeWire's realtime thread each cycle the block calls `AudioCode::process` with one span
   per port. `white` fills its output with noise.
4. `LiveGraph` reports each port as the engine's registry announces it (node name, port name,
   direction, port id). When both ends of a wanted route are live, the `Reconciler` asks `Links`
   for a link by port ids; the route is `linked` once the link is active, and `pending` again
   if an end goes, relinked when it comes back.
5. `file` cannot write a file on the realtime thread. Its audio code writes into a handoff
   (`ThreadHandoffs`, `src/pipewire/handoffs.cpp`): a lock-free ring of interleaved floats (two
   seconds) that wakes the handoff's own thread, where libsndfile writes the frames. A full ring
   drops frames and counts them; the count is the `dropped=` of the node's `stopped` line.
6. Stopping (`NodeHost::stopAll`) takes each node through `stopping` and tears it down in one
   order: port block destroyed (no `process` after that), `release`, handoffs drained and
   finished, insides destroyed; then `stopped`. EAR's main loop quits after the last node is
   gone.

Not built yet (parked in the design page, section 8): the control port (7777) and the rest of
the control calls, serving PipeWire's socket, sound-card output, the program supervisor and the
`airplay` type.

## Tests

Catch2 v2; each package has one test executable, and `catch_discover_tests` makes each test
case its own CTest test, so `ctest --parallel` runs them side by side.

| Executable | What it proves |
|---|---|
| `ear_core_tests` | The desired graph's checks and errors, the node state machine (every state against every fact), `NodeHost` with fake factories, the reconciler. Pure, milliseconds |
| `ear_nodes_tests` | `white`'s ports and noise statistics; `file` writing exact samples through a fake grant, and refusing an unopenable path |
| `ear_control_tests` | The graph-file parser (error pointers, unknown keys) and replay order |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample over a link, the clock, teardown, SIGTERM; and the handoff (order, drops, close, no allocation on the writer) |
| `ear_system_tests` | The owner's end-to-end tests: spawn the real `out/ear` on a graph file, wait for `linked`, record about a second, send SIGTERM, then read the WAV back (noise on the routed channel only, exact zeros on the other, header finished, `dropped=0`); a route to an undeclared port exits 1 |

`test/CMakeLists.txt` also runs `ear --help` (the binary loads its shared libraries), and
`ear_engine_conf_beside_ear` checks the copy beside the binary matches the source. Tests are
event-gated: they wait for the event they need, and a deadline only fails a test, never decides
when something happens. Verify audio here, never on real hardware. Run one, inside the image:

```bash
scripts/dev.sh ctest --test-dir out -R "lands on FL" --output-on-failure   # by test name regex
scripts/dev.sh out/src/nodes/ear_nodes_tests "[white]"                     # by Catch tag
```

## CI

`.github/workflows/test.yml` runs on every pull request and every push to `master`: one
`ubuntu-24.04` job builds the image with buildx (layers cached in GitHub's cache), restores the
Boost tree and ccache, and runs `scripts/dev.sh make test` with `EAR_DEV_IMAGE=ear-dev` so the
script uses that image. On failure it uploads ctest's logs as `ctest-output`. Its comments give
the reasons for each step and cache.

## Vendored and fetched code

`vendor/json` (nlohmann) and `vendor/catch` (Catch 2.12.2) are used; `vendor/jsonrpccxx`,
`vendor/jsonrpc` and `vendor/libjsonrpc` are not built (left for the control port). All were
added with `git subtree`; leave them alone unless you are updating a dependency.
`test/main.cpp` turns off Catch's POSIX signal handlers, which do not compile against glibc 2.34
and newer (its comment says why; Catch 2.13.5 fixes it). Boost is fetched, not vendored (see
Build and run). PipeWire and libsndfile come from the image's Debian packages.

## PipeWire references

The `pipewire-engine` skill (`.claude/skills/pipewire-engine/SKILL.md`) explains running the
engine in-process: what `engine.conf` sets and why, the modules, the dummy driver, running
without a session manager, the traps found while building it, and how to bump PipeWire. The
PipeWire source behind each engine fact is cited in `docs/design/m1-white-to-file.md`
section 1.
