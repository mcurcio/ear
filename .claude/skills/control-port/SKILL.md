---
name: control-port
description: How to talk to EAR's control port (7777, HTTP for the graph editor plus JSON-RPC 2.0 over WebSocket at /rpc), its flags, the Host and Origin rules, the graphFile.* calls that read and replace ear.conf, write-behind and hand edits, how it runs on its own thread and reaches the graph through the graph file keeper and the gateway, per-client queues, levels, stopping, and the traps found while building it. Use when calling ear from a script, test or browser, when a request gets 403, file-changed, file-invalid or shared-definition, a socket closes with 1008 or a reset, levels never arrive, an edit is lost or not written, stop hangs, or when changing src/server/, src/control/ (rpc, commands, events, gateway, session), src/config/ or web/src/rpc.ts.
---

# EAR's control port

`ear` serves one TCP port (7777 by default): the graph editor's files over HTTP and JSON-RPC 2.0
over WebSocket at `/rpc`. The wire format (every method, notification, error code and the
Graph object) has one home: `docs/design/batch2.md` section 6, with the fixtures in
`test/protocol/*.json` that the server's and the editor's tests both check. This skill is how
to use it and what bites.

## Flags

| Flag | Default | What |
|---|---|---|
| `--control-address` | `0.0.0.0` | the address to listen on |
| `--control-port` | `7777` | `0` takes any free port (tests); `ear` prints the bound one |
| `--web-root` | `<directory of ear>/web` | the editor's built files (the run image has `/opt/ear/web`; a local build serves `web/dist` with `--web-root web/dist` after `make web`) |
| `--allowed-host` | none | a DNS name the Host header may carry (repeatable) |

Once listening, `ear` prints `control listening on <address>:<port>`; wait for that line, never
for a time. A port it cannot bind exits 1 (`cannot listen on 0.0.0.0:7777: Address already in
use`).

## Talking to it

- **From a script:** `node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get` (Node 24's own
  WebSocket, no packages; the third argument is the params as JSON). It prints the result, or
  the error on standard error with exit 1. Inside the image: `scripts/dev.sh node
  scripts/rpc-call.mjs ...`.
- **From a browser:** `http://<host>:7777/`. In development, `EAR_DEV_PUBLISH=5173:5173
  scripts/dev.sh bash -c 'cd web && npm run dev -- --host 0.0.0.0'` serves the editor with hot
  reload and forwards `/rpc` to an `ear` on `localhost:7777` (run `ear` in the same container,
  or publish 7777 with `EAR_DEV_PUBLISH`).
- **From a C++ test:** `test/support/ws_client.hpp` (Boost.Beast): one HTTP request, or a
  WebSocket client whose every call waits for its own completion (its deadline only fails the
  test).
- Params are named only (positional params are -32602). `graph.subscribe` answers with the whole
  graph, then sends every change as a notification: list, then watch. `levels.subscribe` answers
  `{"windowsPerSecond": 30}` and then sends `levels` snapshots. A refused edit is -32000 with
  `data.code` (`unknown-port`, `stopping`, ...).

## The graph file over the port

Every edit is an edit of `ear.conf` (knobly), the one source of the wanted graph
(`docs/design/knobly-config.md`; the wire is `docs/design/batch2.md` section 6):

- `graphFile.get` and `graphFile.subscribe` answer with the GraphFile: `version` (opaque),
  `text`, `diagnostics`, `running` (the version the graph runs), `refusal`, `pendingRestart` and
  `writeFailed`. A subscriber then gets every `graphFile.changed` (GraphFile plus `cause`:
  `edit`, `disk` or `write`, and `droppedEdits` with `disk`).
