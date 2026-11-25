# The control plane and parameters: slices 5 and 6 of the node framework

Status: design, step 5.1 built (the type schema on the wire, `types.get`, `typeSchemaHash`, `invalid-settings`' `key` and
`detail`, and the editor's settings form with Apply asking first: `docs/control-port.md#type-schemas`,
`docs/writing-a-node-type.md#the-type-schema`, `docs/editor.md#the-settings-form`), step 5.2 built (status fields,
events and log codes in the spec, `Report`, the per-node queue, the tick and its paces, the `status` and `event` records,
the feature `status` with `node.statusChanged`, `node.eventRaised` and `node.diagnostic`, `rpc-call.mjs --follow`, the
editor's status list, badges and activity: `docs/writing-a-node-type.md#status-and-events`,
`docs/architecture.md#status-and-events`, `docs/output-records.md#status`, `docs/control-port.md#status`,
`docs/editor.md#status-events-and-diagnostics`), step 5.3's realtime channels built (`rt` fields, `io.set`, `io.raise`,
`io.log`, the ring with its log budget, cycle stamps and the gated bell, bound when the code is prepared and rebound at a
swap: `docs/writing-a-node-type.md#reporting-from-the-code`, `docs/architecture.md#the-realtime-channels`; left of it:
the bell on the main loop in `ear_pipewire`); the rest not built. Slice 5 (the
control plane: status, events, node diagnostics on the wire, actions, the realtime coded ring, sendspin's notes split)
follows slice 3c (built: the node
handle and loops, `docs/writing-a-node-type.md#the-node-handle`, `#loops`). Slice 6 (parameters: decimal, indexed and live rows, parameter slots and their ramps, retunes,
gestures, the `gain` type, the editor's faders) follows slice 3d (built: the plan's outcomes, `docs/architecture.md#restarts-and-changes-of-settings`, and `node.set`,
`docs/control-port.md#nodeset-and-dry-runs`)
and slice 5. The two are designed together because they share one type schema, one write path for what lasts (the graph
file keeper), one wire vocabulary, one family of realtime channels and one editor model.

What is built and not repeated here (`docs/control-port.md`, `docs/output-records.md`): protocol 1 with `protocol` and
`features` in the graph and every enum open; feature `lifecycle` (health on `node.state`, snapshots and `node.added`:
`state`, `phase`, `since`, `wanted`, `blockLost`, `cause`, `waitingFor`, `reason`; `disabled` on a node;
`node.start`, `node.stop`, `node.restart`, `node.disable`, `node.enable`; `node-disabled`, `not-restartable`;
`node.gone` beside `node.stopped`; `applied` on `node.updated`); `data.pointer` on every -32602; the `node`, `action`
(for the lifecycle verbs), `diagnostic` (code, fields, detail) and `still` records; `rpc-call.mjs --until`. Companions on
the wire are built (`docs/control-port.md#companions`), `node.set`, `dryRun`, `dropRoutes` and the reconfigure outcomes are built
(`docs/control-port.md#nodeset-and-dry-runs`, `#changes-of-ports`), recovery's members slice 8's (`recovery.md`).

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
graph {
    rate 48khz;
    block_size 256;
}
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
feature that adds them (ear's decoders refuse an unknown key, -32602 with `data.pointer`). Step 5.1 built the feature
`types` ([control-port.md](../control-port.md#type-schemas)) and step 5.2 the feature `status` (status, events, node
diagnostics: [control-port.md](../control-port.md#status)); these slices add the features `actions` (5.4), `live` (6.3: decimal,
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
| `gesture.begin` | `gestures` | `{"name", "key"}` | `{"gesture", "value", "reach", "min", "max", "decimals"}` | `unknown-node`, `not-live`, `shared-definition`, `parameter-held`, `file-invalid`, `file-refused`, `stopping` | control thread, then the main loop (7.2) |
| `gesture.change` | `gestures` | `{"gesture", "seq", "value"}` | `{}` if sent with an id | `unknown-gesture`, `invalid-settings`, `not-in-batch` | control thread, then the main loop directly |
| `gesture.end` | `gestures` | `{"gesture", "value"?}` | `{"value"}`: what was committed | `unknown-gesture`, `invalid-settings` (the gesture stays open), `stopping`, the desired graph's (the gesture is aborted) | keeper |
| `gesture.cancel` | `gestures` | `{"gesture"}` | `{}` if sent with an id | `unknown-gesture`, `not-in-batch` | control thread, then the main loop directly |
| `node.invoke` | `actions` | `{"name", "action", "args"?}` | `{}` | `unknown-node`, `unknown-action`, `invalid-args`, `not-running`, `restarted`, `stopping`, `action-failed`, `action-timeout` | keeper, then the node's loop |

`node.set` and `node.update` (3d) gain `parameter-held` and the outcome `retune` from feature `live`.

**`types.get`** is built ([control-port.md](../control-port.md#type-schemas)), with each type's `status` and `events`
(5.2). Later steps add to each type `actions` (5.4), and to rows the kind `decimal` with `decimals`, `taper` (`linear`, `log`, `db`, 8.3)
and `minIsSilence`, `live` (`{"smoothingMs"}`, with `change` `live`) and `indexed` (`{"by": <the count row's key>,
"from": 1}`) (slice 6), as this example of slice 6's `gain` shows:

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

- An action: `name`, `by` (`type`; open), `target` (`code` for a trigger, `loop` for a call), `help`, `args` (setting
  rows).

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

`graph.get` and `graph.subscribe` carry, per node, `status` and `diagnostics` (built:
[control-port.md](../control-port.md#status)), and gain `gestures` (the open gestures moving one of this node's keys,
with their live value; always present, in `node.added` too).

| Notification | Params | Rate |
|---|---|---|
| `gesture.changed` | `{"gesture","phase", and by phase: "reach","value","reason","code","key","detail"}` | `change`: at most 30 a second per gesture |

```json
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

**Order**, added to the built rules (status before the event it precedes and the reset are built:
[control-port.md](../control-port.md#order)):

3. A gesture's begin: `gesture.changed` `begin` to the others, then the reply, in one turn. Its end: `graphFile.changed`,
   `node.updated` (`applied: "retune"`), `gesture.changed` `end`, the reply.
4. An abort by a related save: `gesture.changed` `aborted` for each related gesture, then `graphFile.changed`, then the
   change's graph events; the slots settle last, in the same turn.

### 4.4 What a client folds

| Notification | Changes | Ignored when |
|---|---|---|
| `gesture.changed` | `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value` wherever the id is; `end` and `aborted` remove it everywhere; `tick-refused` changes nothing | no node holds the id (except `begin`) |

### 4.5 Error codes

All -32000 with `data.code`; every new code is a case in `test/protocol/errors.json`.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-settings` (extended) | a value a row or the type's `check` refuses, from `Gestures` too | `key` (an indexed member's full key, `trimDb.2`), `detail`, as built ([control-port.md](../control-port.md#error-codes)) |
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


### 4.6 Node diagnostics on the wire

Built ([control-port.md](../control-port.md#node-diagnostics), [output-records.md](../output-records.md#diagnostic)):
`node.diagnostic`, kept per code and subject with `active` and `count`, and `detail` beside `msg`. Left:
`ear.records-dropped` gains subject `realtime` (5.3), and `ear.realtime-overrun` (`codeUs`, `cycleUs`) stays reserved.
Slice 8 adds `ear.health-flapping`, `ear.recovery-attempt` and `ear.readiness-missed` in the same shape
(`recovery.md`).

### 4.7 Fixtures

| File | Covers |
|---|---|
| `test/protocol/gestures.json` | begin (no echo to the owner, `begin` to another first, then the reply), changes (one stale, one keep-alive), end with its order; a reach of two keys and of 64; a reach refused `shared-definition`; `parameter-held`; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings` leaving the gesture open; aborts by a related hand edit and a related `graphFile.set`; an unrelated hand edit and `graphFile.set` leaving it open; ends for a closed and a silent session; `unknown-gesture` after an abort; `not-in-batch` |
| `test/protocol/actions.json` | a trigger; a call; `not-running` with its `phase`; every refusal an action can get |
| `unknown-values.json`, `errors.json`, `snapshot.json`, `edits.json`, `output-records.json` | extended with this page's members, codes and records |

## 5. The C++ SPI

Everything extends `type_spec.hpp`, `settings_table.hpp`, `lifecycle.hpp` and `node_type.hpp`; nothing replaces them.
Every new constant is `inline constexpr` in the type's named namespace, and every new template keyed on a constant
refuses one in an unnamed namespace (`linkage::named`, as built).

### 5.1 New row kinds: decimal, indexed, live

`.shapesPorts()`, `.defaultFrom(DefaultFrom)` and a text row's hints (`TextHints`) are built
([writing-a-node-type.md](../writing-a-node-type.md#settings-one-table-of-typed-rows)), and so is `NodeType::schema()`
([#the-type-schema](../writing-a-node-type.md#the-type-schema)).

```cpp
namespace ear::setting {
enum class Taper { linear, log, db };            // how a control maps its travel (8.3); log also ramps geometrically
struct DecimalHints {
	std::uint8_t decimals = 2;                       // display precision, and what a gesture's end rounds to
	Taper taper = Taper::linear;                     // log: low > 0; db: low <= -60 and high >= 0 (else no compile)
	bool minIsSilence = false;                       // the control shows `low` as -inf
};
/// A real number in [low, high]: a JSON number (an integer is taken too), never NaN. Otherwise
/// "<key>: must be a number[ of <unit>] from <low> to <high>". low above high, or a non-finite bound, does not compile.
consteval Rule<double> decimal(double low, double high, std::string_view unit = {}, DecimalHints hints = {});
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
- An indexed family's count row is `.shapesPorts()` only when `ports()` reads it.
- `SettingRow` gains `liveMs`, `indexedBy` and a `DecimalRule`; `SettingKind` gains `decimal`; `SettingDeclaration`
  gains `live` and `indexed`.
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

Built: the declarations, `StatusTable` and the spec's `.status`
([writing-a-node-type.md](../writing-a-node-type.md#status-fields)). The tables take `const auto&...`, as
`SettingsTable` does, since their entries hold views. The realtime writer's side is built too
([writing-a-node-type.md](../writing-a-node-type.md#reporting-from-the-code)). What is left is `writer::companion`
(slice 8).

### 5.3 Events, log codes, `Report<spec>`

```cpp
namespace ear {
enum class EventSeverity { info, warn, error };
// event::of, arg::*, log::code, EventTable and LogCodes are built (docs/writing-a-node-type.md#events).

// Report<spec> and NodeOf::report() are built (docs/writing-a-node-type.md#report).
}
```

- `TypeSpec` gains `.actions` between `.events` and `.logCodes` (the rest are built:
  [writing-a-node-type.md](../writing-a-node-type.md#the-spec)).
- `NodeHandle::say` stays for `sendspin` until step 5.5 removes it.

### 5.4 What `process` gets: `Io<spec>` grows

```cpp
template <const auto& Spec> class Io {
public:
	// built: operator[](bank), frames(), set, raise, log (docs/writing-a-node-type.md#reporting-from-the-code)
	template <const auto& Row> Smoothed param() const noexcept;                         // a live, non-indexed row
	template <const auto& Row> Smoothed param(std::size_t member) const noexcept;       // a live indexed row, from 0
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
- `io.triggered` compares the trigger's counter with the last the code saw. Every handle resolves to an index when the
  code is prepared, as banks do.

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

- **Per configuration** (made when `configure` is called from empty, freed at teardown step 5), beside the realtime
  channels, which are built and bound this way ([architecture.md](../architecture.md#the-realtime-channels)): one
  `std::atomic<double>` slot per live member, one `std::atomic<std::uint32_t>` per trigger, each live member's ramp
  state. All of it is the node's, not the code's: the code is bound to it when prepared, and a code an in-place change
  swaps in is bound to the same objects before its barrier. A stop, a start and an in-place change keep all of it; a
  reshape in place resizes an indexed family's slots (survivors keep their ramps, new members start at the committed
  value, vanished ones are dropped after the swap's barrier).
- **Per node**: the status view, the live value of each held key (seeded into every new configuration's slots, so a
  restart mid-gesture keeps the hand's value), the node diagnostics.
- **Erased for the rest of ear**: `NodeType::schema()` returns a plain `TypeSchema` (core, nlohmann only), encoded once
  by `ear_control`. `NodeConfig::liveValues()` gives each live key's checked value, for retunes.
  `NodeType::checkLive(config, key, value)` runs the row's rule then the type's `check` with that value in place.
- **New core interfaces**, implemented outside core: `RealtimeBell` (built in core,
  [architecture.md](../architecture.md#the-realtime-channels); left: its `ear_pipewire` side, a `pw_loop` event on the
  main loop), `NodeStatusListener` (`statusChanged`, `statusReset`, `eventRaised`), `NodeDiagnosticListener`
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
		return cx.applied(GainCode{});   // no change version: its live rows retune, `channels` restarts
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
`param-row-not-in-spec`, `param-indexed-without-member`, `param-member-of-plain-row`, `call-loop-not-in-spec`,
`triggered-not-a-trigger`, `action-named-lifecycle` (the status and event declarations' cases are built,
`src/core/tests/compile_fail/status_spec_bad.cpp`, and so are `Io`'s and `Report`'s,
`src/core/tests/compile_fail/realtime_bad.cpp`).

### 5.9 The harness

`TypeHarness` (built over a real `NodeHost`; 3c adds loops and its clock; the status and event captures and the
realtime bell's count are built: [writing-a-node-type.md](../writing-a-node-type.md#its-own-tests)) gains, on the test's
thread:

| Call | Does |
|---|---|
| `h.live<row>(value)`, `h.live<row>(member, value)` | moves the slot, as a gesture tick (runs `checkLive`, failing the test on a refusal) |
| `h.retune(settings)` | a change of live rows only: slots move, no restart (asserts the plan would retune) |
| `h.trigger<action>()`, `h.invoke<action>(args)` | a trigger; a call, on the harness's loops |
| `h.outcome(settings)` | the plan's outcome of a change, without applying it |

The all-type tests gain (the schema's checks are built: [writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)):
every live row ramps from its default to another value with no sample unwritten (that every status field's initial value
is of its kind or null, and that the status view survives every in-place change, are built).

## 6. The realtime contract

### 6.1 Channels into and out of the realtime thread

| Channel | Direction | Mechanism | Bound | On overflow |
|---|---|---|---|---|
| Parameter slot | in | one `std::atomic<double>` per live member, written by the main loop, loaded once a block | 256 live members per node | — |
| Trigger | in | one `std::atomic<std::uint32_t>` counter per trigger | — | several invokes in one block are one |

The outward channels (`rt` fields, the ring, the bell) are built
([architecture.md](../architecture.md#the-realtime-channels)); `rt` fields are stored as 64-bit words, so only the
parameter slots need `std::atomic<double>`, lock-free on x86-64 and arm64; a `static_assert` on `is_always_lock_free`
makes it a build failure elsewhere [U: no arm64 build yet].

It loads each live slot once a block and advances each ramp, before `process`. It never allocates, locks, formats, or
makes a syscall.

### 6.2 Off the realtime thread: one queue per node

Built ([architecture.md](../architecture.md#status-and-events)).

### 6.3 The drain and the tick, on the main loop

Built for the queue and the realtime channels ([architecture.md](../architecture.md#status-and-events),
[#the-realtime-channels](../architecture.md#the-realtime-channels)). Left: a gesture tick taken marks a node dirty too;
one `gesture.changed` `change` per gesture moved.

### 6.4 Rates and bounds

| What | Bound | Policy |
|---|---|---|
| The tick, `status` records, events and flapping | built | [architecture.md](../architecture.md#rates-and-bounds) |
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

The outward channels (`rt` words and counters, the ring, the queue, the `Report`) are built across the lifecycle this
way ([architecture.md](../architecture.md#the-realtime-channels)); what is left is the slots' and triggers':

| Moment | What happens to the slots and triggers |
|---|---|
| `configure` from empty | made: slots seeded from the committed values or the node's held live values, trigger counters |
| configured | the code is prepared and bound to them; nothing runs on the realtime thread yet |
| the code drives | the first block starts every ramp at its slot's value; then `start()` |
| a retune, in any phase | the slots move at once, wait-free; with no configuration only the node's live value moves and seeds the next one; while `stopped` the slots move and the code ramps from them at the next start |
| an in-place change (3d) | the code keeps running and ramping |
| a code swap (3d) | the new code is bound to the same slots and counters; each ramp goes on, an unseen trigger is seen once by the new code |
| a stop | the code stops; the slots stay |
| teardown step 5 | freed, after the code and the loops' states |

No channel is allocated, freed or resized on the realtime thread or while the code that uses it can run. One node's
transition never touches another's channels (the owner, 2026-11-08: "it should be per-node. Some node
stopping/reconfiguring/failing does not effect the graph globally"). A companion writes no status, events or records.

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
(beside the built outcomes: `docs/architecture.md#restarts-and-changes-of-settings`). It reaches `NodeHost` through the built `nodeChanged` with outcome `retune` and
the changed keys; `NodeHost` keeps the new config for later configurations and writes each key's slot from
`config->liveValues()`, unless a gesture holds the key, in any phase. So a hand edit, `node.update` or `node.set` that
changes only `gainDb` retunes: the audio ramps, no `node.state` follows, and standard output has a `param` record. A
loop's state and companions are not told of a retune: a value that must reach the loop's state is an `.inPlace()` row.

| Row | `node.set` | A gesture | Node `running` or `stopped` | Node `reconfiguring` | Not yet rested | `failed`, `recovering` |
|---|---|---|---|---|---|---|
| **live** | `retune`: slots move at once | yes | ramps; no `node.state` | ramps at once; the change goes on | the node's live value moves and seeds the next configuration | the same; no restart, no recovery skip |
| **in place** | `reconfigure` | refused `not-live` (X5) | built: a change in place | built: supersedes it, or waits after its commit point | a restart | a restart at once |
| **restart-only** | `restart` | refused `not-live` | built | built: a restart | built | built |

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

The node panel this section names is the inspector's Node subject
([editor.md](../editor.md#the-inspector)); its settings form and status list go there, under the lifecycle bar.

### 8.1 Stores

| Module | Holds | Tested by |
|---|---|---|
| `protocol.ts` | the wire types of section 4; `ControlErrorCode`; `NodeDiagnostic`, `EventSeverity` | typecheck |
| `graph.ts` | each node's `status`, `gestures`, `diagnostics`; handlers for the new notifications | vitest, every fixture |
| `gesture.ts` (new, pure) | the session's own gestures as a state machine; what to send for each input; the control mapping | vitest, a fake client |
| `activity.ts` (new) | per node, the last 100 events and diagnostics since connecting; notices, coalesced per node and code | vitest |

`types.ts` and `form.ts` are built ([editor.md](../editor.md#the-settings-form)). Features gate the UI: without
`gestures` live rows render as plain number fields committed by `node.set`. Nothing in `web/` names a node type.

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

The settings form is built for today's kinds, with its hints, drafts, refusals in place and Apply
([editor.md](../editor.md#the-settings-form)); every non-live field is sent by Apply. Slice 6 adds:

| Row | Control | Commits |
|---|---|---|
| decimal | a number field with `decimals`, unit | by Apply |
| live decimal | a fader in the panel, mapped by `taper`, with a number field beside it | a gesture; the field by `node.set` |
| live whole number | steppers and a number field (it steps) | `node.set` |
| indexed family | one control per member, `n` from the count row's value; above 8, a compact strip behind a disclosure | as its kind |

- **The box** shows a knob for each live, non-indexed row when the type declares one or two of them (`gain` shows
  `gainDb`).
- Live controls follow the server, except one the operator holds. They are disabled while the graph file has errors or
  was refused, and while the connection is not open.

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
  deletes the node. React Flow 12.12.0's own classes are `nodrag`, `nowheel` and `nopan`, and its key filter skips
  input, select, textarea, contenteditable and anything inside `.nokey` ([editor.md](../editor.md#shortcuts)).
- Another session's gesture: the control moves, is disabled, and says "held by another editor".
- Notices for its own gesture's abort: `hand-edit` "ear.conf was edited by hand and changed amp gainDb while you held
  it; it now says -10." [Set -3.5 again]; `file-saved` the same from the text pane; `node-removed` "amp was removed or
  replaced while you held gainDb."; `key-removed` "amp2 now has 2 channels, so trimDb.3, which you held, is gone.";
  `refused` the end's refusal message. An `idle` end: "Your change to amp gainDb was kept at -3.5 after 30 s without a
  word from this editor." A socket lost mid-drag: "Connection lost while you held amp gainDb; ear kept -3.5."; after a
  1001 close: "ear restarted while you held amp gainDb; your change was not kept: it is -6 again."

### 8.4 Status, events, diagnostics and actions

- Status, events and diagnostics are built ([editor.md](../editor.md#status-events-and-diagnostics)).
- **Actions**: one button per type action after the built lifecycle buttons; a call with arguments opens a small form;
  disabled while in flight and when the node has no configuration.

### 8.5 Accessibility

Faders and knobs are `role="slider"` with `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, `aria-valuetext` ("-3.5
dB", "-inf dB") and a label from the key, keyboard per the WAI-ARIA slider pattern; held by another editor:
`aria-disabled` with the reason. A polite live region announces aborted gestures (callouts are built as one; flapping and events reach the toast
queue's). Flapping is never shown by colour alone.

### 8.6 Apply asks first

Built ([editor.md](../editor.md#the-settings-form)).

## 9. Standard-output records

New kinds, each with `v`, `seq`, `t`, `kind`, its fields, `msg`, as built. Every change is additive, so `v` stays 1.

```json
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
{"v":1,"seq":128,"t":"2026-11-08T10:05:20.770000Z","kind":"action","node":"kitchen","action":"forget-pairing","result":"action-failed","detail":"no pairing with ma-0000","msg":"node kitchen forget-pairing refused: no pairing with ma-0000"}
```

- `status`, `event`, and the diagnostic's `subject`, `active` and `count` are built
  ([output-records.md](../output-records.md#status)), and so is a realtime event's cycle start, carried as the wire's
`t` ([control-port.md](../control-port.md#status)).
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

**5.1 The schema on the wire** is built ([control-port.md](../control-port.md#type-schemas),
[writing-a-node-type.md](../writing-a-node-type.md#the-type-schema), [editor.md](../editor.md#the-settings-form)).

**5.2 Status and events off the realtime thread.** `StatusTable`, `EventTable`, `LogCodes` in `TypeSpec`; `Report` and
`NodeOf::report`; the per-node queue, the drain and the tick; `node.statusChanged` with its reset, `node.eventRaised`,
the `status` and `event` records; `status` in snapshots and `node.added`; `node.diagnostic` with subject, `active` and
`count`; flapping, `ear.events-dropped`, `ear.records-dropped`, and 3b's, 3c's and 3d's diagnostics on the wire; the
harness's captures; `rpc-call.mjs --follow`. Tests: `ear_core_tests` with a test type (status before event, a `lost`
after the last status, a closed configuration's write dropped, the reset after the last values, status kept through a
stop and a start, the queue's bound, flapping and settling on the harness's clock, the tick's 33 ms cap, the trailing
record within 1 s and matching the view); `status.json`, `diagnostics.json`; the editor's status list and badges; a
system test of ten chatty nodes and a slowly reading client.

**5.3 The realtime ring** is built in core ([architecture.md](../architecture.md#the-realtime-channels),
[writing-a-node-type.md](../writing-a-node-type.md#reporting-from-the-code)). Left: the bell in `ear_pipewire`, a
`pw_loop` event on the main loop that tells NodeHost, and `main.cpp`'s wiring. Tests: `ear_pipewire_tests` (a test
block's code sets a field and raises an event, and both are drained; an idle block rings nothing).

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
- [U] Aggregate deduction of `TypeSpec` with designated initialisers and defaulted parameters on GCC 14: it holds for
  `.stores` and `.loops` (every subset spelling compiles, `type_spec_test.cpp`) and is still to check for this page's
  members; fallback a consteval `makeSpec` with the same members in the same order.
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
| A base class with virtual `onParam`, `onAction` (JUCE, VST3) | nothing to override: a type declares, so it writes only what is its own (the owner, 2026-11-07: "what are the core primitives that ear should host so that new nodes can be added simply and only require defining the truly custom bits that its node type demands") |
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
| X5 | A fader only on `.live` rows | A drag over an in-place row would chop the audio into holds |
| X6 | Values applied off the realtime thread are `.inPlace()` rows the type posts to its loop; no `watch` hook until a type needs one | Covers every built-in need with 3d's machinery |
| W1 (built) | One `node.state` whenever any health member changes | — |
| new | A type bug found at run time (a choice value not among its values, a rule broken) fails the node with the rule's text, never throws out of the framework | As built in 3a for every context rule |
