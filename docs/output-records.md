# Standard output: records

Every line ear writes on standard output is one JSON object, a **record**, from one encoder
(`src/control/status_log.cpp`). The schema's home is the fixture
`test/protocol/output-records.json`, which holds a record of every kind; ear's tests check the
encoder against it field for field and in order. Bad flags and `--help` come before any record, as
plain text; every later reason to exit 1 is an `exit` record.

```json
{"v":1,"seq":15,"t":"2026-11-07T19:32:03.096982Z","kind":"route","from":"noise:noise.mono","to":"rec:record.left","status":"linked","msg":"route noise:noise.mono -> rec:record.left linked"}
```

*Why records:* tools read the fields, and people read `msg`. `jq -r .msg` prints the plain lines
ear printed before it had records.

## The envelope

| Field | What |
|---|---|
| `v` | The schema's version, 1. Additive changes keep it; a breaking one bumps it |
| `seq` | Counts from 1. A gap is a record a full queue dropped: the next record carries `"dropped": <n>` and its `msg` ends `(<n> records dropped before this)` |
| `t` | UTC, to the microsecond |
| `kind` | The record's kind |
| (the kind's fields) | In a fixed order |
| `msg` | A short sentence (for `graph-file`, several lines joined by `\n`) |

Text from peers and libraries is a JSON string, escaped, never interpreted; a byte that is not
UTF-8 becomes U+FFFD.

## Kinds

| Kind | Fields | `msg` |
|---|---|---|
| `engine` | `realtime`, `scheduling` | `engine: realtime SCHED_FIFO <n>` or `engine: not realtime (SCHED_OTHER)` |
| `control` | `address`, `port` | `control listening on <address>:<port>` |
| `node` | `node`, `state`, `phase`, and by case below | below |
| `note` | `node`, `text` | `node <name> says <text>` |
| `route` | `from`, `to`, `status`, `detail`? | `route <node>:<port> -> <node>:<port> pending\|linked\|refused[: <detail>]` |
| `graph-file` | `cause`, `version`, `running`, `diagnostics`, `refused`?, `droppedEdits`, `pendingRestart`, `writeFailed`? | below |
| `graph-file-written` | `version` | `graph-file written <version>` |
| `graph-file-write-failed` | `detail` | `graph-file write-failed <detail>` |
| `log` | `node` or `unattributed: true`, `level`, `source`, `topic`?, `text`, `cut`?, `heldBack`? | `[node <name> ]<source>[ <topic>] <level>: <text>` |
| `diagnostic` | `node`?, `code`, the code's fields, `detail` | below |
| `action` | `node`, `action`, `result`, `detail`? | below |
| `exit` | `status`, `reason` | `ear: <reason>` |

The order at boot: `engine` (how the audio thread is scheduled), then `control`, then the first
`graph-file` (cause `disk`, once the main loop runs) and the records it causes; then a record for
every change.

### `node`

A `node` record is written when a node's **state** (its colour) changes, or a failed node's reason
does; it carries the `phase` beside the state.

| `msg` | When | Extra fields |
|---|---|---|
| `node <name> starting` | Added (phase `added`) | |
| `node <name> starting: <cause>` | Configuring again after a restart, a retry or an enable | `cause` |
| `node <name> running`, `stopped`, `disabled` | Reached that state | |
| `node <name> stopping[: <cause>]` | Stopping; the cause is `stop`, `disabling`, `removing` or `restarting (<why>)` | `cause` |
| `node <name> failed <code>: <detail>` | Failed; code `engine-refused`, `start-failed` or `lost` | `failure: {code, detail}` |
| `node <name> still <phase> after <s>s: waiting for <what>` | A phase that has lasted 2 s, when what it waits for first appears, then at most once per 10 s while that changes | `waitingFor` |
| `node <name> removed dropped=<frames>` | Gone: everything it made is destroyed (state `stopping`, phase `removed`) | `droppedFrames` |

`<why>` and the configuring cause read `settings changed: <keys>`, `restart invoked` (for
`node.restart`), `retry` or `enabling`. `<what>` names `the previous <name>` (an older node of its
name), `the engine` (its block), an engine companion's engine name (`fpp.rtp`), or `its consumer`.
`configuring`, `configured` and `starting` after the first add write nothing of their own (they
share the colour `starting`). The `cause` field is `{"kind"}`, with `restart: {"kind", "keys"?}`
for a restart, as on the wire ([control-port.md](control-port.md#node-health)).

```json
{"v":1,"seq":66,"t":"...","kind":"node","node":"rec","state":"stopping","phase":"stopping","cause":{"kind":"restarting","restart":{"kind":"settings","keys":["path"]}},"msg":"node rec stopping: restarting (settings changed: path)"}
{"v":1,"seq":74,"t":"...","kind":"node","node":"fpp","state":"starting","phase":"configuring","waitingFor":[{"kind":"companion","name":"fpp.rtp"}],"msg":"node fpp still configuring after 2s: waiting for fpp.rtp"}
```

A node superseded by a newer node of its name (a removal and an add in one change) keeps its own
records apart from the newer one's.

### `note`

A note from a node's insides (`node kitchen says connected "Music Assistant" unpaired`), never a
state. A type's texts are its own; the `sendspin` node's are listed in
[node-types.md](node-types.md#notes). Notes reach only standard output, not the control port or the
editor.

### `graph-file`

One record per change to the graph file, before the node and route records it causes, carrying every
diagnostic of that version, warnings included. `cause` is `disk` for the boot file and for hand
edits, `edit` for an edit through the control port, `write` when a write failed or recovered;
`running` is the version the graph runs, or `null`. Its `msg` is these lines, the first always, then
each that applies (only errors among the diagnostics):

```
graph-file <edit|disk|write> <version> running=<version|none>
graph-file error <line>:<column> <code>: <message>
graph-file refused <code>[ at <line>:<column>]
graph-file dropped <n> edits
graph-file pending-restart <graphFrequency|blockSize>...
graph-file write-failed <detail>
```

`graph-file-written` follows each write of `ear.conf`. `graph-file-write-failed` is the last
write's failure as ear exits, which reaches nowhere else.

### `log`

Free text from a source: `stdout` and `stderr` (captured, below), `pipewire` (with its `topic`),
`alsa-lib`, `ear` (a type's hook or consumer that threw during a teardown), or a type's name (`sendspin`'s boot report
of key files it could not narrow). `level` is `debug`, `info`, `warn` or `error`; captured `stderr`
is `warn`, `stdout` `info`. A line longer than its bound (4096 bytes) is cut and marked `cut`.

**Pacing.** Log records are paced per node, or per source when unattributed: 32 at once, then one a
second. The next record that passes after a flood carries `heldBack` and its `msg` ends `(<n> more
held back before this: over 32 at once)`. *Why:* a flooding library cannot push states, routes or
notes (a pairing code) out of the output queue.

### `diagnostic`

The framework's own observations. With a node:

| Code | Fields | When |
|---|---|---|
| `ear.close-overdue` | | A configuration's teardown did not finish within 10 s; it is leaked, never freed |
| `ear.fade-unacknowledged` | `ms` | A planned silence's fade was not acknowledged within 100 ms; the silence was taken unfaded |
| `ear.barrier-overdue` | `ms` | A drive or silence posted to the data loop did not complete within 5 s; the configuration and block are leaked |
| `ear.hook-slow` | `ms` | A hook (`configure`, `start` or `stop`, named in `detail`) ran past 20 ms on the main loop; once per node and hook |
| `ear.internal-error` | `callback` | ear's own bug in one of the node's adapters, caught: `callback` names the callback or adapter call that threw (`FilterPortBlock::onStateChanged`, `SinkAdapter::make`), `detail` reads `<callback> threw[ during <where>]: <what>`. The node's `failed lost` record follows, unless the node is already being stopped, restarted or removed (the fault is then dropped with the rest of its teardown); the rest of the graph runs on. Said once per adapter, however often its callbacks throw |

`msg`: `node <name> <code>: <detail>`. Without a node (`msg`: `<code>: <detail>`):

| Code | Fields | When |
|---|---|---|
| `ear.data-loop-wedged` | `ms` | Once: the data loop is judged wedged. No further engine change is made, and ear's stop leaves through `_Exit` |
| `ear.main-loop-stalled` | `ms`, `in`? | The watchdog found the main loop's heartbeat older than 500 ms (`ms` is its age at the look); `in` is the PipeWire call or hook running, when known. Once per stall |
| `ear.main-loop-resumed` | `ms` | The heartbeat came back; `ms` is the gap between the beats either side |
| `ear.internal-error` | `callback`, `fatal`? | ear's own bug with no owner but ear's core (NodeHost, a route link's listeners, the main loop's posted work, a sink's thread), caught by the callback boundary: `fatal: true`, and the process ends right after it, with the line below on standard error. Without `fatal`: a route's link whose own callback threw, which fails only that route, and ear runs on |

These name no node or happen while the main loop is stuck, so they are standard output only, never
on the control port. What they mean for the graph is in
[architecture.md](architecture.md#bounds-the-wedged-data-loop-and-the-watchdog). Who owns an
`ear.internal-error`, and so whether ear runs on, is
[architecture.md](architecture.md#faults-by-owner)'s; a node it fails shows on the control port
only as that node's failure (`lost`, `internal error in <callback>: <what>`).

### `action`

An operator's `node.start`, `node.stop` or `node.restart`, answered, written before the `node`
records it causes. `result` is `ok`, or the refusal's code (`node-disabled`, `not-restartable` with
`detail` its reason, `unknown-node`, `stopping`).

```
node <name> <start|stop|restart> invoked
node <name> <start|stop|restart> refused: <code>[ (<reason>)]
```

`node.disable` and `node.enable` write no `action` record: they are edits, and their `graph-file`
record says so.

### `exit`

Why ear exits 1 after the flags were read: a folder it cannot make or open, folders that overlap,
an engine that does not start, a folder watch it cannot arm, a control port it cannot bind.

## How output is written

- **One writer.** Once the flags are read, before any thread starts, ear keeps the real standard
  output for its output thread alone. Every record goes there through a bounded queue (4096 records
  and 4 MiB; a record larger than the byte bound still goes when nothing else waits). A full queue
  refuses the record and the next one counts it. *Why:* the main loop never waits on a slow reader
  (a container runtime, `kubectl logs`).
- **Capture.** Descriptors 1 and 2 are replaced with pipes, read by the capture thread a line at a
  time into `log` records marked `unattributed` (source `stdout` or `stderr`). The `sendspin` node's
  protocol library and IXWebSocket write there, since the library has no log hook to name the node.
- **PipeWire and alsa-lib** log through ear's own `pw_log` (source `pipewire`, with its topic) and
  alsa-lib's error handler (source `alsa-lib`), both safe on the data loop: messages go into a ring
  (256 slots, texts cut at 1024 bytes, a full ring counted) drained on the capture thread.
- **Nodes never touch standard output.** A CTest test refuses any file under `src/nodes/` that names
  standard output or error or writes to descriptor 1 or 2; the capture is what makes the rule true
  for libraries.
- **A crash keeps its last words.** On `std::terminate`, SIGABRT, SIGSEGV, SIGBUS, SIGFPE or SIGILL,
  ear writes what is queued and what the pipes hold to the real descriptors and puts them back
  before the process dies, so abort's, terminate's and assert's messages reach `kubectl logs` as
  plain text. An exception caught at the engine's callback boundary first has what is queued and
  what the pipes hold written out, then one plain line written whole straight to the real standard
  error, just before terminate's own message: `ear: internal error in <callback>[, during <where>]:
  <detail>`, naming the callback that threw and what the main loop was in
  ([architecture.md](architecture.md#the-callback-boundary)). It is no record; the
  `ear.internal-error` record written just before it, among what is queued, says the same.
- **At stop**, every record still queued is written before ear exits.
