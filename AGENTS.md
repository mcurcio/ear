# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine
inside its own process and builds an audio graph from its graph file, `ear.conf`, written in
the knobly configuration language (mcurcio/knobly): it starts the nodes the file names, links
their ports as the file's routes say, prints a status line for every change, and stops cleanly
on SIGINT or SIGTERM. While it runs it keeps the file live: an edit made in the browser is
applied to the running graph and written back into `ear.conf` (comments and layout kept), and a
hand edit to `ear.conf` is applied and shown in the browser. Its control port (7777) serves a
graph editor, with a text pane for `ear.conf`, and a JSON-RPC interface that reads and edits
the graph and the file and streams every change and per-port audio levels. The built-in node
types are `white` (white noise), `file` (records a stereo WAV file), `card` (a sound card
through ALSA) and `rtp` (one RTP audio stream received from the network, such as FPP's). It
ships as a container image published from GitHub Actions. The system design is
`docs/design/pipewire-graph.md`; milestone 1's module design is `docs/design/m1-white-to-file.md`,
batch 2's (control port, graph editor, sound card, image) is `docs/design/batch2.md`, the `rtp`
node's is `docs/design/rtp.md`, and the graph file's is `docs/design/knobly-config.md` (the
owner's words for it: `docs/design/knobly-config-notes.md`).

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
  reused and when it is fetched again. The tree lives in each checkout's own `.cache/deps/`: a
  worktree with no `CMAKE_ARGS` fetches and keeps its own copy. To avoid that, point `CMAKE_ARGS`
  at a tree another checkout already keeps (as above); the main checkout may have none, as it
  may be deleted to save space. Bump Boost by changing `boost_url` and `boost_sha256` in
  `CMakeLists.txt` together.
- knobly (the graph file's language) comes from the dev image's `/opt/knobly`: its C++ release
  tree (CMake project and headers, its generated ones included), which the Dockerfile's
  throwaway `knobly` stage builds from a pinned commit with knobly's own release script (it needs
  python3, uv and a clang-format download, at image build only; configure needs none of them).
  ear adds it as a subproject in compiled mode (`src/config/knobly_implementation.cpp`), and only
  `ear_config` links it. A Linux build outside the image points the CMake cache variable
  `EAR_KNOBLY_DIR` at such a tree (`CMAKE_ARGS=-DEAR_KNOBLY_DIR=<path>`); configure fails naming
  it when `include/knobly/knobly.hpp` is not there. Bump knobly by changing `KNOBLY_COMMIT` in
  `docker/Dockerfile` (the pin's one home); `scripts/dev.sh` rebuilds the image.
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
./out/ear                      # ./config/ear.conf; --help lists the options; file nodes write into ./recordings
./out/ear --config-dir ~/ear-config --web-root web/dist   # another graph file folder; the editor built by make web
```

| Flag | Default | What |
|---|---|---|
| `--config-dir` | `config` | the graph file folder, which holds `ear.conf`; made at boot if missing (not its parents). It must be writable for edits to be saved. It may not be the recordings folder or the web root, nor inside or around either (exit 1, naming both) |
| `--graph-frequency` | `48000` | the engine's rate, used only when `ear.conf` cannot be read or does not give `graph_frequency` and `block_size` |
| `--block-size` | `256` | the engine's block size, under the same rule |
| `--control-address` | `0.0.0.0` | the control port's address |
| `--control-port` | `7777` | its port; `0` takes any free one (tests) |
| `--web-root` | `<directory of ear>/web` | the editor's built files |
| `--allowed-host` | none | a DNS name the Host header may carry (repeatable) |
| `--recordings-dir` | `recordings` | the folder `file` nodes write into; made at boot if missing (not its parents), and a folder it cannot make or open exits 1 |

`ear` reads `engine.conf` from beside its own binary. It prints how its audio thread is
scheduled (`engine: realtime SCHED_FIFO <n>` or `engine: not realtime (SCHED_OTHER)`), then
`control listening on <address>:<port>`, then status lines on standard output (their one home
is `src/control/status_log.cpp`):

```
node <name> starting|running|stopping
node <name> failed <engine-refused|start-failed>: <detail>
node <name> stopped dropped=<frames>
route <node>:<port> -> <node>:<port> pending|linked|refused[: <detail>]
graph-file <edit|disk|write> <version> running=<version|none>
graph-file error <line>:<column> <code>: <message>
graph-file refused <code>[ at <line>:<column>]
graph-file dropped <n> edits
graph-file pending-restart <graphFrequency|blockSize>...
graph-file write-failed <detail>
graph-file written <version>
```

A `graph-file <cause>` line comes with each change to the graph file (`disk` for the boot file
and for hand edits, `edit` for an edit through the control port, `write` when a write failed or
recovered), before the node and route lines it causes; the lines under it say what that version
holds: each error, the graph's refusal, edits the hand edit dropped, settings that apply only at
restart, a write the disk refused. `graph-file written` follows each write of `ear.conf`.

A boot whose `ear.conf` is missing, has errors, or is refused by the graph (a route to a port
that does not exist) runs nothing: ear stays up, prints why, and the editor's pane shows it
until the file is fixed (by hand or with Save). SIGINT or SIGTERM first refuses graph edits
(clients get `stopping`), stops every node (a `file` node finishes its WAV file), closes every
client with WebSocket code 1001, writes the last accepted version of `ear.conf` if it was not
written yet (a failure is printed as `graph-file write-failed <detail>`, and reaches nowhere
else) and exits 0. Bad flags, a folder it cannot make or open, folders that overlap, an engine
that does not start, a folder watch it cannot arm, or a control port it cannot bind exits 1.
There is no lint config.

## Control port and graph editor

Port 7777 serves the editor at `/` and JSON-RPC 2.0 over WebSocket at `/rpc`: `graph.get`,
`graph.subscribe` (the graph, then every change), `levels.subscribe` (per-port peak and RMS 30
times a second), `node.add`, `node.update`, `node.remove`, `route.add` and `route.remove`
(each an edit made at `ear.conf`'s source), and `graphFile.get`, `graphFile.subscribe` (the
file's text, version, diagnostics and state, then every `graphFile.changed`) and
`graphFile.set` (a whole new text, based on a version). The wire format's one home is
`docs/design/batch2.md` section 6 and the fixtures in `test/protocol/`. Every accepted edit is
written back to `ear.conf` once the keeper is free (write-behind), so it survives a restart
unless a hand edit lands first or ear crashes before the write.

```bash
node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get   # one call from a script
```

There is no login: anyone who reaches the port can edit the graph. It answers only requests
whose Host is an IP address, `localhost` or an `--allowed-host` name, and refuses WebSocket
upgrades from another site's page. The port runs on its own thread, never on PipeWire's; graph
requests go to the graph file keeper's thread, and graph edits cross from there to PipeWire's
main loop. The `control-port` skill covers talking to it, the rules, the threads and the traps.

The editor (`web/src/`) draws ear's nodes as boxes with port handles and live meters and its
routes as wires coloured by status and drawn by level; dragging from an output to an input adds
a route, the delete key removes the selected wire or node, a panel adds nodes (type and JSON
settings) and edits settings, and a badge shows when the audio thread is not realtime. A text
pane shows `ear.conf` with its diagnostics (a click puts the caret there) and saves it with
Save (`graphFile.set`); a draft is never overwritten by a newer file (the pane says the file
changed and offers a reload), and banners say when the graph refused the file, a write failed,
editor edits were dropped by a hand edit, or a setting applies only at restart. It draws only
what ear announced and never parses knobly; node positions live in the browser. On a lost
socket it reconnects (1 s doubling to 30 s) and subscribes again. In development, `npm run dev`
(Vite, hot reload) forwards `/rpc` to an ear on `localhost:7777`; `scripts/dev.sh`'s header
shows how to publish its port.

## Graph file (`ear.conf`)

`ear.conf` in the graph file folder (`--config-dir`) is the one graph file (no includes yet). It
is not committed, it is written in knobly (`mcurcio/knobly`), and ear keeps it live
(`src/config/`, `docs/design/knobly-config.md`):

```conf
graph_frequency 48000;
block_size 256;
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:MONO rec:FL;
route noise:MONO desk:playback_1;
```

- Only ear's four directives are allowed: `graph_frequency <Hz>;` and `block_size <samples>;`
  (whole numbers above zero, both required), `node <type> <name>;` or `node <type> <name> {
  <settings> }`, and `route <node>:<port> <node>:<port>;` (from an output to an input). Anything
  else is `ear/unknown-directive`; a malformed one is `ear/bad-directive`, and a missing
  `graph_frequency` or `block_size` is `ear/missing-directive`.
- knobly's `@set` variables and `@snippet` with `@use` work. `@include` is refused
  (`ear/include-unsupported`: "@include is not supported yet: write the whole graph in
  ear.conf"), so no other file is ever read.
- Each setting is read and written as the kind its node type declares (`NodeType::settingKinds`):
  text (a bare word or a quoted string) or a whole number. A key the type does not declare is
  invalid settings.
- Problems are diagnostics, each with a severity, a code (knobly's, such as `E2001`, or ear's
  `ear/...`), a message and a line and column. A file with any error does not apply: the graph
  keeps what it ran, the editor shows why, and `node.*` and `route.*` edits are refused
  (`file-invalid`) until the text is fixed by hand or with Save.
- `graph_frequency` and `block_size` are fixed at boot, and the engine runs at exactly these.
  When the file cannot be read or does not give both, the engine takes `--graph-frequency` and
  `--block-size`. A file whose two values differ from the engine's still runs, and says
  `pendingRestart` (they apply at the next start).
- Edits at the source: an editor edit changes the file where the value is defined. Setting
  `playback 4` on `desk` rewrites `@set $ch 4;`, so both cards change in one commit; setting
  `card` on `desk` rewrites only `MOTU` on its `@use` line. An edit that would have to change or
  remove a line that also makes other nodes or routes (removing `playback` from `desk`, which
  sits in the shared snippet) is refused as `shared-definition`, with that line's place.
  `node.remove` removes the node's line and every `route` line naming it; added nodes and routes
  are appended. Comments and layout survive.
- Every change is judged first: an edit becomes the working copy, is applied and is written only
  if the desired graph accepts it. A node whose ports change (a card's channel count) is
  replaced, routes included.
- Hand edits win. ear watches the folder (inotify): whenever `ear.conf` differs from what ear
  last read or wrote, ear reloads it, and accepted editor edits not yet written are dropped and
  counted (the editor says so). A hand edit the graph refuses is shown, and the graph keeps
  what it ran.
- `ear.conf` is never read or written through a symlink, and at most 1 MiB is read; a symlink, a
  directory, another kind of file or a larger file is `ear/unreadable-file`, and nothing is
  written while it is. A missing file is `ear/missing-file`; `graphFile.set` (the pane's Save)
  creates it. ear writes by swapping a new file in (`renameat2(RENAME_EXCHANGE)`) and keeps it
  only if the file it replaced is what it last read or wrote, keeping the file's mode.
- Node names are unique, 1 to 64 letters, digits, `-` and `_`.
- `white` has one output, `MONO`, uniform noise in [-1, 1), and takes no settings.
- `file` has inputs `FL` and `FR` and needs `path` (text), relative to the recordings folder
  (`--recordings-dir`): non-empty, not absolute, no `..` component, else invalid settings. It is
  opened beneath the folder only (`openat2` with `RESOLVE_BENEATH` on the folder's descriptor,
  held from boot), so a symlink that leads out fails the node's start; a missing subfolder fails
  it too (none is made). It truncates the file at start and writes 32-bit float stereo WAV at the
  graph frequency, complete once the node is gone.
- `card` is a sound card: `card` (text: the card id), `playback` and `capture` (whole numbers,
  channels). The card id is the ALSA name in `/proc/asound/cards` (letters, digits, `-`, `_`, at
  most 15), never an index or an ALSA device string; each count is 0 to 64 (default 0), at least
  one above 0. Inputs `playback_1`..`playback_N`, outputs `capture_1`..`capture_M`. Its rate is
  the graph frequency. The card drives the graph's clock only while it works; a card that cannot
  open or is lost makes the node `failed engine-refused: <detail>`, its routes stay, and the rest
  of the graph keeps running on the dummy driver. `node.update` retries it.
- `rtp` receives one RTP stream (`docs/design/rtp.md`), for example `node rtp fpp { address
  "0.0.0.0"; port 5004; format "L24"; channels 2; rate 48000; latencyMs 100; }`. `address`
  (text), `port` (1 to 65535), `format` (text: `L16` or `L24`, big-endian PCM) and `channels` (1
  to 64) are required; `rate` (8000 to 192000 Hz) defaults to the graph frequency, and a sender
  at another rate is resampled to it; `latencyMs` (1 to 1000) is the receive buffer, default
  100. Outputs `channel_1`..`channel_N`, no inputs. `address` is `0.0.0.0` (any sender) or an
  IPv4 multicast group (224.0.0.0 to 239.255.255.255) as a plain dotted quad. A unicast address
  is refused, because PipeWire would hear only that sender, and only when it sends from the same
  port it sends to; a host name is refused, because looking it up blocks the main loop. The
  stream follows the graph's clock and never drives it. With no packets the node is `running`
  and silent (its meters show it); a sender that restarts is heard again. A port another program
  holds leaves the node `starting` for good with silent outputs: PipeWire only logs the failed
  bind. A packet lost or out of order resets the receiver, so about `latencyMs` of silence
  follows. Tested only with a sender on 127.0.0.1; multicast is not tested yet.

`ear.json` and its parser are gone; there is no compatibility with it.

## Architecture

Seven libraries, the binary and the editor, each C++ package in `src/<package>/` with its own
`CMakeLists.txt`, public headers in `include/ear/<package>/` and tests in `tests/`:

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | Desired graph (`DesiredGraph`: wanted nodes and routes, checked against declared ports; the one judge of every change: `plan` the whole wanted graph, then `commit` it, observers told before subscribers; `close()` when stopping; `CommandError`), node catalogue contract (`NodeType` with its settings' kinds, `NodeConfig`, `AudioCode`, handoff ends, sound card and RTP receiver requests, `CardId`, `RtpAddress`), node framework (`NodeLifecycle`, the one node state machine; `NodeHost`: a node's port block lives as long as the node, its insides start, restart and fail behind it, with the outside nodes they ask for), reconciler (route links and route status), levels (`LevelMeter`), the graph read (`GraphStatus`), the executor interface | Any PipeWire call, socket, thread or file access; nlohmann/json is its only dependency |
| `ear_folder` (`src/folder`) | `ConfinedFolder`: one folder held open from boot, every path resolved beneath it by the kernel (`openat2`): the recordings folder's files made, and the graph file read and replaced only if unchanged (`renameat2`); `requireApart`, the boot check that no two folders overlap | Following `..`, absolute paths or (for the graph file) any symlink; anything else of EAR's |
| `ear_nodes` (`src/nodes`) | The built-in types `white`, `file`, `card` and `rtp`, each declaring its settings' kinds | PipeWire and ALSA (`file` uses libsndfile; `card` and `rtp` ask the framework for their outside nodes) |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement core's interfaces: port blocks (`pw_filter`, with level metering), links (`link-factory`), live ports (the registry), handoffs (`pw_thread_loop`), outside nodes (one lifecycle, `joinOutsideNode`, with two kinds: sound cards as ALSA adapter nodes and RTP receivers as PipeWire's rtp-source module), the main-loop executor, the scheduling report | Policy |
| `ear_control` (`src/control`) | Status lines; the JSON-RPC protocol (envelope, the one decoder of node and route JSON, commands, events, sessions and subscriptions); the graph file's plain types on the wire (`GraphFile`, `Diagnostic`, the file's refusals); the two interfaces at its edge, `GraphRequests` (where every request goes) and `GraphFileListener` (who hears each file change); `MainLoopGateway`, the one crossing between the main loop and the control thread | Sockets; graph rules; knobly; holding the graph on the control thread |
| `ear_config` (`src/config`) | The graph file keeper (`GraphFileKeeper`): `ear.conf`'s model through knobly (read, diagnostics, edits at the source), the keeper's one background thread (the working copy, the disk base, the folder watch, writes) and its main-loop side (plan and commit through the desired graph, the running version, the last refusal, pending restart); the only package that sees knobly, privately | Judging a graph; letting a knobly type leave its thread; timers |
| `ear_server` (`src/server`) | The control thread and port 7777 (Boost.Beast): the editor's files, `/rpc`, Host and Origin checks, per-client queues, the level bell | Graph state; PipeWire |
| `ear` (`src/main.cpp`) | The composition root: wiring, boot and stop order | Computing anything |
| `web/` | The graph editor; shares no code with C++, only the wire format and `test/protocol/` | Graph rules (ear is the one source of truth) |

Dependencies point one way: `ear_core` <- {`ear_nodes`, `ear_pipewire`, `ear_control`,
`ear_config`}, `ear_folder` <- {`ear_nodes`, `ear_config`}, `ear_control` <- {`ear_server`,
`ear_config`}, and all of them <- `ear`; `ear_folder` depends on nothing of EAR's. No Boost or
knobly type is in a public header.
An interface lives with the package that consumes it: `node_host.hpp` declares
`PortBlockFactory` and `HandoffFactory`, `reconciler.hpp` declares `LinkMaker` and the
live-port and link listeners, and `ear_pipewire` implements them; `requests.hpp` declares
`GraphRequests` and `GraphFileListener`, and `ear_config` implements the first while the gateway
and `StatusLog` implement the second. Each package's headers form a CMake header set that is
compiled on its own (`VERIFY_INTERFACE_HEADER_SETS`); `ear_pipewire`'s headers only
forward-declare PipeWire's types, so PipeWire's headers stay private to it.

Events, not polling, move everything. `main.cpp` wires: the graph file keeper tells the gateway
and then `StatusLog` of each change to the file, before the commit it causes; `DesiredGraph`
tells its observer (the gateway) of each change first, then `NodeHost` about nodes and the
`Reconciler` about routes; `LiveGraph` tells the `Reconciler` which ports exist; `Links` tells it when a link is active,
failed or gone; `NodeHost` and the `Reconciler` tell `StatusLog` and the gateway. So a change's
notification always comes before the statuses it causes.

Threads (the owner, 2026-10-30: "The control port shouldn't run in the PipeWire context, because
it will not be real time"):

| Thread | Runs |
|---|---|
| PipeWire's data loop (realtime) | every port block's `process`, metering and level windows; the handoff writers |
| PipeWire's main loop (the main thread) | every piece of graph state: the engine, desired graph, node framework, reconciler, adapters, the gateway's graph side, the keeper's main-loop side (plan and commit, the running version); `ear.conf`'s first read at boot |
| The control thread | port 7777: sockets, JSON-RPC, sessions, queues; it holds only `GraphGateway`, which sends every request whole to the keeper and gets replies and events back in order from the main loop |
| The keeper thread (`ear_config`) | the graph file keeper: every request, verdict, disk check and write, one step at a time in arrival order; the working copy (knobly's graph object, which never leaves this thread); the inotify watch on the graph file folder; writes of `ear.conf` when no step is waiting. It waits in `poll()` with no timeout and posts each step to the main loop |
| Handoff threads | the `file` node's writer |

How a change moves (`docs/design/knobly-config.md`, design rules 1 to 3): a request reaches the
keeper's thread whole; for an edit the keeper loads the working copy's text afresh and applies
the edit at its source (a candidate), then posts it to the main loop, where the keeper's
main-loop side asks `DesiredGraph::plan` for a verdict. If accepted, the file change is told,
the plan is committed (its node and route events follow) and the reply is sent, all in one
main-loop turn; the keeper then keeps the candidate as its working copy and writes `ear.conf`
once no step is waiting: it swaps the new text in and keeps it only if the text it replaced is
what it last read or wrote (otherwise it puts the hand's text back and reloads). A refused candidate is dropped, and nothing is told or written. A hand edit
arrives as an inotify event, is read on the keeper's thread and offered the same way (cause
`disk`); the graph may refuse it, but the working copy still follows the disk.

How audio moves:

1. `Engine` (`src/pipewire/engine.cpp`) runs PipeWire's engine in-process: its main loop, a
   context loaded from `engine.conf`, and EAR's own connection to it. There is no daemon, no
   socket and no session manager; a dummy driver clocks the graph at the graph frequency with
   the block size's frames a cycle (both fixed at boot). The `pipewire-engine` skill covers the
   configuration and traps.
2. A node starts (`NodeHost`) block first: `FilterPortBlocks` makes its port block, once for
   the node's life: one always-processing `pw_filter` named by the node's name, with one 32-bit
   float mono port per declared port. Then its type's `NodeConfig::start` makes the type's
   insides, `AudioCode::prepare` runs and the block is driven with that code; new settings
   restart the insides behind the same block. The node is `running` once the block is ready
   (its first STREAMING state) and the insides are ready, every outside node they asked for
   included (`NodeLifecycle`). Every block processes whether routed or not; an unlinked input
   reads zeros.
3. On PipeWire's realtime thread each cycle the block calls `AudioCode::process` with one span
   per port. `white` fills its output with noise.
4. `LiveGraph` reports each port as the engine's registry announces it (node name, port name,
   direction, port id). When both ends of a wanted route are live, the `Reconciler` asks `Links`
   for a link by port ids; the route is `linked` once the link is active, and `pending` again
   if an end goes, relinked when it comes back.
5. Outside nodes. A `card` or `rtp` node's port block has inside ports no route can name. Once
   the block is ready the framework makes the outside nodes its insides asked for, named
   `<node>.playback`, `<node>.capture` or `<node>.rtp`. Every kind shares one lifecycle
   (`NodeHost` in ear_core, `joinOutsideNode` in `src/pipewire/outside_nodes.cpp`): the outside
   node runs alone until its body says it runs, then one link per channel joins it, by its
   global id, to the block's inside ports; it is ready once every link is active, and on any
   failure its links and node are torn down before it is reported lost, so the node is `failed
   engine-refused: <detail>`, its block and routes stay. Sound cards (`AlsaSoundCards`): one ALSA
   adapter node per direction, running once its PCM runs; through the blocks' graph group
   (`node.group = ear.graph`) a linked card drives every block, and a lost one hands the clock
   back to the dummy driver at once. RTP receivers (`RtpReceivers`): PipeWire's
   `libpipewire-module-rtp-source` loaded into the engine, running once its socket is bound; it
   follows the graph's clock and resamples the sender to it. The `pipewire-engine` skill has the
   details and traps.
6. Levels: each block meters its public ports every cycle and publishes a window (1/30 s of the
   graph clock, on the same clock positions for every block) into a lock-free mailbox, then
   rings an eventfd; the control thread reads every block's latest window and sends it to levels
   subscribers.
7. `file` cannot write a file on the realtime thread. Its audio code writes into a handoff
   (`ThreadHandoffs`, `src/pipewire/handoffs.cpp`): a lock-free ring of interleaved floats (two
   seconds) that wakes the handoff's own thread, where libsndfile writes the frames. A full ring
   drops frames and counts them; the count is the `dropped=` of the node's `stopped` line.
8. Stopping: `DesiredGraph::close()` refuses every later edit, the keeper is told to stop
   (edits still queued get `stopping`, reads are still answered), then `NodeHost::stopAll` takes
   each node through `stopping` and tears it down in one order: port block destroyed (no
   `process` after that), `release`, outside nodes destroyed, handoffs drained and finished,
   insides destroyed; then `stopped`. A restart (`node.update`) runs the same teardown but keeps
   the block, only silencing it (`drive(nullptr)`) first. After the last node, the control port
   closes every client (1001), the control thread stops and EAR's main loop quits. Last, the
   main thread joins the keeper's thread, which first writes the last accepted version of
   `ear.conf` if it is not written yet (a hung disk delays exit without limit).

Not built yet: serving PipeWire's socket, the program supervisor and the `airplay` type, Music
Assistant's Sendspin (next batch), a login on port 7777, card retry and hotplug, arm64 images;
for the graph file: `@include` (more files than `ear.conf`), a separate writer thread (so an
edit never waits behind a write), undo, and changing the graph frequency or block size while
ear runs. For `rtp` (`docs/design/rtp.md` section 9): a failure for a port that cannot be
bound, a "receiving" status, multicast tested, a larger datagram size (`net.mtu`) for many
channels, SAP discovery, IPv6, an RTP sender.

## Tests

Catch2 v2; each package has one test executable, and `ear_catch_tests` (`test/CMakeLists.txt`:
`catch_discover_tests` with Catch's `--warn NoTests`) makes each test case its own CTest test, so
`ctest --parallel` runs them side by side; a CTest test that matches no test case fails. Keep `;`
out of test names: CMake splits a name at it.

| Executable | What it proves |
|---|---|
| `ear_core_tests` | The desired graph's checks, plan and commit (event order, a port change as a replacement, stale plans) and errors, observer order, closing; the node state machine (every state against every fact); `NodeHost` with fake factories (restart, removal, sound cards and RTP receivers, both in either order, a lost block); the reconciler; levels; the graph read; card ids and RTP addresses. Pure, milliseconds |
| `ear_folder_tests` | The confined folder on real temporary folders: made if missing (never its parents), `create()` beneath only, `read()` and `replaceIf()` refusing symlinks, directories, FIFOs, large files and paths out, replace-if-unchanged keeping the file's mode, a folder it cannot write, an inotify watch through `heldPath()`, `requireApart()` (one folder twice, nesting, a symlinked alias) |
| `ear_nodes_tests` | `white`'s ports and noise statistics; `file` writing exact samples through a fake grant, refusing an unopenable path, absolute and `..` paths (invalid settings) and a symlink out of the recordings folder (at start); `card`'s settings, ports and pass-through; `rtp`'s settings (each refusal's exact detail), ports, the request it makes and pass-through; every built-in type's setting kinds |
| `ear_control_tests` | The JSON-RPC envelope (the spec's examples), commands, events, the graph file's types and encoders, status lines, gateway and sessions, all against `test/protocol` (`graph-file.json` included) |
| `ear_config_tests` | The graph file model (the record's example, each of ear's codes at its place, `@include`, missing and unreadable files, knobly's nesting limit), places in UTF-16 columns, edits at the source (a value changed where it is defined, `shared-definition`, removal with its routes, quoting); the keeper's sequencer stepped by hand on one thread over a real folder (boot offers, accepted and refused edits, both hand-edit collisions, a typo, `graphFile.set`, its own write's watch event, batches, shared writes, write failures, pending restart, stop); the keeper's thread and the whole keeper with inotify |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample over a link, the clock, teardown, SIGTERM; the handoff (order, drops, close, no allocation on the writer); the graph group, `drive`, level windows on the clock, the main-loop executor, the scheduling report; sound cards through a test ALSA card (alsa-lib's file and null PCMs): driving, channels sample for sample, a card that cannot open, a lost card; RTP receivers fed by a test sender on 127.0.0.1: following the dummy driver, channels by sine fit, L16 at 44100 resampled, a sender restarting with a new SSRC, a lost receiver, a receiver made again at once for the same names |
| `ear_server_tests` | Port 7777 on real sockets with a fake main loop and gateway: files, Host and Origin, round trips, the 1008 bound, a client that resets, stop with 1001, levels |
| `ear_system_tests` | The owner's end-to-end tests, through the real `out/ear` on an `ear.conf`: white noise recorded to the routed channel only (exact zeros on the other, header finished, `dropped=0`); a recordings folder it cannot make exits 1; the graph file (the record's example booted, an editor edit announced, applied, answered and written back, a hand edit winning, a hand typo blocking edits until Save fixes it, `ear.conf` as a symlink out and an `@include` out refused, boots with a typo, a refused route or no file running nothing, the fallback flags, overlapping folders exiting 1); an `rtp` node's tone recorded channel for channel, and its silence with no sender; and through the control port: the scheduling and listening lines and the editor's files, a client's edits heard in the WAV in event order, levels, removal and errors, file paths outside the recordings folder refused with nothing written, edits during stop refused as `stopping`, a sound card node linked |
| web (vitest, `make web-test`) | The editor's reducer against every `test/protocol` fixture, the graph file's state and the text pane's logic (drafts, stale files, Save, refusals), the reconnecting client with a fake socket, connection rules, levels and layout. No browser; rendering is checked by eye |

`test/CMakeLists.txt` also runs `ear --help` (the binary loads its shared libraries), and
`ear_engine_conf_beside_ear` checks the copy beside the binary matches the source. Tests are
event-gated: they wait for the event they need, and a deadline only fails a test, never decides
when something happens. Verify audio here, never on real hardware. The engine, ramp and RTP
tests need PipeWire's data thread to keep time: on a heavily loaded host it stalls for hundreds
of milliseconds (graph xruns), and they fail without a bug (`docs/design/rtp.md` erratum 3; the
`pipewire-engine` skill says how to tell). Run one, inside the image:

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
runs `scripts/image-smoke.sh` on it. On a push to `master`, once all three pass, `release` and
then `publish` run (Run image and releases, below). A newer push cancels a pull request's run,
never a `master` run. Its comments give the reasons for each step and cache.

## Run image and releases

`docker/Dockerfile` has three targets: `dev` (the build and test image: Debian 13, PipeWire,
GCC 14, CMake, Node.js 24, knobly at `/opt/knobly`), `build` (`make release` and the editor's
build) and `run`, what ships: `debian:trixie-slim` with only the run-time libraries, no
compiler and no knobly (it is compiled in); `/opt/ear/ear`, `engine.conf` and `web/`; user `ear`
(uid 10001); port 7777; default `--config-dir /var/lib/ear/config --recordings-dir
/var/lib/ear/recordings`, two volumes owned by `ear` (arguments given to the container replace
both, so pass them again). The graph file folder must be writable by uid 10001: a read-only
mount (a ConfigMap) cannot be edited, and a first boot without `ear.conf` runs nothing until the
pane saves one. A fourth stage, `knobly`, is thrown away: it only builds knobly's release tree
for `dev`. The binary carries CAP_SYS_NICE as a permitted-only file capability that
EAR makes effective at boot, so the audio thread is realtime where the container is given
SYS_NICE (`docker run --cap-add SYS_NICE`) and plain elsewhere; EAR prints which, and
`graph.get` says it. Built for amd64 only until the Talos nodes' CPU is known.

```bash
docker build --target run --tag ear:local --file docker/Dockerfile .
scripts/image-smoke.sh ear:local   # what CI checks: user, no compiler, files, ear.conf, graph.get, graphFile.get
```

Releases are automatic: merging a pull request to `master` is the release. After the checks
pass on that push, `test.yml`'s `release` job runs semantic-release (`.releaserc.json`, with
its tooling pinned in the root `package.json` and `package-lock.json`, under `web/.nvmrc`'s
Node): the Conventional Commits since the last `v*` tag decide the bump (`fix` a patch, `feat` a
minor, `build(deps)` a patch, `!` or `BREAKING CHANGE:` a major, other types none; the notes
list Features, Bug Fixes, Performance, Reverts and Dependencies, and hide every other type). It appends the release notes to
`CHANGELOG.md`, commits that to `master` as `chore(release): <version> [skip ci]` (a push made
with GITHUB_TOKEN starts no workflow), tags that commit `v<version>` and makes its GitHub
Release. The version is the tag; no file holds it. A squash-merge counts only its title, so
the title must be a Conventional Commit. Then `publish` pushes the run image to
`ghcr.io/mcurcio/ear`, released or not, built from the release's tag (else the merged commit)
and tagged `master`, `sha-<merged commit>` and one version per `v*` tag on what it built
(`scripts/version-tags.sh` reads them from git; a rerun of a failed publish keeps the release
job's tag). One gap: if the release job fails after it pushed the `CHANGELOG.md` commit and
tag (making the GitHub Release failed), a rerun of it finds `master` ahead of its checkout and
releases nothing and `publish` never runs, and nothing reruns from the new head (its `[skip
ci]` commit started no run, and it already carries the tag). The GitHub Release is then made by
hand from that version's `CHANGELOG.md` section (`gh release create v<version> --verify-tag`),
and that version's image is not published. Never tag or version by hand otherwise. The ghcr package is public
(anonymous pulls work). Running it on Talos with the MOTU: the `ear-on-talos` skill.

## Vendored and fetched code

`vendor/json` (nlohmann) and `vendor/catch` (Catch 2.12.2) are used; `vendor/jsonrpccxx`,
`vendor/jsonrpc` and `vendor/libjsonrpc` are not built (the control port has its own JSON-RPC
code in `ear_control`; removing them is deferred). All were
added with `git subtree`; leave them alone unless you are updating a dependency. Update one
with `git subtree pull --prefix vendor/<x> <repo> <ref> --squash -m "build(deps): upgrade <X> to
<a.b.c>"`: with `--squash` the upstream commits never enter ear's history, so the release notes
get only the merge commit's message (one Dependencies line and a patch release); the squash
commit's own "Squashed '...' changes" message is not a Conventional Commit, so it is ignored.
Bumps of the fetched dependencies (Boost's `boost_url` and `boost_sha256`, `KNOBLY_COMMIT`) use
the same message, `build(deps): upgrade <X> to <version>`. The subtrees were first added without
`--squash`, so their upstream histories (thousands of other projects' commits) are in ear's
history; the baseline tag `v0.0.0` on cb6d71f64c (the last `git subtree add`, 2020-05-27) ends
them, since semantic-release reads only the commits after the last `v*` tag.
`test/main.cpp` turns off Catch's POSIX signal handlers, which do not compile against glibc 2.34
and newer (its comment says why; Catch 2.13.5 fixes it). Boost is fetched, not vendored (see
Build and run), and so is knobly, built in the image from a pinned commit (`KNOBLY_COMMIT`).
PipeWire, alsa-lib and libsndfile come from the image's Debian packages; the
editor's npm packages are pinned by `web/package-lock.json`.

## Skills and references

Repo skills in `.claude/skills/`:

- `pipewire-engine`: running the engine in-process: what `engine.conf` sets and why, the
  modules, the dummy driver, the graph group, outside nodes (sound cards and the clock, RTP
  receivers through rtp-source), `drive`, levels, realtime priority, running without a session
  manager, the traps (xruns on a loaded host among them), and how to bump PipeWire.
  The PipeWire source behind each engine fact is cited in `docs/design/m1-white-to-file.md`
  section 1, `docs/design/batch2.md` sections 4.3 and 5 and `docs/design/rtp.md` section 4.
- `control-port`: talking to port 7777, its Host and Origin rules, the `graphFile.*` calls and
  write-behind, threads, queues, stopping and traps.
- `ear-on-talos`: the run image on a Talos node with the MOTU 24Ao (kernel module, privileged
  namespace, `/dev/snd`, SYS_NICE, the writable graph file volume, the card's own settings,
  checks on the hardware) and FPP's RTP stream into the pod.
