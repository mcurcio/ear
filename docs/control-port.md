# The control port

ear serves one TCP port (7777 by default): the graph editor's files over HTTP at `/`, and
JSON-RPC 2.0 over WebSocket at `/rpc`. Through `/rpc` a client reads and edits the graph and its
file, streams every change and per-port audio levels, and stops, starts, restarts, disables and
enables nodes. This page is the wire's one reference, **protocol 1**. Its examples are the fixtures
in `test/protocol/*.json`, which ear's C++ tests and the editor's tests both check: when this page
and a fixture disagree, the fixture is right.

The port runs on its own thread, never on PipeWire's; it holds no graph state, and every graph
request goes to the graph file keeper and from there to the main loop
([architecture.md](architecture.md#how-a-change-moves)).

## Talking to it

- **From a script:** `node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get` (Node 24's own
  WebSocket, no packages; the third argument is the params as JSON). It prints the result, or the
  error on standard error with exit 1. With `--until '<node> <state>'` it subscribes first and,
  after the reply, waits for that node's `node.state` with that state (`--timeout <s>`, 60 by
  default; 10 without `--until`):

  ```bash
  node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc --until 'porch running' node.start '{"name":"porch"}'
  ```

  Exit codes: 0 done; 1 an error reply, a `failed` node or a lost socket; 2 the state did not come
  in time; 3 bad arguments; 4 no answer in time (without `--until`).
- **From a browser:** `http://<host>:7777/`.
- **Startup:** wait for the `control listening on <address>:<port>` record on standard output,
  never for a time. `--control-port 0` takes any free port, which that record names.

## Who may connect

There is no login: anyone who reaches the port can edit the graph. Two checks keep other web
sites' pages out:

- **Host.** Every request's Host name must be an IP literal (IPv4, or IPv6 in brackets),
  `localhost`, or a name given with `--allowed-host` (case-insensitive); otherwise 403, and `/rpc`
  is not upgraded. A missing Host is refused. *Why:* DNS rebinding; a name the operator did not
  give cannot reach the port. Reaching ear as `http://ear.lan:7777/` needs `--allowed-host ear.lan`.
- **Origin.** A WebSocket upgrade carrying an Origin is accepted only when the Origin's
  `host[:port]` equals the Host header; one with no Origin (scripts, tests) is accepted.

**Files.** GET and HEAD under the web root (`--web-root`); `/` and a trailing `/` name
`index.html`, a `..` segment never leaves the root, a missing file is 404 and another method 405.

**Sizes.** A WebSocket message or HTTP body is at most 64 KiB.

## The envelope

JSON-RPC 2.0 (the specification's shapes), **named params only**.

| Error | When | `data` |
|---|---|---|
| -32700 `Parse error` | The text is not JSON | |
| -32600 `Invalid Request` | Not a request (an empty batch is one invalid request) | |
| -32601 `Method not found` | An unknown method | |
| -32602 | Malformed params: positional params, a missing member, a member of the wrong kind, **an unknown member** | `{"pointer"}`: the JSON pointer the message names (`"/from/port: missing"`) |
| -32000 | A refused request; the message is a short description (`unknown-port rec:FX`) | `{"code", ...}`: the refusal's code ([error codes](#error-codes)) |

A request without an `id` is run but not answered. Members ear *sends* may grow at any time (a
client ignores members it does not know); members a client sends may not (an unknown one is
-32602), so a client sends a member only once the feature that added it is present.

**Batches.** A batch is one message, sent to the keeper whole. Its requests run in order, **each as
its own step** (an edit is judged and committed, or refused, before the next is taken), and the
whole batch runs before any later message or disk change. Consecutive reads are answered in one
main-loop turn; the reply comes in the turn of the batch's last step, after every event the batch
caused. A batch is not atomic: a refused edit does not undo the ones before it. A batch's
`graph.subscribe` and `graphFile.subscribe` take their snapshots after its other requests, so the
snapshots include every edit the batch made.

## Protocol and features

`graph.get` says `"protocol": 1` and `"features": ["lifecycle"]`. An absent `protocol` is 0 (the
wire before the lifecycle). The number moves only on a breaking change: removing or renaming a
method, notification, member or code, changing a member's type, or changing an order guarantee.
Additive growth is a new **feature** name.

- Gate every method and request member on its feature, never on a -32601 probe. `lifecycle` adds
  the health members `phase`, `since`, `wanted`, `cause`, `waitingFor` and `blockLost`; `disabled`
  on a node; `node.start`, `node.stop`, `node.restart`, `node.disable`, `node.enable`, `node.add`'s
  `disabled`; the codes `node-disabled` and `not-restartable`; and `node.gone`.
- **Every enum is open.** `state`, `phase`, `cause.kind`, `waitingFor`'s `kind`, a route's `status`,
  `reason.code`, `applied`, `severity` and every code may gain values. Draw a value you do not know
  neutrally (a state as `starting`'s style, a code as its text), and ignore a notification you do
  not know; never fail on either (`test/protocol/unknown-values.json`).
- **A client that sees a higher `protocol`** shows a banner and keeps working with what it
  understands.
- **Protocol 0 clients** keep working: they see `state` as before plus values they do not know
  (`stopped`, `disabled`), every -32602 now carries `data.pointer`, a node added disabled is followed
  by one `node.state` `disabled` repeating its `node.added`, and `node.gone` follows `node.stopped`.

## The Graph

`graph.get` and `graph.subscribe` answer with the Graph (`test/protocol/snapshot.json`):

```json
{"protocol": 1, "features": ["lifecycle"],
 "graphFrequency": 48000, "blockSize": 256, "realtime": true, "scheduling": "SCHED_FIFO 88",
 "types": ["card", "file", "white"],
 "nodes": [
   {"name": "noise", "type": "white", "settings": {}, "inputs": [], "outputs": ["noise.mono"],
    "state": "running", "phase": "running", "since": "2026-11-09T10:00:00.000000Z", "wanted": "running",
    "blockLost": false},
   {"name": "quiet", "type": "white", "settings": {}, "inputs": [], "outputs": ["noise.mono"],
    "state": "disabled", "phase": "disabled", "wanted": "disabled", "blockLost": false, "disabled": true},
   {"name": "tape", "type": "file", "settings": {"path": "nope/t.wav"},
    "inputs": ["record.left", "record.right"], "outputs": [],
    "state": "failed", "phase": "failed", "wanted": "running", "blockLost": false,
    "reason": {"code": "start-failed", "detail": "cannot open nope/t.wav: No such file or directory"}}],
 "routes": [
   {"from": {"node": "noise", "port": "noise.mono"}, "to": {"node": "rec", "port": "record.left"}, "status": "linked"},
   {"from": {"node": "noise", "port": "noise.mono"}, "to": {"node": "tape", "port": "record.left"},
    "status": "refused", "detail": "the engine removed the link before it was active"}]}
```

| Member | What |
|---|---|
| `graphFrequency`, `blockSize` | The engine's, fixed at boot |
| `realtime`, `scheduling` | Whether the audio thread is realtime, and its policy (`SCHED_FIFO 88`, `SCHED_OTHER`) |
| `types` | The node types ear knows |
| `nodes[]` | Each node: `name`, `type`, `settings` (as in `ear.conf`), `inputs` and `outputs` (public ports only; inside banks are never on the wire), its [health](#node-health), and `disabled: true` when `ear.conf` disables it |
| `routes[]` | Each route: `from` and `to` (`{node, port}`), `status` (`pending`: an end is not live yet; `linked`; `refused`), `detail` when refused |

One main-loop turn makes the whole snapshot, so its parts agree.

### Node health

Every node in a snapshot and in `node.added`, and every `node.state`, carries the health members.
The meaning of each phase and state is [architecture.md](architecture.md#the-node-lifecycle)'s.

| Member | Always | What |
|---|---|---|
| `state` | yes | The colour: `starting`, `running`, `stopping`, `stopped`, `disabled`, `failed` |
| `phase` | yes | The exact state: `added`, `configuring`, `configured`, `starting`, `running`, `stopping`, `stopped`, `disabled`, `failed`. Explain by it; colour by `state` |
| `since` | when known | When this phase began, UTC to the microsecond, stamped as the gateway hears it (a snapshot's is the one its phase had when the graph was read) |
| `wanted` | yes | `running`, `stopped` (a stop held in memory) or `disabled` (in `ear.conf`) |
| `blockLost` | yes | The node's block is gone for good; it cannot restart until removed and added, or ear restarts |
| `cause` | `stopping`; `configuring` after a restart, retry or enable | `{"kind"}`: `stop`, `disabling`, `removing`, `restarting` (with `restart: {"kind"}`, the restart's own kind: `settings` with its `keys`, or `action` for `node.restart`), and for `configuring`: `settings` (with `keys`), `action`, `retry`, `enabling` |
| `waitingFor` | once the phase has lasted 2 s | What the node still waits for: a list of `{"kind", "name"?}`, kind `older-node` (a removed or renamed node of its name still closing), `block` (the engine), `companion` (an outside node, by engine name: `fpp.rtp`), `consumer` (a sink's consumer). Changed at most once a second |
| `reason` | `failed` | `{"code", "detail"}`, code `engine-refused`, `start-failed` or `lost` |

A `node.state` is sent whenever any member changes, never coalesced, and replaces every health
member: a member absent from it is absent now. A normal start is `node.added` (phase `added`) and
three or four `node.state`s within milliseconds.

## Methods

| Method | Params | Result | Refusals (`data.code`) |
|---|---|---|---|
| `graph.get` | none | Graph | none |
| `graph.subscribe` | none | Graph; then every graph notification on this connection | none |
| `levels.subscribe` | none | `{"windowsPerSecond": 30}`; then `levels` notifications | none |
| `node.add` | `{"name", "type", "settings"?, "disabled"?}` | `{}` | `invalid-name`, `unknown-node-type`, `invalid-settings`, `duplicate-node`, `file-invalid`, `invalid-text`, `stopping` |
| `node.update` | `{"name", "settings"}`: the node's whole new settings | `{}` | `unknown-node`, `invalid-settings`, `shared-definition`, `file-invalid`, `invalid-text`, `stopping` |
| `node.remove` | `{"name"}` | `{}` | `unknown-node`, `shared-definition`, `file-invalid`, `invalid-text`, `stopping` |
| `route.add` | `{"from": {"node", "port"}, "to": {"node", "port"}}` | `{}` | `unknown-node`, `unknown-port`, `wrong-direction`, `file-invalid`, `invalid-text`, `stopping` |
| `route.remove` | `{"from", "to"}` | `{}` | `unknown-route`, `shared-definition`, `file-invalid`, `invalid-text`, `stopping` |
| `node.start` | `{"name"}` | `{"applied"}`: `start` (the stop cleared), `retry` (a failed node configured afresh) or `unchanged` | `unknown-node`, `node-disabled`, `not-restartable` (`block-lost`), `stopping` |
| `node.stop` | `{"name"}` | `{"applied"}`: `stop` or `unchanged` (already stopped) | `unknown-node`, `node-disabled`, `stopping` |
| `node.restart` | `{"name"}` | `{"applied"}`: `restart`, `retry` (a failed node) or `unchanged` (a restart already under way) | `unknown-node`, `node-disabled`, `not-restartable` (`block-lost`; or `not-started` for a node still `added`), `stopping` |
| `node.disable` | `{"name"}` | `{"applied"}`: `disable` or `unchanged` (already disabled; nothing written) | `unknown-node`, `shared-definition`, `file-invalid`, `stopping` |
| `node.enable` | `{"name"}` | `{"applied"}`: `enable` or `unchanged` | `unknown-node`, `shared-definition`, `file-invalid`, `stopping` |
| `graphFile.get` | none | GraphFile | none |
| `graphFile.subscribe` | none | GraphFile; then every `graphFile.changed` | none |
| `graphFile.set` | `{"text", "version"}` | `{}` once the graph runs the text | `file-changed`, `invalid-text`, any code the graph refuses the text with, `stopping` |

**Edits** (`node.add`, `node.update`, `node.remove`, `route.*`, `node.disable`, `node.enable`,
`graphFile.set`) are edits of `ear.conf` made at the source, judged before they are kept, and written
behind ([graph-file.md](graph-file.md#edits-at-the-source)). A node whose ports change (a card's
channel count) is replaced: `route.removed` for its routes, `node.removed`, then `node.added` and
`route.added` again. An update with the same settings changes nothing. `disabled` is not a
setting: a `disabled` key inside `settings` is `invalid-settings`.

**`node.start`, `node.stop` and `node.restart` edit nothing.** They ride with the other requests
in the keeper's order, reach the node framework on the main loop and are answered in the turn that
applies them, after the `node.state` they cause there. They make no file version and send no
`graphFile.changed` or `node.updated`; they work while `ear.conf` has errors; a stop is forgotten
when ear restarts. Each writes an `action` record on standard output before the node records it
causes ([output-records.md](output-records.md#action)). On a failed node, `node.stop` changes only
`wanted` (one `node.state`, the phase still `failed`); `node.start` or `node.restart` retries it.

**`node.disable` and `node.enable`** edit the node's own `disabled` line, written behind like any
edit. Their effect shows as `node.updated` (`applied` and `disabled`) and the health it causes. No
`action` record: the `graph-file` record says it.

## Notifications

| Notification | Params | When |
|---|---|---|
| `node.added` | the node as in the Graph, with its health (`phase` `added`, or `disabled` for a node added disabled) | A node was added |
| `node.updated` | `{"name", "settings", "applied", "disabled"?}`; `applied`: `restart`, `disable`, `enable` or `stored` (a change to a node that stays disabled) | A node's settings or `disabled` changed |
| `node.removed` | `{"name"}` | A node was removed (after `route.removed` for its routes). The client drops the node now |
| `node.state` | `{"name"}` and every health member | Any health member changed |
| `node.stopped` | `{"name", "droppedFrames"}` | Deprecated (removed at protocol 2): everything the node made is gone |
| `node.gone` | `{"name", "droppedFrames"}` | The same, under its protocol 1 name; follows `node.stopped`. `droppedFrames` sums what its sinks dropped |
| `route.added`, `route.removed` | `{"from", "to"}` | A route was added or removed |
| `route.status` | `{"from", "to", "status", "detail"?}` | A route became `pending`, `linked` or `refused` |
| `graphFile.changed` | GraphFile, `"cause"`, `"droppedEdits"?` | `ear.conf`'s state changed |
| `levels` | `{"nodes": {"<node>": {"<port>": [peak, rms]}}}` | 30 times a second, to `levels.subscribe` clients |

A client ignores an event naming a node it does not know. After `node.removed` the removed node's
`node.state` (`stopping`, cause `removing`), `node.stopped` and `node.gone` name a node the client
no longer holds and are ignored by it. When the same commit adds a node of that name back, ear
sends none of the old node's health.

### Order

- **A change before the statuses it causes.** `graphFile.changed` first, then the node and route
  events the change causes, then the reply to the request that made it, all in one main-loop turn.
- **One change's events** come in one order: routes removed, nodes removed, nodes updated, nodes
  added, routes added.
- Adding a node: `node.added`, then its `node.state`s. Removing: `route.removed` for each route,
  `node.removed`, `node.state` `stopping` (cause `removing`), the reply; later `node.stopped` and
  `node.gone`.
- A restart: `node.state` `stopping` (cause `restarting`), the reply, then `configuring`,
  `configured`, `starting`, `running` as they happen. A stop: `stopping`, the reply, `stopped`. A
  start: `starting`, the reply, `running`. A disable: `graphFile.changed`, `node.updated`,
  `node.state` `stopping` (cause `disabling`), the reply, then `disabled`. An enable:
  `graphFile.changed`, `node.updated`, `node.state` `configuring` (cause `enabling`), the reply, then
  as a start.
- **A refused edit announces nothing**: only its reply carries the refusal. A hand edit gives
  `graphFile.changed` with cause `disk` and the events it causes, and no reply; a hand edit the graph
  refuses is announced with `refusal` set.
- **List, then watch.** A subscriber gets every change after its snapshot and none from before it.
  `test/protocol/lifecycle.json` is a node's whole life in order.

## The graph file over the port

```ts
type GraphFile = {
  version: string;              // opaque; changes whenever the text changes
  text: string;                 // ear.conf's text; "" when it is missing or cannot be read
  diagnostics: Diagnostic[];    // any error means the file does not apply
  running?: string;             // the version the graph runs; absent: none yet
  refusal?: Refusal;            // the desired graph refused `version`
  pendingRestart: ("graphFrequency" | "blockSize")[];  // the file's value differs from the engine's
  writeFailed?: string;         // the disk refused the last write
};
type GraphFileChanged = GraphFile & {
  cause: "edit"                 // an edit through the control port, from any client
       | "disk"                 // ear.conf changed on disk and ear reloaded it (also the boot offer)
       | "write";               // writeFailed was set or cleared
  droppedEdits?: number;        // disk only: accepted edits not yet written, now gone
};
type Diagnostic = {
  severity: "error" | "warning";
  code: string;                 // knobly's ("E2001") or ear's ("ear/include-unsupported")
  message: string;
  line: number;                 // from 1
  column: number;               // from 1, in UTF-16 code units
};
type Refusal = { code: string; message: string; line?: number; column?: number };
```

An absent optional member is left out, never `null`. ear's diagnostic codes are in
[graph-file.md](graph-file.md#diagnostics) (`test/protocol/graph-file.json`).

- **`graphFile.set {"text", "version"}`** replaces the whole text. `version` must be the current
  one (HTTP's `If-Match` pattern), else `file-changed`. A text with errors is `invalid-text` with
  `data.diagnostics`; a text the graph refuses gets the graph's code. The reply comes once the graph
  runs the text; the same text again is `{}` and changes nothing. It creates a missing file.
- **Handling `file-changed`:** another client, or a hand edit, changed the file since your
  version. Never resend your text with the new version blindly (that overwrites their change): take
  the new text, apply your change to it, and send that. The editor keeps the person's draft and
  offers a reload.
- **While the file has errors** every `node.*` and `route.*` edit (and `node.disable`/`node.enable`)
  is `file-invalid`; only `graphFile.set` or a hand edit fixes it. The lifecycle verbs still work.
- **Whole texts:** each change sends the whole text, comments included, to every
  `graphFile.subscribe` client; there is no diff. Never put a secret in `ear.conf`.
- **Write-behind:** the reply comes when the graph runs the edit; `ear.conf` is written after, when
  the keeper has nothing waiting. `graph-file written <version>` on standard output says when it
  landed; a failed write sets `writeFailed` (cause `write`) and is retried after the next accepted
  change. Unwritten edits are lost if ear crashes, or dropped (and counted in `droppedEdits`) when
  a hand edit lands first.

## Error codes

Every refusal is -32000 with `data.code`. Its message is the code and what it names.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-name` | A node name that is not 1 to 64 of `[A-Za-z0-9_-]` | |
| `unknown-node-type` | `node.add` with a type ear does not know | |
| `invalid-settings` | Settings the node's type refuses; the message carries the type's text (`invalid-settings tape: path: missing`) | |
| `duplicate-node` | `node.add` with a name already taken | |
| `unknown-node` | A name no node has (also a route naming one) | |
| `unknown-port` | A route naming a port the node does not have | |
| `wrong-direction` | A route not from an output to an input | |
| `unknown-route` | `route.remove` of a route that does not exist | |
| `stopping` | ear is stopping: every edit and lifecycle verb, even one already queued | |
| `file-changed` | `graphFile.set` with a stale `version` | |
| `invalid-text` | `graphFile.set`, or an edit whose candidate text has errors | `diagnostics` |
| `file-invalid` | An edit while `ear.conf` has errors | |
| `shared-definition` | The line the edit must change or remove also makes other nodes or routes | `line`, `column` |
| `node-disabled` | `node.start`, `node.stop` or `node.restart` on a node `ear.conf` disables | |
| `not-restartable` | `node.start` or `node.restart` on a node whose block is lost; `node.restart` on a node still `added` | `reason`: `block-lost` or `not-started` |

Examples are in `test/protocol/errors.json`:

```json
{"jsonrpc": "2.0", "id": 8, "error": {"code": -32000, "message": "unknown-port rec:FX", "data": {"code": "unknown-port"}}}
{"jsonrpc": "2.0", "id": 15, "error": {"code": -32000, "message": "not-restartable desk: block-lost", "data": {"code": "not-restartable", "reason": "block-lost"}}}
{"jsonrpc": "2.0", "id": 18, "error": {"code": -32602, "message": "/colour: unknown key", "data": {"pointer": "/colour"}}}
```

## Levels

`levels.subscribe` answers `{"windowsPerSecond": 30}` and then sends `levels` snapshots
(`test/protocol/levels.json`): for every port block that has a window, each public port's sample
peak and RMS, linear, to 4 decimals. Each snapshot is complete: a node missing from it has no level
(gone, or not yet measured). A route carries one mono channel, so **a route's level is its source
port's level**; an input port's level is what arrives there, the sum when several routes mix into
it. Windows are 1/30 s of graph time, on the same clock positions for every block. Levels never
queue: a subscriber always gets the newest snapshot, replacing one not yet written.

## Per-client limits and stopping

- One message in flight per connection: the next is read only after every reply to this one is
  queued.
- At most 256 responses and notifications wait per client; one more closes the connection with
  WebSocket code 1008. Reconnect and subscribe again (the editor does, after 1 s doubling to 30 s).
  Whole graph file texts in the queue fill it sooner.
- There are no timers: a connection stays until its peer or ear ends it.
- **Stopping** (SIGINT or SIGTERM): every edit and lifecycle verb, including one already queued,
  gets `stopping`, while `graph.get` and `graphFile.get` still answer and node events (`stopping`,
  then gone) still arrive. Once every node has stopped, ear closes every client with 1001 and waits
  for each close handshake.

## Traps

- **Keep reading while ear stops.** ear's close waits for the client's close reply; a socket closed
  with the client's data unread sends a TCP reset instead (`Connection reset by peer`), and a client
  that never answers the close holds ear's exit until it goes.
- **Parallel tests and port 7777.** Every test that starts `out/ear` passes `--control-port 0` and
  reads the bound port from the `control` record.
- **The editor's development proxy** (`npm run dev`) sends Host `localhost:7777`, which the Host
  rule accepts.
- **Never wait for a quiet period** after an edit: wait for its reply, a `node.state`, or the
  `graph-file written <version>` record.

## Fixtures

| File | Covers |
|---|---|
| `snapshot.json` | A Graph with every health shape |
| `edits.json` | Adds, updates, removals and their statuses |
| `lifecycle.json` | A node's whole life: start, stop, restart, disable, enable, failure and retry, removal, refusals, `waitingFor` |
| `graph-file.json` | `graphFile.*`, edits at the source, hand edits, refusals; also its `requests` and final `file` |
| `errors.json` | Every refusal code and envelope error |
| `levels.json` | `levels.subscribe` and `levels` |
| `unknown-values.json` | Values, members and notifications a client does not know, taken or ignored |
| `output-records.json` | Standard output's records ([output-records.md](output-records.md)) |

Each wire fixture is `{"description", "requests"?, "messages", "graph", "file"?}`: the server's
texts in order and the graph a client holds after applying them.
