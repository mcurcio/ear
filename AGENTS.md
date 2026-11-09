# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

## What this is

EAR ("Einstein Audio Router") is a C++23 program for Linux that runs PipeWire's audio engine
inside its own process and builds an audio graph from its graph file, `ear.conf`, written in
the knobly configuration language (mcurcio/knobly): it starts the nodes the file names, links
their ports as the file's routes say, prints a JSON record for every change, and stops cleanly
on SIGINT or SIGTERM. While it runs it keeps the file live: an edit made in the browser is
applied to the running graph and written back into `ear.conf` (comments and layout kept), and a
hand edit to `ear.conf` is applied and shown in the browser. Its control port (7777) serves a
graph editor, with a text pane for `ear.conf`, and a JSON-RPC interface that reads and edits
the graph and the file and streams every change and per-port audio levels. The built-in node
types are `white` (white noise), `file` (records a stereo WAV file), `card` (a sound card
through ALSA), `rtp` (one RTP audio stream received from the network, such as FPP's) and
`sendspin` (Music Assistant's audio in and out, over the Sendspin protocol). It
ships as a container image published from GitHub Actions. The system design is
`docs/design/pipewire-graph.md`; milestone 1's module design is `docs/design/m1-white-to-file.md`,
batch 2's (control port, graph editor, sound card, image) is `docs/design/batch2.md`, the `rtp`
node's is `docs/design/rtp.md`, the `sendspin` node's is `docs/design/sendspin.md`, and the graph
file's is `docs/design/knobly-config.md` (the owner's words for it:
`docs/design/knobly-config-notes.md`).

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
scripts/dev.sh make format        # clang-format -i over the tracked C++ under src/ and test/ (.clang-format: tabs, 120 columns)
scripts/dev.sh make format-check  # the same, failing on any file it would change (CI's format job)
scripts/dev.sh bash               # a shell in the image
```

- Needs CMake 3.28 or newer (the `$<COMPILE_ONLY:...>` link and `FetchContent`'s
  `EXCLUDE_FROM_ALL`), pkg-config, `libpipewire-0.3`, `libspa-0.2` and their run-time modules,
  `sndfile`, and `git` (it clones the `sendspin` node's dependencies); the Dockerfile's comments
  say why each package is there. Configure names every pkg-config package it did not find.
- Build output goes to `out/` (gitignored). A post-build step copies the binary to `out/ear`,
  and configure copies `src/pipewire/engine.conf` beside it to `out/engine.conf`.
- The `sendspin` node's fetched dependencies are in `cmake/SendspinDeps.cmake`, each at a full
  commit or by SHA-256: the protocol library sendspin-cpp (with one carried patch,
  `cmake/sendspin-listener.patch`; a failed apply, or a tree without it, stops configure), ArduinoJson, IXWebSocket
  (likewise, `cmake/ixwebsocket-limits.patch`), micro-flac and noise-c, which it needs, and mjansson/mdns (one header). Each is kept in
  `.cache/deps/` as Boost's is, and `ear_node_sendspin` is the only library that sees them. Configure
  copies their licences to `out/licenses/`. Configure with `BUILD_TESTS=ON` stops naming
  `EAR_SENDSPIN_SERVER_PYTHON` when the image's `/opt/sendspin-server/bin/python` (the real
  Sendspin server the tests run against, `test/sendspin/requirements.txt`) is missing.
- Boost 1.92.0 (pinned by SHA-256): `program_options` is its only compiled library, and Beast and
  Asio (header-only) serve the control port; configure fails naming Beast if it is not there. Its
  source tree is kept in `.cache/deps/boost-src/` (about 800 MB,
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
| `--config-dir` | `config` | the graph file folder, which holds `ear.conf` and each `sendspin` node's key file; made at boot if missing (not its parents). It must be writable for edits to be saved. It may not be the recordings folder or the web root, nor inside or around either (exit 1, naming both) |
| `--graph-frequency` | `48000` | the engine's rate, used only when `ear.conf` cannot be read or does not give `graph_frequency` and `block_size` |
| `--block-size` | `256` | the engine's block size, under the same rule |
| `--control-address` | `0.0.0.0` | the control port's address |
| `--control-port` | `7777` | its port; `0` takes any free one (tests) |
| `--web-root` | `<directory of ear>/web` | the editor's built files |
| `--allowed-host` | none | a DNS name the Host header may carry (repeatable) |
| `--recordings-dir` | `recordings` | the folder `file` nodes write into; made at boot if missing (not its parents), and a folder it cannot make or open exits 1 |

`ear` reads `engine.conf` from beside its own binary. Every line it writes on standard output
is one JSON record (`docs/design/node-framework.md` section 8.8), from one encoder (its one home
is `src/control/status_log.cpp`; the schema is the fixture `test/protocol/output-records.json`,
a record of every kind):

```json
{"v":1,"seq":15,"t":"2026-11-07T19:32:03.096982Z","kind":"route","from":"noise:noise.mono","to":"rec:record.left","status":"linked","msg":"route noise:noise.mono -> rec:record.left linked"}
```

`v` is the schema's version (additive changes keep it), `seq` counts from 1 (a gap is a record a
full queue dropped: the next record carries `"dropped":<n>` and its `msg` says so), `t` is UTC to
the microsecond, then `kind`, the kind's fields, and `msg`, a short sentence: the plain line ear
printed before records, so `jq -r .msg` reads as it always did. It first says how its audio
thread is scheduled (`engine`), then `control listening on <address>:<port>` (`control`), then a
record for every change. By kind, with its `msg`:

```
engine                   engine: realtime SCHED_FIFO <n> | engine: not realtime (SCHED_OTHER)
control                  control listening on <address>:<port>
node                     node <name> starting                       (added)
                         node <name> starting: <cause>              (configuring after a restart, retry or enable)
                         node <name> running|stopped|disabled
                         node <name> stopping[: <cause>]            (stop, disabling, removing, restarting (<why>))
                         node <name> failed <engine-refused|start-failed|lost>: <detail>
                         node <name> still <phase> after <s>s: waiting for <what>
                         node <name> removed dropped=<frames>
note                     node <name> says <text>
route                    route <node>:<port> -> <node>:<port> pending|linked|refused[: <detail>]
graph-file               graph-file <edit|disk|write> <version> running=<version|none>
                         graph-file error <line>:<column> <code>: <message>
                         graph-file refused <code>[ at <line>:<column>]
                         graph-file dropped <n> edits
                         graph-file pending-restart <graphFrequency|blockSize>...
                         graph-file write-failed <detail>
graph-file-written       graph-file written <version>
graph-file-write-failed  graph-file write-failed <detail>
log                      [node <name> ]<source>[ <topic>] <level>: <text>
diagnostic               node <name> <code>: <detail>   (ear.close-overdue, ear.fade-unacknowledged, ear.barrier-overdue)
                         <code>: <detail>               (no node field: ear.data-loop-wedged, ear.main-loop-stalled,
                                                        ear.main-loop-resumed)
                         ear.main-loop-stalled: no main-loop heartbeat for <ms> ms[, in <call>[ <node>]]
                         ear.main-loop-resumed: the main-loop heartbeat came back after a gap of <ms> ms
exit                     ear: <reason>
```

A `note` (`says`) is a note from a node's insides, never a state; a type's texts are its own (the
`sendspin` node's: `src/nodes/sendspin/notes.hpp`, below), and only standard output has them, so the
control port and the editor do not. A `graph-file` record comes with each change to the
graph file (`cause` `disk` for the boot file and for hand edits, `edit` for an edit through the control port, `write` when a write failed or
recovered), before the node and route records it causes, and carries every diagnostic of that
version, warnings included, with the graph's refusal, edits the hand edit dropped, settings that
apply only at restart and a write the disk refused; its `msg` is the lines above that apply,
joined by newlines (errors only). `graph-file-written` follows each write of `ear.conf`.

Nothing else reaches standard output or error directly: once the flags are read, before any
thread starts, ear keeps the real standard output for its output thread alone (behind a bounded
queue, so the main loop never waits on a slow reader) and puts pipes on descriptors 1 and 2,
read by a capture thread into `log` records marked `"unattributed":true` (`source` `stdout` or
`stderr`; the `sendspin` node's library and IXWebSocket write there, as sendspin-cpp has no log
hook to name the node). PipeWire logs through
ear's own `pw_log` (`source` `pipewire` with its `topic`) and alsa-lib through ear's error
handler (`source` `alsa-lib`), both safe on the data loop (`src/pipewire/engine_log.cpp`). A
`sendspin` key file it cannot narrow at boot is a `log` record from `sendspin`. Log records are
paced per node, or per source when unattributed (32 at once, then one a second; the next one
that passes counts what was held back as `heldBack`), so a flooding library cannot push states,
routes or notes out of the queue. A crash keeps its last words: on `std::terminate`, SIGABRT,
SIGSEGV, SIGBUS, SIGFPE or SIGILL, ear writes what is queued and what the pipes hold to the real
descriptors and puts them back before the process dies, so abort's, terminate's and assert's
messages reach `kubectl logs` as plain text. Bad flags and `--help`
come before the capture, as plain text; every later reason to exit 1 is an `exit` record.

A boot whose `ear.conf` is missing, has errors, or is refused by the graph (a route to a port
that does not exist) runs nothing: ear stays up, prints why, and the editor's pane shows it
until the file is fixed (by hand or with Save). SIGINT or SIGTERM first refuses graph edits
(clients get `stopping`), stops every node (a `file` node finishes its WAV file), closes every
client with WebSocket code 1001, writes the last accepted version of `ear.conf` if it was not
written yet (a failure is printed as a `graph-file-write-failed` record, and reaches nowhere
else), writes every record still queued and exits 0. Bad flags, a folder it cannot make or open, folders that overlap, an engine
that does not start, a folder watch it cannot arm, or a control port it cannot bind exits 1.
A node with an output (every type but `file`) fades its outputs, a card's playback included, in over 5 ms at every
start and out over 5 ms before every restart, removal, failure and stop; a fade not acknowledged in 100 ms is
`ear.fade-unacknowledged` and the silence is taken unfaded (`docs/design/node-lifecycle.md` sections 5.3 and 5.4). A
drive or silence the data loop has not completed in 5 s (after a grace when the whole process stalled) is
`ear.barrier-overdue` and then, once, `ear.data-loop-wedged`: from then on ear makes no PipeWire call that could wait
on the data loop (no block, outside node or link is made or destroyed, no latency is told; a new node fails
`engine-refused: the data loop is wedged`, a new route is `refused` with that detail), what it would destroy is
leaked, and ear's stop still writes `ear.conf` and every record, then leaves through `std::_Exit(0)` (as it does
whenever anything was leaked, a run past its close deadline included).

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
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
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
  invalid settings. A type declares each key once, in one settings table (`SettingsTable` of typed
  rows, `setting::required`, `optional` or `defaulted`, in `ear_core`'s `settings_table.hpp`: key,
  a whole number's range, unit and default, or a text's parse function and what it must be). The
  table checks the object (unknown keys, missing keys, kinds, ranges, `onlyWithout`, in a fixed
  order, so the first problem a refusal names is stable). `NodeType`'s `name()`, `settingKinds()` and
  `parse()` are not virtual: they answer from the table of the type's spec (`TypeSpec`, `type_spec.hpp`;
  `NodeTypeOf<table>` for `sendspin` until it moves), so the graph file reader cannot disagree with
  `parse`, and a type that disagrees does not compile. The type's class gets the checked values
  (never the raw object), each read by its row's handle: a required or defaulted row gives its value, an
  optional row an `std::optional`, a text row the type its parse function made (`CardId`, `RtpAddress`,
  a file path), parsed once; a misspelt row, or an optional one read as a plain value, does not compile
  (`src/core/tests/compile_fail/`). A rule across settings that is only a key not to be given with
  another is data on the row (`onlyWithout`); any other rule across settings (card's playback or capture
  above 0, file's path staying inside the recordings folder) is the class's `check`, run by `parse`
  after the table's walk, its one home, so it is still invalid settings and never reaches `ear.conf`.
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
- A public port is `<bank>.<member>`, and a route names one by its whole name, `<node>:<bank>.<member>`
  (`route kitchen:from.left desk:play.1;`). A type declares each bank with its direction, name and members
  (`PortBank`, `node_type.hpp`), and `portName` in `ear_core` makes the whole name. An old name (`MONO`, `FL`, `playback_1`)
  is `unknown-port`; no compatibility is kept. The owner's words: `docs/design/sendspin.md` section 1.

  | Type | Inputs | Outputs |
  |---|---|---|
  | `white` | none | `noise.mono` |
  | `file` | `record.left`, `record.right` | none |
  | `card` | `play.1`..`play.N` (audio going to the card) | `capture.1`..`capture.M` |
  | `rtp` | none | `stream.1`..`stream.N` |
  | `sendspin` | `to.left`, `to.right` | `from.left`, `from.right` |
- `white` has one output, `noise.mono`, uniform noise in [-1, 1), and takes no settings.
- `file` has inputs `record.left` and `record.right` and needs `path` (text), relative to the recordings folder
  (`--recordings-dir`): non-empty, not absolute, no `..` component, else invalid settings. It is
  opened beneath the folder only (`openat2` with `RESOLVE_BENEATH` on the folder's descriptor,
  held from boot), so a symlink that leads out fails the node's configure; a missing subfolder fails
  it too (none is made). It truncates the file at configure and writes 32-bit float stereo WAV at the
  graph frequency through an interim sink on its inside output bank `writer` (a 2 s ring; the WAV is
  opened on the sink's own thread, and a libsndfile refusal fails the node `start-failed` before it is
  configured): every cycle its code drove, none while it is stopped (a stop closes nothing, a start
  appends again), complete once its configuration is torn down (a restart, a disable, a removal).
- `card` is a sound card: `card` (text: the card id), `playback` and `capture` (whole numbers,
  channels). The card id is the ALSA name in `/proc/asound/cards` (letters, digits, `-`, `_`, at
  most 15), never an index or an ALSA device string; each count is 0 to 64 (default 0), at least
  one above 0. Inputs `play.1`..`play.N`, outputs `capture.1`..`capture.M`. Its rate is
  the graph frequency. The card never drives the graph's clock: ear's internal clock always does,
  and a linked card follows it, resampled to it (`docs/design/node-framework.md` section 10), so
  no output is bit-perfect. A card that cannot open or is lost makes the node `failed
  engine-refused: <detail>`, its routes stay, and the rest of the graph keeps its clock. `node.update` with changed settings starts it
  again; an update with the same settings changes nothing (`graph_file_model.cpp`,
  `desired_graph.cpp`), so with none to change, restart ear.
- `rtp` receives one RTP stream (`docs/design/rtp.md`), for example `node rtp fpp { address
  "0.0.0.0"; port 5004; format "L24"; channels 2; rate 48000; latencyMs 100; }`. `address`
  (text), `port` (1 to 65535), `format` (text: `L16` or `L24`, big-endian PCM) and `channels` (1
  to 64) are required; `rate` (8000 to 192000 Hz) defaults to the graph frequency, and a sender
  at another rate is resampled to it; `latencyMs` (1 to 1000) is the receive buffer, default
  100. Outputs `stream.1`..`stream.N`, no inputs. `address` is `0.0.0.0` (any sender) or an
  IPv4 multicast group (224.0.0.0 to 239.255.255.255) as a plain dotted quad. A unicast address
  is refused, because PipeWire would hear only that sender, and only when it sends from the same
  port it sends to; a host name is refused, because looking it up blocks the main loop. The
  stream follows the graph's clock and never drives it. With no packets the node is `running`
  and silent (its meters show it); a sender that restarts is heard again. A port another program
  holds leaves the node `starting` for good with silent outputs: PipeWire only logs the failed
  bind. A packet lost or out of order resets the receiver, so about `latencyMs` of silence
  follows. Tested only with a sender on 127.0.0.1; multicast is not tested yet.
- `sendspin` plays Music Assistant's audio and sends it audio, over the Sendspin protocol
  (`docs/design/sendspin.md`; the `sendspin` skill), for example `node sendspin kitchen { server
  "ws://192.168.1.10:8927/sendspin"; name "Kitchen"; }`, which dials, or `node sendspin porch { port
  8928; }`, which listens and advertises `_sendspin._tcp` over mDNS. All settings are optional, and
  the node with none listens on 8928. `server` (text): `ws://<host>[:<port>]/<path>`, no `wss://`,
  user, query or fragment. `port` (1 to 65535, default 8928) only without `server`. `name` (1 to 64
  bytes of UTF-8, no control characters; default the node's name) is what Music Assistant shows.
  Outputs `from.left`, `from.right`; inputs `to.left`, `to.right`. The server
  sends FLAC 24 stereo, resampled by it to the graph frequency; the node sends
  PCM 24 stereo when the server asks. It follows the graph's clock and never drives it. It is
  `running` with no server, its outputs silent (the graph keeps running whatever the server does):
  the connection is told in `says` notes, not node states. Pairing code required: until the operator
  types the code it prints into Music Assistant it gets no roles and no audio. Each node's identity
  and pairings are `sendspin-<node>.key` in the graph file folder, mode 0600 (a wider one is narrowed
  at boot and at each start, whatever its node; renaming a node makes a new identity; removing one leaves the file). A key file it
  cannot trust fails the node, and it mints no new identity over it. A listening port another program
  or another `sendspin` node holds is `failed start-failed: cannot listen on port <p>: <reason>`. A
  node whose insides failed starts again when a setting changes (`name` can be changed and changed
  back), or when ear restarts; an update with the same settings changes nothing. An upgrade carrying
  an `Origin` header is closed before any Sendspin message, and a frame or message over 1 MiB before
  it costs anything. A name added back waits for its old node to go. mDNS answers only queries from
  this link, once a datagram, by multicast once a second. Sockets that have proved nothing give way to
  a newcomer, whichever address it comes from (`docs/design/sendspin.md` section 11, item 6), so holding them keeps no server out;
  a sustained flood still does for as long as it lasts, and a peer that finishes the published handshake and activates nothing is what can still hold the node from an unpaired server (item 2). Notes pass 32 at once and
  then one a second. Its notes:

  ```
  node <n> says id <client id>
  node <n> says dialling <url>
  node <n> says listening <port> as ear-<first 50 bytes of node>-<first 8 of client id>
  node <n> says connected "<server name>" paired|unpaired
  node <n> says disconnected
  node <n> says pairing-code <123-456>
  node <n> says pairing-code-cleared
  node <n> says paired <server id>
  node <n> says pairing-failed <attempt-timeout|concurrent-attempt|method-not-supported|pairing-code-mismatch|user-cancelled|unknown>
  node <n> says pairing-held
  node <n> says stream <flac|pcm> <rate> <channels> <bits>
  node <n> says stream-ended
  node <n> says sending <rate> 2 24
  node <n> says sending-stopped
  node <n> says key-write-failed <detail>
  node <n> says advertise-failed <detail>
  node <n> says notes-dropped <n>
  ```

`ear.json` and its parser are gone; there is no compatibility with it.

## Architecture

Thirteen libraries (eight packages, and the five node types each its own library), the binary and the
editor, each C++ package in `src/<package>/` with its own `CMakeLists.txt`, public headers in
`include/ear/<package>/` and tests in `tests/`; a node type lives in `src/nodes/<type>/`, with its
header in `include/ear/nodes/<type>.hpp` and its tests in `tests/`:

| Target | Owns | Never |
|---|---|---|
| `ear_core` (`src/core`) | Desired graph (`DesiredGraph`: wanted nodes and routes, checked against declared ports; the one judge of every change: `plan` the whole wanted graph, then `commit` it, observers told before subscribers; `close()` when stopping; `CommandError`), node catalogue contract (`NodeType` with its settings' kinds, `SettingsTable` and its typed rows in `settings_table.hpp`, the one table a type declares its settings in, the spec half of the type SPI in `type_spec.hpp` (`TypeSpec`, bank handles, `Members`, `Io<spec>`, `bankPair`) and its class half in `lifecycle.hpp` (`Configured`, `NodeOf`, `CreateOf`, the erased `CreateContext`, `NodeContext` and `ErasedInstance` NodeHost drives, `makeNodeType<spec, Type>()`, the one way a type is made, its class's shape checked at compile time); `NodeConfig::make`, whose default is the private legacy adapter (`legacy_instance.hpp`) over the old SPI's `start` for `sendspin`'s `NodeTypeOf` alone (until slice 3c); `sink.hpp`, the interim sink NodeHost runs: `Chunk`, `SinkConsumer` and `SinkShape` for a type, `SinkTap`, `SinkBinding`, `Sink` and `SinkAdapter` (ear_pipewire's `ThreadSinks`) for the framework, `NodeFolders`, the interim folders `sendspin`'s factory alone takes until slice 3 (and where it reports what it finds at boot), the log seam (`LogEntry`, `LogSink`: where node code's and captured output's free text goes), `NodeConfig`, `Ports` declared by `PortBank`s, `portName`, the one maker of a public port's name, `AudioCode`, `StartResult`, handoff ends, sound card, RTP receiver and recording requests, `CardId`, `RtpAddress`), node framework (the node lifecycle in `node_lifecycle.hpp`: the block machine, the configuration machine and the node they make, `NodeAggregate`, pure and tested by table; `NodeHost`: a node's port block lives as long as the node, its configurations start, restart and fail behind it, one at a time, each an instance its config makes, whose `configure` acquires its resources through `CreateContext` (or a legacy type's `start` through `StartContext`, on the same internals) from the `ResourceRegistry` (one adapter per kind), a legacy run object built only once they are ready, its `start()` called once driven and its `stop()` before each silence that follows, every hook timed (`ear.hook-slow`), a rule the type breaks failing the node `start-failed`, torn down by dependency with a close deadline; `NodeHealth`, a node's state (the phase's colour), phase, wanted state, cause, failure and what it waits for, told after every transition; `NodeOperations`, the stop, start and restart that edit nothing; named inside banks and the framework's `passThrough` between banks (`pass_through.hpp`); `Timers`, the main loop's; `GraphCycle`, what a block's code is told of each cycle; `Latency`, a configuration's own declared through `CreateOf::declareLatency` (legacy: `StartContext::declareLatency`), and the `LatencyPlan` a block carries per bank pair (`PortBlock::carryLatency`); `InsidesEvents`, notes and `lost` from a run's threads, landing only on that run; `FrameRing`, the handoff's lock-free ring), reconciler (route links and route status), levels (`LevelMeter`), the graph read (`GraphStatus`), the executor interface. With the tests only, beside it, `ear_type_harness` (`ear/testing/`: `NodeHarness` and `TypeHarness`, a type run over a real `NodeHost` with in-memory fakes, without PipeWire) | Any PipeWire call, socket, thread or file access; nlohmann/json is its only dependency |
| `ear_folder` (`src/folder`) | `ConfinedFolder`: one folder held open from boot, every path resolved beneath it by the kernel (`openat2`): the recordings folder's files made (behind the recording kind's adapter, `main.cpp`'s glue), and the graph file and key files read and replaced only if unchanged (`renameat2`), a file's mode narrowed; `requireApart`, the boot check that no two folders overlap | Following `..`, absolute paths or (for the graph file) any symlink; anything else of EAR's |
| `ear_node_white` (`src/nodes/white`), `ear_node_file` (`src/nodes/file`), `ear_node_card` (`src/nodes/card`), `ear_node_rtp` (`src/nodes/rtp`) | One built-in type each (`white`, `file`, `card`, `rtp`), each one `TypeSpec` (its settings table, which also gives its setting kinds, and its banks) and one class with `configure` (`file`'s consumes an interim sink on its inside bank `writer`) | PipeWire and ALSA (`file` uses libsndfile, seen by `ear_node_file` alone; `card` and `rtp` ask the framework for their outside nodes, `file` for its recording); a folder; another node type |
| `ear_node_sendspin` (`src/nodes/sendspin`) | The built-in type `sendspin` as a port block with its own audio code and threads, not an outside node: the feed, the worker and notifier threads over the protocol library sendspin-cpp, the connection watch, the key store, `PortClaims`, mDNS (`Advertiser`); the only library that sees that library, privately | The library on a realtime thread or the main loop; PipeWire (it reaches the engine only through core's interfaces); a node state for a connection; another node type |
| `ear_all_node_types` (`src/all_node_types`) | `allNodeTypes(NodeFolders)` (only `sendspin` reads them): the one list of node types, generated at configure into `out/src/all_node_types/all_node_types.cpp` from the folders in `src/nodes/` (re-checked on every build; configure prints `Node types: ...`, and no source file calls a `make<Type>NodeType` by name); `main.cpp` and the all-type tests call and link it alone | Naming a type by hand; anything else; node types' own code (it links their libraries privately) |
| `ear_pipewire` (`src/pipewire`) | The in-process engine and the adapters that implement core's interfaces: port blocks (`pw_filter`, with level metering, each cycle's `GraphCycle` and latency carried per bank pair), links (`link-factory`), live ports (the registry), interim sinks and legacy handoffs (`pw_thread_loop`), outside nodes (one lifecycle, `joinOutsideNode`, with two kinds: sound cards as ALSA adapter nodes and RTP receivers as PipeWire's rtp-source module), the main-loop executor, the scheduling report | Policy |
| `ear_output` (`src/output`) | Where standard output goes: the output thread (`OutputWriter`), the one writer of the real standard output, behind a bounded queue that refuses instead of waiting; the capture of descriptors 1 and 2 (`DescriptorCapture`), installed before any thread starts, pipes read a line at a time by its own thread, which also drains the descriptors it is given (the engine log's bell); the main-loop watchdog (`StallWatch`): a thread reading a heartbeat the main loop beats, told where the loop is by a callback (`main.cpp` passes `MainLoopActivity::current()`) | A record's text (that is `ear_control`'s); anything else of EAR's |
| `ear_control` (`src/control`) | Standard output's one encoder (`StatusLog`: every record, a node's notes and the log among them, onto `OutputLines`); the JSON-RPC protocol (envelope, the one decoder of node and route JSON, commands, events, sessions and subscriptions); the graph file's plain types on the wire (`GraphFile`, `Diagnostic`, the file's refusals); the two interfaces at its edge, `GraphRequests` (where every request goes) and `GraphFileListener` (who hears each file change); `MainLoopGateway`, the one crossing between the main loop and the control thread | Sockets; graph rules; knobly; holding the graph on the control thread |
| `ear_config` (`src/config`) | The graph file keeper (`GraphFileKeeper`): `ear.conf`'s model through knobly (read, diagnostics, edits at the source), the keeper's one background thread (the working copy, the disk base, the folder watch, writes) and its main-loop side (plan and commit through the desired graph, the running version, the last refusal, pending restart); the only package that sees knobly, privately | Judging a graph; letting a knobly type leave its thread; timers |
| `ear_server` (`src/server`) | The control thread and port 7777 (Boost.Beast): the editor's files, `/rpc`, Host and Origin checks, per-client queues, the level bell | Graph state; PipeWire |
| `ear` (`src/main.cpp`) | The composition root: wiring, boot and stop order, and the glue between packages that do not see each other (the output lines, the recording kind's adapter over the recordings folder) | Computing anything |
| `web/` | The graph editor; shares no code with C++, only the wire format and `test/protocol/` | Graph rules (ear is the one source of truth) |

Dependencies point one way: `ear_core` <- {the five `ear_node_<type>` libraries, `ear_pipewire`,
`ear_control`, `ear_config`}, `ear_node_<type>` <- `ear_all_node_types`, `ear_folder` <- {`ear_node_sendspin`, `ear_config`},
`ear_control` <- {`ear_server`, `ear_config`}, and all of them <- `ear` (which links the node types only through `ear_all_node_types`); no node type links another, `ear_output` depends on nothing of EAR's, and `ear_folder` depends on
nothing of EAR's. No Boost, knobly or sendspin-cpp type is in a public header.
An interface lives with the package that consumes it: `node_host.hpp` declares
`PortBlockFactory`, the resource kinds' adapters (`HandoffAdapter`, `SoundCardAdapter`, `RtpReceiverAdapter`; `SinkAdapter` in `sink.hpp`) and the
`ResourceRegistry` that holds one of each, `timers.hpp` declares `Timers`, `reconciler.hpp` declares `LinkMaker` and the
live-port and link listeners, and `ear_pipewire` implements them; `requests.hpp` declares
`GraphRequests` and `GraphFileListener`, and `ear_config` implements the first while the gateway
and `StatusLog` implement the second. Each package's headers form a CMake header set that is
compiled on its own (`VERIFY_INTERFACE_HEADER_SETS`); `ear_pipewire`'s headers only
forward-declare PipeWire's types, so PipeWire's headers stay private to it.

Adding a node type (`src/nodes/` holds node types and nothing else). A new folder registers its type: nothing
else names it, not `main.cpp`, not `src/nodes/CMakeLists.txt`, not a list of types, and configure looks for folders
again on every build. The one entry a type adds outside its folder is its valid settings (step 5), with its refusals'
cases.

1. A folder `src/nodes/<type>/` (a lower-case letter, then lower-case letters and digits) with `<type>.cpp`, the
   public header `include/ear/nodes/<type>.hpp` (one function, `make<Type>NodeType()`, no argument: what a type needs
   from outside it acquires in its `configure`, its recording file included) and `tests/<type>_test.cpp`.
   `sendspin` alone still takes `const NodeFolders&` (its key files in the config folder and the boot log), an interim
   seam until slice 3 of `docs/design/node-framework.md` gives it stores; the generated list calls each function as it
   is declared.
2. Its `CMakeLists.txt`: `ear_node_type(SOURCES <type>.cpp ... TESTS tests/<type>_test.cpp ...)` makes the library
   `ear_node_<type>` (a header set with `VERIFY_INTERFACE_HEADER_SETS`, linking `ear_core`) and, under `BUILD_TESTS`,
   `ear_node_<type>_tests` through `ear_catch_tests`, linking the type harness, so a type cannot leave out its tests or
   header check (a folder without the call, or with a stray file beside it in `src/nodes/`, stops configure). Plain
   CMake after the call adds what the type else links: any outside library (libsndfile, sendspin-cpp) `PRIVATE`, seen
   by no other target. No type links another: configure refuses it once the whole project is read
   (`ear_refuse_node_type_links`), through a helper, an alias or a generator expression too.
3. In the type's private header `src/nodes/<type>/<type>_spec.hpp`, inside its own named namespace
   `ear::nodes::<type>` and never an unnamed one, every constant `inline constexpr` (`ear/core/type_spec.hpp` and
   `ear/core/lifecycle.hpp`, which the header includes; `docs/design/node-framework.md` sections 6.4, 6.10, 7.1, 7.2 and
   9.2, its "Slice 2 as built" and `docs/design/node-lifecycle-modules.md` section 3: GCC 14 gives a template keyed on
   an object in an unnamed namespace external linkage, so two files' same-named rows or specs would silently share one
   table or one `Io`; `SettingsTable`, `NodeTypeOf`, `Io`, `Members`, `NodeOf`, `CreateOf` and `makeNodeType` refuse
   one at compile time): the type's settings as one `constexpr SettingsTable` of typed rows, its banks as constants
   (`bank::output<"stream">`, `bank::input`, `bank::insideOutput`, `bank::insideInput`: name and direction fixed at
   compile time), and both in one `constexpr TypeSpec spec{.settings = ..., .banks = Banks<...>{}}`. Then one class,
   not final, which the framework makes once per configuration (each change of settings, restart or retry makes a new
   one), its members found by name whatever their access, so each must be public and not overloaded:
   `static Members<spec> ports(const CheckedOf<spec>&)` (each bank once, `members(n)` for `1`..`n` or
   `members("left", "right")`); optionally `static std::expected<void, std::string> check(const CheckedOf<spec>&)` (a
   rule across settings, run at parse); a constructor taking `NodeOf<spec>&` (kept for the instance's life: `format()`,
   `node()`, and `handle()`, the configuration's `say` and `lost`, which a thread or consumer the instance owns may
   keep) or none; and `Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&)`, which only acquires and
   constructs: `cx.soundCard(bank, SoundCardRequest{...})` and `cx.rtpReceiver(bank, RtpReceiverRequest{...})` on an
   inside bank (the handle names the bank: leave `.bank` out), `cx.openRecording(RecordingRequest{path})`,
   `cx.sink(bank, SinkShape{.frames = ...}).consumedBy(std::make_unique<Consumer>(...))` on an inside output bank (the
   interim sink, `ear/core/sink.hpp`: the block writes the bank's frames into its ring after the code, every cycle the
   code was driven, and the sink's own thread hands its `SinkConsumer` `begin()` once, before the node is configured,
   then each stamped `Chunk` in order, then `finish()` at the teardown; exactly one consumer per sink, and the bank is
   never faded; `file`'s `WavWriter`), and
   `cx.declareLatency(from, to, Latency{...})` for its own code's latency on an output bank; then `cx.use(code)`
   exactly once, with `passThrough` of `bankPair`s or a struct with `void process(Io<spec>, const GraphCycle&) noexcept`
   and optional `prepare` and `release`, held to exactly those signatures, release `noexcept`; and answers
   `Configured::applied()`, or `Configured::failed(detail)` for what only the type can see (the node fails
   `start-failed: <detail>`). Optionally `void start()` (once its code is driven) and `void stop()` (once before each
   silence after a `start()` that returned); each hook is timed, and one past 20 ms on the main loop is `ear.hook-slow`.
   The change version, `Changed configure(ChangeOf<spec>&, const CheckedOf<spec>&, const CheckedOf<spec>&)`, compiles
   (its signature checked) but is ignored until slice 3d: in 3a every change of settings restarts the node (the owner,
   2026-11-09: "Accept, ignore until 3d"). A rule the type breaks at a call (a second `use`, a call after its
   `configure` returned, a card's direction against its bank's, latency twice for a bank) or at the return (no `use`,
   a pass-through the ports refuse, a sink with no consumer or two), and a hook that throws, fail the node
   `start-failed` with the rule's text
   (`configure threw: use called twice`), even when the type catches the throw; none takes ear down. What needs a library only the type links is declared
   there and defined in `<type>.cpp`, which defines `make<Type>NodeType()` as `makeNodeType<<type>::spec,
   <type>::<Class>>()`. Name, kinds and `parse` come from the table and cannot be overridden; a table not named for its
   folder does not compile, nor does a class missing `ports` or `configure` or with one of the wrong shape, a static
   `start` (the old SPI), a `start`, `stop`, `check`, `prepare` or `release` of the wrong signature, a call `CreateOf`
   does not offer (`hold`, `whenReady`, `unclaim`: slice 3d's), a sound card on a public bank, a bank the spec lacks in
   `Io` or a context, a misspelt bank, or a write into an input (`src/core/tests/compile_fail/type_spec_bad.cpp`). A
   bank handle is only its name, direction and reach, so another type's bank with all three is the same handle (sound:
   `Io` resolves it by name); a code written against another type's spec fails the node at `use`. A refusal's text and
   order are fixed by the table, then `check`. (`sendspin` alone still writes the old SPI, a `NodeTypeOf` whose
   config's `start` returns a `StartResult`, run through the legacy adapter until slice 3c.)
4. Its tests include `<type>_spec.hpp` (`ear_node_type()` puts the folder on their include path) and run it through
   `ear::testing::TypeHarness<<type>::spec, <type>::<Class>>` (`ear/testing/`), a real `NodeHost` over in-memory
   fakes, naming banks by the spec's handles (`harness.input(card::play, 0)`): `parse` (nothing, or the refusal's
   text), `add`, `blockReady`, `ready(bank)` and `lose(bank, detail)` for each companion it asked for (`companion(bank)`
   shows what was made), `process` over the harness's buffers (outputs NaN-filled first, the block's 5 ms fade applied),
   `stop`, `start`, `restart`, `change`, `disable`, `enable`, `remove`, and what the node shows (`phase()`, `health()`,
   notes, diagnostics, `latencyPlan()`, `recordingRequests()`, a legacy type's `handoffs()`, and each sink's chunks as
   its consumer got them, at once on the test's thread: `companion(bank).chunks`); no PipeWire. The harness does not see a class's `start()` and `stop()`: their
   pairing is `node_host_test.cpp`'s.
5. Valid settings for it in `validSettings` (`test/nodes/all_types.hpp`), which together give every key it declares.
   The all-type tests list no type but need these, and fail naming the type (never skip) without them; they run every
   type through the harness (added, configured, running once every companion is ready, every output sample written,
   then stopped, started, restarted, disabled, enabled and removed), but those `notYetInHarness` names (`sendspin`,
   until slice 3). Its refusals' texts and order are tested in `test/nodes/settings_refusals_test.cpp`.

Removing a type is deleting its folder and its entry in `validSettings` (and in `notYetInHarness`, if there), its cases in
`settings_refusals_test.cpp` (a case left there fails naming the type) and its name in
`test/nodes/node_types_test.cpp`.

Events, not polling, move everything, with one exception: a `sendspin` node's worker polls its
library every 10 ms, and watches its connection with 5 s and 31 s timers, because the library has
no wake hook and no event for a lost connection or a failed dial (`docs/design/sendspin.md`
section 11); they stay inside that thread. `main.cpp` wires: the graph file keeper tells the gateway
and then `StatusLog` of each change to the file, before the commit it causes; `DesiredGraph`
tells its observer (the gateway) of each change first, then `NodeHost` about nodes and the
`Reconciler` about routes; `LiveGraph` tells the `Reconciler` which ports exist; `Links` tells it when a link is active,
failed or gone; `NodeHost` and the `Reconciler` tell `StatusLog` and the gateway, and `NodeHost`
tells `StatusLog` alone of a node's notes. So a change's
notification always comes before the statuses it causes.

Threads (the owner, 2026-10-30: "The control port shouldn't run in the PipeWire context, because
it will not be real time"):

| Thread | Runs |
|---|---|
| PipeWire's data loop (realtime) | every port block's `process`, metering and level windows; the sinks' taps and the handoff writers |
| PipeWire's main loop (the main thread) | every piece of graph state: the engine, desired graph, node framework, reconciler, adapters, the gateway's graph side, the keeper's main-loop side (plan and commit, the running version); `ear.conf`'s first read at boot |
| The control thread | port 7777: sockets, JSON-RPC, sessions, queues; it holds only `GraphGateway`, which sends every request whole to the keeper and gets replies and events back in order from the main loop |
| The keeper thread (`ear_config`) | the graph file keeper: every request, verdict, disk check and write, one step at a time in arrival order; the working copy (knobly's graph object, which never leaves this thread); the inotify watch on the graph file folder; writes of `ear.conf` when no step is waiting. It waits in `poll()` with no timeout and posts each step to the main loop |
| Sink and handoff threads | a `file` node's sink (its `WavWriter`); a `sendspin` node's handoff (`write_audio`) |
| The output thread (`ear_output`) | every write to the real standard output: the encoder's lines, from a bounded queue (a full one refuses the line, and the next record counts it) |
| The capture thread (`ear_output`) | reads the pipes on descriptors 1 and 2 into `log` records, and drains the engine log's ring (PipeWire's and alsa-lib's messages) when its bell rings |
| The watchdog thread (`ear_output`) | `StallWatch`: every 250 ms reads the heartbeat a 100 ms main-loop timer beats; once a look finds the heartbeat older than 500 ms it writes `ear.main-loop-stalled` (with `ms`, the heartbeat's age, and `in`, where the loop is; any stall longer than 750 ms is seen, a shorter one past 500 ms may not be), and `ear.main-loop-resumed` (`ms`, the gap between the beats either side, measured by the beat itself) once beats return, straight to the output queue; it never touches PipeWire or waits on the main loop (`docs/design/node-lifecycle.md` 5.12) |
| A `sendspin` node's own threads | the worker (the library's "main loop": the key file, its start and stop, every listener callback but `on_audio_write`, the connection watch, mDNS; a 10 ms `poll`, since the library has no wake hook or connection event), the notifier (`notify_audio_played`, waiting on the block's bell) and the library's own |

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
   socket and no session manager; ear's internal clock, the dummy driver, clocks the graph at the
   graph frequency with the block size's frames a cycle (both fixed at boot). It is always the
   driver, whatever links or fails (it joins the blocks' group and outranks every card), and it
   tracks `CLOCK_MONOTONIC_RAW` (`clock.id = monotonic-raw`), so NTP's slewing never reaches the
   graph. The data thread flushes denormals to zero. The `pipewire-engine` skill covers the
   configuration and traps.
2. A node starts (`NodeHost`) block first: `FilterPortBlocks` makes its port block, once for
   the node's life: one always-processing `pw_filter` named by the node's name, with one 32-bit
   float mono port per declared port, routable and inside banks alike. Then a configuration starts
   (`docs/design/node-lifecycle-modules.md` section 3): its config makes the instance (the type's
   class, or for a legacy type the adapter over its `start`), whose `configure` only acquires
   resources through `CreateContext` (sound cards, RTP receivers, recording files, interim sinks; a
   legacy type's handoffs too, through `StartContext`: a closed list, one adapter each in the
   `ResourceRegistry`)
   and gives the code (its own, against `Io<spec>`, erased once into an `AudioCode`, or the
   framework's `passThrough` between banks); the framework readies every resource, then a legacy
   recipe (`Ready&`) builds the run object with its threads, then `AudioCode::prepare` runs; once
   the block is ready (its first STREAMING state) the code drives it by an asynchronous drive,
   applied between two cycles on the data loop and completed on the main loop, and then the
   instance's `start()` runs. New settings restart the configuration behind the same block, one at
   a time. The node is `running` once that drive completed (the lifecycle machines,
   `node_lifecycle.hpp`, `docs/design/node-lifecycle.md`). Every block processes whether routed or
   not; an unlinked input reads zeros.
3. On PipeWire's realtime thread each cycle the block calls `AudioCode::process` with one span
   per port and the graph cycle (`GraphCycle`, made by the framework: its start on both
   `CLOCK_MONOTONIC` and `CLOCK_MONOTONIC_RAW`, the driver's position, id and rate against the
   raw clock, the next cycle's start, an xrun count, whether the clock jumped, and the latency
   downstream of each output), so no type reads a clock. `white` fills its output with noise.
   Latency is carried per bank pair (`docs/design/node-framework.md` section 9.4): each block
   tells its linked ports its banks' latency (a pass-through's pair forwards what lies beyond it,
   plus what a configuration declared with `CreateOf::declareLatency` or a legacy run with
   `StartContext::declareLatency`: `rtp`'s `latencyMs`, `sendspin`'s feed).
4. `LiveGraph` reports each port as the engine's registry announces it (node name, port name,
   direction, port id). When both ends of a wanted route are live, the `Reconciler` asks `Links`
   for a link by port ids; the route is `linked` once the link is active, and `pending` again
   if an end goes, relinked when it comes back.
5. Outside nodes. A `card` or `rtp` node's port block has inside banks no route can name
   (`to_card`, `from_card`, `from_rtp`), and its code is the framework's pass-through between
   them and its public banks: the type writes no audio code. Once the block is ready the
   framework makes the outside nodes its run acquired, each joined to the inside bank its request
   names and named `<node>.playback`, `<node>.capture` or `<node>.rtp` (one run never names two
   alike). Every kind shares one lifecycle
   (`NodeHost` in ear_core, `joinOutsideNode` in `src/pipewire/outside_nodes.cpp`): the outside
   node runs alone until its body says it runs, then one link per channel joins it, by its
   global id, to the block's inside ports; it is ready once every link is active, and on any
   failure its links and node are torn down before it is reported lost, so the node is `failed
   engine-refused: <detail>`, its block and routes stay. Sound cards (`AlsaSoundCards`): one ALSA
   adapter node per direction, running alone as its own driver until its PCM runs; once linked to
   its block it follows the internal clock with its own rate match, and its playback latency
   reaches every block routed to it. A lost one moves no clock. CI's test card has no clock, so
   its tests turn its rate matching off (`clocklessCardProperties`); a real card keeps it on. RTP receivers (`RtpReceivers`): PipeWire's
   `libpipewire-module-rtp-source` loaded into the engine, running once its socket is bound; it
   follows the graph's clock and resamples the sender to it. The `pipewire-engine` skill has the
   details and traps.
6. Levels: each block meters its public ports every cycle and publishes a window (1/30 s of the
   graph clock, on the same clock positions for every block) into a lock-free mailbox, then
   rings an eventfd; the control thread reads every block's latest window and sends it to levels
   subscribers.
7. A `sendspin` node's audio code reads the library's decoded audio from a feed (a lock-free ring
   of about 43 ms, the handoff's reverse) and tells the library what played, dated from the
   cycle's start on the raw clock (`GraphCycle::startRawNs`) plus the latency downstream of its
   outputs (a following card's buffer), never from when the thread woke (`docs/design/sendspin.md`
   section 7). Its inputs go through a handoff to the library.
8. `file` cannot write a file on the realtime thread, and writes no audio code. Its configure
   opens the recording (a descriptor the framework opens beneath the recordings folder and closes
   after the configuration's sink and instance) and asks for an interim sink on its inside output
   bank `writer` (`ThreadSinks`, `src/pipewire/handoffs.cpp`; docs/design/node-lifecycle.md 5.11):
   the framework's pass-through copies `record` to `writer`, and the block, after the code, writes
   each cycle into the sink's lock-free ring of interleaved floats (two seconds) with a stamp
   (the cycle's position and clocks) in a stamp ring beside it, waking the sink's own thread, which
   hands each stamped chunk to `file`'s `WavWriter` (libsndfile). The taps ride in the drive
   request, so the sink takes exactly the cycles the code drove: a stop's silence unbinds them, a
   start binds them again. The two rings drop together (no frame without its stamp); a full ring
   drops frames and counts them, and the count is the `dropped=` of the node's `removed` line.
   `sendspin`'s inputs still go through the legacy handoff (`ThreadHandoffs`) until slice 3c.
9. Stopping: `DesiredGraph::close()` refuses every later edit, the keeper is told to stop
   (edits still queued get `stopping`, reads are still answered), then `NodeHost::stopAll` takes
   each node through `stopping` and tears its run down by dependency (`docs/design/node-framework.md`
   section 6, I4 to I7): the barrier, a drive of no code (no `process` of the code once it
   completed), `release`, outside nodes destroyed and the port block destroyed, sinks and handoffs
   drained and finished on their own threads, the run object
   closed (a `sendspin` node's worker says goodbye, on its own thread, so nothing waits on the
   main loop), then on the main loop the run object, its code and its config freed; then
   `stopped`. Anything a run's threads tell lands only on that run (its generation), never on the
   next. Draining and closing have a deadline (10 s): a run past it is leaked, never freed
   (a `diagnostic` record, `node <n> ear.close-overdue: ...`), and the node moves on. A restart
   (`node.update`) runs the same teardown but keeps the block. A failed node's run is torn down
   at once, its block and routes kept. After the last node, the control port
   closes every client (1001), the control thread stops and EAR's main loop quits. Last, the
   main thread joins the keeper's thread, which first writes the last accepted version of
   `ear.conf` if it is not written yet (a hung disk delays exit without limit).

Not built yet: routes naming a whole bank (the owner, on them, 2026-11-06: "Later (Recommended)"), any grouping of banks in the editor, serving PipeWire's socket, the program supervisor and the `airplay` type, a login
on port 7777, card retry and hotplug, arm64 images; for the clock (`docs/design/node-framework.md` section 10): an
internal clock anchored to one card (that card bit-perfect), a follower watchdog for a frozen card, latency a run reports
while it runs (only declared at start), and the MOTU checked as a follower; for the graph file: `@include` (more files than `ear.conf`), a separate writer thread (so an
edit never waits behind a write), undo, and changing the graph frequency or block size while
ear runs. For `rtp` (`docs/design/rtp.md` section 9): a failure for a port that cannot be
bound, a "receiving" status, multicast tested, a larger datagram size (`net.mtu`) for many
channels, SAP discovery, IPv6, an RTP sender. For `sendspin` (`docs/design/sendspin.md` section
11): volume and mute, line sense, the editor showing notes
and the code, a restart verb on the control port, IPv6, `wss://`, several servers at once,
choosing the advertised interface, RFC 6762 probing, more than stereo.

## Tests

Catch2 v2; each package has one test executable, and `ear_catch_tests` (`test/CMakeLists.txt`:
`catch_discover_tests` with Catch's `--warn NoTests`) makes each test case its own CTest test, so
`ctest --parallel` runs them side by side; a CTest test that matches no test case fails. Keep `;`
out of test names: CMake splits a name at it. A test case whose load would starve the realtime-timed
tests beside it is named by tag in `ear_catch_tests(<target> SERIAL <tag>)`, which gives it CTest's
`RUN_SERIAL` (only S2, `[s2]`, in `ear_pipewire_tests`).

| Executable | What it proves |
|---|---|
| `ear_core_tests` | Port banks and whole port names (a mis-shaped part refused); the desired graph's checks, plan and commit (event order, a port change as a replacement, stale plans) and errors, observer order, closing; the node state machine (every state against every fact); `NodeHost` with fake adapters (restart, removal, sound cards and RTP receivers, both in either order, a lost block, the two-phase start and its recipe, one name per engine object of a run, the pass-through, an instance of the new SPI with a probe: `start()` once driven and `stop()` paired with it, a `start()` or `stop()` that throws, `ear.hook-slow` once per node and hook, every rule a type breaks failing its node `start-failed`, even when the type catches it, a legacy recipe beside an outside node, a throwing recipe or close and a legacy engine refusal contained, a recording opened by a `configure` that threw closed after its instance, the interim sink (its consumer begun before the node is configured, a lost in begin, its bank never faded, its tap bound by every drive and no silence, a stop closing nothing, a restart waiting for its close, every rule about it a failure, its engine name checked against the outside nodes'), a run's latency plan reaching its block, runs with threads: their notes and `lost` crossing to the main loop, `close`; the teardown invariants of `docs/design/node-framework.md` section 6: the barrier first, a handoff consumer blocked at teardown, a run mid-handler, a late write from an old run, a run that never closes leaked past the close deadline); `FrameRing`; the reconciler; levels; the graph read; card ids and RTP addresses; settings tables and their typed rows; the type SPI through the harness (a spec's banks as ports, `check` at parse, the class made from `NodeOf` per configuration and its hooks called, `Io` by bank, a layout that disagrees with the spec, a code of another spec failing the node at `use`, a configure answering failed, outside resources readied and lost, a class with the change version restarted on a change) and recordings opened at start and closed with the run, after its run object even when the host goes without stopping. Pure, milliseconds. Beside it, CTest tests that what must not compile is refused for its reason (`src/core/tests/compile_fail/settings_table_bad.cpp`: a bad row, a row or rule made or changed by hand, a misspelt or unlisted row, an unchecked read of an optional row, a type that disagrees with its table or folder; `type_spec_bad.cpp`: a bank the spec lacks, a misspelt bank or a write into an input in `Io`, `ports()` naming a bank the spec lacks or leaving one out, a backwards pass-through, a bad or doubled bank name, a process that may throw, a class of the wrong shape (final, no `ports`, a `configure`, `start`, `stop` or change version of the wrong signature or context, a private or overloaded `start`, `stop` or `check`, neither constructor, a static `start`), a `check`, `prepare` or `release` of the wrong signature, a call `CreateOf` does not offer, a context or `declareLatency` given a bank the spec lacks or the wrong way, a sound card, RTP receiver or sink on the wrong bank, `Configured::restart`, a spec or a row in an unnamed namespace, a spec not named for its folder, the harness given a bank the spec lacks). These are the slow part: each runs the compiler, 82 of them about 110 s at one job, and they match the image's GCC's wording, so a compiler bump may need their regexes edited |
| `ear_folder_tests` | The confined folder on real temporary folders: made if missing (never its parents), `create()` beneath only, `read()` and `replaceIf()` refusing symlinks, directories, FIFOs, large files and paths out, replace-if-unchanged keeping the file's mode, a new file's mode under any umask, `restrictMode()`, a folder it cannot write, an inotify watch through `heldPath()`, `requireApart()` (one folder twice, nesting, a symlinked alias) |
| `ear_node_white_tests` | `white`'s ports, noise statistics, stop, start and restart, through `TypeHarness`, by its spec's bank handles |
| `ear_node_file_tests` | `file` through `TypeHarness`, by its spec's bank handles, over a real recordings folder and the harness's sink: exact samples written, every cycle the code drove and none while stopped, a restart finishing the old WAV and opening the new one, a WAV libsndfile cannot open failing the node from the sink's begin, an unopenable path (no sink asked for), absolute and `..` paths (invalid settings), a symlink out of the recordings folder and no recordings folder at all (at configure) |
| `ear_node_card_tests` | `card` through `TypeHarness`, by its spec's bank handles: settings (its `check` among them), ports (inside banks included), the cards its configure asks for, readiness and loss, its playback bank's fade, a stop keeping its cards, and what the framework's pass-through between its banks carries |
| `ear_node_rtp_tests` | `rtp` through `TypeHarness`, by its spec's bank handles: settings (each refusal's exact detail), ports, the receiver its configure asks for, the latency it declares in the block's plan and what the framework's pass-through carries |
| `ear_output_tests` | The output thread on real pipes (order, a reader that does not read refused at once, everything written at stop, a closed reader survived) and the capture in its own test program (C streams, iostreams and `write` on descriptors 1 and 2 as lines, a line past its bound cut and marked, a last partial line, a watched descriptor drained on the capture thread, the real descriptors put back, stop past a surviving duplicate of descriptor 2), the queue's byte bound, a bounded spill, and in forked children a crash's last words on the real descriptors (`std::terminate`, `abort`, a fault); the main-loop watchdog (its judge on a clock stepped by hand, a real short stall reported once and resumed once, nothing under steady beats, before the first beat or after stop, stop joining at once) |
| `ear_control_tests` | The JSON-RPC envelope (the spec's examples), commands, events, the graph file's types and encoders, standard output's records (every kind, the framework's `diagnostic` among them, field for field and in order against `output-records.json`, drops counted, a flooding source paced while notes and states get through, bad UTF-8 replaced, one order from many threads), gateway and sessions, all against `test/protocol` (`graph-file.json` included) |
| `ear_config_tests` | The graph file model (the record's example, each of ear's codes at its place, `@include`, missing and unreadable files, knobly's nesting limit), places in UTF-16 columns, edits at the source (a value changed where it is defined, `shared-definition`, removal with its routes, quoting); the keeper's sequencer stepped by hand on one thread over a real folder (boot offers, accepted and refused edits, both hand-edit collisions, a typo, `graphFile.set`, its own write's watch event, batches, shared writes, write failures, pending restart, stop); the keeper's thread and the whole keeper with inotify |
| `ear_node_sendspin_tests` | The feed on real threads, PCM, the audio code's clock and skips, the connection watch with fake time, settings (each refusal's exact text), key files on a real folder, port claims and the probe (a real `TIME_WAIT` among them), the mDNS instance's name and its on-link check, what a peer's text becomes on a status line, the notes' rate, capture stamps; through a real engine and the real server, the time filter: within 1 ms on the dummy driver and through a following test card, dated by the card's latency, a lost card leaving the clock alone, a stream ended mid-play |
| `ear_node_types_tests` | What holds for every type together (`test/nodes/`, which lists no type: it takes them from `allNodeTypes`, as `main.cpp` does): a folder in `src/nodes` makes one type of its own name, the five names, each type's settings table (every key declared once, a value of each kind parses, setting kinds are what `parse` checks) and every refusal's text and order; every type but those `notYetInHarness` names run through the harness (configures, runs once its companions are ready, writes every output sample of every cycle, stops, starts, restarts, is disabled, enabled and removed); also, as CTest tests, that configure refuses one node type linking another, in each spelling (a scratch project, `test/nodes/link_refusal/`), and refuses an empty list of types (`test/nodes/no_types/`); and that no file under `src/nodes/` names standard output or error or writes to descriptor 1 or 2 (`test/nodes/standard_output_guard.cmake`, itself tested on each spelling it refuses, `std::print` and `std::println` among them, on look-alikes it lets through, and failing when it would check no file) |
| `ear_pipewire_tests` | The real in-process engine with no daemon, runtime directory or D-Bus: blocks process unrouted, live ports, a ramp carried sample for sample over a link, the clock and each cycle's `GraphCycle` (both clocks, the driver, its rate, the next start, discontinuities), latency carried per bank pair, denormals flushed in `process`, teardown, SIGTERM; the legacy handoff (order, drops, close, no allocation on the writer); the interim sink (`sink_test.cpp`: order and stamps, a chunk wrapping the ring in two calls, the stamp ring and frame ring dropping together, close draining then finishing, a `lost` said in begin landing before `begun`, no allocation on the tap, and a ramp through a real block's sink starting at the first driven sample and ending at the last, across a stop and a start, under an invoke storm); the graph group, `drive`, level windows on the clock, the main-loop executor (and the watchdog reporting a real stall of its loop, with where it was), the scheduling report; sound cards through a test ALSA card (alsa-lib's file and null PCMs, made clockless): following the internal clock, channels sample for sample, its latency carried through the pass-through, a card that cannot open, a lost card leaving the clock alone; RTP receivers fed by a test sender on 127.0.0.1: following the dummy driver, channels by sine fit, L16 at 44100 resampled, a sender restarting with a new SSRC, a lost receiver, a receiver made again at once for the same names; ear's own log: PipeWire's messages from the main thread and the data loop, the context giving it to every plugin, a full ring counted, alsa-lib's errors |
| `ear_server_tests` | Port 7777 on real sockets with a fake main loop and gateway: files, Host and Origin, round trips, the 1008 bound, a client that resets, stop with 1001, levels |
| `ear_system_tests` | The owner's end-to-end tests, through the real `out/ear` on an `ear.conf`, reading its records (`child_process.hpp` matches each record's `msg` lines): white noise recorded to the routed channel only (exact zeros on the other, header finished, `dropped=0`), every line a record with `seq` unbroken; every type's old port names refused as `unknown-port`; a recordings folder it cannot make exits 1, its last record an `exit`; the graph file (the record's example booted, an editor edit announced, applied, answered and written back, a hand edit winning, a hand typo blocking edits until Save fixes it, `ear.conf` as a symlink out and an `@include` out refused, boots with a typo, a refused route or no file running nothing, the fallback flags, overlapping folders exiting 1); an `rtp` node's tone recorded channel for channel, and its silence with no sender; a `sendspin` node against the real server: pairing by a code, a ramp frame for frame in the WAV (the library's clock corrections alone excepted, at most one blended frame per chunk: `checkRamp`, itself tested on hand-made frames, `docs/design/sendspin.md` erratum 42), no audio for a server that trusts without pairing, noise to the server, identity across restarts, 20 wrong rounds and their release, a corrupt key file, no server at all, peers that never answer at a restart or stop, a slow upgrade, listening, mDNS discovery and removal, a server lost and back, a held or shared port, silent sockets held or flooding from its address and others beside a server pairing, waiting on a code and playing, a slow upgrade, a paired server not yet activated, a peer resetting sockets as they close, an upgrade with `Origin`, one dialling and one listening node together, no socket on a dialling node; and through the control port: the scheduling and listening lines and the editor's files, a client's edits heard in the WAV in event order, levels, removal and errors, file paths outside the recordings folder refused with nothing written, edits during stop refused as `stopping`, a sound card node linked |
| web (vitest, `make web-test`) | The editor's reducer against every `test/protocol` fixture, the graph file's state and the text pane's logic (drafts, stale files, Save, refusals), the reconnecting client with a fake socket, connection rules, levels and layout. No browser; rendering is checked by eye |

`test/CMakeLists.txt` also runs `ear --help` (the binary loads its shared libraries),
`ear_carried_patch_check` runs what configure asks of a tree that must carry a patch, and
`ear_engine_conf_beside_ear` checks the copy beside the binary matches the source. Tests are
event-gated: they wait for the event they need, and a deadline only fails a test, never decides
when something happens. Verify audio here, never on real hardware. The engine, ramp, RTP and
`sendspin` clock tests need PipeWire's data thread to keep time: on a heavily loaded host it stalls for hundreds
of milliseconds (graph xruns), and they fail without a bug (`docs/design/rtp.md` erratum 3; the
`pipewire-engine` skill says how to tell). Run one, inside the image:

```bash
scripts/dev.sh ctest --test-dir out -R "lands on left" --output-on-failure   # by test name regex
scripts/dev.sh out/src/nodes/white/ear_node_white_tests "[white]"          # by Catch tag
```

## CI

`.github/workflows/test.yml` runs on every pull request and every push to `master`, four
`ubuntu-24.04` jobs in parallel: `test` builds the dev image with buildx (layers cached in
GitHub's cache), restores the fetched trees and ccache, and runs `scripts/dev.sh make test` with
`EAR_DEV_IMAGE=ear-dev` (on failure it uploads ctest's logs as `ctest-output`); `web` runs `make
web-test` in the same image; `format` runs `make format-check` in the same image and fails
on any C++ file `.clang-format` would change; `image` checks `scripts/version-tags.sh`, builds the run image and
runs `scripts/image-smoke.sh` on it. On a push to `master`, once all four pass, `release` and
then `publish` run (Run image and releases, below). A newer push cancels a pull request's run,
never a `master` run. Its comments give the reasons for each step and cache.

## Run image and releases

`docker/Dockerfile` has three targets: `dev` (the build and test image: Debian 13, PipeWire,
GCC 14, CMake, Node.js 24, knobly at `/opt/knobly`, `git` and the real Sendspin server's venv at
`/opt/sendspin-server`), `build` (`make release` and the editor's
build) and `run`, what ships: `debian:trixie-slim` with only the run-time libraries, no
compiler and no knobly (it is compiled in); `/opt/ear/ear`, `engine.conf`, `web/` and `licenses/`
(what the `sendspin` node compiles in); user `ear` (uid 10001); ports 7777 and 8928 (a listening
`sendspin` node's default); default `--config-dir /var/lib/ear/config --recordings-dir
/var/lib/ear/recordings`, two volumes owned by `ear` (arguments given to the container replace
both, so pass them again). The graph file folder must be writable by uid 10001: a read-only
mount (a ConfigMap) cannot be edited, a `sendspin` node cannot make its key file there and fails,
and a first boot without `ear.conf` runs nothing until the pane saves one. A fourth stage, `knobly`, is thrown away: it only builds knobly's release tree
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
Bumps of the fetched dependencies (Boost's `boost_url` and `boost_sha256`, `KNOBLY_COMMIT`, those
in `cmake/SendspinDeps.cmake`) use the same message, `build(deps): upgrade <X> to <version>`. The subtrees were first added without
`--squash`, so their upstream histories (thousands of other projects' commits) are in ear's
history; the baseline tag `v0.0.0` on cb6d71f64c (the last `git subtree add`, 2020-05-27) ends
them, since semantic-release reads only the commits after the last `v*` tag.
`test/main.cpp` turns off Catch's POSIX signal handlers, which do not compile against glibc 2.34
and newer (its comment says why; Catch 2.13.5 fixes it). Boost and the `sendspin` node's
dependencies are fetched, not vendored (see Build and run), and so is knobly, built in the image from a pinned commit (`KNOBLY_COMMIT`).
PipeWire, alsa-lib and libsndfile come from the image's Debian packages; the
editor's npm packages are pinned by `web/package-lock.json`.

## Skills and references

Repo skills in `.claude/skills/`:

- `pipewire-engine`: running the engine in-process: what `engine.conf` sets and why, the
  modules, the internal clock (the dummy driver, always driving, tracking the raw clock), the graph
  group, `GraphCycle`, latency per bank pair and its trap, denormals, outside nodes (sound cards following the clock, RTP
  receivers through rtp-source), `drive`, levels, realtime priority, running without a session
  manager, the traps (xruns on a loaded host among them), and how to bump PipeWire.
  The PipeWire source behind each engine fact is cited in `docs/design/m1-white-to-file.md`
  section 1, `docs/design/batch2.md` sections 4.3 and 5 and `docs/design/rtp.md` section 4.
- `control-port`: talking to port 7777, its Host and Origin rules, the `graphFile.*` calls and
  write-behind, threads, queues, stopping and traps.
- `ear-on-talos`: the run image on a Talos node with the MOTU 24Ao (kernel module, privileged
  namespace, `/dev/snd`, SYS_NICE, the writable graph file volume, the card's own settings,
  checks on the hardware), FPP's RTP stream into the pod, and a `sendspin` node in the pod.
- `sendspin`: the `sendspin` node: pairing it with Music Assistant, connection modes, the identity
  file, what its notes mean, its threads and clock, what the spike learned, the protocol library's
  traps, trying it against the test server, the carried patch, and the real Music Assistant check
  still owed.

Research notes in `docs/research/`:

- `motu-24ao-drivers/`: MOTU's macOS driver (static analysis) compared with the Linux driver
  `snd-usb-motu`, and the MOTU 24Ao's USB descriptors; observations, inferences and comparisons
  kept apart (its `README.md` says how).
