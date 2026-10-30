---
name: control-port
description: How to talk to EAR's control port (7777, HTTP for the graph editor plus JSON-RPC 2.0 over WebSocket at /rpc), its flags, the Host and Origin rules, how it runs on its own thread and reaches the graph through the gateway, per-client queues, levels, stopping, and the traps found while building it. Use when calling ear from a script, test or browser, when a request gets 403, a socket closes with 1008 or a reset, levels never arrive, stop hangs, or when changing src/server/, src/control/ (rpc, commands, events, gateway, session) or web/src/rpc.ts.
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
  `MainLoopGateway` (`src/control/gateway.cpp`) runs each message's commands in one main-loop
  turn (`MainLoopExecutor`) and posts the replies and events back to the control thread in one
  ordered stream. So a change's notification always comes before the statuses it causes, and a
  subscriber gets no event from before its snapshot.
- One message in flight per connection: the next is read only after every reply to this one is
  queued. A batch runs in order in one crossing.
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

On SIGINT or SIGTERM, `ear` closes the graph to edits first: from then on every edit gets
-32000 `stopping`, while `graph.get` still answers and node events (`stopping`, `stopped`) still
arrive. When every node has stopped (files finished), `ControlPort::stop` closes every client
with 1001 and returns only once every close handshake has ended; then the control thread stops
and the engine quits.

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

## Tests

- `ear_server_tests` (`src/server/tests/control_port_test.cpp`): real sockets on
  `127.0.0.1:0`, a thread-backed fake main loop and a fake gateway: files, Host and Origin,
  round trips, two clients, the 1008 bound, a client that resets mid-write, stop with 1001.
- `ear_control_tests`: the envelope, commands, events, gateway and sessions, all pure, against
  `test/protocol`.
- `ear_system_tests` (`test/system/control_port_test.cpp`): the real `out/ear`, end to end.
- `scripts/image-smoke.sh`: the run image serves `/` and answers `graph.get`.
- `web/` vitest: the editor's client and reducer against the same fixtures.
