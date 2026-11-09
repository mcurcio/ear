# The control plane and parameters: slices 5 and 6 of the node framework

Status: design, not built. Slice 5 (the control plane: the type schema on the wire, status, events, node diagnostics on
the wire, actions, the realtime coded ring, sendspin's notes split) follows slice 3c (`node-primitives.md`: the node
handle and loops). Slice 6 (parameters: decimal, indexed and live rows, parameter slots and their ramps, retunes,
gestures, the `gain` type, the editor's faders) follows slice 3d (`reconfigure.md`: the plan's outcomes and `node.set`)
and slice 5. The two are designed together because they share one type schema, one write path for what lasts (the graph
file keeper), one wire vocabulary, one family of realtime channels and one editor model.

What is built and not repeated here (`docs/control-port.md`, `docs/output-records.md`): protocol 1 with `protocol` and
`features` in the graph and every enum open; feature `lifecycle` (health on `node.state`, snapshots and `node.added`:
`state`, `phase`, `since`, `wanted`, `blockLost`, `cause`, `waitingFor`, `reason`; `disabled` on a node;
`node.start`, `node.stop`, `node.restart`, `node.disable`, `node.enable`; `node-disabled`, `not-restartable`;
`node.gone` beside `node.stopped`; `applied` on `node.updated`); `data.pointer` on every -32602; the `node`, `action`
(for the lifecycle verbs), `diagnostic` (code, fields, detail) and `still` records; `rpc-call.mjs --until`. Companions on
the wire are 3b's (`companions.md`), `node.set`, `dryRun`, `dropRoutes` and the reconfigure outcomes 3d's
(`reconfigure.md`), recovery's members slice 8's (`recovery.md`).

[V] = verified in the source named, as of 2026-11-09; [U] = unverified, with the step whose test checks it (section 12).

## 1. The owner's words that bind these slices

The R numbers are the register the node framework's design used.

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R2 | "consider how to abstract configuration and control plane events so that nodes can build on a shared base primitive layer" | 2026-11-07 | Status, events, actions and parameters are framework subsystems; a type declares them and writes no plumbing |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07 | `Report<spec>` off the realtime thread, `Io<spec>`'s coded calls on it |
| R10 | "Would status changes be useful for debugging things like flapping? Does this status relate to diagnostic events?" | 2026-11-07 | Status is a view and a record of every change; flapping is the framework's diagnostic |
| Q2 | "View + change record" | 2026-11-07 | The live view on the wire, coalesced; changes as `status` records on standard output, paced so the last value of each field is always written |
| Q6 | "Commit at gesture end" | 2026-11-07 | Ticks move the audio; only `gesture.end` writes `ear.conf` |
| Q9 | "JSON with a msg field" | 2026-11-07 | Every new record carries `msg` |
| P1 | "3 seems like a standalone unit, while 5 and 6 need to be designed together." | 2026-11-08 | One page for slices 5 and 6 |
| P2 | "Let's start the design and architecture phase for both. I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08 | Journeys first; every request, record and declaration shown whole; how a control maps a pointer is specified |
| R14 | "G4 is asking the wrong question. The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives to connect inputs/outputs across nodes AND anytime a node needs to cross inputs/outputs to non-realtime code (eg pipe outputs through a mailbox?)" | 2026-11-08 | `process` uses this page's wait-free channels (parameters, `rt` status, coded records), never a ring of audio |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. For example, changing a network port on a node is something that a node can handle on its own — even if it must output null audio frames for a few seconds. But if reconfiguring changes the shape of the input/output bus, then that might not be fully transparent" | 2026-11-08 | The schema says per row whether a change is live, in place or a restart, and which rows change the ports |
| R30 (W3) | "I would like each port to show meter levels so I can see audio traversing the graph" | 2026-11-08 | Meters on every port in every phase (built); no "held" marks |
| G1 (owner) | A fader for a value several settings share: "(b)", as recommended: the fader moves every user together when all are faders of one kind and unit, within the range they all allow; the release writes the shared value once; other sharing is refused | 2026-11-08 | A gesture's reach (4.2, 7.2) |
| R31 (G3) | "Only a related save" | 2026-11-08 | A save of `ear.conf` stops a fader only when it changes the dragged value, the line it is defined on, or which settings share it (7.3) |
| R32 (G4) | "Keep last value" | 2026-11-08 | A fader whose browser disconnects or goes silent is ended as released: ear writes the last value it heard (7.3) |

Design rules 1 to 3 of the system design hold: one owner per piece of state; a node reaches the engine only through
powers the framework grants; the graph always runs. The graph file's rules hold (`docs/graph-file.md`): the working copy
leads, the disk wins; judge before keep; one keeper thread, one step at a time.

## 2. Journeys

Against one `ear.conf`:

```conf
graph_frequency 48000;
block_size 256;
@set $trim 0;
node white noise;
node gain amp { gainDb -6; }
node gain amp2 { gainDb -6; trimDb.1 $trim; trimDb.2 $trim; }
node card desk { card MOTU; playback 2; }
node sendspin kitchen { server "ws://192.168.1.10:8927/sendspin"; name "Kitchen"; }
route noise:noise.mono amp:in.1;
route amp:out.1 desk:play.1;
route kitchen:from.left desk:play.2;
```

`gain` is slice 6's new type (5.7): `channels` (default 2; it makes the ports), a live `gainDb` and a live indexed
family `trimDb.<i>`.

### 2.1 The operator, in the editor

**Turns a gain knob, and sees and hears it.**

1. The operator selects `amp`. The panel draws its settings from the type schema (4.2): `channels` as a number field
   marked "changes the ports", `gainDb` as a fader from -inf to +24 dB showing `-6.0 dB`, `trimDb.1` and `trimDb.2` as
   small faders. The box shows `gainDb` as a knob.
2. Pointer down on the knob: `gesture.begin {"name":"amp","key":"gainDb"}`. The main loop answers from the reach index
   (7.2) without waiting for the keeper: `{"gesture":"g4","value":-6,"reach":[{"name":"amp","key":"gainDb"}],
   "min":-90,"max":24,"decimals":1}`. Every other editor gets `gesture.changed {"phase":"begin",...}` and draws the knob
   as held by another editor; the owner gets no echo.
3. Drag. Each animation frame sends `gesture.change {"gesture":"g4","seq":n,"value":v}` as a notification (no id), read
   even while another request of this socket is in flight (6.6). The main loop stores `v` in `amp`'s parameter slot; on
   the realtime thread the framework ramps to it over 20 ms (5.4), so the audio follows the hand without zipper noise.
   The meters move at once. Other editors see the knob move, at most 30 times a second.
4. The operator holds still for half a minute: the editor sends the same value every 2 s while it holds the pointer,
   so the gesture stays open; a tick that does not change the value moves nothing and tells no one.
5. Pointer up at `-3.5`: `gesture.end {"gesture":"g4","value":-3.5}`. The keeper edits `gainDb -6;` to `gainDb -3.5;`
   at its source, the plan judges it a *retune*, and in one main-loop turn every client gets `graphFile.changed`,
   `node.updated` with `"applied":"retune"`, and `gesture.changed {"phase":"end","value":-3.5}`; then the reply. No
   `node.state` follows. Standard output has a `graph-file` record and one `param` record (`node amp gainDb -6 ->
   -3.5`).

**A hand edit lands mid-gesture, elsewhere in the file.** A colleague adds a comment above `node card desk` and saves.
The keeper offers the file with its reach index; on the main loop, before the plan, `g4`'s defining line is unchanged
(one line lower), its value and reach the same: the save is not related, nothing is sent about it, and its place is
rebased. The release later edits the new text, keeping the comment.

**A hand edit lands mid-gesture, on the dragged value.** A colleague saves `gainDb -10;`. The save is related: every
client gets `gesture.changed {"gesture":"g4","phase":"aborted","reason":"hand-edit"}`, then `graphFile.changed`, the
plan, the retune to `-10` with `node.updated`; last in the turn every reached slot is set to the value now running, so
the audio ramps once, from -3.5 straight to -10. The owner's editor snaps the knob to `-10`, ignores the pointer until
it is released, and says "ear.conf was edited by hand and changed amp gainDb while you held it; it now says -10." with
"Set -3.5 again" (a `node.set`).

**The laptop's Wi-Fi drops mid-gesture.** No ticks arrive; the socket stays open on ear's side. 30 s after the last
message about `g4` the control thread ends the gesture on its owner's behalf, as a `gesture.end` with no value (R32):
the keeper writes `gainDb -3.5;`; other clients get `gesture.changed {"phase":"end","value":-3.5,"reason":"idle"}`;
standard output has `node amp gainDb -6 -> -3.5` with `"reason":"idle"`. Had the socket closed first, the same end
would have come at once, with reason `session-closed`. When the laptop reconnects, the snapshot shows `-3.5`.

**A node flaps.** `kitchen`'s connection drops and comes back every few hundred milliseconds. Its worker loop writes
`connection`; standard output has a `status` record per change up to the field's pace (32 at once, then one a second),
then a trailing record with the value the field now has and how many were held back, so `kubectl logs` always ends on
the true value. Fourteen changes in ten seconds pass the field's threshold: `ear.status-flapping` with subject
`connection`, `active: true`, as a `node.diagnostic` and a `diagnostic` record. The editor badges "flapping:
connection"; when a whole window passes at or below the threshold, the same code and subject with `active: false`
removes it.