- `graphFile.set {"text", "version"}` replaces the whole text. `version` must be the current one
  (HTTP's `If-Match` pattern), else `file-changed`: get the file again and redo the change on its
  text. A text with errors is `invalid-text` with `data.diagnostics`; a text the graph refuses
  gets the graph's code. Its `{}` comes once the graph runs the text. Sending the same text is
  `{}` and changes nothing.
- `node.*` and `route.*` keep their params but edit at the source: a value from `@set` or a
  snippet changes where it is defined, so every node using it changes. An edit that must change
  or remove a line that also makes other nodes or routes is `shared-definition` with
  `data.line` and `data.column`; send a `graphFile.set` instead. While the file has errors every
  `node.*` and `route.*` edit is `file-invalid`; only `graphFile.set` (or a hand edit) fixes it.
  A port change (a card's channel count) replaces the node: its routes and the node are removed
  and added again; there is no `ports-changed`.
- Order: `graphFile.changed` first, then the node and route events it causes, then the reply. A
  refused edit announces nothing; only its reply carries the code.
- **Handling `file-changed`:** another client, or a hand edit, changed the file since your
  `version`. Never resend the same text with the new version blindly (that overwrites their
  change); take the new text, apply your change to it, and send that. The editor's pane keeps
  the person's draft and says the file changed, with a reload (VS Code's "file changed on
  disk").
- **Whole texts:** each change sends the whole text, comments included, to every
  `graphFile.subscribe` client; there is no diff. A comment is seen by everyone who can reach
  the port, so never put a secret in `ear.conf`. A client's 256-message queue now holds whole
  texts, so a slow subscriber is closed with 1008 sooner. A message is at most 64 KiB, so a
  text over about 64 KiB cannot be sent with `graphFile.set` (ear reads up to 1 MiB from disk).
- **Write-behind:** the reply comes when the graph runs the edit; `ear.conf` is written after,
  when the keeper's thread has nothing waiting, so quick edits share one write (no timer) and an
  edit can wait a few milliseconds behind a write. `graph-file written <version>` on standard
  output says when it landed; a failed write sets `writeFailed` (cause `write`) and is retried
  after the next accepted change. Unwritten edits are lost if ear crashes.

## Who may connect

No login yet: anyone who reaches the port can edit the graph. Two checks keep browsers on other
sites out (`src/server/request_rules.hpp`, `hostAllowed` and `originAllowed`):

- **Host.** Every request's Host name must be an IP literal (IPv4, or IPv6 in brackets),
  `localhost`, or a name given with `--allowed-host` (case-insensitive); else 403, and `/rpc`
  is not upgraded. A missing Host is refused. This stops DNS rebinding: a name the operator did
  not give cannot reach the port. So reaching EAR as `http://ear.lan:7777/` needs
  `--allowed-host ear.lan`.
- **Origin.** A WebSocket upgrade with an Origin is accepted only when the Origin's host[:port]
  equals the Host header; no Origin (scripts, tests) is accepted. Another site's page gets 403.
- Files: GET and HEAD under the web root (`/` and a trailing `/` are `index.html`), a `..`
  segment never leaves it, 404 for a missing file, 405 for other methods. A WebSocket message or
  HTTP body is at most 64 KiB.

## How it runs

The owner (2026-10-30): "The control port shouldn't run in the PipeWire context, because it
will not be real time". So:

- `ControlThread` (`src/server/control_thread.cpp`) is one `io_context` on one thread; all of
  the port's sockets, sessions, JSON-RPC and queues run there, never on PipeWire's threads.
- The port holds no graph state. Its only handle on the graph is `GraphGateway`;
  `MainLoopGateway` (`src/control/gateway.cpp`) sends each message's commands whole to the graph
  file keeper (`GraphRequests`, `src/config/`), which runs on its own thread, and posts the
  replies, file changes and events back from the main loop to the control thread in one ordered
  stream. So a change's notification always comes before the statuses it causes, and a
  subscriber gets no event from before its snapshot.
- One message in flight per connection: the next is read only after every reply to this one is
  queued. A batch runs in order, each request as its own step through the keeper (an edit is
  judged and committed before the next is taken); consecutive reads are answered in one
  main-loop turn, and the reply comes in the turn of the batch's last step. A batch is not
  atomic: a refused edit does not undo the ones before it. Put subscribes last so their
  snapshots include the batch's edits.
- At most 256 responses and notifications wait per client. One more closes it with WebSocket
  code 1008; the client reconnects and subscribes again (the editor does, after 1 s doubling to
  30 s).
- Levels never queue. The realtime thread rings the level bell (an eventfd) once per window;
  the control thread reads `latestLevels()`, encodes once and gives each levels subscriber the
  newest snapshot, replacing one not yet written (each is complete, so nothing is lost).
- Sockets set `TCP_NODELAY`: messages are small and wanted now, and Nagle's wait behind a
  delayed acknowledgement cost tens of milliseconds a message on loopback.
- No timers: a connection stays until its peer or `stop()` ends it (the owner: "It's all event
  driven").

## Stopping

On SIGINT or SIGTERM, `ear` closes the graph to edits first and tells the keeper to stop: from
then on every edit, including one already queued, gets -32000 `stopping`, while `graph.get` and
`graphFile.get` still answer and node events (`stopping`, `stopped`) still arrive. When every
node has stopped (files finished), `ControlPort::stop` closes every client with 1001 and returns
only once every close handshake has ended; then the control thread stops and the engine quits.
Last, the keeper's thread writes the last accepted version if it was not written; a failure is
printed (`graph-file write-failed <detail>`) and reaches no client.

## Traps

- **A socket closed with the client's data unread sends a TCP reset.** The client then sees
  `Connection reset by peer` instead of the 1001 close. Beast's `async_close` writes the close
  frame, reads until the client's close reply (discarding what it sent meanwhile), and only then
  closes; `stop()` waits for every one of those handlers. Cost: a client that never answers the
  close holds `stop()` and `ear`'s exit until it goes (Beast's usual remedy, a close timeout, is
  the owner's call).
- **A client must keep reading while `ear` stops,** or its close reply never comes (above). The
  tests' clients read until closed (`Ear::stop(rpc)` in `test/system/control_port_test.cpp`).
- **Parallel tests and port 7777.** Every test that starts `out/ear` passes `--control-port 0`
  (and reads the bound port from the `control listening on` line); two on 7777 collide under
  `ctest --parallel`.
- **The level bell and a port with no levels subscriber.** The bell is rung at most once per
  window from the realtime side alone (`port_blocks.cpp`, `rungFor`). An earlier flag cleared by
  the reader went silent for good when the control thread answered a ring without reading levels
  (nobody subscribed): see `docs/design/batch2.md` section 11.
- **The editor's dev proxy** sends Host `localhost:7777` (`changeOrigin` in
  `web/vite.config.ts`), which the Host rule accepts.
- **Including the server headers:** no Boost type is in a public header of `ear_server`; Beast
  and Asio are PRIVATE to it.
- **Hand edits drop unwritten edits.** Whenever `ear.conf` on disk differs from what ear last
  read or wrote, ear reloads it, and editor edits accepted but not yet written are dropped
  (`graphFile.changed` cause `disk` with `droppedEdits`, and `graph-file dropped <n> edits`).
  If the graph then refuses the hand text, it keeps playing what it ran, including the dropped
  edit, and `running` names a version no file holds.
- **Editors that save in place (truncate, then write)** change the file before they finish. If
  ear reads it or writes in that moment, it sees an empty or partial file, counts that as a hand
  edit (dropping unwritten edits early) and may show errors until the editor's close event
  reloads the whole text. Editors that write a new file and rename it over `ear.conf` are seen
  once, whole.
- **In-place and mmap writers fire no event ear watches until they close.** ear watches
  close-after-write, renames, creates and deletes in the folder, not each write: a program that
  writes through `mmap`, or writes in place and keeps the file open, is not seen until it closes
  the file, and meanwhile ear's own write of unwritten edits can collide with it.
- **A raced reader can see ear's text for an instant.** A write swaps ear's text in
  (`renameat2(RENAME_EXCHANGE)`) and then compares what it replaced; if a hand save landed in
  between, it swaps the hand text back. A reader in that window sees ear's text.
- **`ear.conf` is never followed through a symlink** (`ear/unreadable-file`), and nothing is
  written while it is unreadable. `@include` is refused (`ear/include-unsupported`).
- **The watch event for ear's own write announces nothing**: the disk check compares content,
  so a test waits for `graph-file written <version>`, never for a quiet period.

## Tests

- `ear_server_tests` (`src/server/tests/control_port_test.cpp`): real sockets on
  `127.0.0.1:0`, a thread-backed fake main loop and a fake gateway: files, Host and Origin,
  round trips, two clients, the 1008 bound, a client that resets mid-write, stop with 1001.
- `ear_control_tests`: the envelope, commands, events, gateway and sessions, all pure, against
  `test/protocol` (`graph-file.json` for the graph file).
- `ear_config_tests`: the keeper stepped by hand on one thread over a real folder, both
  hand-edit collisions included; its thread with inotify.
- `ear_system_tests` (`test/system/control_port_test.cpp`, `graph_file_test.cpp`): the real
  `out/ear`, end to end, edits written back and hand edits winning.
- `scripts/image-smoke.sh`: the run image serves `/` and answers `graph.get` and
  `graphFile.get`.
- `web/` vitest: the editor's client and reducer against the same fixtures.