**A node reports a pairing code.** The worker writes `connection = connected`, `serverName = "Music Assistant"`,
`paired = false`, then `pairingCode = "123-456"`, which the schema marks a **callout**. The editor shows "Pairing code
123-456: type it into Music Assistant" with a copy button, announced politely; standard output has `node kitchen
pairingCode "" -> "123-456"`. Once paired, the worker raises the event `paired {"server":"ma-7f3c"}` and writes
`paired = true`, `pairingCode = ""`; the callout goes. A wrong code raises `pairing-failed {"reason":"pairing-code-
mismatch"}` (severity `warn`), also shown as a notice. If the node's configuration is torn down, the server sends the
node's status reset (4.3), and box status and callouts stay hidden while the node is neither `running`, `stopped` nor
`reconfiguring`.

### 2.2 The script author

```bash
U=ws://127.0.0.1:7777/rpc
node scripts/rpc-call.mjs $U node.set '{"name":"amp","settings":{"gainDb":-12}}'      # a live row: no restart
{"applied":"retune"}
node scripts/rpc-call.mjs $U node.set '{"name":"amp2","settings":{"trimDb.2":null}}'  # back to its default
{"applied":"retune"}
node scripts/rpc-call.mjs $U graph.get | jq -r '.nodes[] | select(.name=="kitchen") | .status.pairingCode'
123-456
node scripts/rpc-call.mjs $U node.invoke '{"name":"amp","action":"reset-clip"}'
{}
node scripts/rpc-call.mjs $U types.get | jq '.typeSchemas[] | select(.name=="gain") | .settings[] | {key, kind, min, max, unit, change}'
node scripts/rpc-call.mjs $U --follow | jq --unbuffered 'select(.method=="node.statusChanged")'   # step 5.2
```

A refusal carries its key, so a tool never parses the message:

```json
{"code":-32000,"message":"invalid-settings amp: gainDb: must be a number of dB from -90 to 24","data":{"code":"invalid-settings","key":"gainDb","detail":"must be a number of dB from -90 to 24"}}
```

A script that holds a gesture must send a tick at least every 30 s, or ear ends it as released; scripts rarely need
one, since `node.set` changes a live row without a gesture.

### 2.3 The client implementer

1. **Gate on features**, never on a -32601 probe: `types`, `status`, `actions`, `live`, `gestures` (4.1).
2. **Treat every enum as open** (built rule): a status field, an event code, a diagnostic code or a gesture phase you
   do not know is drawn neutrally or ignored.
3. **Fetch the schemas once per hash**: `typeSchemaHash` in the graph; `types.get {"ifNoneMatch":"<cached>"}` answers
   the set or `notModified`.
4. **Fold notifications** (4.4): `node.statusChanged` with `reset: true` replaces the node's status, otherwise merges;
   `node.diagnostic` replaces the entry of its code and subject; `node.eventRaised` goes to an activity list.
5. **Gestures**: begin, tick with notifications (keep-alive every 2 s while held), end. No `begin` or `change` echo for
   your own; on `aborted` stop the control and tell the person; an `end` with a `reason` means ear ended it for you.

### 2.4 The node type author

The complete type is `gain` (5.7). The part that matters:

```cpp
inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2).shapesPorts();
inline constexpr auto gainRow =
	setting::defaulted("gainDb", setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db,
	                                                                  .minIsSilence = true}), 0.0)
		.live(setting::smooth(20));
inline constexpr auto clipping = status::flag<"clipping">(writer::rt).help("an output went above full scale");
inline constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "channel {channel} clipped at {peakDb} dB",
                                                     arg::number<"channel">(1, 64), arg::decimal<"peakDb">());
inline constexpr auto resetClip = action::trigger<"reset-clip">("clear the clipping flag");
```

Nothing else is written: no slot, no queue, no thread, no wire code, no editor code. Misspell `gainRow` in `io.param`,
read a row that is not live, `raise` a text argument from `process`, `set` a field another writer owns, or read a live
row with `s.get<gainRow>()` in `configure` (where it would go stale at the first retune), and it does not compile.
Three limits: a live row reaches only the node's realtime code (a value applied off the realtime thread is an
`.inPlace()` row, posted to a loop's state by the change version, X6); status written off the realtime thread is written
from a loop of the spec the field names, never from a library's thread directly; and a stop, a start and an in-place
change keep the configuration, so the status stays (a change that makes a field stale has its loop's state write it
anew).

## 3. The domain model

### 3.1 The words

| Concept | What it is | Declared in | Written by | Kept | On the wire | On standard output |
|---|---|---|---|---|---|---|
| **Setting** | What the operator wants, surviving restarts | a `SettingsTable` row | the keeper | `ear.conf` | node `settings`; `node.updated` | `graph-file` record |
| **Parameter** | A setting declared `.live`: it also has a realtime value that moves without a restart | a row with `.live(smoothing)` | committed: the keeper; live: a gesture's ticks | committed: `ear.conf`; live: memory, during a gesture | `gestures`; `gesture.changed` | `param` record at commit and abort |
| **Status** | A fact the node observes that has a current value | a `StatusTable` field with one writer | that writer, through `Report` or `Io` | memory, per node; reset when its configuration is torn down | node `status`; `node.statusChanged` | `status` record per change, paced |
| **Event** | Something that happened at an instant, with no lasting value | an `EventTable` code | the node, through `Report` or `Io` | not kept | `node.eventRaised` | `event` record |
| **Action** | A command into a node that changes no desired state | an `ActionTable` entry (the lifecycle verbs are the framework's, built, and reserved names) | a client, ordered by the keeper | not kept | `node.invoke` | `action` record |
| **Node diagnostic** | What the framework observes about a node's machinery | the framework's code list (4.6) | the framework alone | per code and subject: the latest, whether active, a count | node `diagnostics`; `node.diagnostic` | `diagnostic` record |
| **Log** | Free text for people debugging | levels; `LogCodes` for the realtime thread | any node code; captured output | not kept | later (`log.subscribe`) | `log` record |
| **Note** | Retired: today's `say` sentence; about 11 of sendspin's 17 are state, the rest edges | `notes.hpp` | sendspin | — | — | `note` record, until step 5.5 |
| **Health** | The lifecycle's judgement (built) | the framework | `NodeHost` alone | memory | built | built |

A **node diagnostic** is not a **graph-file diagnostic** (a problem in `ear.conf`'s text at a line and column). They
keep apart names everywhere: `NodeDiagnostic`, node `diagnostics` beside `GraphFile.diagnostics`, and an event's
`EventSeverity { info, warn, error }` beside the file's `Severity { error, warning }`.

### 3.2 The rule for choosing

Ask in order; the first yes decides.

1. **Would the operator expect it after a restart of ear?** A **setting**. If it must move while audio plays without a
   gap, is a number, and is used by the node's realtime code, it is a **parameter** (`.live`). A value the node can take
   while it runs, off the realtime thread, is an `.inPlace()` setting. Any other is restart-only.
2. **Does it have a value right now?** **Status**. A value that is "none" most of the time is still status
   (`pairingCode` is `""` until a code is offered); a value not known yet is `null` (a choice starts unknown).
3. **Did something happen that someone might act on, with nothing to read once it passed?** An **event**. If it also
   leaves a value behind, write both: `paired` (event) and `paired = true` (status).
4. **A request from outside to do something now, which nothing should redo after a restart?** An **action**.
5. **About the node's machinery?** The framework's **node diagnostic**, or a companion's counter; a type cannot write
   either.
6. **For a person debugging, in words?** A **log** line.
7. **Up or down?** **Health**, the framework's alone.

| Today (sendspin's note) | Becomes |
|---|---|
| `id <client id>` | status `clientId` |
| `dialling <url>`, `listening <port> as <instance>` | status `connection` (`dialling`, `listening`, `connected`; unknown until written), `url`, `listenPort`, `instance` |
| `connected "<name>" paired\|unpaired`, `disconnected` | status `connection`, `serverName`, `paired` |
| `pairing-code <c>`, `pairing-code-cleared` | status `pairingCode`, a callout |
| `paired <server id>` | event `paired {server}`, and status `paired = true` |
| `pairing-failed <reason>` | event `pairing-failed {reason}`, severity `warn` |
| `pairing-held` | status `pairingHeld` (a flag) |
| `stream <codec> <rate> <ch> <bits>`, `stream-ended` | status `stream` (`none`, `flac`, `pcm`), `streamRate`, `streamChannels`, `streamBits` |
| `sending <rate> 2 24`, `sending-stopped` | status `sending` (flag), `sendingRate` |
| `key-write-failed <detail>` | event `key-write-failed {detail}`, severity `error` |
| `advertise-failed <detail>` | event `advertise-failed {detail}`, severity `warn` |
| `notes-dropped <n>` | gone: the framework's `ear.events-dropped` |

Other examples: a card's PCM state (slice 8) is status; `rtp`'s `receiving` is status written by its companion (slice
8); "a configuration did not close in 10 s" is `ear.close-overdue`; a library's warning text is a log line.

### 3.3 Aggregates and owners

- **The node** gains the status view (one value per declared field), transition counters, the node diagnostics kept
  per code and subject, and, per live row, the live value and the gesture holding it. All on the main loop, owned by
  `NodeHost`. Diagnostics belong to the node (a restart does not clear them); status belongs to the configuration (a
  restart, a failure or a removal resets it; a stop, a start and an in-place change keep it).
- **The gesture** is its own aggregate, owned by a new main-loop service, `Gestures` (`ear_core`): an id (opaque, never
  reused), the owning session, its reach with the combined range and decimals, the committed value, the live value, the
  last sequence number and tick time, and whether its end or cancel has been read.
- **The reach index** is a value the keeper makes on its thread for each version of the working copy and the main loop
  holds for the running one (7.2).
- **The type schema** is made from the type spec at compile time and encoded once at boot.

## 4. The wire

### 4.1 Features and compatibility

The frame is built: an absent `protocol` is 0, the number moves only on a breaking change, additive growth is announced
in `features`, every enum is open, members ear sends are additive and members a client sends exist only from the
feature that adds them (ear's decoders refuse an unknown key, -32602 with `data.pointer`). These slices add the
features `types` (5.1), `status` (5.2: status, events, node diagnostics), `actions` (5.4), `live` (6.3: decimal,
indexed and live rows, `applied: "retune"`) and `gestures` (6.4).

Names: requests are `<entity>.<verb>`, notifications say what happened; object keys, status field names and setting
keys camelCase; codes, event codes and action names kebab-case, the framework's under `ear.`; durations end in `Ms`;
instants are `t`, UTC to the microsecond. **One value-kind vocabulary** for setting rows, status fields, event arguments
and action arguments: `text`, `wholeNumber`, `decimal`, `flag`, `choice` (with `values`). Inside a batch,
`gesture.change` and `gesture.cancel` are refused `not-in-batch` (they skip the keeper, so they could not keep a
batch's order).

Today's clients keep working: a `node.update` that changes only live rows no longer restarts the node (what the
operator wanted, not observable as an error), and `node.update` refused with the new `parameter-held` shows its message
as any refusal does.

### 4.2 Requests

| Method | Feature | Params | Result | Refusals (`data.code`) | Path |
|---|---|---|---|---|---|
| `types.get` | `types` | `{"ifNoneMatch"?}` | `{"protocol","hash","typeSchemas"}`, or `{"protocol","hash","notModified": true}` | none | control thread (fixed at boot) |
| `gesture.begin` | `gestures` | `{"name", "key"}` | `{"gesture", "value", "reach", "min", "max", "decimals"}` | `unknown-node`, `not-live`, `shared-definition`, `parameter-held`, `file-invalid`, `file-refused`, `stopping` | control thread, then the main loop (7.2) |
| `gesture.change` | `gestures` | `{"gesture", "seq", "value"}` | `{}` if sent with an id | `unknown-gesture`, `invalid-settings`, `not-in-batch` | control thread, then the main loop directly |
| `gesture.end` | `gestures` | `{"gesture", "value"?}` | `{"value"}`: what was committed | `unknown-gesture`, `invalid-settings` (the gesture stays open), `stopping`, the desired graph's (the gesture is aborted) | keeper |
| `gesture.cancel` | `gestures` | `{"gesture"}` | `{}` if sent with an id | `unknown-gesture`, `not-in-batch` | control thread, then the main loop directly |
| `node.invoke` | `actions` | `{"name", "action", "args"?}` | `{}` | `unknown-node`, `unknown-action`, `invalid-args`, `not-running`, `restarted`, `stopping`, `action-failed`, `action-timeout` | keeper, then the node's loop |

`node.set` and `node.update` (3d) gain `parameter-held` and the outcome `retune` from feature `live`.

**`types.get`**

```json
{"jsonrpc":"2.0","id":7,"result":{"protocol":1,"hash":"fnv1a64:9c2e41f07a3b5d18","typeSchemas":[
  {"name":"gain",
   "settings":[
     {"key":"channels","kind":"wholeNumber","required":false,"default":2,"min":1,"max":64,"unit":"channels","change":"restart","shapes":true},
     {"key":"gainDb","kind":"decimal","required":false,"default":0,"min":-90,"max":24,"unit":"dB","decimals":1,"taper":"db","minIsSilence":true,"change":"live","live":{"smoothingMs":20}},
     {"key":"trimDb","kind":"decimal","required":false,"default":0,"min":-24,"max":24,"unit":"dB","decimals":1,"taper":"linear","change":"live","live":{"smoothingMs":20},"indexed":{"by":"channels","from":1}}],
   "inputs":["in"],"outputs":["out"],"reconfigureMs":10000,
   "status":[{"name":"clipping","kind":"flag","initial":false,"display":"panel","help":"an output went above full scale","flapsAbove":{"transitions":10,"windowMs":10000}}],
   "events":[{"code":"clipped","severity":"warn","sentence":"channel {channel} clipped at {peakDb} dB","args":[{"name":"channel","kind":"wholeNumber","min":1,"max":64},{"name":"peakDb","kind":"decimal"}]}],
   "actions":[{"name":"reset-clip","by":"type","target":"code","help":"clear the clipping flag","args":[]}]},
  {"name":"rtp",
   "settings":[
     {"key":"address","kind":"text","required":true,"change":"inPlace","mustBe":"\"0.0.0.0\" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad","pattern":"^[0-9]{1,3}(\\.[0-9]{1,3}){3}$"},
     {"key":"port","kind":"wholeNumber","required":true,"min":1,"max":65535,"change":"inPlace"},
     {"key":"format","kind":"text","required":true,"mustBe":"\"L16\" or \"L24\"","values":["L16","L24"],"change":"inPlace"},
     {"key":"channels","kind":"wholeNumber","required":true,"min":1,"max":64,"change":"restart","shapes":true},
     {"key":"rate","kind":"wholeNumber","required":false,"min":8000,"max":192000,"unit":"Hz","defaultFrom":"graphFrequency","change":"inPlace"},
     {"key":"latencyMs","kind":"wholeNumber","required":false,"default":100,"min":1,"max":1000,"unit":"milliseconds","change":"inPlace"}],
   "inputs":[],"outputs":["stream"],"reconfigureMs":10000,"status":[],"events":[],"actions":[]}]}}
```

- A setting row: `key`, `kind` (`text`, `wholeNumber`, `decimal`), `required`, and as they apply `default` or
  `defaultFrom` (an open enum: `graphFrequency`, `nodeName`), `min`, `max`, `unit`, `decimals`, `taper` (`linear`,
  `log`, `db`, 8.3), `minIsSilence`, `live` (`{"smoothingMs"}`), `indexed` (`{"by": <the count row's key>, "from":
  1}`), `onlyWithout`, `mustBe` (the row's own prose), and the text hints `values`, `pattern`, `maxLength`, `help`.
  Text hints are documentation for controls: the row's parse function stays the judge.
- **`change`** (`live`, `inPlace`, `restart`, from 3d's and 6's marks) and **`shapes: true`** (`.shapesPorts()`, 5.1)
  on every row: the editor's hints come from these alone. `reconfigureMs` on the type: its in-place deadline (3d's
  `.lifecycle`).
- `inputs` and `outputs` are the public banks' names, in declaration order; inside banks are never on the wire.
- No type's `settings` lists `disabled`: it is the framework's reserved key (built), never a row.
- A status field: `name`, `kind` (`flag`, `wholeNumber`, `decimal`, `choice`, `text`), `initial` (`null` when it starts
  unknown), `display` (`panel`, `box`, `callout`), `flapsAbove`, and as they apply `min`, `max`, `unit`, `values`,
  `maxBytes`, `help`. Its writer is not on the wire.
- An event: `code`, `severity` (`info`, `warn`, `error`), `sentence` (with `{arg}` places), `args` (each `name`, `kind`,
  `min`, `max`, `values`).
- An action: `name`, `by` (`type`; open), `target` (`code` for a trigger, `loop` for a call), `help`, `args` (setting
  rows).
- The hash is opaque: FNV-1a 64 over the exact bytes of the `typeSchemas` encoding made at boot, written
  `fnv1a64:<16 hex>`; one hash for the whole set, since types are compiled in and change only with ear. The graph's
  `types` (a sorted list of names, built) stays.

**`gesture.begin`** opens a gesture on one live row of one node. Its reach is every node and key whose value is defined
at the same place in `ear.conf` (G1): `amp2`'s `trimDb.1` and `trimDb.2` share `@set $trim 0;`, so a gesture on either
moves both. Every reached row must be live and of one kind and unit; the range is the intersection, `decimals` the
smallest. A reach that holds a row that is not live, or rows of another kind or unit, or ranges that do not meet, is
refused `shared-definition` with `data.place` and `data.with` (the operator then uses `node.set` or the text pane). A key
the type declares but `ear.conf` does not give reaches only that node, at the row's default. A node with no
configuration (failed, starting, disabled) can be held: ticks set the node's live value, which seeds its next
configuration's slots.

```json
{"jsonrpc":"2.0","id":9,"method":"gesture.begin","params":{"name":"amp2","key":"trimDb.1"}}
{"jsonrpc":"2.0","id":9,"result":{"gesture":"g5","value":0,"reach":[{"name":"amp2","key":"trimDb.1"},{"name":"amp2","key":"trimDb.2"}],"min":-24,"max":24,"decimals":1}}
```

**`gesture.change`** moves the live value. `seq` counts from 1 per gesture; a tick whose `seq` is not above the last
taken is dropped. A tick equal to the live value only keeps the gesture alive. Sent as a notification it has no reply: a
value out of range, or one the type's `check` refuses with the other settings, is dropped and the owner alone gets
`gesture.changed {"phase":"tick-refused","code":"invalid-settings","key":...,"detail":...}`, at most once a second per
gesture. Only the owning session can move it; any other gets `unknown-gesture`.

**`gesture.end`** commits. `value`, when given, is a last tick; without it, the last live value. The value is rounded to
the reach's `decimals` (G5: half away from zero on the shortest decimal text; a zero is written `0`, never `-0`) and
written at its definition. A value the rows refuse is refused `invalid-settings` and the gesture stays open; any other
refusal aborts it (`refused`). An end whose value equals the committed one writes nothing and still closes it.

**`gesture.cancel`** aborts the caller's own gesture: every reached slot returns to the committed value.

**`node.invoke`** runs one of the type's actions. A trigger is answered once handed to the code; a call with its
handler's outcome.

```json
{"jsonrpc":"2.0","id":13,"method":"node.invoke","params":{"name":"amp","action":"reset-clip"}}
{"jsonrpc":"2.0","id":13,"result":{}}
{"jsonrpc":"2.0","id":14,"method":"node.invoke","params":{"name":"kitchen","action":"forget-pairing","args":{"server":"ma-7f3c"}}}
```

### 4.3 The graph, and notifications

`graph.get` and `graph.subscribe` gain `typeSchemaHash` at the top and, per node, `status` (every declared field, each
at its initial value until written; always present, `{}` for a type with none), `gestures` (the open gestures moving
one of this node's keys, with their live value; always present) and `diagnostics` (one entry per code and subject, each
with `active`, `count`, `t`, `msg` and its fields; always present). `node.added` carries the same, so a client need not
fetch the schema to show a new node. One main-loop turn makes the whole snapshot, so status, health, gestures,
diagnostics and companions agree.

| Notification | Params | Rate |
|---|---|---|
| `node.statusChanged` | `{"name","fields":{name: value},"reset"?: true}`: the fields changed since the last; with `reset`, every field at its initial value | at most 30 a second per node |
| `node.eventRaised` | `{"name","code","severity","t","args":{...},"msg"}` | 32 at once, then 1 a second per node |
| `node.diagnostic` | `{"name","code","subject","active","count","t","msg", the code's fields}` | per code (4.6) |
| `gesture.changed` | `{"gesture","phase", and by phase: "reach","value","reason","code","key","detail"}` | `change`: at most 30 a second per gesture |

```json
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","fields":{"pairingCode":"123-456"}}}
{"jsonrpc":"2.0","method":"node.eventRaised","params":{"name":"kitchen","code":"pairing-failed","severity":"warn","t":"2026-11-08T10:01:52.004211Z","args":{"reason":"pairing-code-mismatch"},"msg":"pairing failed: pairing-code-mismatch"}}
{"jsonrpc":"2.0","method":"node.diagnostic","params":{"name":"kitchen","code":"ear.status-flapping","subject":"connection","active":true,"count":2,"t":"2026-11-08T10:02:11.120400Z","transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"begin","value":-6,"reach":[{"name":"amp","key":"gainDb"}]}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"end","value":-3.5,"reason":"session-closed"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"aborted","reason":"cancelled","value":-6}}
```

- **One notification per gesture**, not per reached key: `begin` carries the reach; later phases name only the
  gesture. A reach of 64 trims costs one message per tick.
- **Own echoes.** The owning session gets no `begin` and no `change`; it gets `end`, `aborted` and `tick-refused`.
  Others get `begin`, `change`, `end` and `aborted`, never `tick-refused`.
- `end` carries `reason` when ear ended the gesture for its owner: `session-closed` or `idle`. `aborted` carries
  `reason`: `hand-edit`, `file-saved`, `cancelled`, `refused`, `node-removed`, `key-removed` (a reshape removed a key it
  reaches), and `value` (the committed value the slots returned to) except on `hand-edit` and `file-saved`, where the
  following `node.updated` gives the new value.
- **A configuration's status reset.** When a configuration closes (or leaks), the node's status view returns to every
  field's initial value, and the drain sends one `node.statusChanged` with `reset: true` and every field, after that
  configuration's last values. A stop, a start and an in-place change send none.

**Order**, added to the built rules (a change's notification before the statuses it causes):

1. Within a node, a `node.statusChanged` with the fields written so far precedes any `node.eventRaised` that followed
   them in the same writer's order, so `connection = connected` reaches a client before `paired`.
2. A configuration's last status values reach a client before the `node.state` its loss causes, and its reset follows
   its last values; on a restart, the reset comes between `stopping` and `configuring`.
3. A gesture's begin: `gesture.changed` `begin` to the others, then the reply, in one turn. Its end: `graphFile.changed`,
   `node.updated` (`applied: "retune"`), `gesture.changed` `end`, the reply.
4. An abort by a related save: `gesture.changed` `aborted` for each related gesture, then `graphFile.changed`, then the
   change's graph events; the slots settle last, in the same turn.

### 4.4 What a client folds

| Notification | Changes | Ignored when |
|---|---|---|
| `node.statusChanged` | with `reset`: `status` replaced; otherwise merged | the node is unknown |
| `node.eventRaised` | nothing in the graph; the activity list | the node is unknown |
| `node.diagnostic` | the entry of the same `code` and `subject` replaced, `count` as sent | the node is unknown |
| `gesture.changed` | `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value` wherever the id is; `end` and `aborted` remove it everywhere; `tick-refused` changes nothing | no node holds the id (except `begin`) |

### 4.5 Error codes

All -32000 with `data.code`; every new code is a case in `test/protocol/errors.json`.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-settings` (extended) | a value a row or the type's `check` refuses, from the plan or from `Gestures` | `key` (the full key, `trimDb.2`; absent for a rule naming none), `detail` |
| `parameter-held` | `node.set`, `node.update`, `gesture.begin`: a key it would change is held by an open gesture (another session's, or the caller's own) | `gesture`, `key` |
| `not-live` | `gesture.begin`: the key is not a live row (or not declared) | `key` |
| `shared-definition` (extended) | `gesture.begin`: the reach holds a key that is not live, rows of another kind or unit, or ranges that do not meet | `place`, `with` |
| `file-refused` | `gesture.begin`: the graph refused the working copy's last hand edit, so the running version is not the file's | |
| `unknown-gesture` | no open gesture of this session by that id | |
| `not-in-batch` | `gesture.change` or `gesture.cancel` inside a batch | |
| `unknown-action` | `node.invoke`: no such action | `action` |
| `invalid-args` | `node.invoke`: the arguments fail the action's table | `key`, `detail` |
| `not-running` | `node.invoke`: the node cannot take it now (no configuration, one tearing down, or one not yet configured); a `stopped` node takes actions | `phase` |
| `restarted` | `node.invoke`: the node's configuration was torn down before the action ran | |
| `action-failed` | the handler refused; message: its text | `detail` |
| `action-timeout` | the handler did not answer in 10 s | |

`InvalidSettings` (`command_error.hpp`) gains `key`, set by the settings walk; `describe` keeps today's text. The editor
places a refusal under its field by `data.key` and never parses the message.

### 4.6 Node diagnostics on the wire

Built: `diagnostic` records on standard output with `code`, fields and `detail` (`ear.close-overdue`,
`ear.fade-unacknowledged`, `ear.barrier-overdue`, `ear.hook-slow`, and node-less `ear.data-loop-wedged`,
`ear.main-loop-stalled`, `ear.main-loop-resumed`). Slice 5 puts node diagnostics on the wire as `node.diagnostic`,
keeps each per **code and subject** (a field, a loop, a claim, a hook or a companion; empty when the code has none), and
gives each `active` and `count`: a condition code (flapping, a claim wait) becomes active and later inactive with the
same code and subject; an observation code (a drop, a slow handler) is sent with `active: false` and only joins the list.
The editor badges active entries and lists all, needing no per-code knowledge. The node-less ones stay standard-output
only (a stalled main loop cannot answer the wire in time).

| Code | Subject | Fields | Owner |
|---|---|---|---|
| `ear.close-overdue` | — | `detail` | built; 3c adds counts |
| `ear.fade-unacknowledged`, `ear.barrier-overdue` | — | `ms` | built |
| `ear.hook-slow` | the hook | `ms` | built |
| `ear.loop-slow`, `ear.loop-source-left`, `ear.claim-wait`, `ear.close-late` | the loop, the claim | `ms`, `sources`, `holder`/`waitedMs`, `detail` | 3c (`node-primitives.md`) |
| `ear.sink-dropped`, `ear.source-shortfall` | the companion | `frames`, `totalFrames` | 3b (`companions.md`) |
| `ear.reconfigure-fell-back`, `ear.reconfigure-superseded`, `ear.retiring-lost` | —, —, the companion | as `reconfigure.md` 8.2 | 3d |
| `ear.status-flapping` | the field | `transitions`, `windowMs` | here: active once a field changes more than its `flapsAbove` within the window; inactive once a whole window passes at or below it; at most once per window while active |
| `ear.events-dropped` | — | `heldBack` | here: events held back by the per-node rate (replaces 3c's `notes-dropped` note); at most once a second |
| `ear.records-dropped` | `status`, `realtime` or `queue` | `count` | here: `status` records held back on stdout, or records a node's ring or queue had no room for; at most once a second per subject |
| `ear.realtime-overrun` | — | `codeUs`, `cycleUs` | reserved, not built |

Slice 8 adds `ear.health-flapping`, `ear.recovery-attempt` and `ear.readiness-missed` in the same shape
(`recovery.md`). There are no framework status fields: a companion's counts are members of its entry in `companions`.

### 4.7 Fixtures

| File | Covers |
|---|---|
| `test/protocol/types.json` | `types.get` with no hash, a stale hash and the current one; every built-in type's schema, each row's `change` and `shapes`, a `defaultFrom` row |
| `test/protocol/status.json` | a snapshot with status (a choice at `null`); merges; an event after a flush, with `severity`; a reset and a new configuration's first fields; status kept across a stop and a start |
| `test/protocol/diagnostics.json` | two fields flapping at once, one settling while the other stays; `count` as sent; an observation code; an unknown code drawn generically |
| `test/protocol/gestures.json` | begin (no echo to the owner, `begin` to another first, then the reply), changes (one stale, one keep-alive), end with its order; a reach of two keys and of 64; a reach refused `shared-definition`; `parameter-held`; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings` leaving the gesture open; aborts by a related hand edit and a related `graphFile.set`; an unrelated hand edit and `graphFile.set` leaving it open; ends for a closed and a silent session; `unknown-gesture` after an abort; `not-in-batch` |
| `test/protocol/actions.json` | a trigger; a call; `not-running` with its `phase`; every refusal an action can get |
| `unknown-values.json`, `errors.json`, `snapshot.json`, `edits.json`, `output-records.json` | extended with this page's members, codes and records |

## 5. The C++ SPI

Everything extends `type_spec.hpp`, `settings_table.hpp`, `lifecycle.hpp` and `node_type.hpp`; nothing replaces them.
Every new constant is `inline constexpr` in the type's named namespace, and every new template keyed on a constant
refuses one in an unnamed namespace (`linkage::named`, as built).

### 5.1 New row kinds: decimal, indexed, live; `.shapesPorts()`

```cpp
namespace ear::setting {
enum class Taper { linear, log, db };            // how a control maps its travel (8.3); log also ramps geometrically
enum class DefaultFrom { graphFrequency, nodeName };
struct DecimalHints {
	std::uint8_t decimals = 2;                       // display precision, and what a gesture's end rounds to
	Taper taper = Taper::linear;                     // log: low > 0; db: low <= -60 and high >= 0 (else no compile)
	bool minIsSilence = false;                       // the control shows `low` as -inf
};
struct TextHints { std::span<const std::string_view> values = {}; std::string_view pattern = {}; std::uint32_t maxLength = 0; };
/// A real number in [low, high]: a JSON number (an integer is taken too), never NaN. Otherwise
/// "<key>: must be a number[ of <unit>] from <low> to <high>". low above high, or a non-finite bound, does not compile.
consteval Rule<double> decimal(double low, double high, std::string_view unit = {}, DecimalHints hints = {});
template <auto Parse> requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints);   // today's text(), with hints
/// A family <key>.1 .. <key>.<n>, n being the checked value of `count`, a whole-number row above it in the table.
template <class V, bool A> consteval IndexedRow<V> indexed(std::string_view key, const Row<std::uint32_t, A>& count,
                                                           Rule<V> rule, V byDefault);
struct Smoothing { std::uint32_t ms; };           // ramp time for a live row; 0: a step at the next block
consteval Smoothing smooth(std::uint32_t ms);
}
```

- `Row::live(Smoothing)` and `IndexedRow::live(Smoothing)` mark a row live (implying `.inPlace()`). Only a row that
  always reads a value (required, defaulted or indexed) of a whole number or a decimal can be live; a live whole-number
  row must have `smooth(0)` (it steps).
- An optional row takes `.defaultFrom(DefaultFrom)`: documentation for controls and the schema.
- `.shapesPorts()`: a change to the row may change the node's ports; the schema's `shapes`. The plan never reads it (it
  compares real ports, 3d); the all-type tests hold it to `ports()`. An indexed family's count row is `.shapesPorts()`
  only when `ports()` reads it.
- `SettingRow` gains `liveMs`, `indexedBy`, a `DecimalRule`, the text hints, `defaultFrom` and `shapesPorts`;
  `SettingKind` gains `decimal`; `SettingDeclaration` gains `live`, `indexed` and `shapesPorts` beside 3d's `inPlace`.
- The walk: a decimal is a JSON number in range, an integer kept as given. An indexed family's keys are `<key>.<i>`, 1
  to the count; another member number is `"<key>.<i>: no such member (<count key> is <n>)"`, placed where the family's
  row sits in the walk's fixed order; unknown keys skip any key whose part before its last `.` names a family. Each
  refusal carries its key.
- Reads: `s.get<row>()` and `s.get<trimRow>(i)` (member `i` from 0, keys from 1). **On a live row `get` does not
  compile** ("a live row is read through io.param"): a value captured in `configure` or a loop's state would go stale at
  the first retune. `s.initial<row>()` reads a live row as configured. The type's `check` sees every row and runs on
  every tick too, so a rule across settings judges what a gesture plays.
- The graph file: a decimal is read with knobly's `number()` and written as the shortest round-trip fixed text
  (`std::to_chars`, `chars_format::fixed`): `-3.5` is `-3.5`, `-6.0` is `-6`, a negative zero `0`. [U: a negative fixed
  number is one word, and a directive name may hold `.` (`trimDb.1`); fallback name `trimDb-1`.]

### 5.2 Status fields

```cpp
namespace ear {
enum class Display { panel, box, callout };   // callout: a banner while not at its initial value
namespace writer {
struct Writer { enum class Kind { rt, loop, companion } kind; LoopName loop = {}; };
inline constexpr Writer rt{Writer::Kind::rt};               // the node's code, through Io
template <class Handle> consteval Writer loop(Handle);      // a loop of the spec (node-primitives.md 6.3)
inline constexpr Writer companion{Writer::Kind::companion}; // a companion kind's adapter (slice 8: rtp's receiving)
}
namespace status {
inline constexpr struct Unknown {} unknown;   // `.initially(status::unknown)`: the field starts null
template <spec_detail::Name N> consteval FlagField<N> flag(writer::Writer w);
template <spec_detail::Name N> consteval NumberField<N> number(writer::Writer w, std::int64_t low, std::int64_t high, std::string_view unit = {});
template <spec_detail::Name N> consteval DecimalField<N> decimal(writer::Writer w, double low, double high, std::string_view unit = {}, std::uint8_t decimals = 2);
template <spec_detail::Name N, std::size_t K> consteval ChoiceField<N, K> choice(writer::Writer w, const std::string_view (&values)[K]);   // starts unknown
template <spec_detail::Name N> consteval TextField<N> text(writer::Writer w, std::uint32_t maxBytes = 256);   // never rt
}
template <auto... Fields> struct StatusTable { /* unique names */ };
}
```

- Every field has `.display(Display)`, `.help(text)`, `.flapsAbove(transitions, windowMs)` (default 10 in 10 000 ms) and
  `.initially(value)`. Defaults: a flag `false`, a number `0`, a text `""`, a choice unknown (`null`).
- A choice's values are named once; `field("connected")` is consteval, so a misspelt value does not compile.
- `rt` fields are flag, number, decimal and choice only (one atomic word each); a text field marked `writer::rt` does not
  compile.
- `writer::loop(worker)` takes a loop handle the spec lists in `.loops`; `Report::set` of a loop field asserts
  `Loop::inLoop()` in debug builds. A library thread with something to report posts to the loop that owns the field;
  there is no `writer::thread`.
- `writer::companion`: a field a companion kind's adapter writes for its node (slice 8's `receiving`); the framework
  binds it to the companion of that kind the node acquired.

### 5.3 Events, log codes, `Report<spec>`

```cpp
namespace ear {
enum class EventSeverity { info, warn, error };
namespace arg { /* number<N>(low, high), decimal<N>(), flag<N>(), choice<N>(values), text<N>(maxBytes) (never from process) */ }
namespace event {
/// `sentence` names each argument once as {name}; a place that names no argument, or an argument with no place, does
/// not compile. It is the record's msg and the editor's line.
template <spec_detail::Name Code, class... Args>
consteval Event<Code, Args...> of(EventSeverity severity, std::string_view sentence, Args... args);
}
namespace log {   // numbers, decimals and flags only, at most four
template <spec_detail::Name Code, class... Args> consteval LogCode<Code, Args...> code(LogLevel level, std::string_view sentence, Args... args);
}
template <auto... Events> struct EventTable {};
template <auto... Codes> struct LogCodes {};

/// The typed control-plane face of one node, over 3c's NodeHandle: status writes and events by the spec's handles, from
/// the loops its fields name. Copyable; never blocks but on one short mutex; a write after the configuration began
/// closing is dropped. A text value is UTF-8 checked, control characters replaced, cut at its bound.
template <const auto& Spec> class Report {
public:
	template <const auto& Field> void set(typename FieldOf<Field>::Value value);   // a writer::loop field
	template <const auto& Event> void raise(typename ArgsOf<Event>::Values... values);
	NodeHandle& handle() const noexcept;
private:
	explicit Report(NodeHandle& handle);   // made by the framework (NodeOf<Spec>::report) and the harness only
};
// NodeOf<Spec> gains: Report<Spec> report();
}
```

- `TypeSpec` gains `.status`, `.events`, `.actions` and `.logCodes`, in that order after 3d's `.lifecycle`
  (`node-primitives.md` 6.11), each defaulting to an empty list.
- A choice argument off the realtime thread takes a `std::string_view`, checked at run time (a value not among them is a
  type bug: the node fails with the rule's text); from `process` a consteval-made index only.
- `Report` carries its writes in the same per-node channel as `NodeHandle::lost`, so a status write, an event and a
  `lost` keep one writer's order. `NodeHandle::say` stays for `sendspin` until step 5.5 removes it.
- The `Report` exists from `configure`; loops' work runs once configured, so in practice writes come from `configured`
  on. A write during an in-place change or while stopped lands on the same configuration.

### 5.4 What `process` gets: `Io<spec>` grows

```cpp
template <const auto& Spec> class Io {
public:
	// built: operator[](bank), frames()
	template <const auto& Row> Smoothed param() const noexcept;                         // a live, non-indexed row
	template <const auto& Row> Smoothed param(std::size_t member) const noexcept;       // a live indexed row, from 0
	template <const auto& Field> void set(typename FieldOf<Field>::Value value) const noexcept;   // writer::rt only
	template <const auto& Event, class... N> void raise(N... values) const noexcept;    // numbers, decimals, flags, choice indexes
	template <const auto& Code, class... N> void log(N... values) const noexcept;       // a LogCodes entry
	template <const auto& Trigger> bool triggered() const noexcept;                    // true once per invoke
};
class Smoothed {   // one live row's values for this block, in the row's own unit
public:
	bool steady() const noexcept;                     // no ramp frame in this block: every sample is value()
	float value() const noexcept;                     // the value at the block's last frame
	float operator[](std::uint32_t frame) const noexcept;
	float target() const noexcept;                    // the slot's latest
};
```

**The ramp's rule** (one for every live row; the framework's, before `process`):

- Once per block, whether or not the code reads the row, the framework loads each live member's slot (one atomic load)
  and advances its ramp. `io.param` only returns what the framework computed.
- **Constant time.** When the slot's target differs from the ramp's, a new ramp starts at the block's first frame, from
  the current value, over `N = max(1, round(smoothingMs × graphFrequency / 1000))` frames. A target that moves
  mid-ramp restarts from where it is, with a full `N`; it never jumps.
- Frame `k` of a ramp has the value `v0 + (t − v0) × (k + 1) / N` for `k < N`, and `t` from `k = N − 1` on; computed in
  double, given as float. `smooth(0)` steps; a whole-number live row always steps.
- **Domain.** Linear in the row's unit for `linear` and `db` tapers (linear in dB is the usual gain ramp); a `log` taper
  row ramps on `ln(v)`.
- The first block a code runs after the node is configured (or started) starts at the slot's value, without a ramp.
- **Worked vector** (step 6.3's test): 48 000 Hz, block 256, smoothing 20 ms, so `N = 960`. Value `-6`, steady; target
  `-3.5` stored before block 0. Frame 0: `-5.9973958`; frame 255: `-5.3333333`; frame 959 (block 3, frame 191): `-3.5`;
  frame 960: `-3.5`. Blocks 0 to 3 are not steady; block 4 is, with `value() == -3.5`. If the target becomes `-12` before
  block 2, block 2's ramp starts from frame 511's value (`-4.6666667`) and reaches `-12` at frame 1471.
- **Members of one reach change together only by chance**: the main loop writes a reach's slots one at a time, so one
  block may see channel 1's new target and channel 2's old one (at most one block of ramp, about 0.07 dB for 2.5 dB over
  20 ms); a seqlock per node was judged not worth its cost.
- `io.set` stores one atomic word, and when the value differs from the last one the code wrote, puts a record in the
  node's ring and counts a transition. `io.raise` and `io.log` put one fixed-size record in the ring, stamped with the
  cycle's start (`GraphCycle::startNs`). `io.triggered` compares the trigger's counter with the last the code saw. Every
  handle resolves to an index when the code is prepared, as banks do.

### 5.5 Actions

```cpp
namespace ear::action {
/// Into the node's code, wait-free: no arguments, answered {} once handed over; io.triggered<t>() true once in the next
/// block. Several invokes before a block are one.
template <spec_detail::Name N> consteval Trigger<N> trigger(std::string_view help);
/// Into a loop's state, posted to `Loop` (a handle the spec lists): arguments checked by `Args`, a SettingsTable; the
/// handler's std::expected<void, std::string> is the reply.
template <spec_detail::Name N, const auto& Args, const auto& Loop> consteval Call<N, Args, Loop> call(std::string_view help);
}
template <auto... Actions> struct ActionTable {};
```

- `start`, `stop`, `restart`, `disable` and `enable` are the framework's lifecycle requests (built); a type action of
  one of those names does not compile.
- A call's handler is a member of the loop's state, `std::expected<void, std::string> act(Invoked<forgetPairing>, const
  CheckedOf<forgetArgs>&)`, found by its argument type and held to that signature. It is posted with 3c's
  `NodeLoop::postFromOutside`; a queued call whose configuration is torn down first is destroyed unrun at teardown step 1
  and its reply object answers `restarted` (or `stopping`) from its destructor.
- An action's arguments are a `SettingsTable`: one checker for settings, arguments and parameters.

### 5.6 How the framework carries them

- **Per configuration** (made when `configure` is called from empty, freed at teardown step 5): the node handle's
  channel with the off-realtime queue (6.3), the realtime ring (6.1), one `std::atomic<double>` slot per live member,
  one `std::atomic<std::uint64_t>` per `rt` field with the last value the realtime side wrote and an
  `std::atomic<std::uint32_t>` transition counter, one `std::atomic<std::uint32_t>` per trigger, each live member's ramp
  state. All of it is the node's, not the code's: the code is bound to it when prepared, and a code an in-place change
  swaps in is bound to the same objects before its barrier. A stop, a start and an in-place change keep all of it; a
  reshape in place resizes an indexed family's slots (survivors keep their ramps, new members start at the committed
  value, vanished ones are dropped after the swap's barrier).
- **Per node**: the status view, the live value of each held key (seeded into every new configuration's slots, so a
  restart mid-gesture keeps the hand's value), the node diagnostics.
- **Erased for the rest of ear**: `NodeType::schema()` returns a plain `TypeSchema` (core, nlohmann only), encoded once
  by `ear_control`. `NodeConfig::liveValues()` gives each live key's checked value, for retunes.
  `NodeType::checkLive(config, key, value)` runs the row's rule then the type's `check` with that value in place.
- **New core interfaces**, implemented outside core: `RealtimeBell` (an eventfd the main loop watches, in
  `ear_pipewire`), `NodeStatusListener` (`statusChanged`, `statusReset`, `eventRaised`), `NodeDiagnosticListener`
  (built, gaining subject, `active`, `count`), and `GestureListener` (carrying the owning session so the gateway can
  leave out own echoes), implemented by the gateway and `StatusLog`.

### 5.7 The `gain` type, whole

```cpp
// src/nodes/gain/gain_spec.hpp: slice 6's proof of decimal rows, indexed rows, .live and Smoothed.
namespace ear::nodes::gain {
inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2).shapesPorts();
inline constexpr auto gainRow =
	setting::defaulted("gainDb", setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db,
	                                                                  .minIsSilence = true}), 0.0)
		.live(setting::smooth(20));
inline constexpr auto trimRow =
	setting::indexed("trimDb", channelsRow, setting::decimal(-24.0, 24.0, "dB", {.decimals = 1}), 0.0)
		.live(setting::smooth(20));
inline constexpr auto in = bank::input<"in">;
inline constexpr auto out = bank::output<"out">;
inline constexpr auto clipping = status::flag<"clipping">(writer::rt).help("an output went above full scale");
inline constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "channel {channel} clipped at {peakDb} dB",
                                                     arg::number<"channel">(1, 64), arg::decimal<"peakDb">());
inline constexpr auto resetClip = action::trigger<"reset-clip">("clear the clipping flag");
inline constexpr TypeSpec spec{.settings = SettingsTable<channelsRow, gainRow, trimRow>{"gain"},
                               .banks = Banks<in, out>{},
                               .status = StatusTable<clipping>{},
                               .events = EventTable<clipped>{},
                               .actions = ActionTable<resetClip>{}};
using Checked = CheckedOf<spec>;

inline float dbToFactor(float db) noexcept { return db <= -90.0F ? 0.0F : std::exp2(db * 0.16609640474F); }
inline double factorToDb(float factor) noexcept { return 20.0 * std::log10(static_cast<double>(factor)); }

struct GainCode {
	bool latched = false;
	void process(Io<spec> io, const GraphCycle&) noexcept {
		if (io.triggered<resetClip>()) latched = false;
		const Smoothed gain = io.param<gainRow>();
		const auto ins = io[in];
		const auto outs = io[out];
		for (std::size_t c = 0; c < outs.size(); ++c) {
			const Smoothed trim = io.param<trimRow>(c);
			if (gain.steady() && trim.steady()) {
				const float factor = dbToFactor(gain.value() + trim.value());
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * factor;
			} else {
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * dbToFactor(gain[i] + trim[i]);
			}
			float peak = 0.0F;
			for (const float sample : outs[c]) peak = std::max(peak, std::abs(sample));
			if (peak > 1.0F && !latched) {
				latched = true;
				io.raise<clipped>(static_cast<std::int64_t>(c + 1), factorToDb(peak));
			}
		}
		io.set<clipping>(latched);
	}
};

class Gain {
public:
	static Members<spec> ports(const Checked& s) {
		return {in(members(s.get<channelsRow>())), out(members(s.get<channelsRow>()))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked&) {
		cx.use(GainCode{});   // no change version: its live rows retune, `channels` restarts
		return Configured::applied();
	}
};
}
```

`src/nodes/gain/gain.cpp` defines `makeGainNodeType()` as `makeNodeType<gain::spec, gain::Gain>()`;
`src/nodes/gain/CMakeLists.txt` is `ear_node_type(SOURCES gain.cpp TESTS tests/gain_test.cpp)`; `validSettings` gains
`{"gain", {{{"channels", 2}, {"gainDb", -6.0}, {"trimDb.1", 0.5}, {"trimDb.2", -0.5}}}}`. Nothing else names the type
(`docs/writing-a-node-type.md`).

A type with a loop's state writes status and events from that loop through the `Report` its `configure` hands the
state. Sendspin's worker after step 5.5:

```cpp
inline constexpr std::string_view connections[] = {"dialling", "listening", "connected"};
inline constexpr auto connection =
	status::choice<"connection">(writer::loop(worker), connections).display(Display::box);   // starts unknown
inline constexpr auto pairingCode = status::text<"pairingCode">(writer::loop(worker), 16).display(Display::callout)
                                        .help("type it into Music Assistant to pair");
inline constexpr std::string_view pairReasons[] = {"attempt-timeout", "concurrent-attempt", "method-not-supported",
                                                   "pairing-code-mismatch", "user-cancelled", "unknown"};
inline constexpr auto pairingFailed =
	event::of<"pairing-failed">(EventSeverity::warn, "pairing failed: {reason}", arg::choice<"reason">(pairReasons));
// In a worker loop handler:
report_.set<connection>(connection("connected"));
report_.raise<pairingFailed>(abortReasonName(reason));      // checked against pairReasons at run time
```

### 5.8 Compile-time refusals

Each a case in `src/core/tests/compile_fail/`, matched against GCC 14's wording: `live-text-row`, `live-optional-row`,
`live-whole-smoothed`, `get-live-row`, `indexed-count-below`, `indexed-count-not-number`, `indexed-no-default`,
`decimal-low-above-high`, `decimal-default-outside`, `log-taper-nonpositive`, `db-taper-range`, `param-not-live`,
`param-row-not-in-spec`, `param-indexed-without-member`, `param-member-of-plain-row`, `set-field-not-in-spec`,
`set-field-not-rt`, `report-set-rt-field`, `rt-text-field`, `writer-loop-not-in-spec`, `call-loop-not-in-spec`,
`choice-value-misspelt`, `initially-not-a-value`, `raise-text-arg-from-process`, `raise-wrong-arg-count`,
`raise-event-not-in-spec`, `sentence-unknown-place`, `sentence-missing-arg`, `report-of-other-spec`,
`report-constructed`, `triggered-not-a-trigger`, `action-named-lifecycle`, `duplicate-status-name`,
`duplicate-event-code`, `status-in-unnamed-namespace`, `event-in-unnamed-namespace`.

### 5.9 The harness

`TypeHarness` (built over a real `NodeHost`; 3c adds loops and its clock) gains, on the test's thread:

| Call | Does |
|---|---|
| `h.live<row>(value)`, `h.live<row>(member, value)` | moves the slot, as a gesture tick (runs `checkLive`, failing the test on a refusal) |
| `h.retune(settings)` | a change of live rows only: slots move, no restart (asserts the plan would retune) |
| `h.status<field>()`, `h.status()` | the live view after the last drain |
| `h.statusRecord()` | every change in order, `{field, from, to}`, and each reset |
| `h.events()`, `h.logs()`, `h.diagnostics()` | in order, each with its formatted `msg` |
| `h.trigger<action>()`, `h.invoke<action>(args)` | a trigger; a call, on the harness's loops |
| `h.drain()` | runs the tick now (the harness drains after every `process` and operation by default) |
| `h.outcome(settings)` | the plan's outcome of a change, without applying it |

The all-type tests gain: every type's schema encodes; every status field's initial value is of its kind or null; every
`values` entry parses with its row's parse function; every optional row without a `default` has a `defaultFrom` or a
`help`; every live row ramps from its default to another value with no sample unwritten; for every row, changing it
alone from its `validSettings` value changes the ports only if the row is `.shapesPorts()`, and every `.shapesPorts()`
row changes them for some value; the status view survives every in-place change.

## 6. The realtime contract

### 6.1 Channels into and out of the realtime thread

| Channel | Direction | Mechanism | Bound | On overflow |
|---|---|---|---|---|
| Parameter slot | in | one `std::atomic<double>` per live member, written by the main loop, loaded once a block | 256 live members per node | — |
| Trigger | in | one `std::atomic<std::uint32_t>` counter per trigger | — | several invokes in one block are one |
| `rt` status field | out | one `std::atomic<std::uint64_t>` per field, sampled at the drain; a transition counter beside it | 64 `rt` fields per type | — |
| Realtime ring | out | per node, single producer (the code), single consumer (the drain), fixed 48-byte records: kind, code, field, up to four 8-byte values, `startNs` | 256 records per node; log records refused once 192 are in use, so status and events keep the last quarter | dropped, counted per kind (`ear.records-dropped`, subject `realtime`) |
| Bell | out | `RealtimeBell::ring()`, an eventfd write gated by an atomic exchange on the window, as the level bell is | at most one per level window per block (30 a second) | — |

`std::atomic<double>` and `std::atomic<std::uint64_t>` are lock-free on x86-64 and arm64; a `static_assert` on
`is_always_lock_free` makes it a build failure elsewhere [U: no arm64 build yet].

It loads each live slot once a block, advances each ramp, stores `rt` fields, writes ring records, and rings the bell
at most once per window, after `process`, only when an `rt` field changed or the ring holds records. It never
allocates, locks, formats, or makes a syscall other than the bell's write.

### 6.2 Off the realtime thread: one queue per node

- `Report<spec>` writes and the handle's `lost` go into the node's queue: records (status change, event, lost) under one
  mutex, bounded at 1024 (a `lost` is never dropped); a full queue drops and counts (`ear.records-dropped`, subject
  `queue`). The handle's `log` goes straight to the log sink, as 3c defines it.
- The first record into an empty queue posts one wake to the main loop (an atomic flag gates it).
- Each writer's records stay in its order, so a status write before an event is drained before it, and a `lost` after
  the last status is drained after it.
- Text is made safe at the write, on the writer's thread: UTF-8 checked, control characters replaced, cut at the
  field's `maxBytes` and marked.

### 6.3 The drain and the tick, on the main loop

1. A wake, the bell, a gesture tick taken, or a trailing record due marks the node dirty and, if no tick is due, arms
   one on `Timers` for 33 ms after the last tick, or now if that has passed.
2. The tick drains every dirty node: its realtime ring first, then its queue. A status change updates the view, is
   offered to `StatusLog` (paced per field), counts a transition and marks the field for the wire; an event first sends
   the node's marked fields as one `node.statusChanged`, then `node.eventRaised` and an `event` record (its `t` the
   record's cycle start mapped to UTC for a realtime event, the write's time otherwise); a `lost` fails the node as
   built.
3. `rt` fields are sampled: a value that differs from the view's is marked for the wire.
4. One `node.statusChanged` per node with marked fields; one `gesture.changed` `change` per gesture moved; flapping is
   judged; drops become `ear.events-dropped` or `ear.records-dropped`; trailing status records due are written.
5. A `lost` from a configuration whose teardown began is dropped; its status and events are still drained until it
   closed, so a goodbye is heard; then its reset is sent.

### 6.4 Rates and bounds

| What | Bound | Policy |
|---|---|---|
| The tick | 30 a second, all nodes together | armed only when something is dirty |
| `node.statusChanged` | 30 a second per node | the latest values |
| `status` records | per node and field: 32 at once, then 1 a second | when the pace held changes back, a **trailing** record with the current value at the next tick the pace allows (at most 1 s after the last held change), carrying `heldBack`; so the last `status` record of each field always matches the view (G9) |
| Events (wire and stdout) | 32 at once, then 1 a second per node | held back counted in `ear.events-dropped` |
| Flapping | a field above its `flapsAbove` (default 10 in 10 s) | counted at each write and by the transition counter, so changes between samples count |
| Gesture ticks into the main loop | one pending per gesture | the control thread keeps the newest until the main loop took the last |
| `gesture.changed` `change` | 30 a second per gesture | never to the owner |
| Action reply | 10 s | `action-timeout`; the handler's late answer dropped |
| Gesture idle | 30 s with no message about it from its owner (keep-alives included), timed on the control thread | ended as released, keeping the last value heard (R32, G15) |
| A session's queued requests | 16 messages with an id (or batches) read ahead | at 16, the socket is not read until one is answered |

### 6.5 The tick path from the socket

**Reading ahead** (G12). Today a connection reads its next message only once every response to the last one is queued.
A held action reply (up to 10 s) or a request behind a keeper write would then stop every tick of that browser. So:

- Every message is read as it arrives. A message with an id, a batch, and any notification but the two below go into
  the session's FIFO and run one at a time, in order, exactly as today. The FIFO holds 16.
- A `gesture.change` or `gesture.cancel` **notification** skips the FIFO: checked for shape on the control thread and
  handed to the gateway at once. These produce no response, so no response order changes; a tick names a gesture whose
  id came from the begin's reply, so the begin already ran. When the session reads a `gesture.end` or `gesture.cancel`
  for an id, it marks that gesture *ending* at once: later ticks for it are dropped there, and its idle timer stops.
- **Idle and close** (R32; G15, G16): the session keeps each open gesture's idle timer, reset by every message about it.
  When 30 s pass, or the socket closes, it first hands the gesture's newest pending tick to the gateway, then marks it
  ending and queues, behind its earlier requests, a `gesture.end` with no value and a cause (`idle` or
  `session-closed`), which goes to the keeper as any end does. A closed session's queued requests with an id still run
  in order, their replies dropped.

**Coalescing.** The gateway keeps, per gesture, the newest pending tick, and posts one task to the main loop if none was
pending; 1000 ticks a second cost at most one task per turn. **On the main loop**, `Gestures::move` checks the gesture
and its owner, drops a stale `seq`, stops for a keep-alive, checks the value against every reached row and the type's
`check` (a refusal: `tick-refused` to the owner, at most once a second), writes every reached slot through
`NodeHost::setLive` (for a node with no configuration, only its live value), and arms the tick.

### 6.6 The channels across the lifecycle

| Moment | What happens to this page's channels |
|---|---|
| `configure` from empty | the channels are made: slots (seeded from the committed values or the node's held live values), `rt` words, counters, the ring, the queue; the status view at its initial values; the `Report` |
| configured | the code is prepared and bound to them; nothing runs on the realtime thread yet |
| the code drives | the first block starts every ramp at its slot's value; the code may write from its first cycle; then `start()` |
| a retune, in any phase | the slots move at once, wait-free; with no configuration only the node's live value moves and seeds the next one; while `stopped` the slots move and the code ramps from them at the next start |
| an in-place change (3d) | the code keeps running and writing; a hold is applied after the code, so a held bank's `rt` fields and events are the code's own, and the meters show the silence |
| a code swap (3d) | the new code is bound to the same channels; each ramp goes on, each `rt` field's last value is kept, an unseen trigger is seen once by the new code |
| a stop | the code stops; the ring is drained; the channels and the status stay |
| a restart, a failure, a removal, a disable | the ring and queue are drained until the configuration closed; then the reset |
| teardown step 5 | the channels are freed, after the code and the loops' states |

No channel is allocated, freed or resized on the realtime thread or while the code that uses it can run. One node's
transition never touches another's channels (R16). A companion writes no status, events or records.

## 7. The keeper path

### 7.1 Who does what

| Step | Thread | Owner |
|---|---|---|
| Decode `gesture.*`, `node.invoke`; read ahead; mark a gesture ending | control | `ear_control` (`decodeCommand`, the session) |
| The reach index of each version of the working copy | keeper | `ear_config` (`GraphFileModel::reachIndex`) |
| `gesture.end`, `node.invoke`: ordering, file checks, the candidate | keeper | `ear_config` |
| `gesture.begin`: file state, the reach index, rows, holds | main loop | the keeper's main-loop side (which holds the running version's index), `Gestures` |
| Open, move, close and abort gestures; write slots; tell listeners | main loop | `Gestures` (`ear_core`), `NodeHost` |
| Plan, commit (each node's outcome to `NodeHost` through `nodeChanged`) | main loop | the keeper's main-loop side, `DesiredGraph`, `NodeHost` |
| `gesture.change`, `gesture.cancel` notifications | control, then main loop | the gateway, `Gestures` |
| A gesture's idle timer; its end on the owner's behalf | control, then keeper | the session, then the keeper |
| Whether a save is related to each open gesture | main loop | the keeper's main-loop side, `Gestures` |

The control thread never holds a gesture (only the newest pending tick and an ending mark); the keeper thread never
holds one either; `Gestures` on the main loop is the one owner.

### 7.2 A gesture, step by step

```mermaid
sequenceDiagram
  participant E as Editor
  participant C as Control thread
  participant K as Keeper thread
  participant M as Main loop
  participant R as Realtime
  K->>M: with each offer: the version's reach index
  E->>C: gesture.begin {amp, gainDb}
  C->>M: BeginGesture{session, amp, gainDb}
  M->>M: file state, index, rows, holds: g4 opened
  M-->>E: (others) gesture.changed begin; (owner) reply {g4, -6, reach, range}
  loop each frame, and every 2 s while held still
    E->>C: gesture.change {g4, seq, v} (no id, read ahead)
    C->>M: newest pending tick
    M->>R: slot = v (atomic)
  end
  E->>C: gesture.end {g4, -3.5}
  C->>C: mark g4 ending
  C->>K: EndGesture{session, g4, -3.5}
  K->>K: candidate: gainDb -6 -> -3.5 at its definition
  K->>M: step: offer (end of g4)
  M->>M: plan: retune; graphFile.changed; commit; Gestures.close(g4)
  M-->>E: graphFile.changed, node.updated, gesture.changed end, reply {-3.5}
  K->>K: write ear.conf when free
```

- **The reach index** (keeper thread). With every offer (an accepted edit's candidate, a disk read, `graphFile.set`) the
  keeper sends `GraphFileModel::reachIndex()`: for each node and live key `ear.conf` gives, the definition's place, its
  **defining lines** (the whole source lines the defining statement is written on, carried as a 64-bit hash with their
  range), every node and key whose expanded value has that definition (knobly's `definition()`), and the committed
  value. Made only when the text changed. A text with errors has none (begin refused `file-invalid`); a disk text the
  graph refused leaves the old running index but marks the file not running (begin refused `file-refused`).
- **Begin** (main loop, no keeper), in order: `stopping`; `file-invalid` or `file-refused`; `unknown-node`; `not-live`;
  the reach from the running index; `shared-definition` unless every reached row is live, of one kind and unit, ranges
  meeting; `parameter-held`. Then `Gestures` opens it, tells the others `begin`, and answers. Nothing is written. Each
  slot ramps with its own row's smoothing.
- **End** (keeper thread): `stopping` after stop; the candidate is the node's settings with `key` set to the rounded
  value through the built `updateNode`, which changes the value at its definition, so every reached node changes in one
  commit. On the main loop the candidate's changed keys must be the gesture's reach (a guard), the gesture's own keys
  pass `parameter-held`, the plan is committed, then `Gestures::close` sets each reached slot to the committed value,
  tells `end`, and the reply carries the value. An end refused for its value leaves the gesture open; any other refusal
  aborts it.
- **Cancel and node removed** abort on the main loop: every reached slot returns to the committed value. **Session close
  and idle** end the gesture as released (R32); if that end is refused, the gesture is aborted and the slots return to
  the committed value.
- **What is written.** Only an end (and `node.set`, `node.update`, `graphFile.set`) changes the working copy; `ear.conf`
  is written behind as built; at most one commit per gesture.

### 7.3 Refusals and collisions

| Lands during a gesture | What happens |
|---|---|
| A hand edit **related** to the gesture | the gesture aborted (`hand-edit`, no value) on the main loop before the offer's plan (R31); the file offered as today; last in the turn each reached slot is set to its node's running value |
| A hand edit **not related** | nothing: no `gesture.changed`, the slots keep the hand's value, its places rebased; its end later edits the new text |
| A hand edit with errors, or one the graph refuses | every open gesture aborted (G14): no end could commit onto it |
| `graphFile.set` | as a hand edit, reason `file-saved` when related |
| `node.set` or `node.update` changing a held key | refused `parameter-held` |
| `node.set` or `node.update` changing other keys of a reached node | applied, whatever its outcome: a reconfigure keeps the slots; a restart keeps the hand's live value |
| A reshape that removes a reached key | applied; the gesture aborted (`key-removed`) before the commit's events |
| The reached node fails, recovers, restarts, stops, starts or is disabled | nothing: ticks move the node's live value, which seeds its next configuration |
| `node.remove` or a replacement of a reached node | applied; the gesture aborted (`node-removed`) |
| Another session's `gesture.begin` on a reached pair | refused `parameter-held` |
| The owning session closes, or is silent 30 s | ended as released, the last value heard committed (`reason` `session-closed` or `idle`) |
| `SIGTERM` | later begins and ends get `stopping`; open gestures are not committed and not announced (clients closed with 1001); the last write holds no live value (G16) |

The keeper's main-loop side learns the change's kind from the offer: a new member `OfferKind` (`disk`, `set`, `edit`,
`gestureEnd{id}`) beside the built `Offer`'s members, and the version's reach index.

**When a save is related** (R31: "only a related save"). Decided per open gesture on the main loop, when the offer of a
disk read or a `graphFile.set` arrives and before its plan, from the running reach index and the new text's (neither
knobly nor the text crosses to the main loop). The save is **related** to gesture `g` when, for any pair of `g`'s reach:

1. **its defining lines changed**: the defining statement (the `@set $trim 0;` its users share, or the setting statement
   itself; for a chain of variables, the chain's end) is written on lines whose text differs between the versions; whole
   source lines, so a comment added at the end of the defining line is related, one on the line above, or the same line
   moved unchanged, is not;
2. **its value changed** after knobly's expansion (catches a chain's base changed elsewhere);
3. **its reach changed**: it is defined elsewhere now, or the set of pairs sharing its definition differs;
4. **it is gone**: the node removed, renamed or replaced, or the key no longer declared;
5. **the new text has errors, or the graph refuses it** (every gesture).

A key the file did not give has no defining lines: the save is related when the new text gives the key, or when its
node's own `node` line changes. Anything else is unrelated: the gesture stays open, its index entries are replaced by
the new text's, and nothing is sent. Its cost is one comparison per open gesture per save.

### 7.4 Retune: a live change without a restart

`DesiredGraph::plan` names a **retune** when every key whose value differs is live and the ports are unchanged
(`reconfigure.md` 3's outcome table). It reaches `NodeHost` through the built `nodeChanged` with outcome `retune` and
the changed keys; `NodeHost` keeps the new config for later configurations and writes each key's slot from
`config->liveValues()`, unless a gesture holds the key, in any phase. So a hand edit, `node.update` or `node.set` that
changes only `gainDb` retunes: the audio ramps, no `node.state` follows, and standard output has a `param` record. A
loop's state and companions are not told of a retune: a value that must reach the loop's state is an `.inPlace()` row.

| Row | `node.set` | A gesture | Node `running` or `stopped` | Node `reconfiguring` | Not yet rested | `failed`, `recovering` |
|---|---|---|---|---|---|---|
| **live** | `retune`: slots move at once | yes | ramps; no `node.state` | ramps at once; the change goes on | the node's live value moves and seeds the next configuration | the same; no restart, no recovery skip |
| **in place** | `reconfigure` | refused `not-live` (X5) | `reconfigure.md` | `reconfigure.md` | a restart | a restart at once |
| **restart-only** | `restart` | refused `not-live` | built | `reconfigure.md` | built | built |

A **disabled** node takes every row the same way: the change is written (`stored`); a live row moves only the node's
live value; a gesture on it is allowed as on a failed node.

### 7.5 Actions

- `node.invoke` reaches the keeper's thread in order with every other request, needs no file, and posts a step to the
  main loop at once, as the built lifecycle verbs do. There `NodeHost::invoke(node, action, args, reply)` checks the
  action and its arguments against the type's table.
- A trigger: the counter is incremented and the reply posted at once; `not-running` (with the `phase`) when the code is
  not driving (any phase but `running` and `reconfiguring`).
- A call: posted with `postFromOutside` to the loop it names; the reply when the handler returns. A queued call gets
  `restarted` (or `stopping`) when the configuration is torn down; a running one finishes and replies before the goodbye
  `stop()` posted behind it. A stopped node's loops take calls.
- **The keeper never waits for a handler.** Its step completes when the action is dispatched; the batch's reply is held
  on the main loop until every action in it answered (or its 10 s deadline passed), then posted behind every event
  before it. A slow handler holds that client's later requests with an id (the FIFO), never its ticks and never the
  keeper's queue.
- An `action` record on standard output (built for the lifecycle verbs) says who asked for what.

## 8. The editor

### 8.1 Stores

| Module | Holds | Tested by |
|---|---|---|
| `protocol.ts` | the wire types of section 4; `ControlErrorCode`; `NodeDiagnostic`, `EventSeverity` | typecheck |
| `graph.ts` | each node's `status`, `gestures`, `diagnostics`; handlers for the new notifications | vitest, every fixture |
| `types.ts` (new) | the type schemas by hash; `types.get` with `ifNoneMatch`; cached in `localStorage` (in `try`) | vitest |
| `gesture.ts` (new, pure) | the session's own gestures as a state machine; what to send for each input; the control mapping | vitest, a fake client |
| `form.ts` (new, pure) | the settings form's per-field drafts; the Apply flow (3d's dry run) | vitest |
| `activity.ts` (new) | per node, the last 100 events and diagnostics since connecting; notices, coalesced per node and code | vitest |

Features gate the UI: without `gestures` live rows render as plain number fields committed by `node.set`; without
`types` the panel is today's JSON area. Nothing in `web/` names a node type.

`gesture.ts`'s states and edges:

| State | Input | Sends | Next |
|---|---|---|---|
| idle | press, or first key | `gesture.begin` | beginning (intent: hold) |
| beginning | move | nothing (keeps the newest) | beginning |
| beginning | release / Escape | nothing yet | beginning (intent: end / cancel) |
| beginning | reply, intent hold | the kept value as tick 1 | active |
| beginning | reply, intent end | `gesture.end` with the shown value | ending |
| beginning | reply, intent cancel | `gesture.cancel` | idle |
| beginning | refusal | nothing | idle; snaps back; a notice names the code |
| active | move | `gesture.change`, at most one per animation frame | active |
| active | 2 s with no tick sent | the same value as a tick (keep-alive) | active |
| active | release or lost capture | `gesture.end` with the shown value | ending |
| active | Escape | `gesture.cancel` | idle |
| ending | reply | — | idle; shows the committed value |
| ending | refusal `invalid-settings` | `gesture.cancel` | idle; notice |
| any | `aborted` for its id | — | aborted until release, then idle; notice |
| any | `end` with a `reason` for its id | — | idle; the committed value; notice |
| any | connection lost | — | frozen until release; then the snapshot's value and a notice |

### 8.2 How each row kind renders

| Row | Control | Commits |
|---|---|---|
| text with `values` | a select | on change |
| text with `pattern` or `maxLength` | a text field, checked as typed (ear still judges) | on Enter or blur |
| whole number | a number field with steppers, `min`, `max`, unit | on Enter or blur |
| decimal | a number field with `decimals`, unit | on Enter or blur |
| optional with `defaultFrom` | empty, its placeholder saying where the default comes from | as its kind |
| live decimal | a fader in the panel, mapped by `taper`, with a number field beside it | a gesture; the field by `node.set` |
| live whole number | steppers and a number field (it steps) | `node.set` |
| indexed family | one control per member, `n` from the count row's value; above 8, a compact strip behind a disclosure | as its kind |
| `onlyWithout` | disabled while the other key is set, its tooltip naming it | — |

- **The box** shows a knob for each live, non-indexed row when the type declares one or two of them (`gain` shows
  `gainDb`).
- **Hints from the schema alone**, beside each non-live field: "applies in place" for `inPlace`, "restarts the node" for
  `restart`, and "changes the ports" for `shapes`.
- **Drafts.** Each form field keeps its own draft once typed in; a server change to a field with a draft marks it
  "changed on ear" and offers to take ear's value, as the text pane does (today `Panels.tsx` resets every field on each
  update). Live controls follow the server, except one the operator holds.
- A refusal is shown under the field `data.key` names, or at the form's top. Live controls are disabled while the graph
  file has errors or was refused, and while the connection is not open.

### 8.3 Gestures from pointer and keyboard

| Taper | Value at travel `t` (0..1) |
|---|---|
| `linear` | `min + t × (max − min)` |
| `log` (min > 0) | `min × (max / min)^t` |
| `db` (min <= -60, max >= 0) | piecewise linear through (0, min), (0.10, -60), (0.25, -40), (0.45, -20), (0.60, -10), (0.75, 0), (1, max): unity at three quarters, as a console fader |

A value is shown and sent rounded to `decimals`; `minIsSilence` shows the minimum as "-inf".

| Input | Sends |
|---|---|
| pointer down on a fader or knob (captured) | `gesture.begin` |
| fader drag | travel follows the pointer along its length |
| knob drag | vertical: 200 px is the whole travel, up increases; Shift: 2000 px |
| pointer up, or lost capture | `gesture.end` with the shown value |
| Escape while dragging | `gesture.cancel` |
| double-click or Alt-click | begin, then end with the row's default |
| Arrow keys on a focused control | the first key begins; each key a tick of 1% of travel (Shift 0.1%); end 400 ms after the last key, or on blur |
| Page Up/Down, Home/End | 10% of travel; the ends |
| the wheel, only on a focused control | 1% per 50 px of `deltaY`; over an unfocused control the wheel scrolls the panel |
| Enter in the number field | `node.set` |

- **The canvas does not see a control's input**: every control on a box carries React Flow's `nodrag`, `nowheel`,
  `nopan` and `nokey` classes and stops the propagation of every key it handles, so Backspace on a focused knob never
  deletes the node [U: React Flow 12's class names and key filter, checked against the pinned package].
- Another session's gesture: the control moves, is disabled, and says "held by another editor".
- Notices for its own gesture's abort: `hand-edit` "ear.conf was edited by hand and changed amp gainDb while you held
  it; it now says -10." [Set -3.5 again]; `file-saved` the same from the text pane; `node-removed` "amp was removed or
  replaced while you held gainDb."; `key-removed` "amp2 now has 2 channels, so trimDb.3, which you held, is gone.";
  `refused` the end's refusal message. An `idle` end: "Your change to amp gainDb was kept at -3.5 after 30 s without a
  word from this editor." A socket lost mid-drag: "Connection lost while you held amp gainDb; ear kept -3.5."; after a
  1001 close: "ear restarted while you held amp gainDb; your change was not kept: it is -6 again."

### 8.4 Status, events, diagnostics and actions

- **Status**: `box` fields on the box after the state line ("connected · Music Assistant"); every field in the panel (a
  flag as yes or no, `null` as "unknown"); `callout` fields as a banner on the box and in the panel while not initial,
  with a copy button for text. Box and callout fields show only while the node is `running`, `reconfiguring` or
  `stopped` and the value is not `null`.
- **Events**: the node's activity list (since this page connected); `warn` and `error` also as a notice, coalesced per
  node and code ("pairing failed ×3"), at most one new notice per node per 5 s.
- **Diagnostics**: a badge on the box for each active entry ("flapping: connection"), its tooltip giving the count and,
  for flapping, `flapsAbove`; the panel lists every entry. An unknown code is shown by its `msg`.
- **Actions**: one button per type action after the built lifecycle buttons; a call with arguments opens a small form;
  disabled while in flight and when the node has no configuration.

### 8.5 Accessibility

Faders and knobs are `role="slider"` with `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, `aria-valuetext` ("-3.5
dB", "-inf dB") and a label from the key, keyboard per the WAI-ARIA slider pattern; held by another editor:
`aria-disabled` with the reason. A polite live region announces callout fields when they change, aborted gestures and
flapping starting, at most one per node per 5 s; `error` events assertively. Flapping is never shown by colour alone.

## 9. Standard-output records

New kinds, each with `v`, `seq`, `t`, `kind`, its fields, `msg`, as built. Every change is additive, so `v` stays 1.

```json
{"v":1,"seq":120,"t":"2026-11-08T10:01:50.311002Z","kind":"status","node":"kitchen","field":"connection","from":"dialling","to":"connected","msg":"node kitchen connection dialling -> connected"}
{"v":1,"seq":140,"t":"2026-11-08T10:02:12.000300Z","kind":"status","node":"kitchen","field":"connection","from":"dialling","to":"connected","heldBack":9,"msg":"node kitchen connection now connected (9 changes held back)"}
{"v":1,"seq":141,"t":"2026-11-08T10:04:00.100000Z","kind":"status","node":"kitchen","reset":true,"msg":"node kitchen status reset"}
{"v":1,"seq":122,"t":"2026-11-08T10:01:52.004211Z","kind":"event","node":"kitchen","code":"pairing-failed","severity":"warn","args":{"reason":"pairing-code-mismatch"},"msg":"node kitchen pairing failed: pairing-code-mismatch"}
{"v":1,"seq":123,"t":"2026-11-08T10:02:11.120400Z","kind":"diagnostic","node":"kitchen","code":"ear.status-flapping","subject":"connection","active":true,"count":2,"transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
{"v":1,"seq":128,"t":"2026-11-08T10:05:20.770000Z","kind":"action","node":"kitchen","action":"forget-pairing","result":"action-failed","detail":"no pairing with ma-0000","msg":"node kitchen forget-pairing refused: no pairing with ma-0000"}
```

- `status`: every change as written, paced per node and field; a trailing record after held-back changes carries
  `heldBack`, so the last record of each field matches the view; a reset is one record with `reset: true`. A text value
  in `msg` is quoted when empty or holding a space; `null` is `unknown`.
- `event`: `msg` is `node <n> ` and the sentence with its arguments; `t` for a realtime event is its cycle's start.
- `diagnostic` (built) gains `subject`, `active` and `count`.
- `param`: one per reached node and key at each committed live change (`cause`: `gesture`, `set` for `node.set` and
  `node.update`, `disk` for a hand edit or `graphFile.set`) and at each abort (`cause: aborted`, `reason`; `to` the value
  it returned to, absent for `hand-edit` and `file-saved`). An end ear made for its owner is `cause: gesture` with
  `reason`.
- `action` (built for the lifecycle verbs) gains one per `node.invoke` answered, with `result` (`ok` or the refusal's
  code) and `detail` for a refusal.
- `note`: written only by `sendspin` until step 5.5; then the kind is removed from the encoder. Operators grep `says
  pairing-code` (the `ear-on-talos` skill), so step 5.5's commit carries a `BREAKING CHANGE:` footer naming the
  replacement, and the release notes say it.

## 10. Migrating sendspin's notes (step 5.5)

- `notes.hpp` and `notes::printable` go (3c already moved note pacing into core): the framework makes text safe and
  paces events.
- The worker and key store write through the `Report<spec>` that `configure` hands the worker's state, with every field
  `writer::loop(worker)`, instead of `NodeHandle::say`.
- **`stream` and `sending` come from the loop's state**: sendspin has no realtime code after 3c. The worker writes
  `stream*` when the library starts or ends a stream, and `sending*` when the library asks for audio and when the sink's
  consumer first gets a chunk after it (a fact the consumer posts to the worker).
- **A rename, a port change or a listen/dial switch keeps the configuration** (3d): the status stays, and the worker
  rewrites the fields the change touches (`instance`, `listenPort`, `url`, `connection`).
- `NodeHandle::say`, core's note pacing and `StatusLog::nodeSays` go once nothing calls them.
- The system tests that match `says` lines match the new `msg` lines (`node kitchen pairingCode "" -> "123-456"`); the
  pairing test reads the code from the `status` record's `to`.
- `docs/node-types.md#notes`, the `sendspin` notes list, becomes its status fields and events; the `sendspin` and `ear-on-talos` skills'
  "what its notes mean" sections name fields and events (the operator's grep becomes `jq -r 'select(.field=="pairingCode")
  | .to'`).

## 11. The cut: buildable steps

Each step keeps every test green, adds its feature to `features`, and changes no audio path unless it says so.

| Step | Needs | Feature | Changes the audio path |
|---|---|---|---|
| 5.1 to 5.5 | 3c; 3d for `change` in the schema | `types`, `status`, `actions` | no |
| 6.1 to 6.5 | 3d, 5.1, 5.2 | `live`, `gestures` | 6.3 (a live change ramps instead of restarting) |

**5.1 The schema on the wire.** `NodeType::schema()` from the settings table (text hints and `defaultFrom` added to
`rtp`'s, `card`'s and `sendspin`'s rows; `change` from 3d's marks; `.shapesPorts()` on `card`'s `playback` and `capture`
and `rtp`'s `channels`); `types.get` answered on the control thread from an encoding made at boot; `typeSchemaHash`;
`InvalidSettings::key`, `data.key`, `data.detail`; `types.ts`; the settings form rendered from the schema with drafts,
hints and the JSON toggle. Tests: `types.json`, `errors.json`; `form.ts`'s drafts; the all-type schema checks;
`image-smoke.sh` calls `types.get`; a system test that `graph.get`'s hash matches `types.get`'s.

**5.2 Status and events off the realtime thread.** `StatusTable`, `EventTable`, `LogCodes` in `TypeSpec`; `Report` and
`NodeOf::report`; the per-node queue, the drain and the tick; `node.statusChanged` with its reset, `node.eventRaised`,
the `status` and `event` records; `status` in snapshots and `node.added`; `node.diagnostic` with subject, `active` and
`count`; flapping, `ear.events-dropped`, `ear.records-dropped`, and 3b's, 3c's and 3d's diagnostics on the wire; the
harness's captures; `rpc-call.mjs --follow`. Tests: `ear_core_tests` with a test type (status before event, a `lost`
after the last status, a closed configuration's write dropped, the reset after the last values, status kept through a
stop and a start, the queue's bound, flapping and settling on the harness's clock, the tick's 33 ms cap, the trailing
record within 1 s and matching the view); `status.json`, `diagnostics.json`; the editor's status list and badges; a
system test of ten chatty nodes and a slowly reading client.

**5.3 The realtime ring.** The ring with its log budget, `rt` fields, `io.set`, `io.raise`, `io.log`, cycle stamps, the
gated bell; the channels bound when the code is prepared and rebound at a swap. Tests: records in order, the bound, log
records refused past 192 while events pass, transitions counted between samples, an event's `t` from its cycle, a swap
keeping an `rt` field's last value; `ear_pipewire_tests` (a test block's code sets a field and raises an event; an
idle block rings nothing); `process` allocates nothing while writing.

**5.4 Actions.** `ActionTable`, triggers, calls on a loop of the spec, `node.invoke` through the keeper, `not-running`,
deferred batch replies with their deadline, the `action` record for type actions, the editor's buttons; sendspin's
`forget-pairing` as the first call. Tests: `actions.json`; an invoke ordered between two edits in one batch; the keeper
taking the next batch while a handler is slow; a trigger seen once; a call refused before the node is configured; a
call taken while stopped; a queued call answered `restarted`.

**5.5 Sendspin's notes split** (section 10). Tests: the sendspin system tests rewritten to read records; each field and
event; a rename through the in-place `configure` keeping `clientId`. The commit carries the `BREAKING CHANGE:` footer.

**6.1 Decimal rows.** The rule, the walk, knobly's `number()` reader and the fixed writer, the schema's `decimal`,
tapers and `minIsSilence`, the editor's number field. Tests: range, integer taken, NaN impossible; compile-fail
`decimal-*`, `log-taper-nonpositive`, `db-taper-range`; read `-3.5`, `-6`, `0.25`, write and read back, `-0.04` at one
decimal written `0`, a value set by `@set`; a `node.update` from today's editor round-tripping `-3.5` and `0.1`.

**6.2 Indexed rows.** The family, its keys, walk and refusals, `get<row>(i)`, the editor's member controls. Tests: walk
order with a family, a member above the count, unknown keys beside a family; a `.` in a directive name, else the
fallback spelling decided and recorded.

**6.3 Live rows, slots and retune** (changes the audio path). `.live`, slots, the ramp, `io.param`, `Smoothed`,
`initial<row>()` and `get` refused on live rows, `checkLive`, `nodeChanged`'s `retune` in `NodeHost`, `setLive` in every
phase, `applied: "retune"`, the `param` record; the `gain` type. Tests: a retune plan; a plan with one non-live key not
a retune; slots seeded on restart; a retune during a reconfigure moving the slots at once; a retune of a failed node
moving only its live value; an indexed family's slots resized by an in-place reshape; `ear_node_gain_tests` (5.4's
vector frame for frame, a restart mid-ramp, steady blocks, a ramp never read still keeping time, a log-taper ramp,
trims, the clip latch, the trigger); a system test: white noise through `gain` into a `file`, `node.set` of `gainDb`
mid-recording, the WAV's level changing over 20 ms with no gap and no `node.state` record.

**6.4 Gestures.** `Gestures`, `gesture.*`, the reach index posted with each offer, begin on the main loop, the
mixed-reach refusal and the combined range, read-ahead and the ending mark, the direct crossing, keep-alives, the idle
deadline and the session's close as ends on the owner's behalf, the related-save rule, every abort of 7.3, `OfferKind`,
rounding at the end, no own echoes, `tick-refused`, the harness's gestures. Tests: `gestures.json`; the reach index of a
`@set` used twice, a key not given, a snippet's line, each with its defining lines and hash; the related-save cases on
pairs of hand-made indexes; `ear_server_tests` (a slow request in flight while ticks still move the slot; an end queued
behind it never losing to the idle deadline; the idle deadline reset by keep-alives and stopped by a read end; a socket
reset mid-gesture sending the end behind earlier requests; the FIFO's bound); begin answered while the keeper is
writing; system tests: two clients, one holds a gesture, the other's `node.set` is `parameter-held`, a hand edit of the
held value aborts it and the WAV goes straight to the new level while a hand edit elsewhere leaves it going; a client
that drops its socket mid-gesture leaves its last value in `ear.conf` and the WAV.

**6.5 The editor's controls.** Fader and knob with 8.3's mapping, keyboard and wheel, held and aborted states, notices
with "Set again", accessibility. Tests: `gesture.ts` against a fake client (every edge of 8.1's table); the taper
formulas and their inverses; Backspace on a focused knob sends no `node.remove`.

## 12. Costs, risks and open items

- **More compile-time machinery**: about 35 compile-fail cases at about 1.3 s each.
- **A wider wire**: four notifications, six requests, eleven codes. The client's queue bound (256 messages) now also
  holds status at up to 30 a second per node; a slow client with many chatty nodes reaches 1008 sooner. If it bites,
  coalesce a node's `node.statusChanged` in the client's queue when it is the last queued message. [U: step 5.2's
  system test.]
- **A behaviour change**: `node.update` of only live rows no longer restarts.
- **The session reads ahead**, a change to the one-in-flight rule for two notifications; the ending mark exists so a
  tick never lands after its gesture's end was read.
- **Gestures cross the control thread straight to the main loop**, and begin answers from the reach index; a bug here
  breaks "the audio never stays on a value `ear.conf` does not hold".
- **The related-save rule can miss or over-reach**; it errs towards aborting.
- **A dropped connection writes its last value** (R32), 30 s later at most; the `param` record says so.
- **Rounding at the end** can differ from the last live value by half a step (0.05 dB at one decimal).
- **Ticks run the type's `check`** on the main loop, once per gesture per turn at most.
- **Triggers are lossy by design**: several invokes in one block are one; an action that must count is a call.
- [U] Aggregate deduction of `TypeSpec` with designated initialisers and defaulted parameters on GCC 14
  (`node-primitives.md` U1); fallback a consteval `makeSpec`.
- [U] knobly reads `-3.5` as one number word in a setting's place, and a directive name may hold `.`; fallback `trimDb-1`.
- [U] `std::atomic<double>` lock-free on arm64.
- [U] Browsers' pointer capture on touch screens (`lostpointercapture` after a cancelled touch), checked by hand in 6.5.
- [U] That knobly's `definition()` gives a source range from which the keeper can take the defining statement's whole
  lines (a setting, a `@set`, a chain's end, a line inside a `@snippet`); if a snippet's use hides it, every save that
  changes the snippet or its `@use` line is related.
- The reach of a value defined through a chain of variables follows `definition()` (the chain's end); whether an
  operator expects that is checked with the owner on the first real file that does it.
- Reserved, not designed: `watch<row>(loop, fn)` for live rows off the realtime thread (X6); a `flag` setting row (G11);
  timestamped parameter changes and events; `log.subscribe`; `ear.realtime-overrun`.

## 13. Rejected alternatives

| Alternative | Why not |
|---|---|
| Normalised 0..1 parameter values on the wire (VST3) | the row's own unit is what `ear.conf` holds and a script writes; the taper maps travel |
| Write `ear.conf` on every tick | the owner chose "commit at gesture end" (Q6) |
| A parameter store beside `ear.conf` | two sources of truth; hand edits could not win |
| Ticks or begins through the keeper | the keeper may be writing; a laggy knob |
| Client-chosen gesture ids | a begin on the main loop is fast enough; ids across sessions need a collision rule |
| One `node.setParam` with a gesture phase field (CLAP's gesture events) | separate methods give each its params, errors and path |
| One notification per reached key | a 64-trim gang is 1920 messages a second per client |
| Begin/end implied by idle timeouts (OSC, MIDI CC) | no commit point the operator chose |
| Sample-accurate parameter events (CLAP, VST3) | a ramp per block is enough for faders; timestamped changes stay reserved |
| Constant-slope ramps | the ramp's length would depend on the distance; constant time keeps every move 20 ms |
| A seqlock per node for a gang's slots | a retry loop on the realtime thread for at most one block of ramp |
| Status as notifications only, no snapshot | a late client could not show a pairing code offered before it |
| Events as status fields that flash | an instant has no value to read |
| Diagnostics per code with a separate settled code | two fields flapping overwrite each other |
| OSC as the control transport | no schema, no replies, no order over UDP |
| JSON Schema as the type schema | a small closed vocabulary reads better and checks in tests |
| A hash per type | types are compiled in and change together |
| Clamping out-of-range ticks | a script sending a wrong value should learn it |
| A base class with virtual `onParam`, `onAction` (JUCE, VST3) | nothing to override: a type declares (R1) |
| Every save aborting every gesture | the owner: "Only a related save" (R31) |
| A dropped tab's gesture returning to the file's value | the owner: "Keep last value" (R32) |
| Gestures on `.inPlace()` rows | a drag would chop the audio into holds (X5) |

## 14. Decisions already taken

Each is the designer's unless marked the owner's; the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| G1 (owner) | A gesture's reach is every node and key sharing the definition, all live and of one kind and unit; the range their intersection | The owner's answer, as recommended |
| G2 | `node.set` (3d), `gesture.begin`, `change`, `end`, `cancel`, one `gesture.changed` per gesture, `node.invoke` | One path and one set of errors per method |
| G3 (owner, R31) | Only a related save aborts a gesture | The owner's answer |
| G4 (owner, R32) | A closed or silent session's gesture ends as released, keeping its last value | The owner's answer |
| G5 | A gesture's committed value is rounded to the reach's `decimals` | `ear.conf` stays readable; at most half a step |
| G6 | Slice 3c before 5; 3d before 6 | `Report` sits on the node handle; a retune is one outcome among the others |
| G8 | `param` and `action` records on standard output | `kubectl logs` says whether a person caused a 3 a.m. change |
| G9 | Status records paced per node and field, with a trailing record of the current value | `kubectl logs` never ends on a value the node left |
| G11 | No flag setting kind yet | No built-in type needs one |
| G12 | A session reads ahead for `gesture.change` and `gesture.cancel` | Nothing with an id changes order |
| G13 | A gesture's begin is answered on the main loop from a reach index the keeper posts | The second drag moves the audio within a turn |
| G14 | "Related" is judged per gesture on the main loop before the plan, from defining lines' hashes, the evaluated value, the reach and the pairs' existence; a text with errors or one refused aborts every gesture | The owner's words name the value and its line; the value catches chains, the reach a new sharer |
| G15 | A tab silent for 30 s about its gesture is treated as released, like a closed one | A Wi-Fi drop usually shows as silence before TCP closes; the two must agree with R32 |
| G16 | The idle timer and the end on the owner's behalf live in the session; a stop of ear commits no open gesture | The session sees every message; a stop is nobody's release |
| K1 | No framework status fields; a companion's counts are on its `companions` entry | Counts belong to the crossing |
| X1 | The schema's per-row `change` and `shapes`; `.shapesPorts()` held to `ports()` by the all-type tests | The hint beside each field needs no round trip |
| X5 | A fader only on `.live` rows | A drag over an in-place row would chop the audio into holds |
| X6 | Values applied off the realtime thread are `.inPlace()` rows the type posts to its loop; no `watch` hook until a type needs one | Covers every built-in need with 3d's machinery |
| W1 (built) | One `node.state` whenever any health member changes | — |
| new | A type bug found at run time (a choice value not among its values, a rule broken) fails the node with the rule's text, never throws out of the framework | As built in 3a for every context rule |
