# The control plane and parameters: one schema, one write path, one wire

Status: proposal, revision 6, 2026-11-08. Revision 1 went to three independent reviews and revision 2 answered them
(section 16). Revision 3 rebased the page onto `docs/design/node-framework.md` revision 5 and was reconciled with that
page's review round 3. Revision 4 rebases it onto node-framework.md revision 6, which took the owner's answers of
2026-11-08 (its R19 to R24): **there is no run**. The graph always runs; operations on a node (add, configure, start,
stop, remove, wire) are asynchronous and ordered, and each reply says what was applied; a node's states are `added`,
`configuring`, `configured`, `starting`, `running`, `reconfiguring`, `stopping`, `stopped`, `failed`, `recovering`
and `removed`; a type writes `configure` (in two versions since revision 6), `start()` and `stop()`. So this page drops the `run` number from
the wire and the records (the framework drops a closed configuration's late messages itself), adds `node.start` and
`node.stop` and the `wanted` member (4.2, 4.8), restates health for the new states (4.8), status as the node's
configuration's (4.3), and the editor and records in each state (8.6, 9.1). It also re-sorts its gate questions to the
owner's new practice (section 14): the owner is asked only what an operator sees; every technical question is decided by
the designer, with its reason. Revision 5 takes the owner's answers of 2026-11-08 to all eight gates (section 1, R25 to
R32; 14.1): two resting states, `stopped` (Stop and Start: keeps everything, never written, forgotten at a restart) and
`disabled` (Disable and Enable: releases everything, `disabled true;` inside the node's braces in `ear.conf`; revision
5.1, R29), with `node.disable` and
`node.enable` beside `node.start` and `node.stop`, which no longer edit `ear.conf` (4.2, 4.8, 7.6, 8.6); no "held"
marks, but a live meter on every port, inputs and outputs, so audio can be followed through the graph (8.6); a save
of `ear.conf` stops a fader only when it changes the dragged value or its line (7.3); a fader whose browser
disconnects or goes silent is ended as released, keeping its last value (7.3); X2, G1 and C2 as recommended.
Revisions 4 to 5.1 went to node-framework.md's review round 4, and revision 6 answers it with that page's revision 8:
one register of the owner's words (the framework page's numbers; this page's R29 to R32 became R30, R31, R32 and
R29); `node.restart` beside `node.start` and `node.stop` under feature `lifecycle`, from step L1, and `restart` no
longer a type action (4.2, 7.6); `node.state` whenever any health member changes (4.3, 4.8); Disable and Enable
editing only the node's own `disabled` line (the owner's R34; 4.2); no `run` left on the wire or in a fixture (4.7,
`output-records.json`); `node.stopped` sent beside `node.gone` within protocol 1 (4.8); `starting` until a node rests
(4.8, 8.6); gestures across a reconnect and a restart of ear (8.3); new framework diagnostics for the main loop and the
data loop (4.6).
Nothing here is built. Layer: module design for slices 5 (control plane) and 6 (parameters) of node-framework.md
(revision 8), beneath that page, and the wire and editor side of its slices 3a, 3b, 3d and 8 (section 11 says which
step lands with which slice). It refines that page's sections 6.2, 6.3, 6.6, 6.7 (on the wire), 8.1 to 8.7, 9.2 where
parameters touch the audio contract, 9.6 (what clients see of companions), and section 13's slices; the table "What
this page changes" below says exactly where. It also lands slice 4's deferred node-facing primitives (8.8's erratum:
event calls on the node handle, and the realtime coded ring). It changes the wire of `docs/design/batch2.md` section 6
additively (protocol 0 to 1, section 4.1), and the standard-output schema of `test/protocol/output-records.json`.

[V] = verified in the source named, as of `f677a50` (revision 3's and 4's new citations as of the working tree on
2026-11-08, `3adc02b`); [U] = unverified, with the step (section 11) whose test checks it, or section 15 keeps it open.
knobly's citations (`/opt/knobly/...`) were checked in the dev image.

## Scope, and what this page changes

Slices 5 and 6 are designed together because they share six things: one type schema (a parameter is a settings row
marked `.live`), one write path for what lasts (the graph file keeper: a gesture commits at its end into `ear.conf`, and
a related hand edit aborts it; actions, start, stop and restart are ordered by the keeper too, and disable and
enable are edits), one wire vocabulary and protocol
number, one line drawn between parameter, status and event, one family of realtime channels (wait-free parameter slots
into the audio thread, the realtime coded ring out of it), and one editor model that renders controls, status and
diagnostics from the schema.

**The boundary with the lifecycle** (node-framework.md section 6, its one home). That page owns the graph's operations,
the states, the transitions T1 to T22, the three hooks and their contexts (its 6.4 and 6.10: `configure` in two
versions, `start()`, `stop()`, `CreateOf`, `ChangeOf`, `NodeOf`), the plan's outcomes (`unchanged`, `retune`, `reconfigure`, `restart`,
`reshape`, `replace`, its 6.6), the rows' change marks (`.inPlace()`, `.live`), holds, reshape (6.7), companions (9.6),
the wanted state (its 6.3: `disabled` in `ear.conf`, the node's stop in memory, R25 to R28), and each state's `node`
record `msg` (6.2's table). This page owns how all of that reaches clients and people: `node.state`'s members for each
state (4.8), `node.start`, `node.stop`, `node.disable` and `node.enable` (4.2),
`applied`, `ports`, `dryRun` and the routes a reshape drops (4.9), the companions' view and notification (4.10), the
type schema's per-row `change` (4.2), the editor in each state (8.6), the `node` record's fields (9.1), and
`node.restart`'s reply by phase (7.6).

**The boundary with slice 3c** (`docs/design/node-primitives.md`, revision 6, rewritten in the same change; its section
14 states the same boundary from its side). Slice 3c owns loops and their states (strands, dedicated), wakes, the I/O
executor, stores, claims, recordings, and **the node handle**: `NodeHandle` (its section 5), shared-owned and bound to
the node's current configuration by an internal generation (node-framework.md I6), with `node()`, `lost`, `log` and an
interim paced `say`, reached by `node.handle()` on the instance's `NodeOf` (the owner's G17). Audio crosses to
non-realtime code only through companions, which slice 3b builds, and the instance or a loop's state owns its
mailboxes. This page adds to the node handle and reshapes none of it: the typed `Report<spec>` over it (status writes
and events), the realtime ring and its bell, parameter slots, `Smoothed`, triggers, actions, and every wire and keeper
change; it removes `say` once sendspin's notes are split. Action calls and `writer::loop` fields run on loops, which
slice 3c declares in the spec as handles (the owner's G14). So slice 5 is built after 3c, and slice 6 after 3d
(node-framework.md 13.1, D-D1; G6 here, decided). Where two pages name one thing, one page is its home and the other
cites it: `TypeSpec`'s whole member list and order is node-primitives.md 6.13; slice 3c's diagnostic codes and their
texts are node-primitives.md's (6.4, 6.9, 6.10), put on the wire by 4.6 here; the realtime calls (`io.set`,
`io.raise`, `io.log`) and the ring the framework makes with each configuration are this page's (5.4, 6.1).

| node-framework.md | What it says | What this page decides |
|---|---|---|
| 6.2, health on the wire | `node.state` carries `state` (a short set) and `phase` (the exact state, with its cause) | every member of `node.state` per state, with `wanted`, `cause`, `keys` and slice 8's recovery members, and no run (no `holds`: the meters show a hold, W3); one notification whenever any health member changes (W1); `reason` kept with today's meaning, so no protocol bump (4.8; W1, W2) |
| 6.2, standard output | a `node` record per change of the short state (`starting`, `running`, `reconfiguring`, `stopping`, `stopped`, `disabled`, `failed`, `recovering`) and `removed`; none for the phases inside `starting` | the records' fields (`phase`, `cause`, `keys`, `afterMs`, no run); one more record, `still <phase>`, when a node stays in `added`, `configuring`, `configured` or `starting` past 2 s (9.1; W4) |
| 6.3, start and stop; disable and enable | `stop` and `start` set the node's stop, held in memory and never written; `disable` and `enable` write and remove `disabled true;` inside the node's braces in `ear.conf` (R25 to R28, R29) | `node.start`, `node.stop` and `node.restart`, ordered by the keeper but writing nothing, with their own replies; `node.disable` and `node.enable`, edits at the source like `node.update`; `start` on a failed node a retry; `start`, `stop` and `restart` refused `node-disabled` on a disabled node (4.2, 4.5, 7.6) |
| 6.6, the plan's outcome | `node.updated` and the edit's reply carry `applied` | `applied` is the plan's outcome, never what the node later did (that is `node.state`'s `cause`); `dryRun` on `node.set` answers the outcome without committing; the schema says per row how a change applies (`change`, `shapes`) (4.2, 4.9; X1, X3) |
| 6.6, 6.7, reshape | a reshape is in place when every changed key is `.inPlace()` or `.live`; a plan keeping a route to a vanishing port is refused | `inPlace` on the reshape's `applied`; through the control port refused `ports-in-use`, naming every such route, and `dropRoutes: true` removes their lines in the same commit (4.9; X2) |
| 6.3, 6.6, 8.4, a change, `stop` or `restart` during an in-place change | a newer change, `stop` or removal supersedes one before its commit point and waits after it; `restart` ends it at once (its D-B6, which absorbed this page's X4) | what a client sees of each: `node.state`'s `keys` replaced on a supersession, `next` for keys waiting after the commit point, `restart`'s reply (4.8, 7.6, 7.7) |
| 9.6, what clients see of companions | `companions: [{name, kind, bank, ready, dropped \| shortfall}]` under each node; badges in the box | that list in snapshots and `node.added`, a `node.companionsChanged` notification, counters sampled once a second, `retiring` during a make-before-break, the framework's `ear.feedShortfall` and `ear.handoffDropped` status counters gone, two observation diagnostics (4.10, 8.7; K1, K2) |
| 8.1 table, Parameters row | request `node.setParam`; notification `node.paramChanged`; stdout at commit only | requests `node.set` (a merge of named keys, for any row) and `gesture.begin`, `gesture.change`, `gesture.end`, `gesture.cancel`; one notification per gesture, `gesture.changed`, never echoed to the gesture's own session for `begin` and `change`; a `param` record on stdout for each committed live change and each abort (4.3, 9) |
| 8.1 table, Actions row | request `node.action`; nothing on stdout | request `node.invoke`; an `action` record on stdout (4.3, 9) |
| 8.1, status on stdout | every change | every change, paced per node and field, with the field's last value always written (6.5, 9; G9) |
| 8.2, text fields | written under a mutex per field | every off-realtime write goes into one bounded queue per node's configuration, under one mutex, drained by the tick; one FIFO per writer gives 8.1's "an event first flushes the status" for free (6.3) |
| 8.3, diagnostics | the framework's, per code | kept per code and **subject** (a field, a loop, a claim), each with `active` and the server's `count` (4.3, 4.6) |
| 8.4 | the handler's result is the reply | the reply of a batch holding an action waits for the handler (deadline 10 s), but the keeper never waits (7.6); `restart` is `node.restart`, a lifecycle request under feature `lifecycle` (not a type action): a stop, a configuration from empty and a start back to the wanted state, replying `{applied}` once the teardown began, refused `not-restartable` on a lost block or before the node was first configured, and `node-disabled` on a disabled node (W12); `disable` and `enable` are edits, `start` and `stop` are not (4.2); a second kind of action, a **trigger**, goes to the node's code wait-free (5.5) |
| 8.5 | one method with a `gesture` field; `begin` refused when the row is on a line that makes other nodes | separate `gesture.*` methods; `begin` answered on the main loop from a **reach index** the keeper posts with each version (7.2); a gesture's **reach** is every node and key whose value is defined at the same place, all live and of one kind and unit, and the gesture moves them all (G1); a save aborts a gesture only when it changes the dragged value, its defining line or its reach (G3, 7.3); a session that closes or goes silent ends its gestures as released, keeping the last value (G4); the committed value is rounded to the reach's `decimals` (G5) |
| 8.5, hand edit of a live row | moves the slot without a restart | the `retune` outcome of node-framework.md 6.6's `nodeChanged`, whose default calls `nodeUpdated`, so `NodeHost` retunes and every other listener is unchanged (7.4); a retune never waits for a reconfigure or a restart in flight (7.7) |
| 8.5, a gesture on a row that is not live | — | refused `not-live` whatever the row's mark: an `.inPlace()` row is edited by `node.set` and reconfigures once, never per tick (7.7; X5) |
| 8.7 | `graph.get` gains `typeSchemaHashes`, a hash per type | one opaque hash for the whole set (`typeSchemaHash`); types are compiled in, so they change only with ear (4.2) |
| 8.7 | a higher `protocol` shows a banner | an absent `protocol` is 0 (today's wire), this page is 1; additive growth is announced in `features`, which clients gate each method on (4.1, G7) |
| 9.2 | `io.param(gainRow)` | `io.param<gainRow>()`, as `s.get<row>()` reads a row today; `Io` also gains `set`, `raise`, `log` and `triggered` (5.4); the ramp's rule is fixed with a worked vector (5.4) |
| 13, slices 3a, 3b, 3d, 5, 6 and 8 | the lifecycle's wire in 3a, companions in 3b, `applied` and `ports` in 3d; slices 5 and 6 one paragraph each | thirteen buildable steps: L1, L2 and L3 land with 3a, 3b and 3d, 5.1 to 5.5 and 6.1 to 6.5 as before, R8 with slice 8 (section 11; O1) |
| 13.3, where each page is the home | this page is the home of `node.state`'s members, `applied`, `inPlace`, `dryRun`, `dropRoutes`, companions on the wire, every method, error and diagnostic code, `.shapesPorts()`'s build, `restart`'s reply and the `node` records | as listed there |

## 1. The owner's words

| # | The owner's words | Date, where | What it means here |
|---|---|---|---|
| R2 | "consider how to abstract configuration and control plane events so that nodes can build on a shared base primitive layer" | 2026-11-07, chat (node-framework.md R2) | Status, events, actions and parameters are framework subsystems; a type declares them and writes no plumbing |
| R9 | "Nodes should not have direct access to stdout or stderr. The core framework should expose the necessary primitives for events, diagnostics, and logging. These should be real-time safe for any audio loops that need them, and there can be non real-time bases for the rest of the system." | 2026-11-07, chat (node-framework.md R9) | `Report<spec>` off the realtime thread, `Io<spec>`'s coded calls on it (5.3, 5.4) |
| R10 | "Would status changes be useful for debugging things like flapping? Does this status relate to diagnostic events?" | 2026-11-07, chat (node-framework.md R10) | Status is a view and a record of every change; flapping is the framework's diagnostic (6.3, 6.5) |
| Q2 | "View + change record" | 2026-11-07, chat (node-framework.md section 14) | The live view on the wire, coalesced; changes as `status` records on stdout, paced so the last value of each field is always written (G9) |
| Q6 | "Commit at gesture end" | 2026-11-07, chat (node-framework.md section 14) | Ticks move the audio; only `gesture.end` writes `ear.conf` (section 7) |
| Q9 | "JSON with a msg field" | 2026-11-07, chat (node-framework.md section 14) | Every new stdout record carries `msg` (section 9) |
| P1 | "3 seems like a standalone unit, while 5 and 6 need to be designed together." | 2026-11-08, chat | One page for slices 5 and 6; slice 3 is its own (scope, above) |
| P2 | "Let's start the design and architecture phase for both. I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08, chat | Journeys first (section 2); every request, record and declaration shown whole (sections 4, 5, 9); how a control maps a pointer is specified, not left to the builder (8.3) |
| R11 | "a Run should not be bounded by config changes. I want config and graph changes to be supported at runtime while audio is running" | 2026-11-08, chat (node-framework.md R11) | An edit's reply says what it did to the node (`applied`), a client can ask before committing (`dryRun`), and a node's status, slots and ring survive an in-place change, a stop and a start (4.9, 5.6, 6.7) |
| R12 | "the start+build model seems overly simplistic. I think each node deserves a dedicated state machine, and hooks for each viable state. At a minimum, there should be setup+build+start" | 2026-11-08, chat (node-framework.md R12) | Every state reaches the wire and the editor with its cause (4.8, 8.6) |
| R13 | "mailboxes could be supportable in either mode. But why must mailboxes be built before the run object exists? Is this a side effect of the naive node state machine model I described?" | 2026-11-08, chat (node-framework.md R13) | The answer agreed with the owner: yes; the instance owns its state. Here: the per-node channels are made before the code first runs and outlive it (6.7) |
| R14 | "G4 is asking the wrong question. The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives to connect inputs/outputs across nodes AND anytime a node needs to cross inputs/outputs to non-realtime code (eg pipe outputs through a mailbox?)" | 2026-11-08, chat (node-framework.md R14) | `process` reads inputs, writes outputs and uses this page's wait-free channels (parameters, `rt` status, coded records), never a ring of audio (6.2) |
| R15 | G3: "(a)": a leaked configuration holding a store, recording path or port: refused with a detail saying the node starts when that configuration closes; the late close starts it | 2026-11-08, chat (node-primitives.md G3) | The wait is visible: `phase` and its `waitingFor` say which configuration holds what (4.8) |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08, chat (node-framework.md R16) | Every lifecycle notification, record and editor cue is per node; one node's state changes nothing another node shows (4.8, 8.6) |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. For example, changing a network port on a node is something that a node can handle on its own — even if it must output null audio frames for a few seconds. But if reconfiguring changes the shape of the input/output bus, then that might not be fully transparent" | 2026-11-08, chat (node-framework.md R17) | The schema says per row whether a change is live, applied in place or a restart, and which rows change the ports; silence on purpose is said in words on the box ("reconfiguring port") and shown by every port's meter, not as a fault (R30); a shape change says which routes it removes before it does (4.2, 4.9, 8.6) |
| R18 | "crossing to a non-rat [non-realtime] thread could be managed by allowing a node to internally pipe its inputs/outputs to a hidden node, to maintain the integrity and consistency of the processing graph" | 2026-11-08, chat (node-framework.md R18) | Companions are hidden, not secret: listed under their node, never a box or a route (4.10, 8.7) |
| R19 | "Shouldn't that all be part of the build stage? How is build different from [re]configure?" | 2026-11-08, chat (node-framework.md R19) | A type writes `configure` once; what the engine readies later is the framework's, and shows as `configuring` with `waitingFor` (4.8) |
| R20, R21 | "A “run” can’t fail — only a node can fail. We should drop the “run” concept entirely and work with “graph” and “node” concepts. The graph is always running, while nodes can be added to it, configured, wired, start/stop all synchronously within the graph runtime"; "I meant “asynchronously”" | 2026-11-08, chat (node-framework.md R20, R21) | No `run` on the wire or in a record; `node.start` and `node.stop`; a reply says what was applied and never waits for a node's slow work (4.8, 4.9) |
| R22 | Gate A1: (b), a short set the editor colours and a `phase` with the exact state and cause | 2026-11-08, chat (node-framework.md R22) | `state` and `phase` (4.8) |
| R24 | "too technical for me to properly answer"; the owner is asked only what an operator or a type author sees | 2026-11-08, chat (node-framework.md R24) | Section 14: five owner gates (all answered on 2026-11-08, R30 to R32 and 14.1); every other question decided by the designer |
| R25 to R28 | On node-framework.md's F1 and F2: "“Disabled” would be a better term"; "Keeps everything"; "Both words"; "Only disabled in file" | 2026-11-08, chat (node-framework.md R25 to R28) | `node.stop` and `node.start` write nothing and a stop is forgotten at a restart; `node.disable` and `node.enable` write and remove `disabled true;` inside the node's braces (R29); `wanted` is `running`, `stopped` or `disabled`; `disabled` is a state the editor colours (4.2, 4.8, 8.6) |
| R29 | "the “disabled” key should be an attribute within the node scope — not a keyword on the node name" | 2026-11-08, chat (node-framework.md R29) | `node.disable` writes `disabled true;` inside the node's braces and `node.enable` removes that line; `disabled` is a key the framework reserves, never one of a node's `settings` on the wire (4.2, 4.3, 4.6; node-framework.md D-F8 to D-F12; revision 5.1) |
| R33 | Asked whether `configure` should come in two versions, each offering only its legal calls: "Two versions" | 2026-11-08, chat (node-framework.md R33) | A type author's change, not a wire change: `gain`'s class writes creation's `configure` (5.7); `.inPlace()` rows reach the change version |
| R34 | Asked what Disable does when the node's `disabled` value comes from a variable other nodes share (`disabled $quiet;`): "Only this node" | 2026-11-08, chat (node-framework.md R34) | `node.disable` and `node.enable` edit only that node's own `disabled` line; the reply names one node, and no other node changes (4.2) |
| R30 | On gate W3: "I would like each port to show meter levels so I can see audio traversing the graph" | 2026-11-08, chat (W3) | No "held" marks: every port of every node, inputs and outputs, shows a live meter, so a hold, a stop or a failure reads as silence on that port and on every port it feeds, and the box says why in words (8.6) |
| R31 | On gate G3: "Only a related save" | 2026-11-08, chat (G3; not the recommendation) | A save of `ear.conf`, by hand or from the text pane, stops a fader only when it changes the dragged value, the line that value is defined on, or which settings share it; any other save leaves the drag going, and its release writes onto the new text (7.3) |
| R32 | On gate G4: "Keep last value" | 2026-11-08, chat (G4; not the recommendation) | A fader whose browser disconnects mid-drag is ended as released: ear writes the last value it heard to `ear.conf`. A tab that goes silent for 30 s is treated the same (designer's decision G15, 14.2) (7.3) |

The same walkthrough's earlier answers to node-primitives.md's gates hold (node-framework.md 14.1): its G1 (a new
`ear_runtime` package for loops, wakes, the I/O executor, the probe and stores), G14 (loops declared in the spec as
handles), G15 (resources reached by handle, now inside `configure`) and G17 (the node handle reached as
`node.handle()`, so `Report::handle()` here). This page's own gate questions (revision 2's G1 to G13, revision 3's W,
X, K and O) are re-sorted in section 14: five were the owner's in revision 4, rewritten to what an operator sees, and
all five were answered on 2026-11-08 (R30 to R32 above; X2 and G1 as recommended; 14.1); the rest are decided by the
designer. **One register** (node-framework.md round 4's m1): every R number here is node-framework.md's section 1;
revision 5 numbered W3's, G3's and G4's answers R29 to R31 and the `disabled` answer R32, which are R30 to R32 and R29
now, and every citation on this page moved with them.

Design rules 1 to 3 of `pipewire-graph.md` hold: one owner per piece of state; a node reaches the engine only through
powers the framework grants; the graph always runs. The graph file's rules hold (`knobly-config.md`, design rules 1 to
3): the working copy leads, the disk wins; judge before keep; one keeper thread, one step at a time.

## 2. Journeys

Each journey is told against one `ear.conf`:

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

`gain` is the new type of slice 6 (5.7): `channels` (default 2; it makes the ports), a live `gainDb` and a live indexed
family `trimDb.<i>`. Journeys about the lifecycle add, to the same file:

```conf
node rtp fpp { address "0.0.0.0"; port 5004; format "L24"; channels 2; }
node file rec { path "take1.wav"; }
route fpp:stream.1 rec:record.left;
route fpp:stream.2 rec:record.right;
```

`rtp`'s `address`, `port`, `format`, `rate` and `latencyMs` are `.inPlace()` and `channels` makes its ports;
`card`'s rows are restart-only and `playback` and `capture` make its ports (node-framework.md 6.6's table).

### 2.1 The operator, in the editor

**Turns a gain knob, and sees and hears it.**

1. The operator selects `amp`. The side panel draws its settings from the type schema (4.2): `channels` as a number
   field marked "changes the ports", `gainDb` as a fader from -inf to +24 dB showing `-6.0 dB`, `trimDb.1` and
   `trimDb.2` as small faders. The box on the canvas shows `gainDb` as a knob (8.2's rule: the type has one plain live
   row).
2. Pointer down on the knob. The editor sends `gesture.begin {"name":"amp","key":"gainDb"}`. The control thread hands
   it to the main loop, which answers from the reach index (7.2) without waiting for the keeper:
   `{"gesture":"g4","value":-6,"reach":[{"name":"amp","key":"gainDb"}],"min":-90,"max":24,"decimals":1}`. Every other
   editor gets `gesture.changed {"phase":"begin",...}` and draws the knob as held by another editor; the operator's own
   editor gets no echo.
3. Drag. Each animation frame sends `gesture.change {"gesture":"g4","seq":n,"value":v}` as a JSON-RPC notification
   (no id, no reply), read even while another request of this socket is in flight (6.6). The control thread keeps only
   the newest tick per gesture until the main loop took the last one; the main loop stores `v` in `amp`'s parameter
   slot. On the realtime thread the framework ramps `amp`'s value to it over 20 ms (5.4), so the audio follows the hand
   without zipper noise. The meters on `amp`'s output and on the wire to `desk` move at once. Other editors see the knob
   move, at most 30 times a second.
4. The operator stops and listens, pointer still down, for half a minute. The editor sends the same value as a tick
   every 2 s while it holds the pointer (8.3), so the gesture stays open; a tick that does not change the value moves
   nothing and tells no one (6.6).
5. Pointer up at `-3.5`. The editor sends `gesture.end {"gesture":"g4","value":-3.5}`. The keeper edits `gainDb -6;` to
   `gainDb -3.5;` at its source, the desired graph judges it a *retune* (only live rows changed), and in one main-loop
   turn every client gets `graphFile.changed` (cause `edit`), `node.updated` with the new settings and
   `"applied":"retune"`, and `gesture.changed {"phase":"end","value":-3.5}`; then the reply `{"value":-3.5}`. The node
   is not restarted: no `node.state` follows, the card keeps playing. `ear.conf` is written behind as today. Standard
   output has a `graph-file` record and one `param` record (`node amp gainDb -6 -> -3.5`).

**A hand edit lands mid-gesture, elsewhere in the file.** The operator is holding `amp`'s knob at `-3.5`; a colleague
adds a comment above `node card desk` and saves `ear.conf` from `vi`.

1. The keeper's inotify watch fires; it reads the file and offers it (cause `disk`), with the new text's reach index.
2. On the main loop, before the plan, each open gesture is compared with the new index (7.3, the owner's G3, R31):
   `g4`'s definition is still the line `node gain amp { gainDb -6; }`, unchanged, now one line lower; its value is still
   `-6`; its reach is still `amp gainDb`. The save is not related to `g4`, so nothing is sent about it; its place moves
   to the new line. Then `graphFile.changed` (cause `disk`), and a plan that changes no node.
3. The knob keeps following the hand, and the audio with it. Pointer up at `-3.5` ends the gesture as before; the keeper
   edits `gainDb -6;` in the new text, so the colleague's comment is kept.

**A hand edit lands mid-gesture, on the dragged value.** The operator is holding `amp`'s knob at `-3.5`; a colleague
saves `ear.conf` from `vi` with `gainDb -10;`.

1. The keeper reads the file and offers it, as above.
2. On the main loop, before the plan, `g4`'s defining line has changed (and so has its value): the save is related, so
   `g4` is closed as aborted: every client gets `gesture.changed {"gesture":"g4","phase":"aborted","reason":"hand-edit"}`,
   with no value. The slots stay where the hand left them for the rest of this turn. Then `graphFile.changed` (cause
   `disk`), the plan, the retune of `amp` to `-10` with `node.updated`; last in the turn every slot the gesture reached
   is set to the value now running. So the audio ramps once, from -3.5 straight to -10 (7.3).
3. The operator's editor snaps the knob to `-10`, ignores the pointer until it is released, and shows a notice: "ear.conf
   was edited by hand and changed amp gainDb while you held it; it now says -10." with a button "Set -3.5 again" (a
   `node.set`). Releasing sends nothing (the gesture is gone). The audio never stayed on a value `ear.conf` did not hold
   once the gesture was over.

**The laptop's Wi-Fi drops mid-gesture.** The operator holds `amp`'s knob at `-3.5` (the file says `-6`) when the
laptop's Wi-Fi drops.

1. No ticks arrive. The socket stays open on ear's side (TCP has not noticed), so ear sees silence, not a close.
2. 30 s after the last message about `g4` (the editor's keep-alive comes every 2 s while the pointer is held), the
   control thread ends the gesture on its owner's behalf, as a `gesture.end` with no value (the owner's G4, R32; 7.3).
   The keeper edits `gainDb -6;` to `gainDb -3.5;`; every other client gets `graphFile.changed`, `node.updated` and
   `gesture.changed {"phase":"end","value":-3.5,"reason":"idle"}`. Standard output has `node amp gainDb -6 -> -3.5`
   with `"reason":"idle"`. Had the socket closed first, the same end would have come at once, with reason
   `session-closed`.
3. When the laptop reconnects, its editor reads the snapshot: the knob shows `-3.5`, what the operator last chose and
   what `ear.conf` now says. A notice says so if the tab survived ("Your change to amp gainDb was kept at -3.5 after 30 s
   without a word from this editor").

**A node flaps.** `kitchen`'s connection to Music Assistant drops and comes back every few hundred milliseconds.

1. Its worker loop writes `connection` (`connected`, `dialling`, `connected`, ...). Standard output has a `status`
   record per change up to the field's pace (32 at once, then one a second), and once the pace holds changes back, one
   more record within a second with the value the field now has and how many were held back, so `kubectl logs` always
   ends on the true value (6.5, 9). The wire carries the latest value at most 30 times a second.
2. The framework counts transitions at each write. Fourteen in ten seconds pass the field's threshold (ten by
   default): it emits `ear.status-flapping` with subject `connection`, `active: true`, `transitions: 14`,
   `windowMs: 10000`, as a `node.diagnostic` and a `diagnostic` record.
3. The editor puts a "flapping: connection" badge on `kitchen`'s box, with the count and the threshold ("more than 10
   changes in 10 s", from the schema) in its tooltip; the panel's diagnostics list keeps it with its time. When a whole
   window passes with the field at or below its threshold, ear sends the same code and subject with `active: false`,
   and the badge goes. A second field flapping at once is a second badge (diagnostics are kept per code and subject).

**A node reports a pairing code.** `kitchen` connects to a server it has not paired with.

1. The worker writes `connection = connected`, `serverName = "Music Assistant"`, `paired = false`, then
   `pairingCode = "123-456"`. The schema marks `pairingCode` as a **callout** (5.2).
2. The editor shows a callout on `kitchen`'s box and in its panel: "Pairing code 123-456: type it into Music
   Assistant", with a copy button, announced by a polite live region. Standard output has
   `node kitchen pairingCode "" -> "123-456"`.
3. The operator types the code into Music Assistant. The worker raises the event `paired {"server":"ma-7f3c"}` and
   writes `paired = true`, `pairingCode = ""`; the callout goes, and the node's activity list says "paired with
   ma-7f3c". A wrong code raises `pairing-failed {"reason":"pairing-code-mismatch"}`, an event of severity `warn`,
   which the editor also shows as a notice (coalesced: "pairing failed ×3", 8.4).
4. If the node's configuration is torn down (a restart, a failure, a removal), the server sends the node's status
   reset (4.3): the callout and the box's status line go at once, and they stay hidden while the node is neither
   `running`, `stopped` nor `reconfiguring` (8.4). A client never shows a pairing code from a configuration that is
   gone. A rename (`name "Kitchen 2"`, applied in place) and a stop keep the configuration, so the callout stays unless
   the worker clears it (sendspin's `stop()` closes the connection, so its worker clears it).

**Starts a failed node again.** `desk` is `failed engine-refused: card lost` after a USB unplug; the card is back.

1. The failed node was torn down when it failed (node-framework.md 6.2): its companions are gone, its block is silent,
   and its wires stay drawn, carrying zeros. The box says `failed` with "the card failed: ..." and an empty companion
   strip (8.7), and offers Start (a retry) beside Restart. The operator clicks Start.
2. The editor sends `node.start {"name":"desk"}`. The keeper orders it with every other request and writes nothing (a
   start never edits `ear.conf`, R28); on the main loop `NodeHost` takes T15 behind the same block. The
   client gets `node.state` `starting` (phase `configuring`, cause `{"kind":"retry"}`), then the reply `{"applied":
   "retry"}`, then `configured`, `starting` and `running` (4.3's order rule 5, 4.8). A failed node has no
   configuration to reset, so no reset is sent.
3. Standard output has `node desk start invoked` (an `action` record), then `node desk starting: retry`, then `node
   desk running` (9.1). Start is disabled while the request is in flight and while the node's block is lost
   (`"blockLost": true` in the snapshot), its description then saying why: "the node's engine block is gone; remove
   and add the node, or restart ear". A script that tries anyway is refused `not-restartable`.

**Stops a node, and starts it again.** The porch is quiet for an hour; the operator clicks Stop on `porch`, a card node.

1. The editor sends `node.stop {"name":"porch"}`. The keeper orders it and writes nothing (R28): there is no
   `graphFile.changed` and no `node.updated`. In one main-loop turn: `node.state` `stopping` (cause `stop`, `wanted:
   "stopped"`), then the reply `{"applied":"stop"}`.
2. The outputs fade over 5 ms; the box turns the `stopped` colour with "stopped until Start or a restart of ear" and a
   Start button; its wires stay drawn and carry silence, and the meters on its ports and on the ports it feeds read
   silence; the card stays open and plays the silence (R26), so the companion strip stays green. Standard output:
   `node porch stop invoked` (an `action` record), `node porch stopping: stop`, then `node porch stopped`.
3. Start sends `node.start`; `starting`, then `running` within a cycle and a main-loop turn. It cannot fail for the
   card: the card never closed. Had the pod restarted in between, the porch would have played again: a stop is not in
   `ear.conf` (8.6 says so beside the button).

**Disables a node, and enables it again.** The porch is closed for the season, and another program wants the USB card.
The operator clicks Disable on `porch`.

1. The editor sends `node.disable {"name":"porch"}`. The keeper writes the attribute first in the node's block, `node
   card porch { disabled true; card USB2; playback 2; }`, comments and layout kept, and commits; in one main-loop turn:
   `graphFile.changed` (cause `edit`), `node.updated {"name":"porch",...,"disabled":true,"applied":"disable"}`,
   `node.state` `stopping` (cause `disabling`, `wanted: "disabled"`), then the reply `{"applied":"disable"}`.
2. The outputs fade; the whole configuration is torn down (node-framework.md T20): the card's companions are destroyed,
   so the device is closed and free for the other program; `node.companionsChanged` empties the strip. Then
   `node.state` `disabled`. The box turns the `disabled` colour with "disabled in ear.conf" and an Enable button; its
   wires stay drawn and carry silence, and the meters on its input ports still show what reaches it. Standard output:
   `graph-file edit ...`, `node porch stopping: disabling`, `node porch disabled`. The text pane shows the new line.
3. In spring the operator clicks Enable: `node.enable` removes the `disabled true;` line; `node.state` `starting` (phase `configuring`,
   cause `enabling`), then `running` once the card opens. If the other program still holds the card, the node fails
   `engine-refused: the card failed: ...` as any start does, and the box offers Start (a retry). A colleague could do
   either by adding or removing the line in `vi`; the editor shows it the same way.

**Restarts a node.** The Restart action (7.6) is offered on every node but one whose block is lost: a full stop, a
configuration from empty and a start back to the wanted state, without editing `ear.conf`; the box shows `stopping`
("restarting: restart invoked"), `starting` ("configuring"), then `running`.

**Watches a node start.** The operator adds `node rtp fpp { ... port 5004; ... }` while another program holds UDP port
5004 on the host.

1. `node.added` (phase `added`), then at once `node.state` phase `configuring`: the box shows
   `starting` with the line "configuring", and the companion strip shows `fpp.rtp` grey (not ready).
2. rtp-source cannot bind; PipeWire only logs it (AGENTS.md). After 2 s in `configuring` standard output has
   `node fpp still configuring after 2s: waiting for fpp.rtp` (9.1), and the box's line says the same. With slice 8's
   readiness deadline the node fails `engine-refused: the RTP receiver never ran` and the box turns `failed`; until
   slice 8 it stays here, said plainly instead of silently.
3. On a free port the same node goes `configuring` → `configured` → `starting` → `running` in milliseconds: four
   `node.state` notifications, one `node fpp running` record, and the strip's dot turns green.

**Changes an `rtp` node's port in place.** The operator sets `fpp`'s `port` from 5004 to 5006 in the panel; the field
says "applies in place" (4.2's `change`).

1. Apply sends `node.set {"name":"fpp","settings":{"port":5006}}`. The keeper edits `port 5004;` to `port 5006;`; the
   plan's outcome is `reconfigure`; in one main-loop turn: `graphFile.changed`, `node.updated {..., "applied":
   "reconfigure"}`, `node.state` `reconfiguring` with `keys: ["port"]` (`rtp`'s hook, called in the commit, holds its
   outputs while the new receiver binds), then the reply `{"applied":"reconfigure"}`.
2. The box turns the `reconfiguring` colour with "reconfiguring port". The meters on `fpp:stream.1` and `fpp:stream.2`
   fall to silence over 5 ms, the wires to `rec` thin as their level falls (they stay drawn), and the meters on
   `rec:record.left` and `rec:record.right` fall with them: the operator follows the silence from the port that makes
   it to every port it reaches (R30). Nothing is shown as a fault: the box says why in words, and the operator asked
   for this silence (R17). The companion strip shows two `fpp.rtp` entries for a moment, the old one marked
   retiring.
3. The new receiver runs; the old one is released; the hold fades out; `node.state` `running` (`afterMs` in the record). Standard output: `node fpp reconfiguring: port`, then `node fpp running`. The recording has about
   `latencyMs` of silence, and no restart: the WAV, the routes and the meters never stopped.
4. Had 5006 been held too, the change would fall back (node-framework.md 6.6 step 5): `node.state` `stopping`
   with cause `{"kind":"restarting","restart":{"kind":"fell-back","reason":"resource-refused",...}}`, then a start that fails as any
   failed start does. The notice says "fpp could not move to port 5006 in place: ..." and the panel keeps 5006, since
   `ear.conf` holds it.

**Changes a card's channel count (a reshape).** `desk` has `playback 2`; routes reach `desk:play.1` and `desk:play.2`.
The operator sets `playback` to 1; the field says "changes the ports; restarts the node".

1. Before sending, the panel asks ear: `node.set {"name":"desk","settings":{"playback":1},"dryRun":true}` answers
   `{"applied":"reshape","inPlace":false,"ports":{"removed":["play.2"],"added":[]},"routes":[{"from":"kitchen:from.left","to":"desk:play.2"}]}`
   (4.9). The panel shows "This removes 1 route: kitchen:from.left → desk:play.2" with Apply and Cancel (X2, answered
   as recommended).
2. Apply sends the same `node.set` with `"dropRoutes": true`. One commit removes the route line and edits `playback 2;`:
   `graphFile.changed`, `route.removed`, `node.updated {..., "applied":"reshape","ports":{...}}`, `node.state`
   `stopping` (cause `{"kind":"restarting","restart":{"kind":"reshape"}}`), then the reply.
3. The box redraws its handles in place and keeps its position; the wire from `amp:out.1` to `desk:play.1` never goes
   `pending` (node-framework.md 6.7). Without `dropRoutes` the edit is refused `ports-in-use` naming the route, and
   nothing changes.

**Edits a node while it reconfigures.** While `fpp` is still reconfiguring onto 5006 the operator sets `latencyMs` to
150. The edit commits at once (`applied: "reconfigure"`, written to `ear.conf`), and the node takes it when the first
change ends (node-framework.md 6.3): the box line says "reconfiguring port; latencyMs next". A `node.set` of a
live row on a reconfiguring node retunes at once (7.7).

### 2.2 The script author, with `scripts/rpc-call.mjs`

```bash
U=ws://127.0.0.1:7777/rpc
# Set one key, leaving the others as they are. A live row retunes; an in-place row reconfigures; any other row
# restarts the node; a row that makes ports reshapes it.
node scripts/rpc-call.mjs $U node.set '{"name":"amp","settings":{"gainDb":-12}}'
{"applied":"retune"}
node scripts/rpc-call.mjs $U node.set '{"name":"fpp","settings":{"port":5006}}'
{"applied":"reconfigure"}
# Ask first: what would this do? Nothing is committed or written.
node scripts/rpc-call.mjs $U node.set '{"name":"desk","settings":{"playback":1},"dryRun":true}'
{"applied":"reshape","inPlace":false,"ports":{"removed":["play.2"],"added":[]},"routes":[{"from":"kitchen:from.left","to":"desk:play.2"}]}
# Do it, removing the routes the reshape would strand, in the same commit.
node scripts/rpc-call.mjs $U node.set '{"name":"desk","settings":{"playback":1},"dropRoutes":true}'
{"applied":"reshape","inPlace":false,"ports":{"removed":["play.2"],"added":[]},"routesDropped":[{"from":"kitchen:from.left","to":"desk:play.2"}]}
# Wait until the node runs again (step L1 adds --until and --timeout): exits 0 on running, 1 on failed, 2 on timeout.
node scripts/rpc-call.mjs $U --until 'desk running' --timeout 30 node.set '{"name":"desk","settings":{"card":"USB2"}}'
# Remove a key (back to its default): null.
node scripts/rpc-call.mjs $U node.set '{"name":"amp2","settings":{"trimDb.2":null}}'
{"applied":"retune"}
# Read a status field.
node scripts/rpc-call.mjs $U graph.get | jq -r '.nodes[] | select(.name=="kitchen") | .status.pairingCode'
123-456
# Stop a node (silent, keeping everything; not written to ear.conf, forgotten at a restart), and start it again;
# start on a failed node retries.
node scripts/rpc-call.mjs $U node.stop '{"name":"porch"}'
{"applied":"stop"}
node scripts/rpc-call.mjs $U --until 'porch running' node.start '{"name":"porch"}'
# Disable a node (written to ear.conf as `disabled true;` in its braces; everything released), and enable it again.
node scripts/rpc-call.mjs $U node.disable '{"name":"porch"}'
{"applied":"disable"}
node scripts/rpc-call.mjs $U --until 'porch running' node.enable '{"name":"porch"}'
# Restart a node (not written to ear.conf); a failed node is retried.
node scripts/rpc-call.mjs $U node.restart '{"name":"desk"}'
{"applied":"restart"}
# What does a type take?
node scripts/rpc-call.mjs $U types.get | jq '.typeSchemas[] | select(.name=="gain") | .settings[] | {key, kind, min, max, unit, live}'
```

A refusal is printed to standard error and exits 1, with the same `data.code` grammar as today, and the key it is
about, so a tool never parses the message:

```json
{"code":-32000,"message":"invalid-settings amp: gainDb: must be a number of dB from -90 to 24","data":{"code":"invalid-settings","key":"gainDb","detail":"must be a number of dB from -90 to 24"}}
```

An edit's reply comes at its commit, never when the node is running again (X3): `applied` is what the change asks of
the node. A script that must wait for the audio uses `--until '<node> running'` (step L1): it subscribes before the
request, and exits on the first `node.state` of that node whose `state` is `running` received after the reply (a
session's replies and notifications share one ordered queue, so nothing older can arrive after it), or with 1 on
`failed`. When the reply's `applied` is `retune` or `unchanged` and the node's `state` is already `running` in the
snapshot it subscribed to, no `node.state` will come (4.8), so it exits 0 at once. `--timeout <s>` (default 60, the
longest in-place deadline) exits 2 with the last `node.state` seen, so a script never waits for ever. To wait for anything else (a pairing code), step 5.2 adds `--follow`: it subscribes to the graph and
prints every notification as one line, so `| jq --unbuffered 'select(.method=="node.statusChanged")'` waits on what it
needs. A script that holds a gesture must send a tick at least every 30 s (6.5), or ear ends the gesture as released,
writing its last value (G4); scripts rarely need one, since `node.set` changes a live row without a gesture.

### 2.3 The client implementer

1. **Connect and read the version.** `graph.subscribe` answers the graph; from this page it carries `"protocol": 1`,
   `"features": [...]` and `"typeSchemaHash": "fnv1a64:9c2e41f07a3b5d18"`. No `protocol` is protocol 0: an ear from
   before this page, which has none of this page's methods. A client gates each new method and member it sends on a
   feature (`"gestures"` before `gesture.begin`), never on a -32601 probe. A `protocol` above what it knows: a banner
   ("this ear speaks a newer protocol; some things may not show"), and it keeps working with what it understands (4.1).
2. **Treat every enum as open.** A node state, a route status, a status field, an event code, a diagnostic code, a
   gesture phase or a notification it does not know is drawn neutrally or ignored, never an error (4.1). Ignore members
   you do not know. The fixture `test/protocol/unknown-values.json` checks this.
3. **Fetch the schemas once per hash.** If the cached hash differs, `types.get {"ifNoneMatch":"<cached>"}`. ear answers
   the whole set, or `{"notModified":true,...}`. The hash is opaque and changes only with ear's build; the editor keeps
   the set in `localStorage` by hash.
4. **Fold notifications into the snapshot** (4.4). A node carries `state`, `phase`, `wanted` and the members 4.8 gives
   each phase, `status`, `gestures`, `diagnostics`, `companions` and `blockLost`. Colour by `state` (a short open set),
   explain by `phase` and `cause`; never infer a phase from `state`. A `node.state` replaces every health member: a
   member it leaves out is absent now (the `keys` of a reconfigure that ended are gone). A `node.statusChanged` with
   `reset: true` *replaces* the status; any other merges. `node.diagnostic` replaces
   the entry of its code and subject, and carries the server's `count`. `node.eventRaised` changes nothing (an event has
   no lasting value) and goes to an activity list that starts when you connect. Order is today's: one ordered stream per
   client, a change's notification before the statuses it causes (batch2.md section 6).
5. **Gestures.** Begin, tick with notifications (keep-alive every 2 s while held), end. You get no `begin` or `change`
   echo for your own gestures, so any `begin` you see is another session's. On `phase: "aborted"` stop the control,
   show `value` when it is given, else wait for the `node.updated` that follows, and tell the person (8.3). A save of
   `ear.conf` that does not touch your value leaves your gesture open: keep going. If you go silent for 30 s, or your
   socket closes, ear ends your gesture with your last value, as if you had released it (`phase: "end"` with a
   `reason`).

### 2.4 The node type author

The author adds, to one type, a live parameter, a status field, an event and an action. The complete type is `gain`
(5.7); this is the part that matters, against slice 2's interface (`type_spec.hpp`, `settings_table.hpp`) and the
additions of section 5.

```cpp
// src/nodes/gain/gain_spec.hpp
namespace ear::nodes::gain {
inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2).shapesPorts();   // a change reshapes
// A live parameter: a decimal row, ramped over 20 ms (linear in dB) when it moves; its minimum means silence.
inline constexpr auto gainRow =
	setting::defaulted("gainDb",
	                   setting::decimal(-90.0, 24.0, "dB",
	                                    {.decimals = 1, .taper = setting::Taper::db, .minIsSilence = true}),
	                   0.0)
		.live(setting::smooth(20));
inline constexpr auto in = bank::input<"in">;
inline constexpr auto out = bank::output<"out">;
// A status field the realtime code writes.
inline constexpr auto clipping = status::flag<"clipping">(writer::rt).help("an output went above full scale");
// An event the realtime code raises: numbers only.
inline constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "channel {channel} clipped at {peakDb} dB",
                                                     arg::number<"channel">(1, 64), arg::decimal<"peakDb">());
// An action into the code: a trigger, no arguments.
inline constexpr auto resetClip = action::trigger<"reset-clip">("clear the clipping flag");
inline constexpr TypeSpec spec{.settings = SettingsTable<channelsRow, gainRow>{"gain"},
                               .banks = Banks<in, out>{},
                               .status = StatusTable<clipping>{},
                               .events = EventTable<clipped>{},
                               .actions = ActionTable<resetClip>{}};

struct GainCode {
	bool latched = false;
	void process(Io<spec> io, const GraphCycle&) noexcept {
		if (io.triggered<resetClip>()) latched = false;
		const Smoothed gain = io.param<gainRow>(); // this block's value per sample, in dB
		const auto ins = io[in];
		const auto outs = io[out];
		for (std::size_t c = 0; c < outs.size(); ++c) {
			float peak = 0.0F;
			for (std::uint32_t i = 0; i < io.frames(); ++i) {
				outs[c][i] = ins[c][i] * dbToFactor(gain[i]); // 5.7 computes one factor for a steady block
				peak = std::max(peak, std::abs(outs[c][i]));
			}
			if (peak > 1.0F && !latched) {
				latched = true;
				io.raise<clipped>(static_cast<std::int64_t>(c + 1), factorToDb(peak));
			}
		}
		io.set<clipping>(latched); // a change is recorded only when the value differs from the last one written
	}
};
} // namespace ear::nodes::gain
```

Nothing else is written: no slot, no queue, no thread, no wire code, no editor code. The schema, the editor's fader,
the `clipping` row in the panel, the `clipped` entry in the activity list, the "Reset clip" button, the standard-output
records and the gesture handling all come from these declarations. Misspell `gainRow` in `io.param`, read a row that is
not live, `raise` a text argument from `process`, `set` a field another writer owns, or read a live row with
`s.get<gainRow>()` in `configure` (where it would go stale at the first retune; `s.initial<gainRow>()` says so in its name),
and it does not compile (5.8). `channelsRow` makes the ports, so it carries `.shapesPorts()` (5.1): the schema then
says "changes the ports", and the all-type tests check the mark against what `ports()` does.

Three limits a type author should know. A live row reaches only the node's realtime code, through `io.param`; a value
that must be applied off the realtime thread (a sound card's mixer volume, a volume sent to a server) is an
`.inPlace()` row, which the type's change version of `configure` posts to its loop's state (node-framework.md 6.6): no
restart, but no fader gesture either (X6). Status written off the realtime thread is written from a loop of the spec
the field names (`writer::loop(worker)`, 5.2), never from a library's thread directly. And an in-place change keeps the configuration,
so the node's status stays, and so do a stop and a start: a change that makes a field stale (a listening port) has
its loop's state write the field anew (6.7).

The type's tests drive it all through the harness (5.9):

```cpp
TEST_CASE("gain follows a gesture and latches clipping", "[gain]") {
	ear::testing::TypeHarness<gain::spec, gain::Gain> h;
	REQUIRE_FALSE(h.parse({{"channels", 1}, {"gainDb", 0.0}}));
	h.add();
	h.blockReady(); // node-framework.md 7.6: one call per transition; h.state() == NodeState::running
	std::ranges::fill(h.input(gain::in, 0), 2.0F);
	h.process(256);
	CHECK(h.status<gain::clipping>());
	CHECK(h.events().at(0).code == "clipped");
	h.live<gain::gainRow>(-12.0); // as a gesture tick
	h.process(256);               // ramping: 20 ms is 960 frames at 48 kHz (5.4's vector)
	h.trigger<gain::resetClip>();
	h.process(256);
	CHECK_FALSE(h.status<gain::clipping>());
}
```

## 3. The domain model

### 3.1 The ten words

| Concept | What it is | Declared in | Written by | Kept | Owner of the truth | On the wire | On stdout |
|---|---|---|---|---|---|---|---|
| **Setting** | What the operator wants, surviving restarts | a `SettingsTable` row | the keeper (from `ear.conf`, `node.*`, `graphFile.set`) | `ear.conf` | `ear.conf`, judged by the desired graph | node `settings`; `node.updated` | `graph-file` record |
| **Parameter** | A setting declared `.live`: it also has a realtime value that moves without a restart | a row with `.live(smoothing)` | committed: the keeper; live: a gesture's ticks | committed: `ear.conf`; live: memory, only during a gesture | committed: `ear.conf`; live: the gesture's owner | `gestures`; `gesture.changed` | `param` record at commit and abort |
| **Status** | A fact the node observes that has a current value | a `StatusTable` field with one writer | that writer, through `Report` or `Io`; the framework's own `ear.` fields | memory, per node; reset when the node's configuration is torn down (a restart, a failure, a removal) | the declared writer | node `status`; `node.statusChanged` | `status` record per change, paced per field (6.5) |
| **Event** | Something that happened at an instant, with no lasting value | an `EventTable` code | the node, through `Report` or `Io` | not kept | the node | `node.eventRaised` | `event` record |
| **Action** | A command into a node that changes no desired state | an `ActionTable` entry (`start`, `stop`, `restart`, `disable` and `enable` are the framework's lifecycle requests, 4.2, not actions) | a client, ordered by the keeper | not kept | the node | `node.invoke` | `action` record |
| **Node diagnostic** | What the framework observes about a node's machinery | the framework's code list (4.6) | the framework alone | per code and subject: the latest, whether active, a count | the framework | node `diagnostics`; `node.diagnostic` | `diagnostic` record |
| **Log** | Free text for people debugging | levels; `LogCodes` for the realtime thread | any node code; captured output | not kept | whoever wrote it | not yet (`log.subscribe`, later) | `log` record |
| **Note** | Retired. Today's `InsidesEvents::say` sentence; about 11 of sendspin's 17 are state, the rest edges | `notes.hpp` | sendspin | not kept | — | — | `note` record, until step 5.5 removes the last |
| **Health** | Where the node is in its lifecycle and why: `state` (the colour), `phase` (the exact state), its `cause`, what the graph wants of it (`wanted`: `running`, `stopped` or `disabled`), the keys an in-place change applies, the failure, whether its block is lost (and from slice 8 the attempt and last failure) | the framework's per-node state machine (node-framework.md section 6) | the framework alone (`NodeHost`'s two machines) | memory | the framework | node `state`, `phase` and 4.8's members, `blockLost`; `node.state` | `node` record (9.1) |
| **Companion** | A hidden engine node a node's configuration made to cross to or from non-realtime code (a card's adapter, a receiver, a sink, a source); machinery the operator may inspect, never route | the type's `configure` (node-framework.md 9.6) | the framework | memory, per node | the framework | node `companions`; `node.companionsChanged` (4.10) | in a failure's detail; drops and shortfalls as diagnostics (4.6) |

A **node diagnostic** is not a **graph-file diagnostic**. The second is today's problem in `ear.conf`'s text (a knobly
or `ear/...` code at a line and column: `Diagnostic`, `graph_file_state.hpp`, and `Diagnostic` in `protocol.ts:70-74`
[V]). They keep apart names everywhere: `NodeDiagnostic` (C++ and `protocol.ts`), node `diagnostics` beside
`GraphFile.diagnostics`, and an event's `EventSeverity { info, warn, error }` (spelled as `LogLevel`'s values) beside the
file's `Severity { error, warning }` (`graph_file_state.hpp:19` [V]).

### 3.2 The rule for choosing

Ask in this order; the first yes decides.

1. **Would the operator expect it after a restart of ear?** It is a **setting**. If it must move while audio plays,
   without a gap, is a number, and is used by the node's realtime code, it is a **parameter** (`.live`). A value the
   node can take while it runs, off the realtime thread (a port, a name, a volume sent to a server), is an `.inPlace()`
   setting (X6). Any other is restart-only, the default. A setting that makes ports is marked `.shapesPorts()` (4.2).
2. **Does it have a value right now, which someone could read at any moment?** It is **status**. A value that is
   "none" most of the time is still status (`pairingCode` is `""` until a code is offered). A value that is not known
   yet is `null` (a choice starts unknown, 5.2).
3. **Did something happen that someone might act on, and is there nothing to read once it passed?** It is an
   **event**. If the thing also leaves a value behind, write both: `paired` (event) and `paired = true` (status).
4. **Is it a request from outside to do something now, which nothing should redo after a restart?** It is an
   **action**. If it should survive a restart, it is a setting.
5. **Is it about the node's machinery (queues, timing, restarts, rates, loops, claims), not its domain?** It is the
   framework's **node diagnostic**, or a companion's counter (4.6, 4.10); a type cannot write either.
6. **Is it for a person debugging, in words?** It is a **log** line.
7. **Up or down?** **Health**, and only the framework judges it (node-framework.md I2).

| Today (sendspin's note) | Becomes | Why |
|---|---|---|
| `id <client id>` | status `clientId` | has a value |
| `dialling <url>`, `listening <port> as <instance>` | status `connection` (`dialling`, `listening`, `connected`; unknown until the worker writes), `url`, `listenPort`, `instance` | values; one field per fact |
| `connected "<name>" paired\|unpaired`, `disconnected` | status `connection`, `serverName`, `paired` | a pair of sentences is one value |
| `pairing-code <c>`, `pairing-code-cleared` | status `pairingCode`, shown as a callout | has a value until cleared |
| `paired <server id>` | event `paired {server}`, and status `paired = true` | an instant that also leaves a value |
| `pairing-failed <reason>` | event `pairing-failed {reason}`, severity `warn` | an instant |
| `pairing-held` | status `pairingHeld` (a flag) | holds until a setting changes or ear restarts |
| `stream <codec> <rate> <ch> <bits>`, `stream-ended` | status `stream` (`none`, `flac`, `pcm`), `streamRate`, `streamChannels`, `streamBits` | values |
| `sending <rate> 2 24`, `sending-stopped` | status `sending` (flag), `sendingRate` | values |
| `key-write-failed <detail>` | event `key-write-failed {detail}`, severity `error` | an instant the operator must see |
| `advertise-failed <detail>` | event `advertise-failed {detail}`, severity `warn` | an instant |
| `notes-dropped <n>` | gone: the framework's `ear.events-dropped` diagnostic | about the machinery |

Other examples: a card's PCM state (slice 8) is status; `rtp`'s `receiving` is status written by the resource (slice
8); `gain`'s `clipping` is status and `clipped` is an event; "a node's configuration did not close in 10 s" is the diagnostic
`ear.close-overdue`; frames a source companion came up short are its companion's `shortfall` (4.10); a library's
warning text is a log line.

### 3.3 Aggregates and owners

- **The node** (node-framework.md section 6) gains: the wanted state (`disabled` from `ear.conf`, the stop from
  `NodeHost`), the health members of 4.8, the status view (one value per declared field), transition counters, the companions' view (4.10), the
  node diagnostics kept per code and subject, `blockLost`, and, per live row, the live value and the gesture holding
  it, if any. All on the main loop, owned by `NodeHost`. Node diagnostics belong to the node, not to one configuration: a
  restart does not clear them (flapping across restarts is exactly what they are for). Status and companions belong to
  the node's configuration: a restart, a failure or a removal resets them, and an in-place change, a stop and a start
  keep them, since the configuration goes on (node-framework.md I6).
- **The gesture** is its own small aggregate, owned by a new main-loop service, `Gestures` (`ear_core`): an id (opaque,
  unique for the process's life, never reused), the session that owns it, its reach (the node and key pairs it moves)
  with the combined range and decimals, the committed value, the live value, the last sequence number, the time of the
  last tick, and whether its end or cancel has been read. It never outlives its nodes or a related save (7.3); when its
  session closes or goes silent it is ended, committing its live value (G4).
- **The reach index** is a value the keeper makes on its thread for each version of the working copy and the main loop
  holds for the running one: for each (node, live key) that `ear.conf` gives, the definition's place, the text of the
  lines it is written on, the reach, and the committed value (7.2). It lets `gesture.begin` answer without the keeper,
  and lets the main loop tell a related save from an unrelated one (7.3).
- **The type schema** is a value made from the type spec at compile time and encoded once at boot: what the wire's
  `types.get` returns and what the editor renders.

## 4. The wire

### 4.1 Naming, versioning and compatibility

- **Names.** Requests are `<entity>.<verb>` (`node.set`, `node.invoke`, `gesture.begin`, `types.get`), as `node.add` and
  `graphFile.set` are. Notifications say what happened (`node.statusChanged`, `node.eventRaised`, `node.diagnostic`,
  `gesture.changed`), as `node.updated` and `graphFile.changed` do. JSON object keys are camelCase (`windowMs`,
  `pairingCode`); status field names and setting keys are camelCase; codes, event codes and action names are kebab-case
  (`pairing-failed`, `reset-clip`), the framework's under `ear.` (`ear.status-flapping`). Durations end in `Ms`;
  instants are `t`, UTC to the microsecond, as on standard output.
- **One value-kind vocabulary** on the wire, for setting rows, status fields, event arguments and action arguments:
  `text`, `wholeNumber`, `decimal`, `flag`, `choice`, with `values` for a choice's values and for a text row's fixed
  set. (In C++ a whole number is `number`, as `setting::number` is today; `decimal` is a real number.) `flag` exists for
  status and arguments; a `flag` setting row is G11.
- **The protocol number.** `graph.get`'s result gains `"protocol": 1` from step L1 (O1). **An absent `protocol` is 0**:
  today's wire, with none of this page's methods. The number moves only on a breaking change: removing or renaming a
  method, notification, member or code, changing a member's type, or changing an order guarantee.
- **Features, for additive growth.** The graph also gains `"features": [...]`, a list of names; a client gates every
  method and every request member it sends on the feature that added it, never on a -32601 probe. This page's features,
  each added by the step that builds it (section 11): `lifecycle` (L1: `phase`, `since`, `wanted`, `cause`, `keys`,
  `waitingFor` on health; `disabled` on a node; `node.start`, `node.stop`, `node.restart`, `node.disable`, `node.enable`, `node-disabled`,
  `not-restartable`; `node.gone`), `companions` (L2), `node.set`, `reshape` and `dryRun` (L3: `node.set`, `applied` with
  `reconfigure` and `reshape`, `ports`, `dropRoutes`, `ports-in-use`, `dryRun`), `types` (5.1), `status` (5.2: status,
  events, node diagnostics), `actions` (5.4: `node.invoke`), `live` (6.3: decimal, indexed and live rows, `applied:
  retune`), `gestures` (6.4), `recovery` (R8: slice 8's members). The list only grows within a protocol. Steps L1 to L3
  land with node-framework.md's slices 3a, 3b and 3d, before 5.1 (O1), so L1 also builds the frame itself: `protocol`,
  `features`, open enums and the batch2.md erratum (10.3).
- **Members ear sends are additive**: results, notifications, records and snapshots may gain members at any time, and
  a client ignores members it does not know. **Members a client sends are not**: ear's decoders refuse an unknown key
  (`requireObject`, `src/control/graph_json.cpp:33-42` [V]), which keeps typo detection, so a request member exists
  only from the feature that adds it and a client checks the feature first. `errors.json` gains the case of an unknown
  request member (-32602, `data.pointer` naming it).
- **Every enum is open from protocol 1** (G7): `state`, a node's `phase` and `cause.kind`, `waitingFor`'s `kind`, a
  companion's `kind`, a route's `status`, a gesture's `phase`, `reason`, `severity`, a row's `kind` and `change`,
  `applied`, `defaultFrom`, codes of every sort. A client draws an unknown value neutrally (state `starting`'s
  style, a code as its text, an unknown event as its `msg`) and never fails on one. So slice 8's `recovering` is
  additive. `web/src/protocol.ts:13` keeps its closed union for typing but gains a fallback branch.
- **A client that sees a higher `protocol`** shows a banner and keeps working with what it understands.
- **Today's clients keep working.** Every request they send keeps its meaning but two: a `node.update` that changes only
  live rows no longer restarts the node (7.4), and one that changes only `.inPlace()` rows reconfigures it
  (node-framework.md 6.6). Both are what the operator wanted, and neither is observable as an error. The editor already
  ignores unknown notifications (`web/src/graph.ts:107-108` [V]). `node.update` refused with the new `parameter-held`
  shows its message as any refusal does (`App.tsx`'s `describe`).
- **Inside a batch**, `gesture.change` and `gesture.cancel` are refused (`not-in-batch`): they skip the keeper, so they
  could not keep a batch's order.

### 4.2 Requests

All are JSON-RPC 2.0 requests with named params. Errors are -32000 with `data.code` (4.5); a malformed param is -32602
whose message names the JSON pointer, as today, and whose `data.pointer` now carries it too.

| Method | Feature | Params | Result | `data.code` refusals | Path |
|---|---|---|---|---|---|
| `types.get` | `types` | `{"ifNoneMatch"?: string}` | `{"protocol","hash","typeSchemas"}`, or `{"protocol","hash","notModified": true}` | none | control thread (fixed at boot) |
| `node.set` | `node.set` | `{"name", "settings": {key: value \| null}, "dryRun"?: bool, "dropRoutes"?: bool}` (the last two from feature `reshape`, 4.9) | `{"applied"}`, and on a reshape `inPlace`, `ports` and `routesDropped`; with `dryRun`, `{"applied", "inPlace"?, "ports"?, "routes"?}` | unknown-node, invalid-settings, parameter-held, shared-definition, file-invalid, ports-in-use, stopping, and the desired graph's | keeper |
| `gesture.begin` | `gestures` | `{"name", "key"}` | `{"gesture", "value", "reach", "min", "max", "decimals"}` | unknown-node, not-live, shared-definition, parameter-held, file-invalid, file-refused, stopping | control thread, then the main loop (7.2) |
| `gesture.change` | `gestures` | `{"gesture", "seq", "value"}` | `{}` if sent with an id | unknown-gesture, invalid-settings, not-in-batch | control thread, then the main loop directly |
| `gesture.end` | `gestures` | `{"gesture", "value"?}` | `{"value"}`: what was committed | unknown-gesture, invalid-settings (the gesture stays open), stopping, and the desired graph's (the gesture is aborted) | keeper |
| `gesture.cancel` | `gestures` | `{"gesture"}` | `{}` if sent with an id | unknown-gesture, not-in-batch | control thread, then the main loop directly |
| `node.invoke` | `actions` | `{"name", "action", "args"?: {...}}` | `{}` | unknown-node, unknown-action, invalid-args, not-running, restarted, stopping, action-failed, action-timeout | keeper, then the node's loop |
| `node.start` | `lifecycle` | `{"name"}` | `{"applied"}`: `start` (the node's stop cleared), `retry` (a failed or recovering node configured afresh), or `unchanged` (it was neither stopped nor failed); `ear.conf` is never touched | unknown-node, node-disabled, not-restartable (a lost block), stopping | keeper (ordering only), then the main loop |
| `node.stop` | `lifecycle` | `{"name"}` | `{"applied"}`: `stop` (the node's stop set), or `unchanged` (already stopped); `ear.conf` is never touched | unknown-node, node-disabled, stopping | keeper (ordering only), then the main loop |
| `node.restart` | `lifecycle` | `{"name"}` | `{"applied"}`: `restart` (the configuration torn down and made again from empty, back to the wanted state), `retry` (a failed or recovering node configured afresh), or `unchanged` (a restart is already under way); `ear.conf` is never touched; 7.6 gives each phase | unknown-node, node-disabled, not-restartable, stopping | keeper (ordering only), then the main loop |
| `node.disable` | `lifecycle` | `{"name"}` | `{"applied"}`: `disable` (the node's own `disabled` line replaced by `disabled true;`, or `disabled true;` written first in its block), or `unchanged` (already disabled; nothing written) | unknown-node, shared-definition, file-invalid, stopping | keeper |
| `node.enable` | `lifecycle` | `{"name"}` | `{"applied"}`: `enable` (the node's own `disabled` line removed), or `unchanged` (no `disabled true;` to remove; a stopped node stays stopped) | unknown-node, shared-definition, file-invalid, stopping | keeper |

`node.update`'s result gains `applied` too (additive, with the `node.set` feature), and takes `dropRoutes` from feature
`reshape` (4.9). `node.disable` and `node.enable` are edits at the source (node-framework.md 6.3, R25, R28, R29): they
add or remove the attribute `disabled true;` inside the node's braces (`node card porch { disabled true; card USB2;
playback 2; }`; node-framework.md D-F8 to D-F11). Both edit only the named node's own `disabled` line, never the
definition behind its value (the owner's R34, "Only this node"): `node.disable` replaces that line with `disabled
true;` whatever it said (`disabled false;`, or `disabled $quiet;` whose `@set $quiet false;` other nodes share; they
stay enabled and the `@set` is untouched), or, when the node has no such line, writes `disabled true;` first in its
block, making the block for a node written without one; `node.enable` removes the node's own `disabled` line, and a
block it leaves empty. So the reply's `applied` is about the named node alone, and no other node changes. Only when
that line itself sits in a snippet another node uses is the edit refused `shared-definition`, with that line's place. `node.add` takes an optional `"disabled":
true` from feature `lifecycle` beside `settings`, and the keeper appends the node with `disabled true;` first in its
block; `node.update` keeps the node's `disabled` line as it is. `disabled` is a key the framework reserves, not a
setting: a `disabled` key inside `settings` on `node.add`, `node.update` or `node.set` is refused `invalid-settings`
("disabled is not a setting: use node.disable and node.enable, or \"disabled\": true on node.add"; node-framework.md
D-F12), and a change of it is always a disable or an enable, never a reconfiguration. `node.start`, `node.stop` and
`node.restart` are not edits (R28): the keeper only orders them with the requests around it, as it does `node.invoke`, and
the main loop applies them to the node's stop, which only the framework holds; they make no graph-file version, send no
`graphFile.changed` or `node.updated`, are answered in the main-loop turn that applies them, and work while `ear.conf`
has errors (node-framework.md D-F6). Their effect is told by `node.state` (its `wanted` and phase), and on standard output
by an `action` record (`node porch stop invoked`, section 9). `node.restart` is the framework's own restart (W12): a
full stop, a configuration from empty and a start back to the wanted state; it is not a type action, no type lists
it, and no type may declare an action named `start`, `stop`, `restart`, `disable` or `enable` (5.5).

**`types.get`**

```json
{"jsonrpc":"2.0","id":7,"method":"types.get","params":{"ifNoneMatch":"fnv1a64:0000000000000000"}}
```
```json
{"jsonrpc":"2.0","id":7,"result":{
  "protocol":1,
  "hash":"fnv1a64:9c2e41f07a3b5d18",
  "typeSchemas":[
    {"name":"gain",
     "settings":[
       {"key":"channels","kind":"wholeNumber","required":false,"default":2,"min":1,"max":64,"unit":"channels",
        "change":"restart","shapes":true},
       {"key":"gainDb","kind":"decimal","required":false,"default":0,"min":-90,"max":24,"unit":"dB",
        "decimals":1,"taper":"db","minIsSilence":true,"change":"live","live":{"smoothingMs":20}},
       {"key":"trimDb","kind":"decimal","required":false,"default":0,"min":-24,"max":24,"unit":"dB",
        "decimals":1,"taper":"linear","change":"live","live":{"smoothingMs":20},"indexed":{"by":"channels","from":1}}],
     "inputs":["in"],
     "outputs":["out"],
     "status":[
       {"name":"clipping","kind":"flag","initial":false,"display":"panel","help":"an output went above full scale",
        "flapsAbove":{"transitions":10,"windowMs":10000}}],
     "events":[
       {"code":"clipped","severity":"warn","sentence":"channel {channel} clipped at {peakDb} dB",
        "args":[{"name":"channel","kind":"wholeNumber","min":1,"max":64},{"name":"peakDb","kind":"decimal"}]}],
     "actions":[
       {"name":"reset-clip","by":"type","target":"code","help":"clear the clipping flag","args":[]}]},
    {"name":"rtp",
     "settings":[
       {"key":"address","kind":"text","required":true,"change":"inPlace",
        "mustBe":"\"0.0.0.0\" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad",
        "pattern":"^[0-9]{1,3}(\\.[0-9]{1,3}){3}$"},
       {"key":"port","kind":"wholeNumber","required":true,"min":1,"max":65535,"change":"inPlace"},
       {"key":"format","kind":"text","required":true,"mustBe":"\"L16\" or \"L24\"","values":["L16","L24"],
        "change":"inPlace"},
       {"key":"channels","kind":"wholeNumber","required":true,"min":1,"max":64,"change":"restart","shapes":true},
       {"key":"rate","kind":"wholeNumber","required":false,"min":8000,"max":192000,"unit":"Hz",
        "defaultFrom":"graphFrequency","change":"inPlace"},
       {"key":"latencyMs","kind":"wholeNumber","required":false,"default":100,"min":1,"max":1000,"unit":"milliseconds",
        "change":"inPlace"}],
     "inputs":[],"outputs":["stream"],"status":[],"events":[],"actions":[]}
  ]}}
```

When `ifNoneMatch` equals the hash: `{"protocol":1,"hash":"fnv1a64:9c2e41f07a3b5d18","notModified":true}`.

- A setting row: `key`, `kind` (`text`, `wholeNumber`, `decimal`), `required`, and as they apply `default` or
  `defaultFrom` (an open enum: `graphFrequency`, `nodeName`: a default that depends on the graph or the node, as
  `rtp`'s `rate` and `sendspin`'s `name` have), `min`, `max`, `unit`, `decimals`, `taper` (`linear`, `log`, `db`: how a
  control maps its travel, formulas in 8.3), `minIsSilence` (the control shows the minimum as -inf), `live`
  (`{"smoothingMs"}`), `indexed` (`{"by": <the count row's key>, "from": 1}`: the keys are `<key>.1` to `<key>.<n>`),
  `onlyWithout` (a key), `mustBe` (the row's own prose), and the text hints `values`, `pattern`, `maxLength`, and
  `help`. Text hints are documentation for controls: the row's parse function stays the judge, and the all-type tests
  check that every `values` entry parses (5.9). The all-type tests also check that every optional row without a
  `default` has a `defaultFrom` or a `help`.
- **How a change to the row applies** (node-framework.md 7.1, the home of the marks), on every row:
  `change` is `live` (a retune: the audio ramps, nothing restarts), `inPlace` (the node applies it while it runs; its
  outputs may be held silent for a while, and the node may still restart for a value it cannot take, saying why), or
  `restart` (the node's configuration is torn down and made again behind the same block). `shapes: true` marks a row whose change can
  change the node's ports (a reshape, node-framework.md 6.7), whatever its `change`; such a change can strand routes
  (4.9); it is in place when every changed key's `change` is `inPlace` or `live`, as the plan judges it. Both are open
  enums or flags a client only displays: ear's plan is the judge, and `dryRun` gives the exact outcome of one edit
  (4.9). The editor's hints come from these alone (8.2).
- **`reconfigureMs`**, on the type: how long a reconfigure of this type may take before it falls back to a restart
  (the spec's `.lifecycle`, node-framework.md 6.10; 10000 unless the type declares longer, at most 60000), so the editor
  can say how long "reconfiguring" may last.
- `inputs` and `outputs` are the public banks' names, in declaration order. Members come from settings, so the schema
  does not list them; inside banks are never on the wire.
- No type's `settings` lists `disabled`: it is a key the framework reserves on every node (node-framework.md D-F9,
  R29), never a row, so the schema, `settingKinds()` and the hash are as they would be without it. A client learns it
  from the `lifecycle` feature (`node.disable`, `node.enable`, `node.add`'s `disabled`), and the editor's settings
  form never offers it as a field.
- A status field: `name`, `kind` (`flag`, `wholeNumber`, `decimal`, `choice`, `text`), `initial` (`null` when it starts
  unknown), `display` (`panel`, `box`, `callout`), `flapsAbove` (`{"transitions","windowMs"}`), and as they apply
  `min`, `max`, `unit`, `values` (a choice's), `maxBytes`, `help`. Its writer is not on the wire.
- An event: `code`, `severity` (`info`, `warn`, `error`), `sentence` (with `{arg}` places), `args` (each `name`, `kind`
  of `wholeNumber`, `decimal`, `flag`, `choice`, `text`, and `min`, `max`, `values` as they apply).
- An action: `name`, `by` (`type`; an open enum, kept for a later framework action), `target` (`code` for a trigger,
  `loop` for a call), `help`, `args` (setting rows, as above). No type lists `restart`: it is the framework's
  `node.restart`, under feature `lifecycle`, and the lifecycle verbs are reserved action names (5.5).
- The hash is opaque: FNV-1a 64 over the exact bytes of the `typeSchemas` encoding made at boot,
  written `fnv1a64:<16 hex>`. Clients only compare it. It is not a security measure.
- The graph's `types` (a list of type names, `protocol.ts:63` [V]) stays as it is; the schemas are `typeSchemas`, the
  name node-primitives.md section 14 uses.

**`node.set`** sets the named keys and leaves the others (a JSON merge patch of the node's settings: `null` removes a
key, which then reads its default; removing a required key is `invalid-settings` with `data.key` and detail `missing`).
It is `node.update` with the node's current settings merged, made at the source like every edit. `applied` is the
plan's outcome for the node (node-framework.md 6.6): `retune`, `reconfigure`, `restart`, `reshape` or `unchanged`
(nothing differed; nothing is written). `dryRun` and `dropRoutes`, and
what a reshape adds to the reply, are 4.9's.

```json
{"jsonrpc":"2.0","id":8,"method":"node.set","params":{"name":"amp","settings":{"gainDb":-12}}}
{"jsonrpc":"2.0","id":8,"result":{"applied":"retune"}}
```

**`gesture.begin`** opens a gesture on one live row of one node. Its reach is every node and key whose value is
defined at the same place in `ear.conf` (G1): `amp2`'s `trimDb.1` and `trimDb.2` share `@set $trim 0;`, so a gesture on
either moves both. The reply gives the reach and the range a control maps its travel over: every reached row must be
live and of one kind and unit; the range is the intersection of their ranges, `decimals` the smallest of theirs
(7.2). A reach that holds a row that is not live, or rows of another kind or unit, or whose ranges do not meet, is
refused `shared-definition`, with the place and the pair that does not fit (`data.place`, `data.with`); the operator
then edits the value with `node.set` or in the text pane, which change every user in one judged commit.

```json
{"jsonrpc":"2.0","id":9,"method":"gesture.begin","params":{"name":"amp2","key":"trimDb.1"}}
{"jsonrpc":"2.0","id":9,"result":{"gesture":"g5","value":0,
  "reach":[{"name":"amp2","key":"trimDb.1"},{"name":"amp2","key":"trimDb.2"}],"min":-24,"max":24,"decimals":1}}
```

A key that the node's type declares but `ear.conf` does not give reaches only that node, and its committed value is the
row's default. A node with no configuration (failed, starting, disabled) can be held: ticks set the node's live value,
which seeds its next configuration's slots, and the end commits as usual.

**`gesture.change`** moves the gesture's live value. `seq` counts from 1 per gesture; a tick whose `seq` is not above
the last taken is dropped. A tick whose value is the gesture's live value already keeps it alive and does nothing else
(the editor's keep-alive, 8.3). Sent as a notification it has no reply: a value out of range, or one the type's `check`
refuses with the other settings (6.6), is dropped and the owning session alone gets
`gesture.changed {"phase":"tick-refused","code":"invalid-settings","key":...,"detail":...}`, at most once a second per
gesture. Sent with an id it is answered `{}` or refused. The owning session alone can move it; any other session gets
`unknown-gesture`.

```json
{"jsonrpc":"2.0","method":"gesture.change","params":{"gesture":"g4","seq":17,"value":-4.25}}
```

**`gesture.end`** commits. `value`, when given, is a last tick; without it, the last live value is committed. The value
is rounded to the reach's `decimals` (G5: half away from zero, on the shortest decimal text of the number; a zero is
written `0`, never `-0`) and written at its definition; the result says what was committed. A value the rows refuse is
refused `invalid-settings` and the gesture stays open, so the client can send a good end or cancel. Any other refusal
(`stopping`, the desired graph's) aborts the gesture (`refused`), and the reply carries the refusal.

```json
{"jsonrpc":"2.0","id":10,"method":"gesture.end","params":{"gesture":"g4","value":-3.5}}
{"jsonrpc":"2.0","id":10,"result":{"value":-3.5}}
```

An end whose value equals the committed one writes nothing and still closes the gesture.

**`gesture.cancel`** aborts the caller's own gesture: every reached slot returns to the committed value
(`gesture.changed {"phase":"aborted","reason":"cancelled","value":<committed>}`).

```json
{"jsonrpc":"2.0","id":11,"method":"gesture.cancel","params":{"gesture":"g4"}}
{"jsonrpc":"2.0","id":11,"result":{}}
```

**`node.invoke`** runs one of the type's actions. A trigger is answered once it is handed to the code (not when the code
took it); a call is answered with its handler's outcome. `restart` is not an action: it is `node.restart`.

```json
{"jsonrpc":"2.0","id":12,"method":"node.restart","params":{"name":"desk"}}
{"jsonrpc":"2.0","id":12,"result":{"applied":"restart"}}
{"jsonrpc":"2.0","id":13,"method":"node.invoke","params":{"name":"amp","action":"reset-clip"}}
{"jsonrpc":"2.0","id":13,"result":{}}
{"jsonrpc":"2.0","id":14,"method":"node.invoke","params":{"name":"kitchen","action":"forget-pairing","args":{"server":"ma-7f3c"}}}
{"jsonrpc":"2.0","id":14,"result":{}}
```

### 4.3 The graph, and notifications

`graph.get` and `graph.subscribe` answer today's Graph with three members more at the top and, per node, the health
members of 4.8 and five more:

```json
{"protocol":1,
 "features":["lifecycle","companions","reshape","types","status","actions","node.set","live","gestures"],
 "typeSchemaHash":"fnv1a64:9c2e41f07a3b5d18",
 "graphFrequency":48000,"blockSize":256,"realtime":true,"scheduling":"SCHED_FIFO 88",
 "types":["card","file","gain","rtp","sendspin","white"],
 "nodes":[
   {"name":"amp","type":"gain","settings":{"gainDb":-6},"inputs":["in.1","in.2"],"outputs":["out.1","out.2"],
    "state":"running","phase":"running","wanted":"running","blockLost":false,
    "status":{"clipping":false},
    "gestures":[{"gesture":"g4","key":"gainDb","value":-4.25}],
    "diagnostics":[],
    "companions":[]},
   {"name":"fpp","type":"rtp","settings":{"address":"0.0.0.0","port":5006,"format":"L24","channels":2},
    "inputs":[],"outputs":["stream.1","stream.2"],
    "state":"reconfiguring","phase":"reconfiguring","wanted":"running","blockLost":false,"keys":["port"],
    "status":{},"gestures":[],"diagnostics":[],
    "companions":[{"name":"fpp.rtp","kind":"rtp-receiver","bank":"from_rtp","ready":true,"retiring":true},
                  {"name":"fpp.rtp","kind":"rtp-receiver","bank":"from_rtp","ready":false}]},
   {"name":"kitchen","type":"sendspin","settings":{"server":"ws://192.168.1.10:8927/sendspin","name":"Kitchen"},
    "inputs":["to.left","to.right"],"outputs":["from.left","from.right"],
    "state":"running","phase":"running","wanted":"running","blockLost":false,
    "status":{"clientId":"5d0c...","connection":"connected","url":"ws://192.168.1.10:8927/sendspin","listenPort":0,
              "instance":"","serverName":"Music Assistant","paired":false,"pairingCode":"123-456","pairingHeld":false,
              "stream":"none","streamRate":0,"streamChannels":0,"streamBits":0,"sending":false,"sendingRate":0},
    "gestures":[],
    "diagnostics":[{"code":"ear.status-flapping","subject":"connection","active":true,"count":2,
                    "t":"2026-11-08T10:02:11.120400Z","transitions":14,"windowMs":10000,
                    "msg":"node kitchen connection flapping: 14 changes in 10s"}],
    "companions":[{"name":"kitchen.from_server","kind":"source","bank":"from_server","ready":true,"shortfall":0},
                  {"name":"kitchen.to_server","kind":"sink","bank":"to_server","ready":true,"dropped":0}]}],
 "routes":[]}
```

- `features`: 4.1.
- `state`, `phase` and the members each phase carries (`cause`, `keys`, `reason`, and slice 8's): 4.8.
- `wanted`: `running`, `stopped` (the node's stop, held in memory) or `disabled` (`disabled true;` in its block in `ear.conf`) (4.8).
- `disabled`: `true` when the node's block in `ear.conf` says `disabled true;`, absent otherwise (`disabled false;` and
  no line alike); part of the node's description beside `settings`, never inside it, so `node.added` and `node.updated` carry it too (W9).
- `blockLost`: the node's block is gone (I1); it cannot restart until it is removed and added, or ear restarts.
- `status`: every declared field, each at its initial value until the node writes it; reset to the initial values when
  its configuration closes (I6), and the reset is sent (below); kept through an in-place change, a stop and a start. Always present (`{}` for a type with
  none).
- `gestures`: the open gestures that move one of this node's keys, with their live value. The committed value is in
  `settings` (or the row's default). Always present.
- `diagnostics`: one entry per code and subject (4.6), each with `active`, `count` (how many times that code and
  subject was raised or became active since the node was added), `t`, `msg` and its fields. Always present.
- `companions`: the node's companions (4.10). Always present (`[]` before it is configuring, or for a type with none).
- One main-loop turn makes the whole snapshot, so status, health, gestures, diagnostics and companions agree with each
  other.

`node.added` gains the health members (`state` `starting`, `phase` `added`, `wanted`, and `waitingFor` when the node
waits for an older one of its name; for a node added disabled, `state` and `phase` `disabled`), `blockLost`, `status` (the initial values), `gestures` (`[]`), `diagnostics`
(`[]`) and `companions` (`[]`), so a client need not fetch the schema to show a new node. `node.state` is 4.8's.
`node.updated` gains `applied` (any outcome but `replace`, which stays `node.removed` and `node.added`, as today:
`disable`, `enable` and `stored` among them), `disabled`, and, on a reshape, `ports` (4.9). `node.start`, `node.stop`
and `node.restart` send no `node.updated`: they change no description. Until step L3, a change of ports on a disabled
node is told as `node.removed` then `node.added` (with `disabled: true`), its reply `applied: "stored"`
(node-framework.md 13.1, D-R23); from L3 it is a `node.updated` with `ports`.

| Notification | Params | From | Rate |
|---|---|---|---|
| `node.statusChanged` | `{"name","fields":{name: value},"reset"?: true}`: the fields that changed since the last one; with `reset`, every field at its initial value | the status drain | at most 30 a second per node |
| `node.eventRaised` | `{"name","code","severity","t","args":{...},"msg"}` | the status drain | 32 at once, then 1 a second per node |
| `node.diagnostic` | `{"name","code","subject","active","count","t","msg", the code's fields}` | the framework | per code (4.6) |
| `gesture.changed` | `{"gesture","phase", and by phase: "reach","value","reason","code","key","detail"}` | `Gestures` | `change`: at most 30 a second per gesture |
| `node.state` | 4.8 | `NodeHost` | one whenever any health member changes (`state`, `phase`, `wanted`, `cause`, `waitingFor`, `keys`, `next`, `reason`), never coalesced |
| `node.companionsChanged` | 4.10 | `NodeHost` | on each change of the list; counters at most once a second per node |

(A gesture's `phase` and a node's `phase` are two closed vocabularies on two notifications; neither is read for the
other.)

```json
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","fields":{"connection":"connected","serverName":"Music Assistant","paired":false}}}
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","fields":{"pairingCode":"123-456"}}}
{"jsonrpc":"2.0","method":"node.eventRaised","params":{"name":"kitchen","code":"pairing-failed","severity":"warn","t":"2026-11-08T10:01:52.004211Z","args":{"reason":"pairing-code-mismatch"},"msg":"pairing failed: pairing-code-mismatch"}}
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","reset":true,"fields":{"clientId":"","connection":null,"url":"","listenPort":0,"instance":"","serverName":"","paired":false,"pairingCode":"","pairingHeld":false,"stream":null,"streamRate":0,"streamChannels":0,"streamBits":0,"sending":false,"sendingRate":0}}}
{"jsonrpc":"2.0","method":"node.diagnostic","params":{"name":"kitchen","code":"ear.status-flapping","subject":"connection","active":true,"count":2,"t":"2026-11-08T10:02:11.120400Z","transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"begin","value":-6,"reach":[{"name":"amp","key":"gainDb"}]}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"change","value":-4.25}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"end","value":-3.5}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"end","value":-3.5,"reason":"session-closed"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"aborted","reason":"cancelled","value":-6}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"aborted","reason":"hand-edit"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"tick-refused","code":"invalid-settings","key":"gainDb","detail":"must be a number of dB from -90 to 24"}}
```

- **One notification per gesture**, not per reached key: `begin` carries the reach; later phases name only the
  gesture, and a client applies them to every node whose `gestures` hold that id. A reach of 64 trims costs one message
  per tick, not 64.
- **Own echoes.** The session that owns a gesture gets no `begin` and no `change` for it (it shows its local value);
  it gets `end`, `aborted` and `tick-refused`. Every other session gets `begin`, `change`, `end` and `aborted`, never
  `tick-refused`.
- `phase: "end"` carries `reason` when ear ended the gesture for its owner (the owner's G4, R32; 7.3): `session-closed`
  (the owner's socket closed) or `idle` (no message about it from its owner for 30 s, 6.5). Its `value` is what was
  committed: the last value heard. An end the owner sent carries no `reason`.
- `phase: "aborted"` carries `reason`: `hand-edit` (a related disk change, 7.3), `file-saved` (a related
  `graphFile.set`), `cancelled`, `refused` (its end was refused: the end's reply carries the code; for an end ear sent
  for a closed or silent session, the refusal is the `aborted` alone), `node-removed` (removed or replaced),
  `key-removed` (a reshape removed a key it reaches, 7.7). It carries `value`, the committed value the slots returned
  to, except on `hand-edit` and `file-saved`, where the slots take the value the new file runs, told by the
  `node.updated` that follows (7.3).
- **A configuration's status reset.** When a node's configuration closes (I6: its teardown's step 5, or its leak past
  the close deadline), the node's status view goes back to every field's initial value, and the drain sends one
  `node.statusChanged` with `reset: true` and every field, after that configuration's last values. A client replaces
  the node's status with it. An in-place change, a stop and a start send no reset: the configuration goes on (6.7).

**Order**, added to batch2.md section 6's rules:

1. Within a node, the drain sends a `node.statusChanged` with the fields written so far before any `node.eventRaised`
   that followed them in the same writer's order, so `connection = connected` reaches a client before `paired`.
2. A configuration's last status values reach a client before the `node.state` its loss causes (the loss crosses after them in
   the node's queue, 6.3), and its reset follows its last values.
3. A gesture's begin: `gesture.changed` `begin` to the other sessions, then the reply to the owner, in one main-loop
   turn. A gesture's end: `graphFile.changed`, then `node.updated` (the committed settings, `applied: "retune"`), then
   `gesture.changed` `end`, then the reply.
4. An abort by a related hand edit or save: `gesture.changed` `aborted` for each gesture the save is related to (7.3),
   then `graphFile.changed`, then that change's graph events; the slots settle last, in the same turn. Gestures the
   save is not related to send nothing.
5. A restart, whatever its cause: `node.state` phase `stopping` (cause `restarting`), then the reply if a request
   caused it, all in the commit's turn; then, once the old configuration closed (I5), its last status values and its
   reset; then `node.state` phase `configuring`; `configured`, `starting` and `running` later. A failed node has no
   configuration to close, so `configuring` comes at once. `node.restart` is ordered the same way, its reply after
   `stopping` (a failed node's after `configuring`). A stop: `node.state` `stopping`, the reply, then `stopped`;
   a start: `starting`, the reply, then `running` (neither sends `graphFile.changed` or `node.updated`). A stop or start
   that changes only `wanted` (`node.stop` on a failed node, `node.stop` on a node still `added` or `configuring`) sends
   one `node.state` with the new `wanted` and the same phase, then the reply. A disable:
   `graphFile.changed`, `node.updated` (`disabled`, `applied: "disable"`), `node.state` `stopping` (cause
   `disabling`), the reply; then the configuration's last status values and its reset, and `node.state` `disabled`. An
   enable: `graphFile.changed`, `node.updated`, `node.state` `configuring` (cause `enabling`), the reply; then as a
   start.
6. An in-place change: `node.updated` (with `applied: "reconfigure"`), then `node.state` phase `reconfiguring`, then
   the reply, in the commit's turn; status the node writes during it; then `node.state` `running` (or `stopped`). A retune of the same node during it changes no `node.state`.
7. A reshape: the dropped routes' `route.removed`, then `node.updated` with `ports`, then rule 5. The block's new
   ports reach `route.status` only after `node.updated` named them.
8. Companions: a `node.companionsChanged` that shows a companion ready precedes the `node.state` that its readiness
   allowed (`running`, or the end of a reconfigure). A `node.state` is never held back for a companions notification:
   a failure is told at once, and the list empties as the teardown goes on.

### 4.4 What a client folds, and what it ignores

| Notification | Changes | Ignored when |
|---|---|---|
| `node.statusChanged` | with `reset`: the node's `status` replaced; otherwise merged | the node is unknown |
| `node.state` | every health member (4.8) replaced: a member absent from it is absent now | the node is unknown |
| `node.companionsChanged` | the node's `companions` replaced | the node is unknown |
| `node.updated` | `settings` and `disabled`; with `ports`, the node's `inputs` and `outputs` replaced (handles redrawn in place) | the node is unknown |
| `node.eventRaised` | nothing in the graph; the activity list | the node is unknown |
| `node.diagnostic` | the node's `diagnostics`: the entry of the same `code` and `subject` replaced, `count` as sent | the node is unknown |
| `gesture.changed` | `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value` wherever the id is; `end` (with or without `reason`) and `aborted` remove it everywhere; `tick-refused` changes nothing (the owner's notice) | no node holds the id (except `begin`) |
| any other unknown method | nothing | always |

### 4.5 Error codes

All -32000, message `describe(...)` as today, `data.code` from this table. Every new code is also a case in
`test/protocol/errors.json`.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-settings` (today's, extended) | a value a row or the type's `check` refuses, from the plan (`node.set`, `node.update`, `gesture.end`) or from `Gestures` (`gesture.change`) | `key` (the full key, `trimDb.2`; absent for a rule across settings that names none), `detail` (the row's text without the node) |
| `parameter-held` | `node.set`, `node.update`, `gesture.begin`: a key it would change is held by an open gesture (another session's, or the caller's own: end it first) | `gesture`, `key` |
| `not-live` | `gesture.begin`: the key is not a live row of the node's type (or not declared at all) | `key` |
| `shared-definition` (today's, extended) | `gesture.begin`: the reach holds a key that is not live, or rows of another kind or unit, or ranges that do not meet | `place` (line, column), `with` (`{name, key}` that does not fit) |
| `file-refused` | `gesture.begin`: the graph refused the working copy's last hand edit, so the running version is not the file's; a gesture's end would be refused with it | |
| `unknown-gesture` | `gesture.change`, `gesture.end`, `gesture.cancel`: no open gesture of this session by that id (it ended, was aborted, or is another session's) | |
| `not-in-batch` | `gesture.change` or `gesture.cancel` inside a batch | |
| `unknown-action` | `node.invoke`: the type declares no such action | `action` |
| `invalid-args` | `node.invoke`: the arguments fail the action's table; message as a settings refusal | `key`, `detail` |
| `not-running` | `node.invoke` of a type's action: the node cannot take it now: no configuration (`added`, `disabled`, `failed`, `recovering`), one tearing down (`stopping` for a restart or removal), or one not yet configured (`configuring`: its loops hold outside work until configured, node-framework.md 6.4). A `stopped` node takes actions | `phase` |
| `not-restartable` | `node.restart` or `node.start`: the node's block is lost (I1); or (`node.restart` only) it has not been configured yet (phase `added`: it is waiting for an older node of its name, and will start by itself) | `reason` (`block-lost`, `not-started`) |
| `node-disabled` | `node.start`, `node.stop` or `node.restart`: the node is disabled in `ear.conf`; enable it first (`node.enable`) (node-framework.md D-F2) | |
| `ports-in-use` | `node.set`, `node.update`: the change would remove ports that wanted routes name, and `dropRoutes` is not set (4.9) | `ports` (the vanishing ports), `routes` (each `{from, to}`) |
| `restarted` | `node.invoke`: the node's configuration was torn down (a restart, a settings change) before the action ran | |
| `action-failed` | the handler refused; message: its text | `detail` |
| `action-timeout` | the handler did not answer in 10 s | |

`InvalidSettings` (`command_error.hpp:12-15` [V]: `node`, `detail`) gains `key`, set by the settings walk; `describe`
keeps today's text, `"{code} {node}: {key}: {detail}"` as `command_error.cpp:44-45` [V] formats it now from a detail that
starts with the key. The editor places a refusal under its field by `data.key` and never parses the message.

```json
{"jsonrpc":"2.0","id":15,"error":{"code":-32000,"message":"parameter-held amp gainDb by g4","data":{"code":"parameter-held","gesture":"g4","key":"gainDb"}}}
{"jsonrpc":"2.0","id":16,"error":{"code":-32000,"message":"not-live amp channels","data":{"code":"not-live","key":"channels"}}}
{"jsonrpc":"2.0","id":17,"error":{"code":-32000,"message":"unknown-gesture g4","data":{"code":"unknown-gesture"}}}
{"jsonrpc":"2.0","id":18,"error":{"code":-32000,"message":"unknown-action desk forget-pairing","data":{"code":"unknown-action","action":"forget-pairing"}}}
{"jsonrpc":"2.0","id":19,"error":{"code":-32000,"message":"action-failed kitchen forget-pairing: no pairing with ma-0000","data":{"code":"action-failed","detail":"no pairing with ma-0000"}}}
{"jsonrpc":"2.0","id":20,"error":{"code":-32000,"message":"not-restartable desk: block-lost","data":{"code":"not-restartable","reason":"block-lost"}}}
{"jsonrpc":"2.0","id":23,"error":{"code":-32000,"message":"node-disabled porch","data":{"code":"node-disabled"}}}
{"jsonrpc":"2.0","id":22,"error":{"code":-32000,"message":"ports-in-use desk: play.2 (1 route)","data":{"code":"ports-in-use","ports":["play.2"],"routes":[{"from":"kitchen:from.left","to":"desk:play.2"}]}}}
{"jsonrpc":"2.0","id":21,"error":{"code":-32000,"message":"shared-definition amp2 trimDb.1 at 3:6: also amp3 latencyMs","data":{"code":"shared-definition","place":{"line":3,"column":6},"with":{"name":"amp3","key":"latencyMs"}}}}
```

`web/src/protocol.ts` gains `ControlErrorCode` beside `CommandErrorCode` and `FileErrorCode`; C++ gains
`ControlRefusal` in `ear_control` beside `FileRefusal` (`graph_file_state.hpp:59` [V]), with `controlRefusalCode` the one
home of these strings.

### 4.6 The framework's node diagnostics

Each diagnostic is kept per **code and subject** (the subject is a field, a loop, a claim, a hook or a companion; empty
when the code has none). `active` says whether the condition holds now: a condition code (flapping, a claim wait)
becomes active and later inactive with the same code and subject; an observation code (a drop, a slow handler) is sent
with `active: false` and only joins the list. The editor badges active entries and lists all, and needs no per-code
knowledge to clear a badge.

| Code | Subject | Fields | When | Rate | Owner |
|---|---|---|---|---|---|
| `ear.close-overdue` | — | `detail` (what was still open, with loops) | a node's configuration did not close within the close deadline and was leaked (I5a; built [V: `node_host.hpp:45-52`]) | once per node | slice 1, wire here |
| `ear.loop-slow` | the loop's name | `ms` | a strand handler ran past 100 ms (node-primitives.md 6.4) | at most once per 10 s per loop | slice 3c, wire here |
| `ear.loop-source-left` | the loop's name | `sources` | a loop's state was destroyed with sources still armed on that loop, cancelled at teardown step 4 (node-primitives.md 6.4) | once per node and loop | slice 3c, wire here |
| `ear.claim-wait` | the claim (`tcp:8928`) | `holder`, `waitedMs` | active while a node waits for a claim a going configuration holds; inactive once granted or failed at 15 s (node-primitives.md 6.9) | on each change | slice 3c (node-primitives.md 6.9 names it and its text), wire here |
| `ear.close-late` | — | `detail` | a configuration leaked at the close deadline closed later; its claims are released (node-primitives.md 6.10) | once per node | slice 3c, wire here |
| `ear.status-flapping` | the field | `transitions`, `windowMs` | active once a field changes more than its `flapsAbove` within the window; inactive once a whole window passes at or below it | on each change, and at most once per window while active (with the new count) | here |
| `ear.events-dropped` | — | `heldBack` | events held back by the per-node rate (node-framework.md 8.3; it replaces slice 3c's `notes-dropped` note) | at most once a second per node | here |
| `ear.records-dropped` | `status`, `realtime` or `queue` | `count` | `status` records held back on stdout (6.5), or records a node's realtime ring or queue had no room for | at most once a second per node and subject | here |
| `ear.hook-slow` | the hook (`configure`, `start`, `stop`) | `ms` | a hook ran past 20 ms on the main loop (node-framework.md 6.4, I11) | once per node and hook | slice 3a (its code and bound are node-framework.md's), wire here |
| `ear.reconfigure-fell-back` | — | `reason` (`restart-asked`, `deadline`, `resource-refused`, `resource-lost`, `failed`, `restart-requested` (`node.restart`; revision 5's `restart-action`)), `keys`, `detail`, `elapsedMs` | an in-place change ended in a restart (node-framework.md 6.6 step 5, T11) | once per change | slice 3d, wire here |
| `ear.reconfigure-superseded` | — | `keys` (the abandoned ones), `next` (the newest) | a newer change, a stop or a removal superseded an in-place change before its commit point (node-framework.md T18) | once per supersession | slice 3d, wire here |
| `ear.retiring-lost` | the companion | `detail` | a resource being replaced in an in-place change was lost before its replacement took over; the node goes on (node-framework.md 6.6 step 3) | once per resource | slice 3d, wire here |
| `ear.sink-dropped` | the companion (`rec.writer`) | `frames` (since the last), `totalFrames` | a sink companion's ring had no room for frames (node-framework.md 9.6) | at most once per 10 s per companion while it drops | slice 3b, wire here |
| `ear.source-shortfall` | the companion (`kitchen.from_server`) | `frames`, `totalFrames` | a source companion played zeros for want of frames while its writer was not idle (9.6) | at most once per 10 s per companion while it falls short | slice 3b, wire here |
| `ear.realtime-overrun` | — | `codeUs`, `cycleUs` | reserved for a code that ran past its cycle; not built here | — | later |
| `ear.fade-unacknowledged` | — | `ms` | a planned silence's fade was not acknowledged within 100 ms, so the barrier was taken unfaded (node-framework.md 6.2, I5a) | once per teardown | slice 3a, wire here |
| `ear.barrier-overdue` | — | `ms` | a barrier ear posted to the data loop did not complete within 5 s; the node's configuration and block are leaked (node-framework.md I5a) | once per node | slice 3a, wire here |

Three framework diagnostics name no node, so they are standard-output records only (a `diagnostic` record with no
`node`), never `node.diagnostic`: `ear.main-loop-stalled` (`ms`, and `in`: the PipeWire call or hook running, when
known), active while the main loop's heartbeat is older than 500 ms, written from the watchdog's own thread while the
stall lasts; `ear.main-loop-resumed` (`ms`, the stall's length); and `ear.data-loop-wedged` (once: no further engine
change is applied, and ear's stop leaves through `std::_Exit`). The control port's graph requests wait on the same main
loop, so the wire could not carry them in time (node-framework.md 6.3; round 4's F1, F6).

Slice 8 adds `ear.health-flapping`, `ear.recovery-attempt` and `ear.readiness-missed` in the same shape (R8, 11).

**No framework status counters.** Revision 2 had two status fields the framework declared (`ear.feedShortfall`,
`ear.handoffDropped`). Feeds and handoffs are gone (node-framework.md 9.6); their counts belong to the companion that
crossed, so they are members of its entry in `companions` (4.10), sampled once a second, and its drops are the
observation diagnostics above (K1, K2). A node's `status` therefore holds only its type's fields, and the `ear.` prefix
names only codes. A sink's drops stay on the node's `removed` record as `droppedFrames` (today's member, the sum over
its sinks) (9.1).

### 4.7 Fixtures

Each new file keeps today's shape (`{"description","requests","messages","graph","file"?}`), read by `ear_control`'s
tests and the editor's reducer tests.

| File | Covers |
|---|---|
| `test/protocol/types.json` | `types.get` with no hash, a stale hash and the current one; the schema of every built-in type, each row's `change` and `shapes`, a `defaultFrom` row |
| `test/protocol/status.json` | a snapshot with status (a choice at `null`); `node.statusChanged` merges; `node.eventRaised` after a flush, with `severity`; a configuration's reset (`reset: true`) and then a new configuration's first fields; status kept across a stop and a start |
| `test/protocol/diagnostics.json` | two fields flapping at once (two entries), one settling (`active: false`) while the other stays active; `count` as sent; an observation code; an unknown code drawn generically |
| `test/protocol/gestures.json` | begin (no echo to the owner, `begin` to another session first, then the reply), changes (one stale, one keep-alive changing nothing), end with its order; a reach of two keys; a reach of 64 keys as one notification per tick; a reach refused `shared-definition` (a non-live row; a row of another unit); `parameter-held`; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings` leaving the gesture open; an abort by a related hand edit (no value) with its order; an abort by a related `file-saved`; an unrelated hand edit and an unrelated `graphFile.set` leaving the gesture open (no `gesture.changed`), then its end; an end for a closed session (`end` with `reason: "session-closed"` to the other sessions) and for a silent one (`reason: "idle"`, to the owner too); `unknown-gesture` after the abort; `not-in-batch` |
| `test/protocol/lifecycle.json` | a node's whole life as notifications: `node.added` (`added`), `configuring`, `configured`, `starting`, `running`; a stop and a start (`node.stop`, `stopping`, `stopped`, `node.start`, `starting`, `running`, with no `graphFile.changed` or `node.updated`); `node.stop` on a `failed` node (one `node.state`, phase `failed`, `wanted: "stopped"`, then the reply) and on a node in `configuring` (one `node.state`, phase `configuring`, `state: "starting"`, `wanted: "stopped"`), each a health change with no phase change; a node configured while stopped (`starting` until it rests, then `stopped`); a disable and an enable (`graphFile.changed`, `node.updated` with `disabled` and `applied`, `stopping` with cause `disabling`, the reset, `disabled`; then `configuring` with cause `enabling` and `running`); a disable of a node whose `disabled $quiet;` other nodes share (only that node's `node.updated` and `node.state`s); a node added disabled (no `starting`); `node.start` on a disabled node refused `node-disabled`; a wait for an older node of the name (`waitingFor` `older-node`); an in-place change with `keys`, then `running`; a failed node retried by `node.start`; `node.restart` on a running node (`stopping` with cause `restarting`, the reply `applied: "restart"`, the old configuration's reset, `configuring`), on a failed node (`applied: "retry"`), during a restart (`unchanged`), and refused (`not-restartable` with `block-lost` and `not-started`; `node-disabled`); `node.gone` and `node.stopped` after a removal; a reconfigure that falls back (`restarting`, cause `fell-back`); a restart by a restart-only row, with the old configuration's reset between `restarting` and `configuring`; a failure (`reason`), then a retry; an unknown `phase` and an unknown `cause.kind` drawn generically |
| `test/protocol/companions.json` | a snapshot with each kind; `node.companionsChanged` as companions become ready; a make-before-break (`retiring`); counters moving; the list emptied by a teardown; the list kept across a stop and a start |
| `test/protocol/node-set.json` | merge, `null` removing a key, a missing required key (`data.key`), each `applied` value, `parameter-held`; `dryRun` for each outcome (nothing committed, no notification); a reshape refused `ports-in-use`, then the same with `dropRoutes` and its `route.removed`, `ports` and `routesDropped`; `node.update` with `dropRoutes` |
| `test/protocol/actions.json` | `node.restart` during an in-place change (node-framework.md D-B6); an in-place change superseded by a newer change or a stop; a trigger; a call; `not-running` with its `phase`; every refusal of 4.5 that an action can get |
| `test/protocol/unknown-values.json` | an unknown state, node phase, `cause.kind`, companion kind, route status, status field, event code, diagnostic code, gesture phase, `applied` value, `change` value, member and notification; the graph after them |
| `test/protocol/errors.json` | every new code of 4.5 with its `data`; an unknown request member (-32602 with `data.pointer`) |
| `test/protocol/snapshot.json`, `edits.json` | regenerated: `protocol`, `features`, `typeSchemaHash`, and each node's `phase`, `wanted`, `disabled`, `blockLost`, `status`, `gestures`, `diagnostics`, `companions` (additive); `node.update`'s `applied`; `node.state`'s new members |
| `test/protocol/output-records.json` | the records of section 9; its log case "a node's own log, cut at its bound" loses its `"run": 2` member (below) |

**The fixture's `run` goes, and `v` stays 1** (round 4's M7). `output-records.json` carries `"run": 2` on one `log`
record today, because `status_log.cpp:197` writes `run` when `LogEntry::run` is set [V]. No production code sets it:
only `status_log_test.cpp:157` does [V], so no record ear has ever written carried it. Removing it from the fixture,
the encoder and `LogEntry` (step L1; node-framework.md 13.2) therefore removes nothing a reader could have seen, and is
not the breaking change 4.1's rule is about; `v` stays 1.

### 4.8 Health on the wire: every lifecycle state

The states, the operations and what the framework does in each are node-framework.md 6.2 and 6.3; this section is the
wire's side of them, and `node.state` its one notification. Health is per node (R16): a node's notification says
nothing about another node, and no state of one node changes another's. There is no run on the wire (R20): the
framework drops what a node's earlier configuration says before it is sent (node-framework.md I6), so a client needs no
counter to order health.

```json
{"jsonrpc":"2.0","method":"node.state","params":{"name":"fpp","state":"reconfiguring","phase":"reconfiguring","since":"2026-11-08T10:06:00.120000Z","wanted":"running","keys":["port"]}}
```

**Members.** Every `node.state` carries `name`, `state`, `phase`, `since` and `wanted` (`running`, `stopped` or `disabled`), and the
members its phase gives below; a member not given is absent now (4.4 replaces every health member). Node snapshots and
`node.added` carry the same members at the top of the node.

- `state`: the colour, a short open set (the owner's answer to A1, node-framework.md R22): `starting`, `running`,
  `reconfiguring`, `stopping`, `stopped`, `disabled`, `failed`, `recovering`. Today's four keep their meaning, so a script that
  waits for `running` keeps working.
- `phase`: the exact state, an open set: `added`, `configuring`, `configured`, `starting`, `running`, `reconfiguring`,
  `stopping`, `stopped`, `disabled`, `failed`, `recovering`. A client explains by it and never colours by it.
- `since`: when this phase began (UTC to the microsecond, as `t`).
- `wanted`: what the graph wants of the node (node-framework.md 6.3): `disabled` when its block in `ear.conf` says `disabled true;`;
  else `stopped` while the node's stop is held (set by `node.stop`, in memory only, forgotten when ear restarts);
  else `running`. A node `failed` with `wanted: "stopped"` stays failed until asked (a `start` retries it and clears
  the stop); `wanted` changes on `node.state` alone when `node.stop` or `node.start` is applied, with the phase it
  causes, and a `node.state` is sent for it even when the phase does not change (4.3's rule 5).
- `cause` (`stopping`, and `configuring` after a restart, a retry or an enable): why, an object with an open `kind`
  (below).
- `keys` and `next` (`reconfiguring` only): the keys this in-place change applies; the keys changed since its commit
  point, which the node takes when it ends (node-framework.md 6.3). A change before the commit point supersedes it
  instead (T18): a new `node.state` `reconfiguring` with the new `keys` (or a `stopping` with cause `restarting` if the
  newest change needs one), after an `ear.reconfigure-superseded` diagnostic.
- `waitingFor` (`added`, `configuring`, `configured`, `starting`, only once the phase has lasted 2 s): what the node
  still waits for, a list of objects with an open `kind`: `older-node` (`name`: a removed or renamed node of its name
  still closing, I7), `claim` (`subject`, `holder`, and `leaked: true` when the holder is a leaked configuration, which
  starts this node when it closes, R15), `store`, `recording`, `loop`, `companion` (`name`), `consumer` (a sink's
  `begin`), `block` (the engine has not made the block ready). Sent again when the list changes, at most once a second.
- `reason` (`failed` and `recovering`): today's `{code, detail}` (`edits.json`'s failed `node.state` [V]), kept by name
  and meaning so today's editor's tooltip (`EarNode.tsx:31` [V]) keeps working (W2, decided). `code` is an open set
  whose home is node-framework.md 6.2's table: today's `start-failed` and `engine-refused`, and `lost` (a node that had
  reached `running` or `stopped` said it cannot go on), which `protocol.ts`'s `FailureCode` (`protocol.ts:15` [V])
  gains in step L1. A companion's failure names it in `detail` (node-framework.md 9.6).
- `attempt`, `retryInMs` (`recovering`), and `lastFailure` (`{code, detail, t}`, kept after the node runs again):
  slice 8's, under feature `recovery` (step R8).

| `phase` | `state` | Entered (node-framework.md 6.3) | Members beyond the five | `cause.kind` | The editor shows (8.6) |
|---|---|---|---|---|---|
| `added` | `starting`, whatever `wanted` says (a node shows `stopped` only once it rests there) | T1: added; the block is made, silent, once an older block of its name is gone | `waitingFor` after 2 s (`older-node`) | — | "starting", with what it waits for |
| `configuring` | as `added` | T2, T12, T15, T22: `configure` called, resources readying | `waitingFor` after 2 s; `cause` after a restart, a retry or an enable | `settings` (`keys`), `reshape` (`added`, `removed`), `action`, `fell-back` (`reason`, `detail`), `retry`, `recovery` (`attempt`), `enabling` | "configuring", with what it waits for |
| `configured` | as `added` | T3: every resource ready | — (momentary) | — | as `configuring` |
| `starting` | `starting` | T4, T8: wanted running | `waitingFor` after 2 s (`block`) | — | "waiting for the engine" |
| `running` | `running` | T5, or T10 (an in-place change ended) | — | — | the node's own status line |
| `reconfiguring` | `reconfiguring` | T9 | `keys`, `next` | — | "reconfiguring port"; every port's meter shows what is held (W3) |
| `stopping` | `stopping` | T6, T11, T17, T20 | `cause` | `stop`, `disabling`, `restarting` (with the restart's own kind, as `configuring`'s), `removing` | "stopping", "disabling", "restarting: ports changed", "removing" |
| `stopped` | `stopped` | T7, or T4 when wanted stopped | — | — | "stopped until Start or a restart of ear", Start and Disable |
| `disabled` | `disabled` | T19 (added disabled), T20's end, T21 | — | — | "disabled in ear.conf", and Enable |
| `failed` | `failed` | T13, T14 | `reason` | — | the failure, and Start (a retry) unless the block is lost |
| `recovering` | `recovering` | T16 (slice 8) | `reason`, `attempt`, `retryInMs` | — | "recovering in 4 s (attempt 3): card lost" |

- **Removed is not a state on the wire.** The node is gone once its configuration closed or leaked and its block went:
  today's `node.stopped {name, droppedFrames}` (`events.cpp:80` [V]) says so; protocol 1 adds `node.gone` with the
  same params (feature `lifecycle`), since `stopped` is now a resting state, and sends both, `node.stopped` first: a
  client never tells ear its protocol, so ear cannot send the old name to old clients alone (round 4's M8).
  `node.stopped` is deprecated and goes at protocol 2. A node removed by an edit is `node.removed` at the commit and
  leaves the graph at once, as today; its `stopping`, `node.stopped` and `node.gone` then name a node the client no
  longer holds, and are ignored (4.4), as `graph.ts:77` ignores `node.stopped` today [V]. At ear's stop every node shows `stopping` before the socket closes with 1001.
- **One notification whenever any health member changes**, never coalesced (W1, decided; round 4's M4): a phase, but
  also `wanted` alone (`node.stop` on a failed node), `cause`, `waitingFor`, `keys`, `next` or `reason`. A normal start
  is `node.added` and three or four `node.state`s (`configuring`, `configured`, `starting`, `running`) within
  milliseconds. Health changes are rare events; coalescing would hide the one a node is stuck in.
- **A retune changes no health.** A `retune` outcome, a gesture and a status write never send `node.state`.
- **Order.** Health is ordered per node by the session's one queue (4.3's rule 8); nothing a closed configuration says
  reaches the wire (node-framework.md I6).
- **Today's clients** (protocol 0, the editor before step L1) see `state` as before plus four values they do not know,
  `reconfiguring`, `stopped`, `disabled` and, with slice 8, `recovering`; `EarNode.tsx:28` [V] then builds the class
  `state-reconfiguring`, `state-stopped` or `state-disabled`, which have no style and read as plain text. Nothing they rely on changes
  meaning, so this is additive within protocol 1 (4.1).

### 4.9 Changes while the node runs: `applied`, `dryRun` and reshape

What a committed edit does to each node is node-framework.md 6.6's plan outcome; this section is how a client sees it,
before and after.

- **`applied` is the plan's outcome, at the commit** (X3, X7): `retune`, `reconfigure`, `restart`, `reshape`,
  `disable`, `enable`, `stored` (any settings change to a node that stays disabled: written, nothing runs, its block
  reshaped if its ports changed) or `unchanged` on the reply of `node.set`, `node.update`, `node.disable` and
  `node.enable` and on `node.updated`. A `reshape` also carries `inPlace`:
  `true` when every changed key is `.inPlace()` or `.live`, so the node reshapes while it runs and keeps its configuration;
  `false` when it restarts (node-framework.md 6.6, 6.7). So a client knows, before the node acts, the one thing an
  operator asks of a change: whether the node restarts. It says what the change asks of the
  node, judged from the rows' marks and the ports alone, the same for every client and for a dry run. It is never
  rewritten by what the node then does: a node that has not yet reached `running` or `stopped` takes any change as a restart (node-framework.md 6.3), and
  `configure` may answer `restart`; both show in `node.state` (`stopping` with cause `restarting`, then `configuring`), which is the one
  place for what happened. `replace` is never an `applied`: it stays `node.removed` and `node.added`.
- **The reply comes at the commit**, never when the node runs again (operations are asynchronous, node-framework.md
  6.1, R21): an in-place change may last seconds (its deadline is 10
  s, a type may declare up to 60 with `.lifecycle`, shown as `reconfigureMs` in the schema, 4.2), and the keeper takes no next request until the reply's step is done (batch2.md section 6,
  the batch rule). A client that must wait for audio waits for the first `node.state` after the reply (2.2's `--until`): a session's
  replies and notifications share one ordered queue.
- **`dryRun: true`** on `node.set` (feature `dryRun`): the keeper makes the candidate as for a real edit and posts it;
  the main loop plans it and answers the outcome without committing, telling, writing or changing the working copy.
  The reply is `{"applied", "ports"?, "routes"?}`: on a reshape, the ports it would add and remove, and every wanted
  route that names a vanishing port. Every refusal a real edit would get, it gets (a dry run of a held key is
  `parameter-held`). A dry run is a read: it may be stale by the next request, and the real edit is judged afresh.
- **A reshape and its routes** (feature `reshape`). Wanted routes must name ports the node will have
  (node-framework.md 6.7). An edit whose new ports drop a port that a wanted route names is refused `ports-in-use`
  (4.5), naming every such route, unless it carries `dropRoutes: true`; then the keeper removes those route lines in
  the same candidate, as `node.remove` removes a node's route lines today, so the routes and the reshape are one commit
  and one write (X2). The keeper knows the new ports on its own thread: a type's `parse` and `ports` are pure and run
  on any thread (node-framework.md 7.4). A route line that also makes other routes or nodes (inside a snippet) is
  refused `shared-definition`, as any edit of it is. The reply and `node.updated` then carry `ports` (`{"added",
  "removed"}`, whole port names), `inPlace`, and the node's new `inputs` and `outputs`, and the reply `routesDropped`.
- **A hand edit or `graphFile.set`** has the same outcomes per node, each on its `node.updated`; a file that keeps a
  route to a vanishing port is refused by the graph as today (`unknown-port`), and the pane shows it.
- **Edits while the node is in a transition** commit at once and are written behind as any edit is; the node takes the
  newest settings when its transition allows (node-framework.md 6.3): an in-place change before its commit point is
  superseded at once, and `node.state`'s `next` names keys waiting behind one past it. A retune is the exception: it
  moves the slots at once in every phase (7.7).

### 4.10 Companions on the wire

A companion is hidden from the graph, not from the operator (node-framework.md 9.6, R18): it is never a node, a port, a
route end or a meter, and each node lists its companions.

```json
{"jsonrpc":"2.0","method":"node.companionsChanged","params":{"name":"rec","companions":[{"name":"rec.writer","kind":"sink","bank":"writer","ready":true,"dropped":4096}]}}
```

- **An entry**: `name` (the engine node's, `<node>.<suffix>`, I9), `kind` (an open set: `card-playback`,
  `card-capture`, `rtp-receiver`, `sink`, `source`, and slice 9's `paced-source`), `bank` (the inside bank it joins;
  inside banks appear nowhere else on the wire), `ready`, and by kind `dropped` (a sink: frames its ring had no room
  for, since the node was configured) or `shortfall` (a source: frames it played as zeros while its writer was not
  idle, since then). During a
  make-before-break (node-framework.md 6.6) the old one carries `retiring: true` and is listed before its replacement,
  which shares its name for a moment (I9).
- **When**: in snapshots and `node.added` (`[]`); `node.companionsChanged {name, companions}`, the whole list,
  whenever an entry is added or removed or its `ready` or `retiring` changes, and when a counter changed, sampled at
  most once a second per node. A teardown empties the list (one notification as it completes). Ordered with health by
  4.3's rule 8.
- **What it never is**: a route naming a companion is refused (a node name holds no `.`, so `invalid-name` or
  `unknown-node`, as today); levels never carry one (`port_blocks.cpp` meters public ports only, node-framework.md
  9.6); `graph.get`'s `nodes` never lists one.
- **Standard output** has no record per companion: a lost one fails its node, whose `node` record names it in the
  failure's detail; drops and shortfalls are `ear.sink-dropped` and `ear.source-shortfall` (4.6); a sink's drops are
  summed on the `removed` record (9.1).
- The editor draws them as small badges inside the node's box (8.7).

## 5. The C++ SPI

Everything below extends `type_spec.hpp`, `settings_table.hpp` and `node_type.hpp` of slice 2; nothing replaces them. Every
new constant is `inline constexpr` in the type's own named namespace, and every new template keyed on a constant refuses
one in an unnamed namespace (`linkage::named`, `settings_table.hpp:157-168` [V]).

### 5.1 New row kinds: decimal, indexed, live

```cpp
namespace ear::setting {
enum class Taper { linear, log, db };            // how a control maps its travel (8.3); log also ramps geometrically
enum class DefaultFrom { graphFrequency, nodeName };
struct DecimalHints {
	std::uint8_t decimals = 2;                       // display precision, and what a gesture's end rounds to (G5)
	Taper taper = Taper::linear;                     // log: low > 0; db: low <= -60 and high >= 0 (else no compile)
	bool minIsSilence = false;                       // the control shows `low` as -inf
};
struct TextHints {
	std::span<const std::string_view> values = {};   // a static array: every value must parse (checked in tests)
	std::string_view pattern = {};                   // an ECMAScript pattern for controls
	std::uint32_t maxLength = 0;                     // 0: none
};
/// A real number in [low, high]: a JSON number (an integer is taken too), never NaN. Otherwise
/// "<key>: must be a number[ of <unit>] from <low> to <high>". low above high, or a non-finite bound, does not compile.
consteval Rule<double> decimal(double low, double high, std::string_view unit = {}, DecimalHints hints = {});
template <auto Parse> requires TextParser<Parse>
consteval Rule<ParsedBy<Parse>> text(std::string_view mustBe, TextHints hints);   // today's text(), with hints
consteval Row<double, true> defaulted(std::string_view key, Rule<double> rule, double byDefault);
/// A family <key>.1 .. <key>.<n>, n being the checked value of `count`, a whole-number row above it in the table.
/// Every member reads `byDefault` when absent. A count row below, or not a whole number, does not compile.
template <class V, bool A> consteval IndexedRow<V> indexed(std::string_view key, const Row<std::uint32_t, A>& count,
                                                           Rule<V> rule, V byDefault);
/// Ramp time for a live row; 0: a step change at the next block.
struct Smoothing { std::uint32_t ms; };
consteval Smoothing smooth(std::uint32_t ms);
}
```

- `Row::live(Smoothing) const` and `IndexedRow::live(Smoothing) const` are consteval and return the row marked live.
  Only a row that always reads a value (required, defaulted, or indexed) of a whole number or a decimal can be live;
  any other does not compile ("a live row must always have a number"). A live whole-number row must have
  `smooth(0)`: it steps, never passing through fractions ("a whole-number live row steps").
- An optional row takes `.defaultFrom(DefaultFrom)` (consteval): documentation for controls and the schema; the type's
  behaviour still reads `std::nullopt` and decides, as `rtp` does today.
- **Change marks** (node-framework.md 7.1 is the home of all four; slice 3d builds `.inPlace()`, this page builds
  `.shapesPorts()` and reads both): `.inPlace()` and `.live(...)` (which implies it) give the schema's `change`; a row
  with neither is `restart`. `.shapesPorts()` (consteval) says a change to the row may change the node's ports: the
  schema's `shapes` (4.2). The plan never reads `.shapesPorts()`: it compares the ports `ports()` makes (node-framework.md 6.7), which
  is the truth; the mark is the schema's promise, so the all-type tests hold it to `ports()` (5.9). An indexed family's
  count row is `.shapesPorts()` only when `ports()` reads it (`gain`'s `channels` does).
- `SettingRow` gains `std::optional<std::uint32_t> liveMs`, `std::optional<std::string_view> indexedBy`, a third rule
  kind `DecimalRule{low, high, unit, hints}`, the text hints, `defaultFrom` and `bool shapesPorts` (slice 3d adds
  `bool inPlace`). `SettingKind` gains `decimal`; `SettingDeclaration` gains `bool live`, `bool indexed` and `bool
  shapesPorts` beside 3d's `bool inPlace` (`node_type.hpp:318-325` [V] today).
- The walk: a decimal is a JSON number in range; an integer is taken and kept as given. An indexed family's keys are
  `<key>.<i>`, 1 to the count; another member number is `"<key>.<i>: no such member (<count key> is <n>)"`, placed at
  step 3 of the walk's order (`settings_table.hpp:205-213` [V]) where the family's row sits; unknown keys at step 2
  skip any key whose part before its last `.` names a family. Each refusal carries its key (4.5).
- Reads: `s.get<row>()` and `s.get<trimRow>(i)` (member `i`, from 0, as the harness's `input(bank, 0)`, while keys count
  from 1, as ports' members) read plain rows. **On a live row `get` does not compile** ("a live row is read through
  io.param"): a value captured in `configure` or a loop's state would go stale at the first retune
  (7.4).
  `s.initial<row>()` reads a live row's value as the node was configured, for the rare start that needs it; its name says it
  does not follow. The behaviour's `check` sees every row, live ones included, and runs on every tick too (6.6), so a
  rule across settings judges what a gesture plays, not only what it commits.
- The graph file. A declared decimal is read with knobly's `number()` (`/opt/knobly/include/knobly/written.hpp:942` in
  the image [V]: a word spelling a number, integer or not). It is written as the shortest round-trip fixed text
  (`std::to_chars` with `chars_format::fixed`), so `-3.5` is `-3.5`, `-6.0` is `-6`, and a negative zero is written `0`.
  Today `valueText` writes only integers and strings (`graph_file_model.cpp:328-342` [V]) and `readNode` reads only
  integers and strings (`graph_file_model.cpp:220-225` [V]); both gain the decimal branch. That a negative fixed number
  is one word, and that a directive name may hold `.` (`trimDb.1`), is [U], checked in steps 6.1 and 6.2; route words
  already hold `.` (`noise:noise.mono` [V: AGENTS.md's example]). Fallback for the name: `trimDb-1`.

### 5.2 Status fields

```cpp
namespace ear {
enum class Display { panel, box, callout };   // where the editor shows a field; callout: a banner while not initial
namespace writer {
struct Writer { enum class Kind { rt, loop, companion } kind; LoopName loop = {}; };
inline constexpr Writer rt{Writer::Kind::rt};               // the node's code, through Io
/// A loop of the spec, by its handle (node-primitives.md 6.4: `loop::dedicated<"sendspin">`, listed in `.loops`).
template <class Handle> consteval Writer loop(Handle);
inline constexpr Writer companion{Writer::Kind::companion}; // a companion kind's adapter (slice 8: rtp's `receiving`)
}
namespace status {
inline constexpr struct Unknown {} unknown;   // `.initially(status::unknown)`: the field starts null
template <spec_detail::Name N> consteval FlagField<N> flag(writer::Writer w);
template <spec_detail::Name N> consteval NumberField<N> number(writer::Writer w, std::int64_t low, std::int64_t high,
                                                               std::string_view unit = {});
template <spec_detail::Name N> consteval DecimalField<N> decimal(writer::Writer w, double low, double high,
                                                                 std::string_view unit = {}, std::uint8_t decimals = 2);
template <spec_detail::Name N, std::size_t K>
consteval ChoiceField<N, K> choice(writer::Writer w, const std::string_view (&values)[K]);   // starts unknown (null)
template <spec_detail::Name N> consteval TextField<N> text(writer::Writer w, std::uint32_t maxBytes = 256); // never rt
}
template <auto... Fields> struct StatusTable { /* unique names; each a field */ };
}
```

- Every field has `.display(Display)`, `.help(text)`, `.flapsAbove(transitions, windowMs)` (default 10 in 10 000 ms)
  and `.initially(value)` (or `status::unknown`). Initial values by default: a flag `false`, a number and a decimal `0`,
  a text `""`, a choice unknown (`null`): a choice's first value is no longer its initial value, so a node whose worker
  has not written yet never shows a domain value that is false. `.initially("listening")` names one explicitly.
- A choice's values are named once; `field("connected")` is consteval, so a misspelt value does not compile:
  `report.set<connection>(connection("connected"))`.
- `rt` fields are flag, number, decimal and choice only (one atomic word each; an unknown choice is a reserved word).
  A text field marked `writer::rt` does not compile.
- `writer::loop(worker)` takes a loop handle of slice 3c (node-primitives.md 6.4; the owner's G14: loops declared in
  the spec as handles); the handle carries the loop's name, checked where it is made. A field whose loop the spec does
  not list in `.loops` does not compile. `Report::set` of a loop field asserts `Loop::inLoop()` of that loop in debug
  builds. A library thread (sendspin-cpp's own, which the worker's state starts in `begin()` and joins when it is destroyed) that has
  something to report posts to the loop that owns the field. There is no `writer::thread`: node-framework.md 8.2 once
  named one, but a library's threads write no status directly here, so one field never has two threads behind one name
  (node-primitives.md section 7 says the same from its side; node-framework.md has dropped it).
- `writer::companion` is node-framework.md 7.3's (revision 4's `writer::resource`, renamed): a field a companion
  kind's adapter writes for its node, as the RTP receiver's `receiving` (slice 8). A type names it in its table; the
  framework binds it to the companion of that kind the node acquired, and a spec with such a field whose `configure`
  acquires no companion of that kind fails its harness test.
- Names: as bank names (1 or more of `[A-Za-z0-9_-]`), camelCase by convention, unique in the table; `spec_detail::Name`
  is today's `BankName` (`type_spec.hpp:47-53` [V]) under a general name. No field is the framework's, so no field name
  holds a dot.

### 5.3 Events, log codes, `Report<spec>` and `TypeSpec`

```cpp
namespace ear {
enum class EventSeverity { info, warn, error };
namespace arg {
template <spec_detail::Name N> consteval NumberArg<N> number(std::int64_t low = INT64_MIN, std::int64_t high = INT64_MAX);
template <spec_detail::Name N> consteval DecimalArg<N> decimal();
template <spec_detail::Name N> consteval FlagArg<N> flag();
template <spec_detail::Name N, std::size_t K> consteval ChoiceArg<N, K> choice(const std::string_view (&values)[K]);
template <spec_detail::Name N> consteval TextArg<N> text(std::uint32_t maxBytes = 256);   // never from process
}
namespace event {
/// `sentence` names each argument once as {name}; a place that names no argument, or an argument with no place, does
/// not compile. It is the record's msg and the editor's line.
template <spec_detail::Name Code, class... Args>
consteval Event<Code, Args...> of(EventSeverity severity, std::string_view sentence, Args... args);
}
namespace log {
template <spec_detail::Name Code, class... Args>   // numbers, decimals and flags only, at most four
consteval LogCode<Code, Args...> code(LogLevel level, std::string_view sentence, Args... args);
}
template <auto... Events> struct EventTable {};
template <auto... Codes> struct LogCodes {};

/// The typed control-plane face of one node, over slice 3c's NodeHandle (node-primitives.md section 5): status writes and
/// events by the spec's handles, from the loops its fields name. Copyable (it holds the handle by reference, and the
/// handle outlives every thread that can reach it); every copy is the same node's. Never blocks but on one short mutex;
/// a write after the node's configuration began closing is dropped (I6). A text value is UTF-8 checked, control characters replaced,
/// and cut at its bound (8.2's rule, the framework's: sendspin's `notes::printable` goes). `lost` and `log` stay the
/// handle's own (`report.handle().lost(...)`, `report.handle().warn(...)`).
template <const auto& Spec> class Report {
public:
	template <const auto& Field> void set(typename FieldOf<Field>::Value value);   // a writer::loop field
	template <const auto& Event> void raise(typename ArgsOf<Event>::Values... values);
	NodeHandle& handle() const noexcept;   // the name of node.handle() (the owner's G17)
private:
	explicit Report(NodeHandle& handle);   // made by the framework (NodeOf<Spec>::report) and the harness only
};
/// node-framework.md 6.10's NodeOf<Spec>, which the instance keeps, gains one member: the node's typed Report. NodeOf is
/// spec-typed, so a Report of another spec cannot be made: a mismatch does not compile. The Report lives as long as the
/// node's configuration (an in-place change, a stop and a start keep it); a type hands it to its loops' states in
/// configure, as it hands the handle.
template <const auto& Spec> class NodeOf {
public:
	// node-framework.md 6.10's members: format, node, handle, post
	Report<Spec> report();
};
}
```

- A choice argument off the realtime thread takes a `std::string_view`, checked at run time against its values (a value
  not among them throws `std::logic_error`: a type bug); from `process` it takes a consteval-made index only.
- `NodeHandle` (slice 3c) replaces `InsidesEvents` (`node_type.hpp:174-180` [V]); `Report` adds the typed writes to it,
  carried in the same per-node channel so a status write, an event and a `lost` keep one writer's order (6.3).
  `NodeHandle::say` stays for `sendspin` until step 5.5 removes it.
- **When a node may report.** The `Report` exists from `configure` (T2). Its loops' work runs once the node is
  configured (node-framework.md 6.4), so in practice they write from `configured` on (sendspin's `clientId`, read from
  its store in `begin()`). A write during an in-place change, or while stopped, lands on the same configuration (the
  generation is kept, I6).
- **`TypeSpec`, whole.** Its one home is node-primitives.md 6.13, which gives every member and the order
  (designated initialisers must follow it): slice 2's `.settings` and `.banks`, slice 3c's `.stores` and `.loops`,
  slice 3d's `.lifecycle`, then this page's `.status`, `.events`, `.actions` and `.logCodes`. This page adds `.status` and `.events` in step 5.2,
  `.logCodes` in 5.3 and `.actions` in 5.4. Each defaults to an empty list, so a spec that gives only `.settings` and
  `.banks` stays as it is today.

  That aggregate deduction with designated initialisers fills the defaulted template parameters on GCC 14 is [U]
  (node-primitives.md U1, the same question); slice 3c compiles it first, and step 5.2 adds the later members to
  that test. The fallback for both pages is a consteval `makeSpec(...)` with the same members in the same order.

### 5.4 What `process` gets: `Io<spec>` grows

`process`'s signature does not change: `void process(Io<spec>, const GraphCycle&) noexcept`. `Io` gains:

```cpp
template <const auto& Spec> class Io {
public:
	// today's: operator[](bank), frames()
	template <const auto& Row> Smoothed param() const noexcept;                         // a live, non-indexed row
	template <const auto& Row> Smoothed param(std::size_t member) const noexcept;       // a live indexed row, from 0
	template <const auto& Field> void set(typename FieldOf<Field>::Value value) const noexcept;   // writer::rt only
	template <const auto& Event, class... N> void raise(N... values) const noexcept;    // numbers, decimals, flags, choice indexes
	template <const auto& Code, class... N> void log(N... values) const noexcept;       // a LogCodes entry
	template <const auto& Trigger> bool triggered() const noexcept;                    // true once per invoke
};

/// One live row's values for this block, in the row's own unit (the ramp's rule is below). Read only; reading it
/// changes nothing.
class Smoothed {
public:
	bool steady() const noexcept;                     // no ramp frame in this block: every sample is value()
	float value() const noexcept;                     // the value at the block's last frame
	float operator[](std::uint32_t frame) const noexcept;
	float target() const noexcept;                    // the slot's latest
};
```

**The ramp's rule** (one rule for every live row; the framework's, before `process`):

- Once per block, before `process` and whether or not the code reads the row, the framework loads each live member's
  slot (one atomic load) and advances its ramp. `io.param` only returns what the framework computed, so a row read
  twice in a block reads one value, and a row the code never reads still keeps time.
- **Constant time.** When the slot's target differs from the ramp's target, a new ramp starts at the block's first
  frame, from the value of the last frame already given (the current value), to the new target, over
  `N = max(1, round(smoothingMs × graphFrequency / 1000))` frames. A target that moves mid-ramp restarts the ramp from
  where it is, with a full `N` frames; it never jumps.
- Frame `k` of a ramp (`k = 0` is its first frame) has the value `v0 + (t − v0) × (k + 1) / N` for `k < N`, and `t`
  from `k = N − 1` on; computed in double, given as float. A ramp may end inside a block; after its last frame
  `operator[]` returns the target. `steady()` is true only for a block that holds no ramp frame.
- `smooth(0)`: the new target holds from the block's first frame (a step). A whole-number live row always steps.
- **Domain.** The ramp is linear in the row's unit for `linear` and `db` tapers (linear in dB is the usual gain ramp,
  geometric in amplitude). A `log` taper row (low > 0) ramps geometrically: the rule above on `ln(v)`, so a frequency
  sweeps evenly in pitch.
- The first block a code runs after the node is configured starts at the slot's value, without a ramp (after a
  start, too).
- **Worked vector** (the harness test of step 6.3): 48 000 Hz, block 256, smoothing 20 ms, so `N = 960`. Value `-6`,
  steady; the target `-3.5` is stored before block 0. Frame 0: `-6 + 2.5 × 1/960 = -5.9973958`; frame 255:
  `-6 + 2.5 × 256/960 = -5.3333333`; frame 959 (block 3, frame 191): `-3.5`; frame 960 (block 3, frame 192): `-3.5`.
  Blocks 0 to 3 are not steady (block 3 holds ramp frames 768 to 959); block 4 is steady with `value() == -3.5`. If the
  target becomes `-12` before block 2, block 2's ramp starts from frame 511's value (`-6 + 2.5 × 512/960 =
  -4.6666667`) and reaches `-12` at frame `512 + 959 = 1471`.
- **Members of one node change together within a block only by chance.** The main loop writes a reach's slots one at a
  time while the realtime thread runs, so one block may see channel 1's new target and channel 2's old one; the next
  block sees both. The difference is at most one block of ramp (about 0.07 dB for a 2.5 dB move over 20 ms); a seqlock
  per node was judged not worth its cost (13).

- `io.set` of an `rt` field stores one atomic word, and when the value differs from the last one the code wrote, puts a
  record in the node's ring and counts a transition (6.4).
- `io.raise` and `io.log` put one fixed-size record in the ring, stamped with the cycle's start (`GraphCycle::startNs`,
  `node_type.hpp:98` [V]): no formatting, no allocation.
- `io.triggered<t>()` compares the trigger's counter with the last one the node's code saw.
- The framework resolves every handle to an index when the code is prepared (as `Io::resolve` does for banks,
  `type_spec.hpp:216-231` [V]), so each call is an array index on the realtime thread.
- These are the realtime calls' names: `io.log`, `io.raise`, `io.set`, over a ring the framework makes per node (6.1),
  never acquired in `start`. This page is their home; node-primitives.md section 14 cites them.

### 5.5 Actions

```cpp
namespace ear::action {
/// Into the node's code, wait-free: no arguments, answered {} once handed over; the code sees io.triggered<t>() true once
/// in its next block. Several invokes before a block are one.
template <spec_detail::Name N> consteval Trigger<N> trigger(std::string_view help);
/// Into the loop's state, posted to `Loop`, a loop handle of the spec (node-primitives.md 6.4 and G14: one listed in
/// `.loops`, else no compile): arguments checked by `args`, a SettingsTable named for the action; the handler's
/// std::expected<void, std::string> is the reply.
template <spec_detail::Name N, const auto& Args, const auto& Loop>
consteval Call<N, Args, Loop> call(std::string_view help);
}
template <auto... Actions> struct ActionTable {};
```

- `start`, `stop`, `restart`, `disable` and `enable` are the framework's lifecycle requests (`node.<verb>`, 4.2), not
  actions: no type lists them, and a type action of one of those names does not compile (W12).
- A trigger needs no loop state and no loop: `gain` uses one. A call runs on the loop it names, posted there with
  slice 3c's `NodeLoop::postFromOutside`; a call naming a loop the spec does not list does not compile. The handler is a
  member of the loop's state:
  `std::expected<void, std::string> act(Invoked<forgetPairing>, const CheckedOf<forgetArgs>&)`, found by its argument
  type and held to that signature, as `check` is (`type_spec.hpp:368-373` [V]). A queued call whose node's configuration is torn down
  first is destroyed unrun at teardown step 1, when `closeToOthers` closes the loop to outside work, and its reply object answers
  `restarted` (or `stopping`) from its destructor (node-primitives.md 6.4, 6.10).
- An action's arguments are a `SettingsTable`, so a refusal has the settings walk's text, order and key: one checker
  for settings, action arguments and parameters (node-framework.md 7.1).

### 5.6 How the framework carries them (core, main loop and realtime)

- **Per configuration** (`NodeHost`, made when `configure` is called from empty, T2, and freed at I5 step 5): the node handle's channel (today's
  `Channel`, `node_host.cpp:173-190` [V], slice 3c's `NodeHandle`, gaining the queue of 6.3), its off-realtime queue
  (6.3), its realtime ring (6.1), one `std::atomic<double>` slot per live row member, one `std::atomic<std::uint64_t>`
  per `rt` field with the last value the realtime side wrote and one `std::atomic<std::uint32_t>` transition counter,
  one `std::atomic<std::uint32_t>` per trigger with the count the code last saw, and each live member's ramp state
  (realtime-owned), and the companions' view (4.10). All of it is the node's, not the code's: the code is bound to it
  when the code `configure` gave is prepared (T3; `BankBoundCode::bindPorts`, `type_spec.hpp:271-276` [V], grows a
  `bindChannels`), and a code an in-place change swaps in is bound to the same objects before its barrier (6.7). **An
  in-place change, a stop and a start keep all of it** (node-framework.md I6: the generation is kept), so the status view, the slots and their ramps, the transition
  counters and an unseen trigger survive an in-place change and a code swap; a reshape in place resizes the slots of an
  indexed family whose count changed (members that remain keep their slot and ramp, new ones start at the committed
  value, and vanished ones are dropped after the swap's barrier).
- **Per node**: the wanted state, the health members of 4.8 (phase, since, cause,
  keys, next, holds, waiting-for, reason), the status view, the live value of each held key (seeded into every new
  node's slots, so a restart mid-gesture keeps the hand's value), the node diagnostics kept per code and subject,
  `blockLost`.
- **Erased for the rest of ear**: `NodeType::schema()` returns a plain `TypeSchema` (core, nlohmann only), encoded once
  by `ear_control`. `NodeConfig::liveValues()` (default: none) gives each live key's checked value, for retunes.
  `NodeType::checkLive(config, key, value)` runs the row's rule and then the behaviour's `check` with that value in
  place, returning `InvalidSettings` with its key.
- **New core interfaces**, each implemented outside core: `RealtimeBell` (`ring() noexcept`; an eventfd the main loop
  watches, in `ear_pipewire`), `NodeHealthListener` (today's, `node_host.hpp:22-37` [V], gaining the members of 4.8;
  slice 3a), `NodeCompanionsListener` (`companionsChanged`; slice 3b), the listeners `NodeStatusListener`
  (`statusChanged`, `statusReset`, `eventRaised`),
  `NodeDiagnosticListener` (today's, gaining subject, `active`, `count` and fields), and `GestureListener`
  (`gestureChanged`, carrying the owning session so the gateway can leave out own echoes), implemented by the gateway
  and `StatusLog`.

### 5.7 The `gain` type, whole

```cpp
// src/nodes/gain/gain_spec.hpp: slice 6's proof of decimal rows, indexed rows, .live and Smoothed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "ear/core/type_spec.hpp"

namespace ear::nodes::gain {

inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2).shapesPorts();   // a change reshapes
inline constexpr auto gainRow =
	setting::defaulted("gainDb",
	                   setting::decimal(-90.0, 24.0, "dB",
	                                    {.decimals = 1, .taper = setting::Taper::db, .minIsSilence = true}),
	                   0.0)
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

inline float dbToFactor(float db) noexcept { return db <= -90.0F ? 0.0F : std::exp2(db * 0.16609640474F); } // 10^(db/20)
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
			float peak = 0.0F;
			if (gain.steady() && trim.steady()) {
				const float factor = dbToFactor(gain.value() + trim.value());
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * factor;
			} else {
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * dbToFactor(gain[i] + trim[i]);
			}
			for (const float sample : outs[c]) peak = std::max(peak, std::abs(sample));
			if (peak > 1.0F && !latched) {
				latched = true;
				io.raise<clipped>(static_cast<std::int64_t>(c + 1), factorToDb(peak));
			}
		}
		io.set<clipping>(latched);
	}
};

class Gain {   // the node's instance (node-framework.md 6.4): nothing of its own to keep
public:
	static Members<spec> ports(const Checked& s) {
		return {in(members(s.get<channelsRow>())), out(members(s.get<channelsRow>()))};
	}
	Configured configure(CreateOf<spec>& cx, const Checked&) {   // node-framework.md 6.10
		cx.use(GainCode{});   // creation only: no change version, as it has no in-place rows (its live rows retune,
		return Configured::applied();   // `channels` restarts)
	}
	// No start() or stop() (the framework drives and silences its code), no loops (no threads, nothing shared with its
	// code but what the framework owns: slots, rt fields, the ring).
};

} // namespace ear::nodes::gain
```

```cpp
// src/nodes/gain/gain.cpp
#include "ear/nodes/gain.hpp"
#include "gain_spec.hpp"
std::unique_ptr<const ear::NodeType> ear::makeGainNodeType() { return makeNodeType<nodes::gain::spec, nodes::gain::Gain>(); }
```

`src/nodes/gain/CMakeLists.txt` is `ear_node_type(SOURCES gain.cpp TESTS tests/gain_test.cpp)`; `validSettings` gains
`{"gain", {{{"channels", 2}, {"gainDb", -6.0}, {"trimDb.1", 0.5}, {"trimDb.2", -0.5}}}}`. Nothing else names the type
(AGENTS.md, "Adding a node type").

A type with a loop's state writes status and events from that loop through the `Report` its `configure` hands the
state from `node.report()`. Sendspin's worker after step 5.5 (`worker` is its spec's `loop::dedicated<"sendspin",
Worker>`, node-primitives.md 4.1):

```cpp
inline constexpr std::string_view connections[] = {"dialling", "listening", "connected"};
inline constexpr auto connection =
	status::choice<"connection">(writer::loop(worker), connections).display(Display::box);   // starts unknown
inline constexpr auto serverName = status::text<"serverName">(writer::loop(worker), 64).display(Display::box);
inline constexpr auto pairingCode = status::text<"pairingCode">(writer::loop(worker), 16).display(Display::callout)
                                        .help("type it into Music Assistant to pair");
inline constexpr std::string_view pairReasons[] = {"attempt-timeout", "concurrent-attempt", "method-not-supported",
                                                   "pairing-code-mismatch", "user-cancelled", "unknown"};
inline constexpr auto pairingFailed =
	event::of<"pairing-failed">(EventSeverity::warn, "pairing failed: {reason}", arg::choice<"reason">(pairReasons));
// Sendspin::configure gives the worker's state node.report() beside node.handle(), its executors, its source's writer
// and its sink's consumer (node-framework.md 6.4; node-primitives.md 4.1).
// In a worker loop handler:
report_.set<connection>(connection("connected"));
report_.set<serverName>(server ? server->name : "");        // made safe by the framework
report_.raise<pairingFailed>(abortReasonName(reason));      // checked against pairReasons at run time
```

### 5.8 Compile-time refusals

Each is a case in `src/core/tests/compile_fail/`, named for its reason, matched against GCC 14's wording as today.

| Case | Refused because |
|---|---|
| `live-text-row`, `live-optional-row` | a live row must always have a number |
| `live-whole-smoothed` | a whole-number live row steps |
| `get-live-row` | a live row is read through `io.param` (or `initial<row>()`) |
| `indexed-count-below`, `indexed-count-not-number`, `indexed-no-default` | the family's count is a whole-number row above it; members read a default |
| `decimal-low-above-high`, `decimal-default-outside` | as the whole-number rows' |
| `log-taper-nonpositive`, `db-taper-range` | a log taper needs low > 0; a db taper needs low <= -60 and high >= 0 (8.3's law) |
| `param-not-live`, `param-row-not-in-spec`, `param-indexed-without-member`, `param-member-of-plain-row` | `io.param` reads only the spec's live rows, as declared |
| `set-field-not-in-spec`, `set-field-not-rt`, `report-set-rt-field`, `rt-text-field` | one writer per field; the realtime thread writes one word |
| `writer-loop-not-in-spec`, `call-loop-not-in-spec` | a field's or a call's loop is a handle the spec lists in `.loops` (a bad loop name is refused where the handle is made, node-primitives.md 6.4) |
| `choice-value-misspelt`, `initially-not-a-value` | a choice value is checked where it is made |
| `raise-text-arg-from-process`, `raise-wrong-arg-count`, `raise-event-not-in-spec` | the realtime ring holds numbers only; arguments as declared |
| `sentence-unknown-place`, `sentence-missing-arg` | every argument has one place in the sentence |
| `report-of-other-spec`, `report-constructed` | a `Report` comes only from `NodeOf<spec>` |
| `triggered-not-a-trigger`, `action-named-lifecycle` (an action named `start`, `stop`, `restart`, `disable` or `enable`), `duplicate-status-name`, `duplicate-event-code` | as named |
| `status-in-unnamed-namespace`, `event-in-unnamed-namespace` | `linkage::named`, as for rows and specs |

### 5.9 The harness

`TypeHarness<spec, Behaviour>` (`src/core/include/ear/testing/`) steps the lifecycle one call per transition; its class
is node-framework.md 7.6, its one home, with every call on `h.`. This page's slices add, in the same spelling, all on
the test's thread:

| Call | Does |
|---|---|
| `h.live<row>(value)`, `h.live<row>(member, value)` | moves the slot, as a gesture tick (and runs `checkLive`, failing the test on a refusal) |
| `h.retune(settings)` | a settings change of live rows only: slots move, no restart (asserts the plan would retune) |
| `h.status<field>()`, `h.status()` | the live view (typed, or as JSON) after the last drain |
| `h.statusRecord()` | every change, in order: `{field, from, to}`, and each reset |
| `h.events()`, `h.logs()`, `h.diagnostics()` | in order, each with its formatted `msg` (diagnostics with subject and `active`) |
| `h.trigger<action>()`, `h.invoke<action>(args)` | a trigger; a call, on the harness's manual loops (node-primitives.md section 10) |
| `h.drain()` | runs the tick now (the harness drains after every `process` and every operation by default) |
| `h.advance(ms)` | moves the harness's clock, node-primitives.md section 10's one fake clock: flapping windows, rate limits and trailing records, the gesture idle deadline, the 2 s after which `waitingFor` is told |
| `h.health()` | every health notification, in order, as 4.8 encodes it (`state`, `phase`, `wanted`, `cause`, `keys`, `waitingFor`), so a type's test checks what an operator would see |
| `h.outcome(settings)` | the plan's outcome of a change from the current settings (what `dryRun` answers), without applying it |

The all-type tests gain: every type's schema encodes; every status field's initial value is of its kind or null;
every `values` entry parses with its row's parse function; every optional row without a `default` has a `defaultFrom`
or a `help`; every live row of every type ramps from its default to another value with no sample unwritten; `restart`
is first in every type's actions; for every row of every type, changing it alone from its `validSettings` value to
another valid value changes the ports only if the row is `.shapesPorts()`, and every `.shapesPorts()` row changes them
for some value it can take (the mark is held to `ports()`); every `.inPlace()` row's change through the harness ends
`running` with the same configuration (no status reset), or in a restart whose `cause` is `fell-back` with `restart-asked` (the hook's own answer);
the status view survives every in-place change.

## 6. The realtime contract

### 6.1 Channels into and out of the realtime thread

| Channel | Direction | Mechanism | Bound | On overflow |
|---|---|---|---|---|
| Parameter slot | in | one `std::atomic<double>` per live member, written by the main loop, loaded once a block by the framework | 256 live members per node | — (a slot always holds one value) |
| Trigger | in | one `std::atomic<std::uint32_t>` counter per trigger | — | several invokes in one block are one |
| `rt` status field | out | one `std::atomic<std::uint64_t>` per field, sampled at the drain; a transition counter beside it | 64 `rt` fields per type | — |
| Realtime ring | out | per node, made by the framework, single producer (the code), single consumer (the drain), fixed 48-byte records: kind, code, field, up to four 8-byte values, the cycle's start (`startNs`) | 256 records per node; log records are refused once 192 are in use, so status and events keep the last quarter | dropped, counted per kind, reported as `ear.records-dropped` (subject `realtime`) |
| Bell | out | `RealtimeBell::ring()`, an eventfd write, gated by an atomic exchange on the window as the level bell's is (`port_blocks.cpp:429` [V]) | at most one per level window per block (30 a second) | — |

`std::atomic<double>` and `std::atomic<std::uint64_t>` are lock-free on x86-64 and arm64; a `static_assert` on
`is_always_lock_free` makes it a build failure elsewhere [U: arm64 builds are not made yet].

### 6.2 What the realtime thread does, and never does

It loads each live slot once a block, advances each ramp, stores `rt` fields, writes ring records, and rings the bell at
most once per window. It never allocates, locks, formats, or makes a syscall other than the bell's write
(node-framework.md 7.4). The block rings the bell after `process` returns when an `rt` field changed or the ring holds
records, so a node whose only output is realtime is drained, and a node at rest costs nothing (8.2's rule, kept).

### 6.3 Off the realtime thread: one queue per node

- `Report<spec>` writes and the node handle's `lost` go into the node's queue: a `std::vector` of records (status change,
  event, lost) under one mutex, bounded at 1024 (a `lost` is never dropped); a full queue drops the record and counts
  it (`ear.records-dropped`, subject `queue`). The handle's `log` goes straight to the log sink, unordered with these,
  as slice 3c defines it.
- The first record into an empty queue posts one wake to the main loop (an atomic flag gates it, so a burst costs one
  post).
- Each writer's records stay in its order, so a status write before an event is drained before it, and a `lost`
  after the last status is drained after it: node-framework.md 8.1's order, with no per-field flush.
- Text is made safe at the write, on the writer's thread: UTF-8 checked, control characters replaced, cut at the
  field's `maxBytes` and marked.
- The mutex is held for a push or a swap only; the main loop may wait behind a preempted writer for that long, never
  for I/O. (8.2 proposed a mutex per text field; one per node is the same cost and gives the order above.)

### 6.4 The drain and the tick, on the main loop

1. A wake (off-realtime), the bell (realtime), a gesture tick taken (6.6), or a trailing record due (6.5) marks the
   node dirty and, if no tick is due, arms one on `Timers` (`timers.hpp`, the main loop's) for 33 ms after the last
   tick, or now if that has passed.
2. The tick drains every dirty node: its realtime ring first, then its queue (a fixed choice; records of different
   writers have no order between them, node-framework.md 8.1). For each record: a status change updates the view, is
   offered to `StatusLog` (paced per field, 6.5), counts a transition, and marks the field for the wire; an event first
   sends the node's marked fields as one `node.statusChanged`, then `node.eventRaised` and an `event` record, its `t`
   the record's cycle start mapped to UTC for a realtime event (the drain's clock for the offset), the write's time
   otherwise; a `lost` fails the node as today.
3. `rt` fields are then sampled: a value that differs from the view's is marked for the wire (its change records came
   through the ring, written where the code changed it). The companions' counters are sampled at most once a second
   per node (4.10).
4. One `node.statusChanged` per node with marked fields; one `gesture.changed` `change` per gesture moved since the
   last tick; flapping is judged (6.5); drops become `ear.events-dropped` or `ear.records-dropped`; trailing status
   records due are written (6.5).
5. A `lost` from a configuration whose teardown began is dropped; its status and events are still drained until it
   closed, so a goodbye is heard; then its reset is sent (4.3); nothing of a configuration is drained once it closed or
   was leaked (I6, as slice 1 built it for notes).

### 6.5 Rates and bounds

| What | Bound | Policy |
|---|---|---|
| The tick | 30 a second, all nodes together | armed only when something is dirty |
| `node.statusChanged` | 30 a second per node | the latest values |
| `status` records on stdout | per node and field: 32 at once, then 1 a second | when the pace held a field's changes back, a **trailing** record with the field's current value is written at the next tick the pace allows (at most 1 s after the last held change), carrying `heldBack`; so the last `status` record of each field always matches the view (G9) |
| Events (wire and stdout) | 32 at once, then 1 a second per node | the pacing slice 3c moves into core for `say`; held back counted in `ear.events-dropped` |
| Flapping | a field above its `flapsAbove` (default 10 transitions in 10 s) | counted at each write (off-realtime) and by the transition counter (realtime), so changes between samples count; settled once a whole window passes at or below it |
| Gesture ticks into the main loop | one pending per gesture | the control thread keeps the newest until the main loop took the last (6.6) |
| `gesture.changed` `change` | 30 a second per gesture, one per gesture whatever its reach | sent at the tick; never to the owner |
| Off-realtime queue | 1024 records per node | dropped, counted |
| Action reply | 10 s | `action-timeout`; the handler's late answer is dropped |
| Gesture idle | 30 s with no message about it from its owner (a tick, keep-alives included), timed by the owner's session on the control thread | ended as released, keeping the last value heard (`end`, `reason: "idle"`, the owner's G4; G15); the timer stops once the owner's end or cancel was read (6.6) |
| A session's queued requests | 16 messages with an id (or batches) read ahead | at 16, the socket is not read until one is answered (6.6) |

### 6.6 The tick path from the socket

**Reading ahead.** Today a connection reads its next message only once every response to the last one is queued
(`control_port.cpp:240` [V]; batch2.md section 2's table, line 129 [V], and section 6, line 1168 [V]). A held action
reply (up to 10 s, 7.6) or a request waiting behind a keeper write would then stop every tick of that browser. So the
session now reads ahead (G12):

- Every message is read as it arrives. A message with an id, a batch, and any notification other than the two below go
  into the session's FIFO and run one at a time, in order, exactly as today: their responses keep request order, and
  batch2.md's order rules for them are unchanged. The FIFO holds 16; at 16 the socket is not read until one finishes
  (today's back-pressure, later).
- A `gesture.change` or `gesture.cancel` **notification** (no id) skips the FIFO: it is checked for shape on the
  control thread and handed to the gateway at once. Its `done` fires when it is stored, not when the main loop takes it.
- **Why order holds.** These notifications produce no response, so no response order changes. A tick names a gesture
  whose id the client learned from the begin's reply, so the begin was already run. When the session reads a
  `gesture.end` or `gesture.cancel` for an id (with an id, queued in the FIFO), it marks that gesture *ending* on the
  control thread at once: later ticks for it are dropped there, and the gesture's idle timer stops. An end queued
  behind a slow request therefore never races a tick, and never loses to the idle deadline.
- **Idle and close** (the owner's G4, R32; G15, G16). The session keeps each of its open gestures' idle timer on the
  control thread, reset by every message about it (ticks and keep-alives, read ahead or not). When 30 s pass, or the
  socket closes, the session first hands the gesture's newest pending tick to the gateway (so the last value heard
  reaches the slots), then marks it *ending* and queues, behind the session's earlier requests, a `gesture.end` with no
  value and a `cause` (`idle` or `session-closed`), which goes to the keeper as any end does. A closed session's queued
  requests with an id are still run in order and their replies dropped, so its end keeps its place among them.

**Coalescing.** The gateway keeps, per gesture, the newest pending tick; if none was pending it posts one task to the
main loop, which takes whichever tick is newest when it runs. So a client sending 1000 ticks a second costs the main
loop at most one task per turn, and the audio still follows the last value.

**On the main loop**, `Gestures::move` checks the gesture and its owner, drops a stale `seq`, and stops there when
the value equals the live value (a keep-alive). Otherwise it checks the value against every
reached row and the type's `check` (`NodeType::checkLive`, 5.6; a refusal is dropped and the owner gets
`tick-refused`, at most once a second), writes every reached slot through `NodeHost::setLive` (for a node with no configuration,
only the node's live value), marks the gesture dirty and arms the tick (6.4).

### 6.7 The per-node channels across the lifecycle

Revision 2 was written to the two-phase start, where the code drove the block as soon as `start` returned. With one
`configure` and the code driven only at T5 (node-framework.md 6.4), every channel of 6.1 exists before the code first
runs and is the node's, never the code's (R13), so nothing in this section needs the code to exist before the instance
or the other way round.

| Moment (node-framework.md 6.3) | What happens to this page's channels |
|---|---|
| T2, `configure` from empty | the node's channels are made: slots (seeded from the committed values, or the node's held live values), `rt` words, transition counters, trigger counts, the ring, the queue; the status view starts at its initial values; the `Report` is made (5.3) |
| T3, configured | the code is prepared and bound to them (`bindChannels`); nothing runs on the realtime thread yet, and the block is silent |
| T5, T8, the code drives (a barrier) | the first block after it starts every ramp at its slot's value, without a ramp (5.4); the code may write `rt` fields and records from its first cycle; the block rings the bell as 6.2 says; then `start()` runs |
| retune, in any phase | the slots move at once, wait-free; with no configuration (`added`, `configuring` before T2's channels, `disabled`, `failed`, `recovering`) only the node's live value moves, and seeds the next configuration's slots (7.7); while `stopped` the slots move and the code, not driven, ramps from them at the next start |
| T9, an in-place `configure`; holds | the code keeps running and writing; a hold is applied by the block after the code (node-framework.md 6.6 step 2), so a held bank's `rt` fields, events and levels are the code's own, and the meters show the silence the hold makes (they read the block's outputs after the hold) |
| T10 with a code swap (a barrier) | the new code is prepared on the main loop and bound to the same channels; at the barrier it replaces the old one between two cycles. Each ramp goes on from where it was (its state is the node's), each `rt` field's last written value is kept (so a value the new code writes the same is no change), and a trigger invoked before the swap and not yet seen is seen once by the new code. The old code is released after the barrier |
| T6 (a stop: `drive(nullptr)`) | the code stops; the ring is drained to its end by the tick; the channels stay, and the status is kept |
| T6 or T11 for a restart, T14, T17, T20 (ear's barrier, `drive(nullptr)`, then, for a removal, the block's destruction), then teardown | the code stops; the ring is drained to its end by the tick; the queue's records are drained until the configuration closed, so a goodbye is heard (6.4 step 5); then the reset (4.3) |
| I5 step 5 | the channels are freed, after the code and the loops' states (node-framework.md I5) |

- **Realtime safety is unchanged by the lifecycle.** Every barrier is `drive`, an invoke on the node's own block,
  asynchronous from slice 3a (node-framework.md I4), and none of this page's channels is allocated, freed or resized on the realtime thread
  or while the code that uses it can run: made before T5, rebound before a swap's barrier, freed after the last
  barrier. A reshape in place resizes an indexed family's slots by preparing new arrays on the main loop and handing
  them over at the swap's barrier, as the code's port arrays are (node-framework.md 6.7).
- **One node's transition never touches another's channels** (R16): each node's slots, ring, bell window and queue are
  its own; the one shared piece is the main loop's tick, which a node's drain cannot block (6.4).
- **Companions are not this page's realtime side.** A sink or source companion's `pw_filter` copies frames and keeps
  counters in atomics the tick samples (4.10); it writes no status, events or records, so it needs no ring or bell.
- **What `process` may still use beyond `Io`'s banks** (R14, node-framework.md I12): its parameters (`io.param`), its
  `rt` fields (`io.set`), coded records (`io.raise`, `io.log`), triggers (`io.triggered`), and wait-free members of its
  loop state (a mailbox of values). Never a ring of audio: that is a companion's.

## 7. The keeper path

### 7.1 Who does what

| Step | Thread | Owner |
|---|---|---|
| Decode `gesture.*`, `node.set`, `node.invoke`; read ahead; mark a gesture *ending* | control | `ear_control` (`decodeCommand`, the session) |
| The reach index of each version of the working copy | keeper | `ear_config` (`Keeper`, `GraphFileModel::reachIndex`) |
| `gesture.end`, `node.set`, `node.invoke`: ordering, file checks, the candidate; `ports-in-use` and `dropRoutes`'s route lines; a dry run's candidate | keeper | `ear_config` (`Keeper`) |
| `gesture.begin`: file state, the reach index, rows, holds | main loop | the keeper's main-loop side (which holds the running version's index), `Gestures` |
| Open, move, close and abort gestures; write slots; tell listeners | main loop | `Gestures` (`ear_core`), `NodeHost` |
| Plan, commit (each node's outcome to `NodeHost` through `nodeChanged`), or a dry run's plan answered without a commit | main loop | the keeper's main-loop side, `DesiredGraph`, `NodeHost` |
| `gesture.change`, `gesture.cancel` notifications | control, then main loop directly | the gateway, `Gestures` |
| A gesture's idle timer; on idle or a session's close, its end on the owner's behalf (G4) | control, then keeper as any end | the session (`ear_control`), then `Keeper` |
| Whether a save is related to each open gesture (G3): the new reach index against the running one, before the plan | main loop | the keeper's main-loop side, `Gestures` |

The control thread never holds a gesture (it holds the newest pending tick and an *ending* mark only); the keeper thread
never holds one either (it makes reach indexes and candidates); `Gestures` on the main loop is the one owner.

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
    R->>R: framework ramps 20 ms
  end
  M-->>E: (others) gesture.changed change (at most 30/s)
  E->>C: gesture.end {g4, -3.5}
  C->>C: mark g4 ending
  C->>K: EndGesture{session, g4, -3.5}
  K->>K: candidate: gainDb -6 -> -3.5 at its definition; its reach index
  K->>M: step: offer (end of g4)
  M->>M: plan: retune; graphFile.changed; commit: nodeChanged(retune); Gestures.close(g4)
  M-->>E: graphFile.changed, node.updated, gesture.changed end, reply {-3.5}
  K->>K: write ear.conf when free
```

- **The reach index** (keeper thread). With every offer it posts (an accepted edit's candidate, a disk read,
  `graphFile.set`), the keeper sends the reach index of that text: `GraphFileModel::reachIndex()`, new, gives for each
  node and live key `ear.conf` gives the definition's place, the text of the whole lines the defining statement is
  written on (its **defining lines**, 7.3), every node and key whose expanded value has that definition (knobly's
  `definition()`, `written.hpp:926` in the image [V]), and the committed value. It is small (one
  entry per live key in the file) and made only when the text changed. On the main loop it becomes the running index at
  commit. A text with errors has none (begin is refused `file-invalid`); a disk text the graph refused leaves the old
  running index but marks the file *not running* (begin is refused `file-refused`), since the working copy is then not
  what runs and an end would be judged against it.
- **Begin** (main loop, no keeper). The control thread hands `gesture.begin` to the gateway's main-loop side. There, in
  order: `stopping` after `DesiredGraph::close()`; `file-invalid` or `file-refused` from the file state; `unknown-node`;
  `not-live` when the type's row for `key` is not live (or not declared); the reach from the running index (a key the
  file does not give reaches its node alone, at the row's default); `shared-definition` unless every reached row is live
  and all share one kind and unit and their ranges meet; `parameter-held` if any reached pair is held. Then `Gestures`
  opens the gesture with the reach, the combined range (the intersection) and `decimals` (the smallest), tells the
  other sessions `begin`, and answers. Each slot will ramp with its own row's smoothing. Nothing is written; the working
  copy does not change. Begin no longer waits behind a keeper write: step 6.4 measures its latency while the keeper is
  writing.
- **Change** (control, then main loop): 6.6.
- **End** (keeper thread). The keeper refuses `stopping` after stop. It makes the candidate: the node's settings with
  `key` set to the rounded value, through today's `updateNode` (`graph_file_model.hpp:80` [V]), which changes the value
  at its definition, so every reached node changes in one commit. The offer carries the gesture id. On the main loop the
  main-loop side checks that the candidate's changed keys are the gesture's reach (a guard: only a hand edit can change
  a reach, and it aborts first), plans, and lets the gesture's own keys through `parameter-held`; an accepted plan is
  told and committed (7.4), then `Gestures::close` sets each reached slot to the committed value (the same, unless
  rounding moved it), tells `gesture.changed` `end`, and the reply carries the committed value. A candidate that
  changes nothing (the value is the committed one) posts a step that only closes the gesture. An end refused for its
  value leaves the gesture open; any other refusal aborts it (`refused`).
- **Cancel and node removed** abort on the main loop: every reached slot returns to the committed value,
  `gesture.changed` `aborted` with the reason and that value. **Session close and idle** end the gesture as released
  (the owner's G4): the control thread queues an end with no value (6.6), which commits the last live value as above;
  `gesture.changed` `end` carries `reason`, and the `param` record too. If that end is refused (its value no longer
  passes the rows, or the desired graph refuses it), the gesture is aborted (`refused`) and the slots return to the
  committed value, since no owner is left to send a better end.
- **What is written, and when.** Only an end (and `node.set`, `node.update`, `graphFile.set`) changes the working copy;
  `ear.conf` is written behind as today, when the keeper is free (`keeper.cpp:78-81` [V]); its fsyncs
  (`confined_folder.cpp:251`, `277` [V]) delay the next keeper step, never a begin or a tick. At most one commit per
  gesture.

### 7.3 Refusals and collisions

| Lands during a gesture | What happens |
|---|---|
| A hand edit (disk) **related** to the gesture: it changes the dragged value, its defining lines or its reach (below) | the gesture closed as aborted (`hand-edit`, no value) on the main loop before the offer's plan (the owner's G3, R31); then the file is offered as today; last in the turn each reached slot is set to its node's running value (the new file's, if the plan accepted it and changed the key; else the committed value) |
| A hand edit **not related** to the gesture | nothing happens to the gesture: no `gesture.changed`, the slots keep the hand's value, its places are rebased onto the new text's index; then the file is offered as today, and the gesture's end later edits the new text |
| A hand edit with errors, or one the graph refuses | every open gesture aborted as a related one, whatever it changed (G14): no end could commit onto it, since every end is judged against the whole working copy; the graph keeps what it ran; slots return to the running version's values |
| `graphFile.set` (the pane's Save) | as a hand edit, reason `file-saved` when related |
| `node.set` or `node.update` changing a held key | refused `parameter-held`; nothing changes |
| `node.set` or `node.update` changing other keys of a reached node | applied, whatever its outcome (7.7): a reconfigure keeps the slots; a restart keeps the hand's live value (the node's live value seeds the new node's slots) |
| A reshape that removes a reached key (an indexed member above the new count) | applied; the gesture aborted (`key-removed`) before the commit's events |
| The reached node fails, recovers or restarts (by `node.restart`) | nothing: the gesture stays open; ticks move the node's live value, which seeds its next configuration |
| `node.remove` of a reached node, or its replacement (another type) | applied; the gesture aborted (`node-removed`) |
| Another session's `gesture.begin` on a reached pair | refused `parameter-held` |
| The owning session closes (socket gone) | ended as released, the last value heard committed (`end`, `reason: "session-closed"`; the owner's G4, R32) |
| No message about the gesture from its owner for 30 s, and no end or cancel read | ended as released, the same (`reason: "idle"`; G15) |
| The reached node is disabled, stopped or started | nothing: the gesture stays open; ticks move the node's live value (a disabled node has no slots: its live value seeds the configuration an enable makes) |
| `SIGTERM` | `DesiredGraph::close()`: later begins and ends get `stopping`; open gestures are not committed and are not announced (clients are closed with 1001); the last write writes the last accepted version, which holds no live value (G16: a stop of ear is not a release). The editor, closed with 1001 while it held a control, says so once it reconnects (8.3), so the operator learns the fader went back |

The keeper's main-loop side learns the change's kind from the offer: a new member `OfferKind` (`disk`, `set`, `edit`,
`gestureEnd{id}`) beside today's members. Today's `Offer` (`main_loop_side.hpp:24-30` [V]) has `file`, `wanted`,
`placements`, `cause` (`edit` or `disk`) and `droppedEdits`; `cause` does not tell `graphFile.set` from a node edit, nor
a gesture's end from either. The offer also gains the version's reach index (7.2).

A gesture's reach changes only by a hand edit or `graphFile.set` (`node.set` sets values, never which definition a key
uses; `node.add` appends literal values), and a save that changes a gesture's reach is related to it and aborts it
first, so a reach never changes under an open gesture.

**When a save is related to a gesture** (the owner's G3, "Only a related save", R31). The owner's words: a save stops a
fader only when it changes the dragged value or the line it is defined on. ear decides it on the main loop, for each
open gesture, when the offer of a disk read or a `graphFile.set` arrives and before its plan, from two reach indexes:
the running one, which the gesture was begun against (or last rebased onto), and the new text's, which the keeper made
on its thread and posted with the offer (7.2). Neither knobly nor the text crosses to the main loop. The save is
**related** to gesture `g` when, for any pair `(node, key)` of `g`'s reach, any of these holds:

1. **The defining lines changed.** The pair's defining statement (knobly's `definition()` for the key: the `@set $trim
   0;` a variable's users share, or the setting statement `gainDb -6;` itself; for a chain of variables, the chain's
   end, as the reach is) is written on lines whose text differs, byte for byte, between the two versions. The lines are
   whole source lines, so a comment added at the end of `@set $trim 0;` is related, and a comment on the line above, or
   the same line moved elsewhere unchanged, is not (its place is rebased).
2. **The value changed.** The pair's evaluated value, after knobly's expansion, differs from the running one. This
   catches a change the lines do not show: a chain's base changed elsewhere (`@set $base -6;` behind `@set $trim
   $base;`).
3. **The reach changed.** The pair is defined elsewhere now (`trimDb.1 $other;`), or the set of pairs sharing its
   definition is not `g`'s reach (a third node now uses `$trim`, or `trimDb.2` stopped using it).
4. **The pair is gone.** The node is removed, renamed or replaced, or its key is no longer declared (a reshape removed
   the indexed member); these also abort as `node-removed` or `key-removed` when the commit says so, and the
   comparison aborts them first, as `hand-edit` or `file-saved`.
5. **The new text has errors, or the graph refuses it** (G14): every gesture, as the table above says.

A key the file did not give (the gesture moves the row's default) has no defining lines: the save is related when the
new text gives the key, or when its node's own `node` line changes. Anything else (another node's settings, a route, a
comment or a blank line elsewhere, a new node, the same lines moved) is unrelated: the gesture stays open, its index
entries are replaced by the new text's (same pairs, new places), and nothing is sent about it. The gesture's end is then
an edit of the new working copy like any other (7.2), so it keeps the colleague's change. The plan of an unrelated save
never touches the gesture's keys (their values did not change), so the slots keep the hand's value without a rule of
their own.

Its cost is one comparison per open gesture per save, over entries the keeper already makes (one per live key the file
gives); the defining lines are carried as their text's 64-bit hash with their line range, so the index stays small.
Tests: `ear_config_tests` (each entry's defining lines and their hash: a setting, a `@set`, a chain's end, a snippet's
line, a key the file does not give); `ear_core_tests` on `Gestures` with pairs of hand-made indexes (a comment on the
line above kept, a comment on the defining line aborted, the line moved kept and rebased, the value changed aborted, a
chain's base changed aborted by value, a third user of `$trim` aborted by reach, another node's setting changed kept,
a text with errors aborting all, a refused text aborting all, an unrelated `graphFile.set` kept; two gestures, one
related and one not); and a system test (a comment added by hand above the node during a gesture, the gesture's end
writing onto the new text with the comment kept; a hand edit of the same value aborting it).

### 7.4 Retune: a live change without a restart

- `DesiredGraph::plan` names each node's outcome (node-framework.md 6.6, built by slice 3d): a **retune** when every key
  whose value differs is live in the type's table (`SettingDeclaration::live`) and the ports are unchanged
  (`desired_graph.cpp:158-160` [V] today tells only "updated"). The outcome is the `applied` of 4.2 and 4.9.
- The retune reaches `NodeHost` through node-framework.md 6.6's
  `DesiredNodeListener::nodeChanged(const NodeSpec&, std::shared_ptr<const NodeConfig>, const NodeChange&)` with
  outcome `retune` and the changed keys; its default calls `nodeUpdated`, so the gateway (which sends `node.updated`
  either way, now with `applied`) and every test listener are unchanged. Revision 2's separate `nodeRetuned` is gone.
  `NodeHost` overrides `nodeChanged`: for `retune` it keeps the new config for later runs and writes each key's slot
  from `config->liveValues()`, unless a gesture holds the key, in any phase (7.7). Commit order is today's; a retune is
  told where an update would be.
- So a hand edit, `node.update` or `node.set` that changes only `gainDb` retunes: the audio ramps, no `node.state`
  follows, and standard output has a `param` record.
- A loop state and companions are not told of a retune: live rows reach only the node's code (2.4). A value that must
  reach the loop's state is an `.inPlace()` row, which its in-place `configure` posts there (X6).

### 7.5 `node.set`

On the keeper thread: `unknown-node` if the working copy has no such node; the node's settings from the working copy's
read, merged with the patch (null removes), then today's `updateNode`. When the type's `ports` for the new settings
drop ports that route lines name, the keeper refuses `ports-in-use` with those routes, or, with `dropRoutes`, removes
those lines in the same candidate (4.9). Everything after is an edit as today; the reply carries the plan's `applied`
for the node. With `dryRun`, the candidate is posted marked as a dry run: the main loop plans it and
answers, and the keeper drops it, so the working copy, the disk and every client are untouched.

### 7.6 Actions and restart

- `node.invoke` reaches the keeper's thread in order with every other request (node-framework.md 8.4), which needs no
  file and posts a step to the main loop at once. There `NodeHost::invoke(node, action, args, reply)` checks the action
  and its arguments against the type's table.
- `node.restart` (4.2) is the framework's own restart (node-framework.md 8.4 is its home; W12), ordered by the keeper
  as `node.start` is and applied on the main loop: a full stop, the node's configuration torn down and made again from
  empty with its applied settings, and back to the wanted state, the block and wires kept, `ear.conf` untouched. Its
  reply is `{"applied"}`. By phase:

  | Phase | `restart` does | Reply |
  |---|---|---|
  | `running` | T6 with cause `restarting` (`action`), the teardown, then T12 | `restart`, once the teardown began, after `node.state` `stopping` (4.3's rule 5) |
  | `stopped` | the teardown, then T12; the node ends `stopped` again | as `running` |
  | `reconfiguring` | ends the in-place change at once as T11 does (every resource, old and new, torn down; a continuation already posted runs first) and restarts with the newest committed settings (node-framework.md 6.3, D-B6) | as `running` |
  | `configuring`, `configured`, `starting` | the configuration not yet ready is torn down as I5's "before it was ready", then T12 | as `running` |
  | `failed` | T15 at once (the failed configuration is already gone) | `retry`, after `configuring` |
  | `recovering` (slice 8) | T15 at once, skipping the wait | `retry`, after `configuring` |
  | `stopping` for a restart | nothing more: a restart is already under way | `unchanged`, at once |
  | `added` | refused `not-restartable` (`not-started`): the node is configured by itself once the older node of its name is gone | the refusal |
  | `stopping` for a removal | refused `stopping` | the refusal |
  | `disabled`, or `stopping` for a disable | refused `node-disabled`: there is nothing configured to restart; `node.enable` configures it afresh (node-framework.md D-F2) | the refusal |

  A node whose block is lost cannot restart (`restartRequested` does nothing then, `node_lifecycle.hpp:29-31` [V]): it
  is refused `not-restartable` (`reason: block-lost`), and the snapshot's `blockLost` lets the editor disable the
  button first. A restart's reply never waits for the node to be `running` (X3); `--until` waits for that.
- `node.start` and `node.stop` (4.2) are neither edits nor type actions (the owner's R28), as `node.restart` is not: they reach the keeper's
  thread in order with every other request, which needs no file and posts a step to the main loop at once, as for
  `node.invoke`; there `NodeHost` sets or clears the node's stop and applies node-framework.md 6.3's rules (T6, T7, T8;
  T15 for `start` on a failed node). The reply is posted in that main-loop turn, after the `node.state` it causes:
  `stop`, `start`, `retry` or `unchanged`; `node-disabled` on a disabled node. Nothing is written, no version is made,
  and `file-invalid` never applies, so Stop and Start work while `ear.conf` has errors. Each writes an `action` record
  on standard output (`node porch stop invoked`), so a stop at 3 a.m. is in `kubectl logs` (G8).
- `node.disable` and `node.enable` (4.2) are edits at the source (R25, R28, R29): the keeper adds or removes the
  `disabled true;` line inside the node's braces in a candidate (node-framework.md D-F10, D-F11), as it edits a
  setting, the plan's outcome is `disable` or `enable`
  (node-framework.md 6.6), and the reply is the edit's, at the commit; `ear.conf` is written behind as for any edit.
- A trigger: the counter is incremented and the reply posted at once; `not-running` (with the `phase`) when the node's
  code is not driving (any phase but `running` and `reconfiguring`).
- A call: posted to the loop it names with `NodeLoop::postFromOutside`; the reply when the handler returns. When the
  node's configuration is torn down, teardown step 1 closes that loop to outside work (`closeToOthers`): a queued call
  gets `restarted` (or `stopping`), and a running one finishes and replies before the goodbye `stop()` posted behind it
  (node-primitives.md 6.4, 6.10). A stopped node's loops take calls.
- **The keeper never waits for a handler.** Its step completes when the action is dispatched. The batch's reply is held
  on the main loop until every action in it answered (or its 10 s deadline passed), then posted to the control thread
  behind every event before it, as today. A slow handler holds that client's later requests with an id (the FIFO of
  6.6), never its gesture ticks and never the keeper's queue.
- An `action` record on standard output says who asked for what (section 9).

### 7.7 Node edits and gestures against each kind of row

The owner: the node controls whether a setting can change in place, with the framework supporting it (R17). Here is
what each kind of row means for `node.set` (and `node.update`, a hand edit, `graphFile.set`) and for gestures, whatever
phase the node is in. The plan's outcome is node-framework.md 6.6's; an edit that changes several kinds of row takes
the strongest outcome (`retune` < `reconfigure` < `restart` < `reshape`), and its live keys move the slots at the
commit whatever that outcome is.

A **disabled** node takes every row the same way: the change is written and the outcome is `stored`; a live row moves
only the node's live value (which seeds the configuration an enable makes), a gesture on it is allowed as on a failed
node, and a change of ports reshapes the block alone (node-framework.md 6.7), still refused `ports-in-use` without
`dropRoutes` when routes would be stranded.

| Row | `node.set` | A gesture | Node `running` or `stopped` | Node `reconfiguring` | Node `added`, `configuring`, `configured`, `starting`, or `stopping` for a restart | Node `failed`, `recovering` |
|---|---|---|---|---|---|---|
| **live** (`.live`) | `retune`: slots move at once | yes: ticks move the slots, the end commits once (section 7) | ramps; no `node.state` | ramps at once; the reconfigure goes on (the slots are the node's, 6.7) | the node's live value moves and seeds the next configuration | the same; no restart and no recovery skip (a retune is not a reason to try again) |
| **in place** (`.inPlace()`) | `reconfigure` | refused `not-live` (X5) | `reconfiguring` (T9), then back, the same configuration | before the commit point: supersedes it (T18), the hook called again with the newest settings; after it: queued, `next` lists it, applied when the first ends, classified afresh against what the node applied (node-framework.md 6.3) | a restart with the newest settings (6.3: not yet running) | a restart at once, skipping the recovery wait (6.3) |
| **restart-only** (no mark) | `restart` | refused `not-live` | `stopping` (cause `restarting`), then `configuring` | before the commit point: ends it as T11, with the newest settings; after it: queued, then a restart (6.3) | the newest settings replace those the next configuration starts with | a restart at once |
| **makes ports** (`.shapesPorts()`) | `reshape`; refused `ports-in-use` without `dropRoutes` when routes would be stranded (4.9) | refused `not-live` (a live row never makes ports, 5.1) | `stopping` with cause `restarting` (`reshape`), the block reshaped between configurations (`inPlace: false`); or `reconfiguring` when every changed key is `.inPlace()` or `.live` (`inPlace: true`, node-framework.md 6.7) | queued, then as running | as restart-only, the block reshaped before the new configuration's `configure` | as restart-only |

- **Gestures meet the lifecycle only through slots.** A gesture holds live keys; whatever happens to the node's
  configuration (reconfigure, restart, failure, recovery, stop, disable), the gesture stays open and its live value is the node's: a reconfigure
  keeps the slots, a new node's slots are seeded from it (5.6). Two lifecycle events end a gesture: the node removed or
  replaced (`node-removed`, as today), and a reshape that removes a key the gesture reaches (an indexed member above the
  new count), aborted as `key-removed` before the commit's events, with the committed value it returns to (4.3).
- **`parameter-held` covers keys, not nodes.** An edit that changes a held key is refused; one that changes other keys
  of a held node commits, whatever its outcome, and the hand's value survives it.
- **An in-place `configure` sees live keys too.** `old` and `now` hold every key; a type whose hook reads a live row with
  `s.initial<row>()` sees the committed value, not the gesture's (5.1).
- **Restart-only by default** is the type's choice (node-framework.md 6.6), so the editor's hint is honest: "restarts
  the node" appears wherever the type has not said otherwise (8.2).

## 8. The editor

### 8.1 Stores and reducers

| Module | Holds | Tested by |
|---|---|---|
| `protocol.ts` | the wire types of section 4; `ControlErrorCode`; `NodeDiagnostic`, `EventSeverity`; open enums typed as `Known \| (string & {})` | typecheck |
| `graph.ts` | the graph, now with each node's health members (4.8), `wanted`, `disabled`, `blockLost`, `status`, `gestures`, `diagnostics`, `companions`; handlers for the new notifications (4.4) | vitest, every fixture of 4.7 |
| `health.ts` (new, pure) | from a node's health members to what 8.6 shows: the state word and style, the line under it, which actions are enabled and why not (Start, Stop, Disable, Enable, Restart, the type's), what to announce | vitest, `lifecycle.json` |
| `types.ts` (new) | the type schemas by hash; `types.get` with `ifNoneMatch` on every snapshot whose hash differs; cached in `localStorage` (wrapped in `try`, as storage may be absent) | vitest, `types.json` |
| `gesture.ts` (new, pure) | the session's own gestures as a state machine (below); what to send for each input; the control mapping (8.3) | vitest, a fake client |
| `form.ts` (new, pure) | the settings form's per-field drafts against server values; the Apply flow with its dry run and the routes a reshape drops (8.2) | vitest |
| `activity.ts` (new) | per node, the last 100 events and diagnostics since this page connected, with their times; notices, coalesced per node and code | vitest |
| `levels.ts`, `graphFile.ts`, `pane.ts` | as today | as today |

`Editor` (`editor.ts`) feeds every server text to each store, as it does now. Nothing in `web/` names a node type.
Features gate the UI: without `gestures` live rows render as plain number fields committed by `node.set`, without
`node.set` by today's `node.update`, without `types` the panel is today's JSON area.

`gesture.ts`'s states and edges:

| State | Input | Sends | Next |
|---|---|---|---|
| idle | press, or first key | `gesture.begin` | beginning(shown value, intent: hold) |
| beginning | move | nothing (keeps the newest) | beginning |
| beginning | release | nothing yet | beginning(intent: end) |
| beginning | Escape | nothing yet | beginning(intent: cancel) |
| beginning | reply ok, intent hold | the kept value as tick `seq` 1 | active |
| beginning | reply ok, intent end | `gesture.end` with the shown value | ending |
| beginning | reply ok, intent cancel | `gesture.cancel` | idle |
| beginning | refusal | nothing | idle; the control snaps back; a notice names the code (`parameter-held`: shown as held, 8.3) |
| active | move | `gesture.change`, at most one per animation frame | active |
| active | 2 s with no tick sent | the same value as a tick (keep-alive) | active |
| active | release or lost capture | `gesture.end` with the shown value | ending |
| active | Escape | `gesture.cancel` | idle |
| ending | reply | — | idle; shows the committed value |
| ending | refusal `invalid-settings` | `gesture.cancel` | idle; notice |
| any | `gesture.changed` `aborted` for its id | — | aborted until release, then idle; notice (8.3) |
| any | `gesture.changed` `end` with a `reason` for its id (ear ended it: `idle`) | — | idle; shows the committed value; notice (8.3) |
| any | connection lost | — | idle; the snapshot after reconnecting shows the value ear committed for it (`session-closed`, G4) |

### 8.2 How each row kind renders

| Row | Control | Commits |
|---|---|---|
| text with `values` | a select | on change |
| text with `pattern` or `maxLength` | a text field, checked as typed against the hint (ear still judges) | on Enter or blur |
| other text | a text field | on Enter or blur |
| whole number | a number field with steppers, `min`, `max`, unit after it | on Enter or blur |
| decimal | a number field with `decimals`, unit after it | on Enter or blur |
| optional with `defaultFrom` | empty, its placeholder saying where the default comes from ("the graph frequency", "the node's name") | as its kind |
| live decimal | a fader in the panel, mapped by `taper` (8.3), with a number field beside it | a gesture (8.3); the number field by `node.set` |
| live whole number | steppers and a number field (it steps, so no fader) | `node.set` |
| indexed family | one control per member, keys `<key>.1` .. `<key>.<n>`, `n` from the count row's current value; above 8 members a compact strip of channel faders behind a disclosure | as its kind |
| `onlyWithout` | disabled while the other key is set, its tooltip naming it | — |

- **The node box.** The box shows a knob for each live, non-indexed row when the type declares one or two of them
  (a family counts as one and is never on the box); otherwise the box shows none. `gain` shows `gainDb`.
- **Hints from the schema alone** (4.2's `change` and `shapes`), beside each non-live field: "applies in place" for
  `inPlace`, "restarts the node" for `restart`, and "changes the ports" added for `shapes`. A live control needs none:
  it moves as the operator moves it.
- Non-live rows are edited in a form and sent together with one `node.set` ("Apply"). **Apply asks first** (feature
  `dryRun`): it sends the same `node.set` with `dryRun: true`, and when the answer is `restart` or `reshape` (or names
  routes) it shows what will happen before sending ("Restarts desk: its audio stops for a moment." or "Restarts desk
  and removes 1 route: kitchen:from.left → desk:play.2", with Apply and Cancel); `retune` and `reconfigure` are sent at
  once, since nothing stops. The confirmed send carries `dropRoutes: true` exactly when the dry run named routes; if
  the real edit is refused `ports-in-use` anyway (another edit landed between), the same prompt is shown again with the
  refusal's routes. Clearing an optional field sends `null`. The JSON text area stays behind an "Edit as JSON" toggle,
  for keys a form cannot show, and is applied the same way.
- **Drafts.** Each form field keeps its own draft once the operator types in it. A server change to a field with no
  draft updates it; a server change to a field with a draft marks it "changed on ear" and offers to take ear's value,
  as the text pane does (`Panels.tsx:80` [V] today resets every field on each update, which this replaces). Live
  controls always follow the server, except a control the operator holds.
- The add-node panel renders the same form from the chosen type's schema, filled with defaults.
- A refusal is shown under the field `data.key` names (`invalid-settings`, `not-live`, `parameter-held`,
  `invalid-args`), or at the form's top when it names none; the message is never parsed.
- Live controls are disabled while the graph file has errors or the graph refused it (the file state already on the
  wire), and while the connection is not open, so the common refusals never happen.

### 8.3 Gestures from pointer and keyboard

**The control mapping.** A control's travel `t` runs from 0 to 1. The value for a travel, by `taper`:

| Taper | Value at travel `t` | Travel of a value |
|---|---|---|
| `linear` | `min + t × (max − min)` | the inverse |
| `log` (min > 0) | `min × (max / min)^t` | the inverse |
| `db` (min <= -60, max >= 0) | piecewise linear through the points (0, min), (0.10, -60), (0.25, -40), (0.45, -20), (0.60, -10), (0.75, 0), (1, max): unity at three quarters, as a console fader | the inverse, piece by piece |

A value is shown rounded to `decimals`; a row with `minIsSilence` shows its minimum as "-inf". The value sent is the
mapped value rounded to `decimals`.

| Input | Sends | Shows |
|---|---|---|
| pointer down on a fader or knob (captured; the box's knob also carries `nodrag nowheel nopan nokey`) | `gesture.begin` | follows the pointer at once |
| pointer move | ticks (8.1's table) | the local value |
| fader drag | travel follows the pointer along the fader's length | |
| knob drag | vertical: 200 px is the whole travel, up increases; Shift: 2000 px (fine) | |
| pointer up, or lost capture | `gesture.end` with the shown value | the committed value from the reply |
| Escape while dragging | `gesture.cancel` | the committed value |
| double-click, or Alt-click | `gesture.begin`, then `gesture.end` with the row's default | the default |
| Arrow keys on a focused control | the first key `gesture.begin`; each key a tick of 1% of travel (Shift: 0.1%); `gesture.end` 400 ms after the last key, or on blur | the local value |
| Page Up/Down, Home/End | 10% of travel; the ends | |
| the wheel, only on a focused control | as keys: 1% of travel per 50 px of `deltaY` (a line is 16 px), Shift fine; over an unfocused control the wheel scrolls the panel | |
| Enter in the number field beside it | `node.set` | |

- **The canvas does not see a control's input.** Every control on a node box carries React Flow's `nodrag`, `nowheel`,
  `nopan` and `nokey` classes, and stops the propagation of every key it handles, so Backspace, Delete and the arrows on
  a focused knob never delete or move the node (`App.tsx:138` [V] sets `deleteKeyCode` to Delete and Backspace, and
  `onBeforeDelete` sends `node.remove`) [U: React Flow 12's class names and its key filter are checked against its
  source in step 6.5]. A vitest test sends Backspace to a focused knob and checks no `node.remove` is sent.
- Another session's gesture: the control shows its live value moving, is disabled, and says "held by another editor".
- `phase: "aborted"` for one of its own: the control stops, ignores the pointer until it is released, and shows a
  notice by reason:

  | Reason | Notice |
  |---|---|
  | `hand-edit` | "ear.conf was edited by hand and changed amp gainDb while you held it; it now says -10." [Set -3.5 again] (or, for a file with errors or one the graph refused: "ear.conf was edited by hand and has a problem, so your change to amp gainDb was dropped; see the text pane.") |
  | `file-saved` | "ear.conf was saved from the text pane and changed amp gainDb while you held it; it now says -10." [Set -3.5 again] |
  | `node-removed` | "amp was removed or replaced while you held gainDb." |
  | `key-removed` | "amp2 now has 2 channels, so trimDb.3, which you held, is gone." |
  | `refused` | the end's refusal message |

  An end ear made for the control (`phase: "end"` with `reason: "idle"`; G4, G15) shows: "Your change to amp gainDb was
  kept at -3.5 after 30 s without a word from this editor." A `session-closed` end reaches no control (its session is
  gone); after reconnecting the snapshot shows the committed value.
- **A socket lost mid-drag** (round 4's m6). The control keeps what the pointer does locally but sends nothing while
  the socket is down; once it reconnects it does not tick the old gesture id (that would only earn `unknown-gesture`):
  it stays frozen at the value the snapshot gives until the pointer is released, then shows "Connection lost while you
  held amp gainDb; ear kept -3.5." (the value ear committed for the closed session, 7.3), and the next drag begins a new
  gesture. A connection closed with 1001 (ear stopping) shows instead, after reconnecting: "ear restarted while you held
  amp gainDb; your change was not kept: it is -6 again." (7.3's `SIGTERM` row, G16). A save that is not related to the gesture (7.3)
  shows nothing on the control: the drag goes on, and the text pane shows the new file as usual.

  "Set ... again" sends `node.set` with the value the operator last chose. On `hand-edit` and `file-saved` the control
  takes the value from the `node.updated` that follows (or keeps the committed value when none follows).
- `parameter-held` on begin: the control shows the holder's value and disables itself until the gesture ends.

### 8.4 Status, events, diagnostics and actions

- **Status**: fields with `display: "box"` on the node box, after the state line ("connected · Music Assistant");
  every field in the panel as a list, a flag as yes or no, a choice as its value, a number with its unit, `null` as
  "unknown"; `callout` fields as a banner on the box and in the panel while not at their initial value, with a copy
  button for text. Box and callout fields show only while the node is `running`, `reconfiguring` or `stopped` (the
  configuration goes on) and the value is not `null`.
- **Events**: the node's activity list in the panel, which starts when this page connected (standard output keeps the
  history); `warn` and `error` events also as a notice, coalesced per node and code ("pairing failed ×3") and at most
  one new notice per node per 5 s.
- **Diagnostics**: a badge on the box for each active entry ("flapping: connection"), its tooltip giving the count and,
  for flapping, the field's `flapsAbove`; the panel lists every entry with its time, count and whether it is active.
  An unknown code is shown by its `msg`, badged while active.
- **Actions**: one button per type action; a call with arguments opens a small form from its rows; disabled while the
  request is in flight, and as 8.6's table says per phase (a type's action needs a configured node). The lifecycle's
  buttons sit before them: Start and Stop, Restart (`node.restart`: disabled in `added`, `stopping` and `disabled`, and
  while `blockLost`), and Disable and Enable, as 8.6's table allows, with the reason as its description; a
  refusal shows its message.
- **Unknown values** (a newer ear): an unknown state is drawn as `starting`'s neutral style with its name; an unknown
  phase or `cause.kind` as its word; an unknown companion kind as its word; an unknown status kind as text; an unknown
  event code as its `msg`.

### 8.5 Accessibility

- Faders and knobs are `role="slider"` with `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, `aria-valuetext` ("-3.5
  dB", "-inf dB") and a label from the key, keyboard per the WAI-ARIA slider pattern (8.3); held by another editor:
  `aria-disabled` with the reason as its description.
- A polite live region announces callout fields when they change ("kitchen pairing code 123-456"), aborted gestures, a
  node turning `failed`, `stopped` or `disabled`, a node reconfiguring ("fpp reconfiguring port") and back, a
  reconfigure that fell back, and flapping starting; at most one announcement per node per 5 s. Phases inside `starting` are not
  announced (they pass in milliseconds), but a `waitingFor` is ("fpp still setting up: waiting for fpp.rtp"). `error`
  events are announced assertively. A refusal is shown near its control with `role="alert"`.
- When the selected node is removed or replaced, focus moves to the panel's heading; after a refused action, focus
  returns to its button.
- A keyboard path joins the canvas and the panel: Enter on a focused node box moves focus into its panel, Escape in the
  panel returns it to the box.
- State, phase, flapping, companions' readiness and route status are never shown by colour alone: each has its word or
  a text badge ("ready", "not ready"). A meter's level is also its tooltip's text ("peak -12.3 dBFS, rms -18.0 dBFS",
  or "silent"), as `Meter.tsx` gives it today.
- Focus is visible on every control; the panel's controls are in reading order; every action button has a text label.

### 8.6 The lifecycle in the editor

What the operator sees of a node in each phase (4.8 gives the members; `health.ts` is the one place that turns them
into this). Four rules run through it: silence the operator asked for is never drawn as a fault (R17), and the box
says in words why a node is silent; every port shows its level, so audio can be followed through the graph (the
owner's W3, R30; below); routes stay drawn in every phase, because the graph keeps them linked (node-framework.md 6.2),
and a wire from a silent node reads zeros, which its meters show; and one node's phase changes nothing on another node's
box (R16).

| `phase` | Box: state word, style | The line under it | Outputs and their wires | Settings panel | Actions |
|---|---|---|---|---|---|
| `added` | "starting", neutral (with "will rest stopped" after it when `wanted` is `stopped`, round 4's m9) | "waiting for the previous desk to stop" (from `waitingFor` after 2 s; empty before) | silent; wires drawn, meters at zero | editable: an edit commits and the node is configured with it | Stop or Start (by the node's stop); Disable; `restart` disabled ("starts by itself"); type actions disabled ("not configured") |
| `configuring` | as `added`, a slow pulse; Start stays offered while it will rest stopped | "configuring"; after 2 s "configuring: waiting for tcp:8928 (held by kitchen, closing)"; after a restart, an enable or a retry, its cause | silent | editable | Stop or Start; Disable; `restart` enabled; type actions disabled |
| `configured`, `starting` | "starting", neutral | "waiting for the engine"; these phases last milliseconds unless the engine is slow | silent | editable | as `configuring`; type actions enabled (its loops run) |
| `running` | "running" | the type's box status fields (8.4), or nothing | live; meters move | editable, with 8.2's hints and Apply flow | Stop, Disable, `restart` and type actions enabled |
| `reconfiguring` | "reconfiguring", its own style (not a warning colour) | "reconfiguring port" (from `keys`); "; latencyMs next" (from `next`) | wires drawn as always; a held bank's meters fall to silence and its wires thin with their level, and the meters of the ports they feed fall with them; unheld banks live. No "held" mark (the owner's W3, R30) | editable; an edit to a key in `keys` or `next` says "applies after this change" | Stop ("ends the change and stops"), Disable ("ends the change and disables"), `restart` ("ends the change and restarts", D-B6); type actions enabled (the configuration goes on) |
| `stopping` | "stopping", neutral | "stopping", "disabling", "restarting: settings changed (card)", "restarting: ports changed", "restarting: restart invoked", "restarting: could not apply port in place: <detail>", "removing" (from `cause`) | fading to silence | editable (read-only while removing) | all disabled |
| `stopped` | "stopped", its own quiet style (not a fault) | the type's box status fields, or "stopped until Start or a restart of ear" | silent, wires kept and drawn; its output meters at zero, its input meters still live | editable: an in-place change applies, any other configures afresh, still stopped | Start; Disable (its description: "release the device and keep it off across restarts; adds disabled true; to its block in ear.conf"); `restart`; type actions enabled (its loops run) |
| `disabled` | "disabled", its own quiet style, apart from `stopped`'s (not a fault) | "disabled in ear.conf" | silent, wires kept and drawn; its output meters at zero, its input meters still live; no companions | editable: an edit is written and kept for when it is enabled (`applied: "stored"`); a change of ports redraws its handles | Enable (its description: "removes disabled true; from its block in ear.conf"); Start, Stop and `restart` disabled ("disabled in ear.conf: enable it first"); type actions disabled ("not configured") |
| `failed` | "failed", the failure style | `reason.detail`, in full in the tooltip and the panel | silent, wires kept | editable: a change configures it afresh (node-framework.md 6.3) | Start ("try again", a retry) and `restart` enabled unless `blockLost`; Stop; Disable; type actions disabled |
| `recovering` (slice 8) | "recovering" | "retry in 4 s (attempt 3): card lost", counting down from `retryInMs` | silent | editable: a change tries at once | Start ("try now"), `restart`, Stop, Disable; type actions disabled |

- **A reshape** redraws the box's handles in place when `node.updated` brings `ports`: surviving handles keep their
  wires (which never go `pending`, node-framework.md 6.7), new handles appear unwired, and the box keeps its position.
  Routes the reshape dropped were confirmed by the operator before the edit (8.2) and leave as `route.removed`. A
  reshape the operator did not make (a hand edit) shows the same way; its dropped routes were in the edit.
- **A notice for a fallback.** A `stopping` whose restart's cause is `fell-back` also raises a notice (coalesced as events are,
  8.4): "fpp could not apply port in place, so it restarted: <detail>". The panel keeps the new value: `ear.conf` holds
  it.
- **The wait is the answer to "why is it silent".** Whenever the node is silent, the box says why in words: not configured
  yet, configuring and what for, reconfiguring and which keys, stopped by the operator (until Start or a restart),
  disabled in `ear.conf`, restarting and why, failed and why, recovering and when.
- **Every port shows its level** (the owner's W3: "I would like each port to show meter levels so I can see audio
  traversing the graph", R30). What is built today, and stays: each block meters every public port, inputs and outputs,
  every cycle, whether its code drives it or not (`port_blocks.cpp:414-422`, after the code or the silence [V]), and
  publishes a peak and RMS window 30 times a second (`levels.subscribe`, keyed by node and public port, `protocol.ts:151`
  [V]); the editor draws a meter beside every port handle, inputs and outputs alike (`EarNode.tsx`'s `Port` renders a
  `Meter` for each, `Meter.tsx` [V]), and draws each wire by its from port's level (`LevelEdge.tsx` [V]). An input's
  meter reads what arrives on it (the sum of its wires), so a silent node's inputs still show the audio that reaches it.
  What revision 5 adds: (1) no "held" marks or dashed held wires, and no `holds` member on the wire (W7): a held bank
  is seen as its meters reading silence, its wires thinning, and the meters of the ports it feeds falling with it, with
  the box's "reconfiguring <keys>" saying why; the framework's hold applies after the code and before metering
  (6.7), so the meters show exactly what peers hear; (2) the meters keep running in every phase, `stopped`, `disabled`
  and `failed` included, since the block is kept and always processes (node-framework.md I1, 6.2), which today's
  editor already does for any state it draws; (3) a reshape registers the block's new public ports for levels and drops
  the vanished ones at its barrier (node-framework.md 6.7), so a new handle's meter moves from its first window: new in
  slice 3d, since today a block's metered ports are fixed when it is made (`port_blocks.cpp:93` [V]); (4) a test in
  step L3 that a held bank's output meter, the wire's level and the fed input's meter all read silence during an `rtp`
  port change and come back after. Inside banks (the companions') stay unmetered: they are never drawn.
- **Fast phases do not flicker.** `health.ts` shows a `starting` phase's line only once it has lasted 250 ms (or at
  once when it carries `waitingFor`), so a normal start reads "starting" then "running". `stopped` is drawn as quiet, never as a fault: the operator
  asked for it.

### 8.7 Companions in the editor

Companions are hidden, not secret (node-framework.md 9.6; gate C2, answered by the owner as recommended, its 14.1 Q28):
the box shows them as small badges along its bottom edge, never as boxes, handles or wires.

- **A badge per companion**: its kind as a short word (`card out`, `card in`, `rtp`, `sink`, `source`) and a dot that
  is filled when `ready` and hollow when not, with "ready" or "not ready" as its accessible text; a `retiring` one is
  drawn faded beside its replacement during a make-before-break.
- **The tooltip and the panel** give the companion's engine name, the inside bank it joins and its counter ("rec.writer:
  4096 frames dropped since it was made"). A counter above zero puts a small warning mark on the badge; `ear.sink-dropped` and
  `ear.source-shortfall` appear in the diagnostics list (8.4).
- **No companions, no strip**: `white` and `gain` show nothing. A failed or disabled node's strip is empty (its
  configuration was torn down), and a failed node's `reason` names the companion that failed. A stopped node's strip
  stays as it was: it keeps its companions (R26).
- Companions never take part in selection, dragging, deletion or route-making on the canvas.

## 9. Standard-output records

New kinds, each with `v`, `seq`, `t`, `kind`, its fields, `msg`, as `status_log.hpp:31-56` [V] frames every record today.
`v` versions a record's *shape*: every change here is additive, so it stays 1. The set of kinds is open (a reader skips
kinds it does not know, and a kind may stop appearing); removing `note` is such a change (below). No record carries a
run or a generation (node-framework.md I6).

```json
{"v":1,"seq":120,"t":"2026-11-08T10:01:50.311002Z","kind":"status","node":"kitchen","field":"connection","from":"dialling","to":"connected","msg":"node kitchen connection dialling -> connected"}
{"v":1,"seq":121,"t":"2026-11-08T10:01:50.311040Z","kind":"status","node":"kitchen","field":"pairingCode","from":"","to":"123-456","msg":"node kitchen pairingCode \"\" -> \"123-456\""}
{"v":1,"seq":140,"t":"2026-11-08T10:02:12.000300Z","kind":"status","node":"kitchen","field":"connection","from":"dialling","to":"connected","heldBack":9,"msg":"node kitchen connection now connected (9 changes held back)"}
{"v":1,"seq":141,"t":"2026-11-08T10:04:00.100000Z","kind":"status","node":"kitchen","reset":true,"msg":"node kitchen status reset"}
{"v":1,"seq":122,"t":"2026-11-08T10:01:52.004211Z","kind":"event","node":"kitchen","code":"pairing-failed","severity":"warn","args":{"reason":"pairing-code-mismatch"},"msg":"node kitchen pairing failed: pairing-code-mismatch"}
{"v":1,"seq":123,"t":"2026-11-08T10:02:11.120400Z","kind":"diagnostic","node":"kitchen","code":"ear.status-flapping","subject":"connection","active":true,"count":2,"transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
{"v":1,"seq":126,"t":"2026-11-08T10:03:41.000300Z","kind":"param","node":"amp","key":"gainDb","from":-3.5,"cause":"aborted","gesture":"g4","reason":"hand-edit","msg":"node amp gainDb gesture g4 aborted (hand-edit) at -3.5"}
{"v":1,"seq":126,"t":"2026-11-08T10:03:50.000300Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","reason":"idle","msg":"node amp gainDb -6 -> -3.5 (gesture g4 ended: idle)"}
{"v":1,"seq":127,"t":"2026-11-08T10:05:12.770000Z","kind":"action","node":"desk","action":"restart","result":"ok","msg":"node desk restart invoked"}
{"v":1,"seq":129,"t":"2026-11-08T10:09:29.990000Z","kind":"action","node":"porch","action":"stop","result":"ok","msg":"node porch stop invoked"}
{"v":1,"seq":128,"t":"2026-11-08T10:05:20.770000Z","kind":"action","node":"kitchen","action":"forget-pairing","result":"action-failed","detail":"no pairing with ma-0000","msg":"node kitchen forget-pairing refused: no pairing with ma-0000"}
```

- `status`: every change as written, paced per node and field (6.5). A record after held-back changes is the trailing
  record: its `to` is the field's value now and `heldBack` counts the changes skipped, so **the last `status` record of
  each field always matches the view**. A configuration's reset is one record with `reset: true` and no field; a `from` after it
  is the field's initial value. A text value in `msg` is quoted when it is empty or holds a space; `null` is written
  `unknown`. The framework's `ear.` counters have no `status` records (4.6).
- `event`: `msg` is `node <n> ` and the sentence with its arguments; `severity` is also the record's level for tools;
  `t` for a realtime event is its cycle's start (6.4).
- `diagnostic`: the kind exists (`ear.close-overdue` [V]); it gains `subject`, `active`, `count` and the codes of 4.6.
- `param`: one per reached node and key at each committed live change, whatever made it (`cause`: `gesture`, `set` for
  `node.set` and `node.update`, `disk` for a hand edit or `graphFile.set`) and at each abort (`cause`: `aborted`, with
  `reason`; `to` is the committed value it returned to, absent for `hand-edit` and `file-saved`, whose new value the
  following `param` or `graph-file` record says). A gesture ear ended for its owner (G4) is a `cause: gesture` record
  with `reason` (`idle` or `session-closed`). A non-live change has no `param` record: its `graph-file` and `node`
  records say it.
- `action`: an operator's request that edits nothing (round 4's m7): one per `node.invoke`, `node.start`, `node.stop`
  or `node.restart` answered (`action` is `start`, `stop` or `restart` for the last three, which leave no `graph-file`
  record), with `result` (`ok` or the refusal's code) and `detail` for a refusal. The word names the record's kind, not
  a type action: the lifecycle requests are not type actions (4.2). `node.disable` and `node.enable` are edits: their
  `graph-file` and `node` records say them.
- `note`: written only by `sendspin` until step 5.5; then no code writes it, and the kind is removed from the encoder.
  No record's shape changes, so `v` stays 1; but operators grep `says pairing-code` (the `ear-on-talos` skill), so step
  5.5's commit carries a `BREAKING CHANGE:` footer naming the replacement (10.1), and the release notes say it.
- `output-records.json` gains one case per new kind, per diagnostic code, a trailing `status` record and a reset;
  AGENTS.md's record table gains the same lines.

### 9.1 The lifecycle's records

Each lifecycle transition that node-framework.md 6.2's table gives a `msg` is one `node` record; that table is the home
of the sentences, and this section of the fields. Today's `node` records (`starting`, `running`, `failed`, `stopped`,
`output-records.json` [V]) keep every field they have; each gains `phase`, and none carries a run (node-framework.md
I6). Today's removal line, `node <n> stopped dropped=<frames>`, becomes `node <n> removed dropped=<frames>`, since
`stopped` now names a resting node (node-framework.md D-A10); the system tests that wait for it change in step L1.

```json
{"v":1,"seq":301,"t":"2026-11-08T10:06:00.118000Z","kind":"node","node":"fpp","state":"reconfiguring","phase":"reconfiguring","keys":["port"],"msg":"node fpp reconfiguring: port"}
{"v":1,"seq":302,"t":"2026-11-08T10:06:02.258000Z","kind":"node","node":"fpp","state":"running","phase":"running","afterMs":2140,"msg":"node fpp running"}
{"v":1,"seq":310,"t":"2026-11-08T10:07:10.000300Z","kind":"node","node":"desk","state":"stopping","phase":"stopping","cause":{"kind":"restarting","restart":{"kind":"reshape","added":[],"removed":["play.2"]}},"msg":"node desk stopping: restarting (ports changed)"}
{"v":1,"seq":320,"t":"2026-11-08T10:08:00.400000Z","kind":"node","node":"fpp","state":"stopping","phase":"stopping","cause":{"kind":"restarting","restart":{"kind":"fell-back","reason":"resource-refused","detail":"udp:5006 is held by another program"}},"msg":"node fpp stopping: restarting (could not apply port in place: udp:5006 is held by another program)"}
{"v":1,"seq":330,"t":"2026-11-08T10:09:02.000000Z","kind":"node","node":"fpp","state":"starting","phase":"configuring","waitingFor":[{"kind":"companion","name":"fpp.rtp"}],"msg":"node fpp still configuring after 2s: waiting for fpp.rtp"}
{"v":1,"seq":335,"t":"2026-11-08T10:09:30.000000Z","kind":"node","node":"porch","state":"stopped","phase":"stopped","msg":"node porch stopped"}
{"v":1,"seq":337,"t":"2026-11-08T10:09:40.000000Z","kind":"node","node":"porch","state":"stopping","phase":"stopping","cause":{"kind":"disabling"},"msg":"node porch stopping: disabling"}
{"v":1,"seq":338,"t":"2026-11-08T10:09:40.250000Z","kind":"node","node":"porch","state":"disabled","phase":"disabled","msg":"node porch disabled"}
{"v":1,"seq":340,"t":"2026-11-08T10:10:00.000000Z","kind":"node","node":"rec","state":"stopping","phase":"removed","droppedFrames":0,"msg":"node rec removed dropped=0"}
```

| Transition (node-framework.md 6.3) | `phase` | Fields beyond `node`, `state`, `phase` | `msg` (node-framework.md 6.2's) |
|---|---|---|---|
| T1 | `added` | — | `node <n> starting`; none for a node added disabled, whose T19 follows in the same turn (round 4's m5) |
| T19, T20's end, T21 | `disabled` | — | `node <n> disabled` (at boot, one per node `ear.conf` disables) |
| T2, T3, T4, T22 | `configuring`, `configured`, `starting` | `cause` on `configuring` after a restart, a retry or an enable | none, but `node <n> starting: <cause>` after a restart, retry or enable (`enabling`) (W4) |
| still in `added`, `configuring`, `configured` or `starting` after 2 s, and again whenever `waitingFor` changes, at most once per 10 s per node | that phase | `waitingFor` | `node <n> still <phase> after <s>s: waiting for <what>`, `<what>` from `waitingFor`: `the previous <n>`, `tcp:8928 (held by kitchen)`, `tcp:8928 (held by a leaked configuration of kitchen; starts when it closes)`, `its store`, `its recording`, `loop <name>`, `<companion>`, `its consumer`, `the engine` (new, W4) |
| T5, T10 | `running` | `afterMs` after an in-place change (from T9) | `node <n> running` |
| T9 | `reconfiguring` | `keys` | `node <n> reconfiguring: <keys, comma-separated>` |
| T6, T11, T17, T20 | `stopping` | `cause` | `node <n> stopping[: <cause>]`, `<cause>` by kind: `stop`, `disabling`, `removing`, `restarting (<restart>)`, `<restart>` by kind: `settings changed: <keys>`, `ports changed`, `restart invoked`, `could not apply <keys> in place: <detail>` (`fell-back`; `<detail>` is the hook's reason, the refusal, `deadline 10s passed` or the loss), `recovery attempt <n>` |
| T7, T4 when wanted stopped, T10 when it was stopped | `stopped` | — | `node <n> stopped` |
| T13, T14 | `failed` | `failure` (today's, `{code, detail}`) | `node <n> failed <code>: <detail>` (today's) |
| T16 (slice 8) | `recovering` | `failure`, `attempt`, `retryInMs` | as node-framework.md 8.8's example |
| gone (T17's end) | `removed` | `droppedFrames` (today's: the sum over the node's sinks) | `node <n> removed dropped=<frames>` |

- **The record's `failure` and the wire's `reason`** are one value under two names, as today (`output-records.json`
  calls it `failure`, `edits.json` `reason` [V]); neither is renamed (W2).
- **Order on standard output** follows the main loop, as today: an `action` record (`restart invoked`) precedes the
  `stopping: restarting` record it causes, a `graph-file` record precedes the `node` records its commit causes, and an
  in-place change's fallback is `ear.reconfigure-fell-back` (a `diagnostic` record) just before its `stopping` record.
- **`still` records are paced** per node (one per 10 s) so a node stuck for an hour writes a line every 10 s at most,
  and stop once the phase ends; they are never dropped by the log pacing, since they are `node` records, which are not
  paced (`status_log.hpp:53-56` paces `log` records only [V]).
- `output-records.json` gains one case per row above and per `cause.kind`; AGENTS.md's record table gains the new
  `msg` lines (`reconfiguring`, `stopping: <cause>`, `stopped`, `disabled`, `removed`, `still`, `recovering`).

## 10. Migration

### 10.1 Sendspin's notes

Step 5.5 rewrites sendspin's reporting against 3.2's table in one change:

- `notes.hpp` and `notes::printable` go (slice 3c already moved note pacing into core: node-primitives.md section 5):
  the framework makes text safe and paces events.
- The worker and key store write through the `Report<spec>` that `configure` hands the worker's state from
  `node.report()` (sendspin is on slice 3c's loops and its three hooks by then, and on its source and sink companions since the same slice), with every
  field `writer::loop(worker)`, instead of `NodeHandle::say`.
- **`stream` and `sending` come from the loop's state, not from realtime code** (node-framework.md 13.3): sendspin has
  no realtime code after slice 3b. The worker writes `stream`, `streamRate`, `streamChannels` and `streamBits` when the
  library starts or ends a stream (its listener callbacks, on the worker), and `sending` and `sendingRate` when the
  library asks for audio and when a loop state's sink consumer first gets a chunk after it (a fact the consumer
  posts to the worker, since a library thread writes no status, 5.2). The source companion's played reports and the
  sink's chunks are facts the loop's state reads; they never write status themselves.
- **A rename, a port change or a listening↔dialling switch keeps the configuration** (node-primitives.md G19, decided
  (a); its 4.2 is the home): the status stays, and the worker rewrites the fields the change touches (`instance`,
  `listenPort`, `url`, `connection`) as the new client connects; the connection itself drops and comes back either way.
  Were G19 overturned to (d), every such change would restart, and the status reset.
- `NodeHandle::say`, core's note pacing and `NodeNoteListener` are removed once nothing calls them; `StatusLog::nodeSays`
  and the `note` kind go.
- The system tests that match `says` lines (`test/system/`, `child_process.hpp` matching `msg`) match the new `msg`
  lines: `node kitchen pairingCode "" -> "123-456"` instead of `node kitchen says pairing-code 123-456`. The pairing
  test reads the code from the `status` record's `to` field instead of parsing text.
- AGENTS.md's `sendspin` notes list becomes its status fields and events; the `sendspin` and `ear-on-talos` skills'
  "what its notes mean" sections are rewritten to name fields and events (the operator's grep becomes
  `jq -r 'select(.field=="pairingCode") | .to'`).

### 10.2 Today's clients

| Client | What changes for it |
|---|---|
| The editor before this page | works: it ignores the new notifications and members, and draws `reconfiguring`, `stopped`, `disabled` (and later `recovering`) as an unstyled word (4.8); it has no Stop, Disable or Enable buttons, and a node another client disabled shows as `disabled` with its wires and meters, as any node. A `node.update` changing only live rows no longer restarts the node, and one changing only in-place rows reconfigures it (better, and not observable as an error). A `node.update` that would strand routes is refused `ports-in-use` where today it is refused `unknown-port` (the plan checks every wanted route against the new ports, node-framework.md 6.7); the editor shows either by its message |
| `scripts/rpc-call.mjs` | works unchanged; gains `--until` (step L1) and `--follow` (step 5.2) |
| `scripts/image-smoke.sh` | works; step 5.1 adds a `types.get` check |
| The system tests | `says` matches move to `status` and `event` records (step 5.5); new tests per step |
| Tools reading stdout | new kinds appear; `node` records gain `phase` and new phases (`reconfiguring`, `stopping: <cause>`, `stopped`, `disabled`, `still`), `action` records for `start` and `stop`, and a removal's line becomes `removed dropped=`; `note` disappears at step 5.5 (a `BREAKING CHANGE:` release). A system test that waits for `node <n> starting` after an edit waits for `node <n> restarting: ...` (node-framework.md 13.2) |

### 10.3 What changes in frozen headers

Each named header's first line says it changes only through a design page and a re-cut. `web/src/protocol.ts:5-6` [V]
says more: "only through an erratum in that page's Errata section (section 11)", that page being batch2.md. So step L1
(the first step to change the wire, O1) adds one erratum to batch2.md section 11 that names this page as the home of the
wire's additions from protocol 1 on (as `knobly-config.md`'s "Wire additions" is for the graph file's methods), and
`protocol.ts`'s header names both pages. Each later step's wire change is then made here and re-cut.

| Header | Change | Step |
|---|---|---|
| `node_lifecycle.hpp` | replaced by slice 3a's machines (node-framework.md 13.2, its erratum to batch2.md section 11); this page adds nothing to it | 3a |
| `node_host.hpp`'s `NodeHealth`, `NodeHealthListener` | `phase`, `since`, `wanted`, `cause`, `keys`, `next`, `waitingFor` (L1; 3a gives `phase` and `cause`, this page the rest); `NodeCompanionsListener` (L2) | L1, L2 |
| `node_type.hpp` | `SettingKind::decimal`; `SettingDeclaration::live`, `indexed`, `shapesPorts`; `NodeConfig::liveValues()`; `NodeType::schema()`, `checkLive()` | 5.1, 6.1, 6.3 |
| `run_handle.hpp` (slice 3c's) | the per-node queue behind it; `say` removed | 5.2, 5.5 |
| `command_error.hpp` | `InvalidSettings::key` | 5.1 |
| `settings_table.hpp` | decimal, indexed and live rows; text hints, `defaultFrom`; `initial<row>()`, `get` refused on live rows | 5.1, 6.1, 6.2, 6.3 |
| `type_spec.hpp` | `TypeSpec`'s later members; `Io`'s `param`, `set`, `raise`, `log`, `triggered`; `Smoothed`; status, event, action and log tables; `Report`, `NodeOf::report` | 5.2, 5.3, 5.4, 6.3 |
| `node_host.hpp` | status, event and gesture listeners; `setLive`, `invoke`; `RealtimeBell`; `blockLost` in the view | 5.2, 5.3, 5.4, 6.3 |
| `desired_graph.hpp` | nothing of this page's: the plan's outcome and `nodeChanged` are slice 3d's (node-framework.md 6.6); a dry run uses `plan` without `commit`, as built | — |
| `commands.hpp`, `events.hpp`, `gateway.hpp`, `session.hpp`, `graph_file_state.hpp` | the new commands and events; `node.state`'s members, `node.companionsChanged`, `node.updated`'s `applied` and `ports`; `dryRun` and `dropRoutes`; the direct gesture crossing; read-ahead; session ids; `ControlRefusal` | L1 to 6.4 |
| `graph_view.hpp` | `NodeView`'s health members, `wanted`, `companions`, `blockLost`, `status`, `gestures`, `diagnostics`; `GraphView`'s `protocol`, `features`, `typeSchemaHash` | L1, L2, 5.1, 5.2, 6.4 |
| `web/src/protocol.ts` | section 4's types | each step |

## 11. The cut: buildable steps

Each step keeps every test green and changes no audio path unless it says so, and adds its feature to `features`
(4.1). node-framework.md 13.1 orders the slices 3a, 3b, 3c, 3d, then 5, 6, 8, 9 (its D-D1). This page's steps
follow it (O1): **L1, L2 and L3 land inside slices 3a, 3b and 3d**, as the wire side of each (they need what those
slices build and nothing of slice 5), so the lifecycle is never on the wire in a shape that must change later; **5.1 to
5.5 follow slice 3c** (the node handle and loops: G6), after L3; **6.1 to 6.5 follow slice 3d and 5.2** (the plan's
outcomes and the node's queue); **R8 lands inside slice 8**.

| Step | Lands with | Needs | Feature | Changes the audio path |
|---|---|---|---|---|
| L1 | slice 3a | 3a's machines | `lifecycle` (and the frame: `protocol`, `features`) | no |
| L2 | slice 3b | 3b's companions | `companions` | no (3b does) |
| L3 | slice 3d | 3d's outcomes, reshape | `node.set`, `reshape`, `dryRun` | no (3d does) |
| 5.1 to 5.5 | slice 5 | 3c, L3 | `types`, `status`, `actions` | no |
| 6.1 to 6.5 | slice 6 | 3d, 5.1, 5.2 | `live`, `gestures` | 6.3 (a live change ramps instead of restarting) |
| R8 | slice 8 | 5.2, slice 8's recovery | `recovery` | no (slice 8 does) |

**L1 The lifecycle on the wire, and the protocol frame** (with slice 3a). The frame first: `protocol: 1` and
`features` in the graph, every enum open in `protocol.ts` (`Known | (string & {})`) with neutral fallbacks, the
batch2.md erratum naming this page as the wire's home from protocol 1 (10.3), `unknown-values.json`. Then health:
`NodeHealth` and `node.state` with `phase`, `since`, `wanted`, `cause`, `keys`, `next` and `waitingFor` (4.8; `keys`
and `next` stay absent until 3d makes an in-place change); the same members in snapshots and `node.added`, and
`disabled` on a node; `node.start`, `node.stop` and `node.restart` (7.6's table; W12) through the keeper writing
nothing, with their `action` records, and
`node.disable` and `node.enable` as edits at the source of the node's own `disabled` line (R34), with the graph file's
reserved key `disabled` inside the node's braces and `invalid-settings` for a `disabled` key inside `settings` (node-framework.md 13.1, 3a, D-F8 to D-F12); `node-disabled`, `not-restartable`; `node.gone` beside `node.stopped`;
`ear.fade-unacknowledged`, `ear.barrier-overdue` and the main loop's stall records (4.6); `LogEntry::run`, its
encoder line and the fixture's `run` member removed (4.7); the `node` record's `phase` and `cause`, the `stopping`, `stopped`, `disabled`,
`removed` and `still` records (9.1);
`ear.hook-slow` on the wire; `health.ts` and 8.6's table for the phases 3a has; `rpc-call.mjs --until`. Tests:
`lifecycle.json` (the phases 3a has) in `ear_control_tests` and vitest; `ear_core_tests` with fake adapters (a
`node.state` per health change, in order with the commit's events (4.3's rule 5), and one for a `wanted` change
alone; `node.restart` in every phase of 7.6's table; a stop and a start keeping status and
companions, with no `graphFile.changed`; a disable resetting status and emptying companions, then `disabled`; an
enable configuring afresh; `start`, `stop` and `restart` on a disabled node refused `node-disabled`; a stop forgotten by
a restart of the harness's ear and kept through a hand edit that keeps the node; a late message from a closed configuration never sent; `waitingFor` told after 2 s of the harness's clock and again on a change, at most once a second; the
`still` record once per 10 s); `output-records.json`'s new cases; vitest for `health.ts` (every phase's line, style
and enabled actions; a starting phase hidden for 250 ms; an unknown phase drawn by its word); a system test: an `rtp`
node on a UDP port another socket holds writes `node fpp still configuring after 2s: waiting for fpp.rtp`, and `graph.get`
shows `waitingFor`; another: `node.stop` writes nothing to `ear.conf` and a restart of ear runs the node again, while
`node.disable` turns `node white noise;` into `node white noise { disabled true; }`, survives the restart (`node noise
disabled` at boot, with no `starting` record before it) and `node.enable` gives back `node white noise;`; another:
`node.disable` of one of two nodes whose blocks read `disabled $quiet;` replaces only its line, and the other keeps
playing; another: `node.restart` of a `card` node on the test card brings it back without a change to `ear.conf`, its
records being `action`, `stopping: restarting (restart invoked)`, `running`; a hand-written `node white noise disabled;` boots
nothing and is shown as `ear/bad-directive` with the hint to write `disabled true;` inside the braces; `scripts/image-smoke.sh` checks `protocol` and `features`.

**L2 Companions on the wire** (with slice 3b). `companions` in snapshots and `node.added`; `node.companionsChanged`,
whole lists, on each change and counters at most once a second (4.10); `ear.sink-dropped` and `ear.source-shortfall`;
the `removed` record's `droppedFrames` summed over sinks; 8.7's badges. Tests: `companions.json`; `ear_core_tests`
(a list per readiness change, `ready` before `running` (4.3's rule 8), counters sampled not pushed, the list emptied by
a teardown, a closed configuration's list never sent); `ear_pipewire_tests` (a sink whose consumer stalls: `dropped` rises in the
sampled view and one `ear.sink-dropped` per 10 s); a system test: `graph.get` lists `rec.writer` ready while `rec`
records, and a route naming `rec.writer` is refused; vitest for the badges' state and text.

**L3 Changes while running** (with slice 3d). `applied` with `reconfigure` and `reshape` on `node.update` and
`node.updated`; `node.set` (the merge patch, 4.2, 7.5); `dryRun` (a plan without a commit, 4.9); `ports` on
`node.updated` and the reply, with `inputs` and `outputs`; `ports-in-use` and `dropRoutes`, the keeper removing
stranded route lines in the candidate; `keys` and `next` on `node.state`; the `fell-back` cause and
`ear.reconfigure-fell-back`; the `reconfiguring` record with `afterMs` on the `running` that ends it; 8.6's reshape
redraw, and levels registered for a reshape's new ports at its barrier (8.6, W3); a disabled node's `stored` outcome
and its block reshaped alone; `node.set`'s abort reason `key-removed` waits for 6.4. Tests: `node-set.json` and the reconfigure
and reshape cases of `lifecycle.json`; `ear_config_tests` (`node.set`'s merge; a dry run changes nothing and tells no
one; `dropRoutes` removes exactly the stranded route lines in one write; a route line inside a snippet refused
`shared-definition`; `ports-in-use` naming every route); `ear_core_tests` (`keys` on `node.state`; an edit during an
in-place change in `next`; a fallback's cause and diagnostic before its `stopping`); `ear_pipewire_tests` (a reshape's
new port metered from its first window, a vanished one gone from the levels); a system test: `node.set` of an `rtp`
node's port answers `reconfigure`, `node.state` goes `reconfiguring` with `keys: ["port"]` and back to `running` with
no status reset, the WAV it feeds has silence and then the tone with no `stopping` record, and `levels` shows
`fpp:stream.1` and `rec:record.left` silent during the change and back after (W3); another: a card's `playback` from 2 to 1 on the test card with `dropRoutes`, the surviving
route's `route.status` never `pending`.

**5.1 The schema on the wire.** `NodeType::schema()` from the settings table (today's rows; text hints and
`defaultFrom` added to `rtp`'s, `card`'s and `sendspin`'s rows; `change` from 3d's marks; `.shapesPorts()` added to
`card`'s `playback` and `capture` and `rtp`'s `channels`); `types.get` answered on the control thread from an encoding
made at boot; `typeSchemaHash` in the graph; `InvalidSettings::key` and `data.key`, `data.detail`; `data.pointer` on
-32602; `types.ts`; the settings form rendered from the schema, with per-field drafts, 8.2's hints, the Apply flow with
its dry run and route prompt, and the JSON toggle. Tests: `types.json` and `errors.json` in `ear_control_tests` and
vitest; `form.ts`'s drafts and its Apply flow against a fake client (a `reconfigure` sent at once; a `restart` asked
first; a reshape's routes shown, then sent with `dropRoutes`; a `ports-in-use` after a stale dry run prompting again);
the all-type tests that every schema encodes, every `values` entry parses, every optional row has a `default`,
`defaultFrom` or `help`, and every `.shapesPorts()` mark agrees with `ports()` (5.9); `image-smoke.sh` calls
`types.get`; a system test that `graph.get`'s hash matches `types.get`'s.

**5.2 Status and events off the realtime thread.** `StatusTable`, `EventTable`, `LogCodes` in `TypeSpec` (with
slice 3c's U1 test extended); `Report<spec>` and `NodeOf::report`; the per-node queue, the
drain and the tick on `Timers`; `node.statusChanged` with its reset, `node.eventRaised`, the `status` (paced per field,
with trailing records and the reset record) and `event` records; `status` in snapshots and `node.added`; node
diagnostics keyed by code and subject, with `active` and `count`; flapping, `ear.events-dropped`,
`ear.records-dropped`, and slice 3c's `ear.loop-slow`, `ear.loop-source-left`, `ear.claim-wait`, `ear.close-late` and
`ear.close-overdue` on the wire; the harness's captures; `rpc-call.mjs --follow`. Tests: `ear_core_tests` with a test
type (order of status before event, a `lost` after the last status, a write of a closed configuration dropped, the reset after
the last values, status kept through an in-place change, a stop and a start with no reset, a write from a loop's first
work drained, the queue's bound, flapping
and settling with the harness's clock, two fields flapping, the tick's 33 ms cap, the trailing record within 1 s of the
last held change and matching the view); `status.json`, `diagnostics.json`; `output-records.json`'s new cases; the
editor's status list, box fields hidden outside `running`, `reconfiguring` and `stopped`, and badges (vitest on the reducer).

**5.3 The realtime ring.** The ring with its log budget, `rt` fields, `io.set`, `io.raise`, `io.log`, cycle stamps, the
gated bell (`ear_pipewire`, an eventfd on the main loop); the channels bound when the code is prepared (T3) and rebound at a code swap
(6.7). Tests: `ear_core_tests` (records in order, the bound, log records refused past 192 while events pass,
transitions counted between samples, an event's `t` from its cycle; a code swap in the harness keeps an `rt` field's
last value, so the new code writing the same value makes no record, and an unseen trigger is seen once by the new
code); `ear_pipewire_tests` (a test block's code sets a field and raises an event; both reach the main loop within a
window; an idle block rings nothing; a held bank's code still writes its fields); a test that `process` allocates
nothing while writing (the handoff's allocation test's method, now the sink's).

**5.4 Actions.** `ActionTable`, triggers, calls on a loop of the spec, `node.invoke` through the keeper, `not-running`
with its `phase`, deferred batch replies with their deadline, `ControlRefusal`, the `action` record for type actions;
the editor's action buttons, enabled by `health.ts`; sendspin's `forget-pairing` as the first call. (`node.restart`,
`blockLost` and `not-restartable` are step L1's.) Tests: `actions.json`; `ear_config_tests` (an invoke ordered between
two edits in one batch; the keeper takes the next batch while a handler is slow); `ear_core_tests` (an in-place change
ended by `node.restart` with every resource torn down, with D-B6; a trigger seen once; a call refused `not-running`
before the node is configured; a call taken while stopped; a queued call answered `restarted`).

**5.5 Sendspin's notes split.** 10.1. Tests: the sendspin system tests, rewritten to read records;
`ear_node_sendspin_tests` for each field and event, `stream` and `sending` written from the loop's state; a rename
through the in-place `configure` keeping `clientId` and the configuration [U: whether sendspin-cpp renames without a new client,
node-framework.md 15]; the standard-output guard still passes. The commit carries the `BREAKING CHANGE:` footer (9).

**6.1 Decimal rows.** The rule, the walk, knobly's `number()` reader and the fixed writer (negative zero as `0`), the
schema's `decimal`, tapers and `minIsSilence`, the editor's number field. Tests: `settings_table` cases (range, integer
taken, NaN impossible), compile-fail cases (`decimal-*`, `log-taper-nonpositive`, `db-taper-range`); `ear_config_tests`
(read `-3.5`, `-6`, `0.25`; write and read back; `-0.04` rounded at one decimal written `0`; a value set by `@set`; [U]
a negative fixed word); `settings_refusals_test.cpp` texts and keys.

**6.2 Indexed rows.** The family, its keys, its walk and refusals, `get<row>(i)`, the reader's key lookup, the editor's
member controls and the strip above 8. Tests: walk order with a family, a member above the count, unknown keys beside a
family; [U] a `.` in a directive name, else the fallback spelling decided and recorded.

**6.3 Live rows, slots and retune** (changes the audio path: a live change no longer restarts). `.live`, slots, the
framework's ramp before `process`, `io.param`, `Smoothed`, `initial<row>()` and `get` refused on live rows,
`checkLive`, `nodeChanged`'s `retune` in `NodeHost`, `NodeHost::setLive` in every phase (7.7), `applied: retune`, the
`param` record; the `gain` type. Tests: `ear_core_tests` (a retune plan; a plan with one non-live key is not a retune;
slots seeded on restart; a retune during a reconfigure moving the slots at once and the reconfigure going on; a
retune of a failed node moving only its live value, with no restart; an indexed family's slots resized by an in-place
reshape, surviving members keeping their ramps); compile-fail `get-live-row`, `live-whole-smoothed`;
`ear_node_gain_tests` through the harness (5.4's worked vector frame for frame, the restart mid-ramp, steady blocks, a
ramp the code never reads still keeping time, a log-taper ramp, trims, clip latch, trigger); a system test: white noise
through `gain` into a `file` node, `node.set` of `gainDb` mid-recording, the WAV's level changes over 20 ms with no gap
and no `node.state` record.

**6.4 Gestures.** `Gestures`, `gesture.*`, the reach index (`GraphFileModel::reachIndex`) posted with each offer, begin
on the main loop, the mixed-reach refusal and the combined range, the session's read-ahead and *ending* mark, the direct
crossing with its pending tick, keep-alives, the idle deadline and the session's close as ends on the owner's behalf
(G4), the related-save rule with its defining lines' hashes (7.3, G3), every abort of 7.3 (`key-removed` among them),
`OfferKind`, rounding at the end, `gesture.changed` without own echoes, `tick-refused`, `not-live` for an `.inPlace()`
row, the harness's gestures. Tests: `gestures.json`; `ear_config_tests` (the index of a `@set` used twice, a key not
given, a snippet's line, each with its defining lines and hash; an index withheld for a text with errors; an end's
candidate, also on a text an unrelated hand edit changed; a related hand edit mid-gesture aborting before the plan and
the slots settling after it); `ear_core_tests` (stale `seq`, a keep-alive moving nothing, out-of-range tick, a tick the
type's `check` refuses, the related-save cases of 7.3, node removed, an end on the owner's behalf refused and so
aborted, a reshape removing a reached member, a gesture surviving the node's in-place change, stop, start,
restart, failure and recovery with its live value seeding the next configuration, a begin on a failed node, a reach mixing a non-live row
refused, two ranges intersected); `ear_server_tests` (a slow request in flight on a socket while that socket's ticks
still move the slot; an end queued behind it never losing to the idle deadline; the idle deadline reset by keep-alives,
stopped by a read end, and on expiry sending an end with the last tick handed over first; a socket reset mid-gesture
sending the end with `session-closed` behind the session's earlier requests; the FIFO's bound); a latency test:
begin answered while the keeper is writing, in less than one main-loop turn after the control thread hands it over; a
system test: two clients, one holds a gesture, the other's `node.set` is `parameter-held`, a hand edit of the held
value aborts it and the WAV goes straight to the file's new level, while a hand edit elsewhere leaves it going; a client
that drops its socket mid-gesture leaves its last value in `ear.conf` and in the WAV.

**6.5 The editor's controls.** Fader and knob with 8.3's mapping, keyboard and wheel gestures, held and aborted states,
notices with "Set again", accessibility of 8.5. Tests: `gesture.ts` against a fake client (every edge of 8.1's table:
release and Escape while beginning, a refused begin, buffered moves, keep-alive after 2 s held still, end on lost
capture, Escape, keyboard idle end, aborted while dragging, ended by ear (`end` with `reason`), reconnect showing the
committed value); the taper formulas and their inverses; Backspace
on a focused knob sends no `node.remove`; rendering by eye, as today.

**R8 Recovery on the wire** (with slice 8). `recovering` with `reason`, `attempt` and `retryInMs`; `lastFailure` kept
after the node runs again; `recovery attempt <n>` causes; the `recovering` record; `ear.health-flapping`,
`ear.recovery-attempt`, `ear.readiness-missed` in 4.6's shape; 8.6's countdown and "try now". Tests: the recovery cases
of `lifecycle.json`; `ear_core_tests` with the harness's clock (the countdown's `retryInMs`, `restart` skipping the
wait, a change skipping it, a retune not skipping it); vitest for the countdown.

## 12. Costs and risks

- **More compile-time machinery.** About 35 new compile-fail cases (5.8) at about 1.3 s each, beside the 59 today
  (AGENTS.md, "Tests"). They reuse the row machinery, so they add cases, not a second machine.
- **A wider wire.** Four notifications, seven requests, eleven codes. The client's queue bound (256 messages,
  `control_port.cpp:127` [V]) now also holds status at up to 30 a second per node; gestures cost one message per tick
  whatever their reach. A slow client with many chatty nodes reaches 1008 sooner. Mitigation if it bites: coalesce a
  node's `node.statusChanged` in the client's queue when it is the last queued message (it then carries the newest
  fields and loses no order).
- **A behaviour change.** `node.update` of only live rows no longer restarts. Someone who relied on that to restart a
  node uses `restart`.
- **The session reads ahead** (6.6), a change to batch2.md's one-in-flight rule for two notifications. Its order
  argument is written there and tested in step 6.4; a bug could let a tick land after its gesture's end was read,
  which the *ending* mark exists to prevent.
- **Gestures cross the control thread straight to the main loop**, beside the keeper, and begin now answers from the
  reach index rather than the keeper. The index is made from the same model `updateNode` edits, at the same version,
  so a begin and its end agree on the reach; a hand edit replaces the index, and aborts the gestures it is related to,
  in one main-loop turn. A bug here breaks "the audio never stays on a value `ear.conf` does not hold".
- **The related-save rule can miss or over-reach** (G3, R31). It compares defining lines' text and evaluated values; a
  change it judges unrelated leaves a gesture going over a file the colleague changed, and its end then writes onto
  that file. The rule errs towards aborting (a comment on the defining line, any error in the file, any change of
  reach), and its cases are tests (7.3). An operator surprised by an abort sees why in the notice.
- **A dropped connection writes its last value** (G4, R32). A laptop that loses Wi-Fi mid-drag leaves `ear.conf` on the
  value the hand was at, 30 s later at most, with nobody pressing release; the `param` record (`reason: "idle"` or
  `session-closed`) says so in `kubectl logs`, and the editor tells the operator when the tab survives.
- **The reach depends on knobly's `definition()`.** A file whose value comes through two variables (`@set $a $b;`) has
  one definition at the end of the chain; whether that is what the operator means by "the same value" is the same
  question `updateNode` answers today, so the two agree by construction.
- **Rounding at the end** (G5) means the committed value can differ from the last live value by half a step (0.05 dB for
  one decimal): inaudible, and announced.
- **Status records** grow standard output: a chatty field is held to 32 at once and 1 a second, plus one trailing
  record; a node with many flapping fields writes one record a second per field.
- **One mutex per node** off the realtime thread: the main loop can wait behind a preempted writer for a push.
- **Ticks run the type's `check`** on the main loop: a parse of the node's settings per taken tick (at most one per
  gesture per main-loop turn). Settings tables are small; if a type's `check` is slow, that type's ticks are slow.
- **Slices 5 and 6 wait for slices 3c and 3d** (G6): the node handle and loops are 3c's, the plan's outcomes 3d's.
- **The lifecycle's wire lands inside other slices** (O1): L1, L2 and L3 make slices 3a, 3b and 3d larger, and each
  must carry its fixtures and editor change, or the editor shows `starting` for a node that is reconfiguring.
- **More health traffic.** A start is three `node.state`s where it was one, a reconfigure two, and `waitingFor` up to one
  a second while a node waits; all per node and rare next to status and levels, but a graph of 48 nodes booting sends
  about 150 notifications in its first second (the client's queue holds 256, `control_port.cpp:127` [V]). If that
  bites, a snapshot is the remedy (a client that subscribes after boot gets one state per node).
- **`applied` is not what happened.** A client that reads `applied: "reconfigure"` and assumes no restart is wrong
  when the hook answers `restart` or the node was not running; `node.state`'s `cause` is the truth, and the editor and
  `--until` read it (X7). The dry run has the same limit.
- **A dry run is advice.** Between it and the real edit another client or a hand edit may change the graph; the real
  edit is judged afresh and may be refused `ports-in-use` with other routes (the editor prompts again, 8.2).
- **`.shapesPorts()` is a promise the type makes**, checked by the all-type tests over the values they try; a type whose
  ports depend on a row only for values the tests do not try would show no "changes the ports" hint where it should.
  The plan, which compares real ports, stays right.
- **Restart or stop during an in-place change ends it**, and a newer change before the commit point supersedes it
  (node-framework.md D-B6): the operator's newest intent wins, at the cost of a change's work thrown away when edits
  come quickly.
- **Triggers are lossy by design**: several invokes in one block are one. An action that must count is a call, to a loop's
  state.

## 13. Rejected alternatives

| Alternative | Reference | Why not |
|---|---|---|
| Normalised 0..1 parameter values on the wire | VST3 (`ParamValue` normalised), CLAP's host-facing value is plain but many hosts normalise | the row's own unit is what `ear.conf` holds and what a script writes; the taper maps a control's travel (8.3), not a second value space |
| Write `ear.conf` on every tick | — | the owner chose "commit at gesture end" (Q6); a write per tick would also announce the whole text to every client each time |
| A parameter store beside `ear.conf` | LV2 state, VST3 `setState` | two sources of truth; hand edits could not win |
| Ticks through the keeper | — | the keeper may be writing or judging; a tick that waits behind a write is a laggy knob (node-framework.md 8.5 already chose this) |
| Begin through the keeper (revision 1) | — | the same lag at the start of every drag, behind the previous drag's write (G13) |
| Client-chosen gesture ids, so ticks can follow a begin before its reply | — | a begin answered on the main loop is fast enough; ids chosen by clients need a collision rule across sessions (G13) |
| One `node.setParam` with a `gesture` phase field (node-framework.md 8.5) | CLAP's `CLAP_EVENT_PARAM_GESTURE_BEGIN`/`END` beside `PARAM_VALUE` | separate methods give each its own params, errors and path (keeper or direct); a script never needs gestures, only `node.set` |
| One `node.paramChanged` per reached node and key (revision 1) | — | a 64-trim gang is 1920 messages a second per client against a 256-message queue |
| Gesture begin/end implied by the first and an idle timeout | OSC controllers, MIDI CC | no commit point the operator chose for a fader in use; the idle timeout remains only for a tab that went silent, where it acts as the release (G4, G15) |
| A 10 s idle per gesture with ticks only on movement (revision 1) | — | a fader held still while listening snaps back |
| Sample-accurate parameter events with offsets in the block | CLAP's input events, VST3's `IParameterChanges` | a ramp per block is enough for faders and gains; timestamped changes stay reserved (node-framework.md 8.5, 15) |
| Constant-slope ramps (re-aimed at the same slope) | — | the ramp's length would depend on how far the value moves; constant time keeps every move 20 ms and the test vectors simple |
| A seqlock per node so a gang's slots change in one block | — | costs a retry loop on the realtime thread for a mismatch of at most one block of ramp (5.4) |
| Status as notifications only, no snapshot | OSC, MQTT without retained messages | a client that connects late could not show a pairing code offered before it |
| Events as status fields that flash | — | an instant has no value to read; it would need a clear, and a late reader would see a stale edge |
| Status as free-text notes (today) | — | the editor cannot show a pairing code; tools parse sentences |
| Diagnostics kept per code, with a separate settled code (revision 1) | — | two fields flapping overwrite each other, and every client must know which code clears which |
| OSC as the control transport | OSC 1.0 | no schema, no replies, no order across messages over UDP, and a second protocol beside JSON-RPC |
| JSON Schema (2020-12) as the type schema | — | a small closed vocabulary (ranges, units, tapers, live, display) reads better and checks in tests; a JSON Schema can be generated from it later for generic tools |
| A hash per type (node-framework.md 8.7) | — | types are compiled in and change together; one hash, one request |
| A separate subscription for status and events | — | the graph snapshot already carries them; a second stream would need its own order with the first |
| Clamping out-of-range ticks | — | a script sending a wrong value should learn it (`tick-refused`); controls send only values in range |
| Decoders that ignore unknown request members | — | loses typo detection; features say what a client may send (4.1) |
| A per-field mutex for text status (node-framework.md 8.2) | — | one queue per node costs the same and gives the writer's order without a flush rule |
| A base class with virtual `onParam`, `onAction` | JUCE's `AudioProcessor`, VST3's `IEditController` | nothing to override: slots, ramps, ring and handlers are the framework's; a type declares (node-framework.md R1) |
| Holding an edit's reply until the node runs again | Kubernetes' `kubectl rollout status` waits on the client, not the API | the keeper takes no next request while a reply's step is open, so a 10 s reconfigure would stall every client's edits; the client waits instead (`--until`, X3) |
| `applied` rewritten to what the node did (a reconfigure that fell back answering `restart`) | — | the reply is sent in the commit's turn, before the hook or a resource has answered; a later correction would need a second notification for the same fact `node.state` already carries (X7) |
| Every phase in `state` (one field) | — | an editor colours by it and scripts wait on `running`; ten values, three of them milliseconds long, make both worse (the owner's A1 answer, node-framework.md R22) |
| A `run` number on health, status and records (revision 3) | — | R20: no run; the framework drops a closed configuration's messages before they reach the wire (node-framework.md I6), so a client needs no counter, and the reply's order in the session's queue serves `--until` |
| `stopped` written in `ear.conf` as a word on the node's line, `node.start` and `node.stop` as edits (revision 4, to node-framework.md gate F1's recommendation) | — | the owner: "Only disabled in file" (R28). The written state is `disabled`, which releases everything; a stop keeps everything and is a moment's state |
| One resting state for both (revision 4) | — | the owner: "Both words" (R27): keeping a card open (Stop) and giving it back (Disable) are two different intents |
| A "held" mark on outputs and a dashed style on their wires during an in-place change (revision 4, W3's recommendation) | — | the owner asked for meters on every port instead (R30): a meter shows the silence wherever it travels, a mark only where it starts |
| Every save of `ear.conf` aborting every open gesture (revision 4, G3's recommendation) | — | the owner: "Only a related save" (R31): a colleague's comment must not stop a fader |
| A dropped or silent tab's gesture returning to the file's value (revision 4, G4's recommendation) | — | the owner: "Keep last value" (R32) |
| A batch of `route.remove`s then `node.set` for a reshape | — | batches are not atomic (batch2.md section 6): a refused `node.set` would leave the routes removed (X2) |
| Silently dropping routes a reshape strands | — | an edit to a channel count must not remove an operator's wiring without saying so (X2) |
| Companions as boxes without handles | — | doubles the boxes on screen for things nobody routes (node-framework.md gate C2) |
| Companion counters as status fields (`ear.feedShortfall`, revision 2) | — | the count belongs to the crossing that drops, and a node with two sinks needs two counts; the companion list holds both (K1) |
| Gestures on `.inPlace()` rows, each tick a reconfigure | — | a reconfigure may hold outputs silent for seconds and replace resources; a drag would chop the audio into holds (X5) |

## 14. Gate questions

Revision 4 follows the owner's change of practice (node-framework.md R24, 14.1): the owner is asked only about what an
operator or a type author sees, each question with plain background, a concrete scenario, options, a recommendation
and why; every technical question is decided by the designer, listed in 14.2 with its reason, so the owner can overturn
any. Across the three pages the owner was asked eight questions (node-framework.md revision 6, this page's revision 4): node-framework.md's F1, F2 and C2, and this
page's W3, X2, G1, G3 and G4. The owner answered all eight on 2026-11-08 (node-framework.md 14.1, Q26 to Q28, for the
first three; 14.1 here for the other five), and revision 5 is written to the answers. No owner gate is open, and the
answers raise no new owner question. The owner's later word on where `disabled` is written (R29, node-framework.md
Q29) is recorded there and here; it raised no question, and its spelling is the designer's (W11; node-framework.md
D-F8 to D-F12). Round 4's review raised two owner questions, both node-framework.md's and answered there on
2026-11-08: "Two versions" of `configure` (R33; a type author's change, seen here only in 5.7's `gain`) and "Only this
node" for Disable on a shared `disabled` value (R34; 4.2). No owner gate is open.

### 14.1 The owner's answers of 2026-11-08

| # | Question (revision 4) | Recommended | The owner's answer | Where it lands |
|---|---|---|---|---|
| W3 | When a node goes silent on purpose during a change (an `rtp` node moving from port 5004 to 5006), how does the editor show it? | (a): "reconfiguring port" on the box, and a "held" mark (a dashed wire) on the outputs and wires silent on purpose | R30: "I would like each port to show meter levels so I can see audio traversing the graph". Neither option as put: no held marks; every port of every node shows a live meter, and the box still says "reconfiguring port" | 8.6 (what is built and what is new), 2.1, 4.8 (no `holds`), 9.1; W7, W8 |
| X2 | A change that removes ports that are wired (`desk`'s playback from 2 to 1 while `kitchen:from.left → desk:play.2` is wired) | (a): the editor lists the wires that would go before applying; Apply removes them and changes the setting in one step (one save of `ear.conf`) or neither; a script must ask (`dropRoutes: true`), else it is refused naming the wires | (a), as recommended | 4.9, 2.1, 7.5, 8.2 |
| G1 | A fader for a value several settings share (`amp2`'s `trimDb.1` and `trimDb.2` both read `$trim`) | (b): the fader moves every user together when all are faders of one kind and unit, within the range they all allow; the release writes the shared value once; other sharing is refused | (b), as recommended | 4.2 (`gesture.begin`), 7.2 |
| G3 | What happens to a fader being dragged when someone saves `ear.conf` (by hand or from the text pane)? | (a): every save stops every fader being dragged | R31: "Only a related save", option (b): only a save that changes the dragged value, or the line it is defined on, stops it; any other save leaves the drag going and its release writes onto the new text | 7.3 (the rule, how ear decides it, its tests), 2.1, 4.3, 8.3; G14 |
| G4 | A fader whose browser disconnects or goes quiet (Wi-Fi drops while its owner holds -3.5 dB; the file says -6) | (a): back to the file's value, also after 30 s without a word | R32: "Keep last value", option (b): ear keeps and writes the last value heard, as if released | 7.2, 7.3, 6.5, 6.6, 2.1, 4.3, 8.3, 9; G15, G16 |
| R29 | Where `ear.conf` writes `disabled` (revision 5 wrote it after the node's name, `node card porch disabled { ... }`); not a gate, the owner's own correction | — | R29: "the “disabled” key should be an attribute within the node scope — not a keyword on the node name" | 4.2, 4.3, 7.6, W9, W11; node-framework.md 6.3, D-F8 to D-F12 (revision 5.1) |

The gates' full texts (background, scenario, options, recommendation and why) are in revision 4 of this page, in git.

### 14.2 Decided by the designer

| # | Decision | Why |
|---|---|---|
| W1 | One `node.state` whenever any health member changes (`state`, `phase`, `wanted`, `cause`, `waitingFor`, `keys`, `next`, `reason`), never coalesced; the editor hides a `starting` phase for 250 ms (revision 6: was per phase change; round 4's M4) | Health changes are rare (four per start); coalescing would hide the one a node is stuck in, and a `wanted` change without a phase change (Stop on a failed node) must still reach the editor |
| W2 | Keep `reason` with today's meaning; slice 8 adds `lastFailure` beside it | Additive within protocol 1; today's editor keeps its tooltip |
| W4 | A `still` record on standard output when a node stays before `running` past 2 s, at most once per 10 s | A stuck node says why within 2 s in `kubectl logs`; a normal start keeps its lines |
| W5 | No `run` member anywhere, the fixture's included (4.7); `wanted` on health (`running`, `stopped` or `disabled`); `node.gone` added in protocol 1 for a removed node gone, sent after `node.stopped`, which is deprecated and goes at protocol 2 (revision 6; round 4's M8); a removal's record is `removed dropped=` | R20; `stopped` now names a resting node, so one word must not mean two things; a client never says its protocol, so both are sent until a protocol bump |
| W6 | `node.start` and `node.stop` are neither edits nor type actions: ordered by the keeper, applied by the framework, writing nothing; replied in the turn that applies them (`stop`, `start`, `retry`, `unchanged`); no `graphFile.changed` or `node.updated`; an `action` record on stdout. `node.disable` and `node.enable` are edits at the source. `start` on a failed node is a retry (node-framework.md D-F6) | The owner: only `disabled` is in `ear.conf` (R28); the owner said a start leaves `failed`; a stop must show in `kubectl logs` though it edits nothing (G8) |
| W7 | No `holds` member on `node.state` or the `node` record, and no "held" marks in the editor; the framework's holds stay (node-framework.md D-B3), seen through the meters | The member existed only for W3's marks, which the owner did not take (R30); every port's meter shows a hold, and where its silence travels, without one |
| W8 | Every port's meter, inputs and outputs, in every phase: kept as built (`levels.subscribe` per public port, a `Meter` per handle); new only: no held styling, levels registered for a reshape's new ports at its barrier, and a system test of a hold seen on the meters (8.6) | R30 asks for what the editor already draws for every port; the lifecycle must not take it away (a disabled or stopped node keeps its block, so its meters keep reading) |
| W9 | `disabled: true` on the node's description (snapshot, `node.added`, `node.updated`) beside `settings`, never inside it, as well as `wanted: "disabled"` on health; `node.add` takes `"disabled": true` beside `settings`; a `disabled` key inside `settings` is refused `invalid-settings` (revision 5.1, R29) | The description is what `ear.conf` says, health what the framework converges on; a client editing the node's line needs the first, one colouring it the second |
| W10 | `node-disabled` refuses `node.start`, `node.stop` and `restart` on a disabled node; the editor disables those buttons with "disabled in ear.conf: enable it first" | node-framework.md D-F2: a Start that does not remove `disabled true;` would be undone by the next restart |
| W11 | `disabled` is not a setting on the wire (revision 5.1, R29): no type schema lists it, `node.add` takes it beside `settings`, `node.disable` and `node.enable` change it, and `node.update`, `node.set` and `dryRun` never see it; the editor's settings form has no `disabled` field, and the text pane shows the `disabled true;` line as written | node-framework.md D-F9: the key is the framework's, reserved on every node; keeping it out of `settings` keeps a change of it a disable or an enable, never a reconfiguration, and keeps the schema's hash per type |
| W12 | `node.restart {name}` under feature `lifecycle`, from step L1, replying `{"applied": "restart" \| "retry" \| "unchanged"}` by 7.6's table; `restart` is no type's action, and `start`, `stop`, `restart`, `disable` and `enable` are reserved action names (revision 6; round 4's M5; node-framework.md D-R20) | It is the lifecycle's sibling of Start and Stop and should arrive with them in 3a, in their shape; slice 5 would otherwise leave an operator with no way to start a node afresh after fixing a card or a key file by hand |
| W13 | A node is `starting` on the wire until it rests, even when wanted stopped; the editor adds "will rest stopped"; a node added disabled writes no `starting` record (revision 6; round 4's m5, m9; node-framework.md D-R22) | `stopped` must mean a node that has reached it; a boot should not print a start for a node it will not start |
| W14 | A control that lost its socket mid-drag freezes until the pointer is up and then says what ear kept; after a 1001 close it says the change was not kept (8.3; round 4's m6) | Ticks against a dead gesture would only be refused; the operator must learn when ear restarted under a held fader |
| W15 | An `action` record is an operator's request that edits nothing (`node.invoke`, `node.start`, `node.stop`, `node.restart`) (9; round 4's m7) | One record kind for every request that leaves no `graph-file` record; the vocabulary says what it is |
| W16 | The main loop's and the data loop's diagnostics (`ear.main-loop-stalled`, `ear.main-loop-resumed`, `ear.data-loop-wedged`) are standard-output records with no node; `ear.fade-unacknowledged` and `ear.barrier-overdue` are node diagnostics (4.6; round 4's F1, F6) | A stalled main loop cannot answer the control port in time; the records come from a thread that does not wait on it |
| X1 | The schema's per-row `change`, `.shapesPorts()` held to `ports()` by the all-type tests, and `dryRun` on `node.set` | The hint beside each field needs no round trip; the dry run gives the exact answer, routes included |
| X3 | An edit's reply comes at the commit, with `applied`; a client waits for `node.state` | R21: operations are asynchronous; holding replies would stall every client behind one slow change |
| X4 | Moved to node-framework.md D-B6 (a newer change, `stop` or `restart` supersedes an in-place change before its commit point) | One rule for the lifecycle |
| X5 | A fader (gesture) only on `.live` rows; in-place rows are typed and applied once | A drag over an in-place row would chop the audio into holds |
| X6 | Values applied off the realtime thread are `.inPlace()` rows the type posts to its loop; no `watch` hook until a type needs a fader on one | Covers every built-in need with machinery slice 3d builds anyway |
| X7 | `applied` is the plan's outcome; what the node then did is `node.state`'s `cause` | Known at the commit, the same for a dry run and every client |
| K1 | `companions` under each node, `node.companionsChanged`, counters sampled once a second, no framework status fields | Counts belong to the crossing that drops; status stays the type's vocabulary |
| K2 | `ear.sink-dropped` and `ear.source-shortfall` at most once per 10 s per companion while it drops | The operator learns of drops while they happen, at a bounded rate |
| O1 | Steps L1, L2, L3 land with slices 3a, 3b, 3d, bringing the protocol frame with L1 | The wire is built once, in its final shape, by the slice that makes its facts |
| G6 | Slice 3c before 5; 3d before 6 | `Report` sits on the node handle; a retune is one outcome among the others |
| G2 | `node.set`, `gesture.begin`, `change`, `end`, `cancel`, one `gesture.changed` per gesture, `node.invoke` | One path and one set of errors per method; a 64-trim gang costs one message per tick |
| G5 | A gesture's committed value is rounded to the reach's `decimals` | `gainDb -3.5;` stays readable; at most half a step of difference |
| G7 | An absent `protocol` is 0; this page is 1; additive growth is announced in `features`; every enum open | A client can tell what an ear offers; new states arrive without a bump |
| G8 | `param` and `action` records on standard output | A morning's `kubectl logs` says whether a person caused a 3 a.m. restart |
| G9 | Status records paced per node and field (32, then 1 a second), with a trailing record of the current value | `kubectl logs` never ends on a value the node left |
| G11 | No flag (switch) setting kind yet | No built-in type needs one; adding it later is additive |
| G12 | A session reads ahead for `gesture.change` and `gesture.cancel` | Nothing with an id changes order, so every rule and fixture holds |
| G13 | A gesture's begin is answered on the main loop from a reach index the keeper posts | The second drag moves the audio within a main-loop turn |
| G14 | "Related" (the owner's G3) is judged per gesture on the main loop, before the plan, from the running and the new reach index: the defining lines' text (whole source lines, compared by a 64-bit hash), the evaluated value, the reach, the pairs' existence; a text with errors or one the graph refuses aborts every gesture; an unrelated save rebases the gesture's places (7.3) | The owner's words name the value and its line; the value catches chains, the reach catches a new sharer; a broken file cannot take any end, so keeping a gesture open over it would only delay the abort. The keeper already makes the index on its thread, so knobly never leaves it |
| G15 | A tab silent for 30 s about its gesture (no tick or keep-alive), its socket still open, is treated as released, like a closed one: its last value is committed (`end`, `reason: "idle"`); the editor's 2 s keep-alive while the pointer is held stays | The owner chose "Keep last value" for a disconnect (R32), and a Wi-Fi drop usually shows as silence before TCP closes the socket, so the two must agree; a hand holding still keeps its gesture by the keep-alive. Returning to the file's value after silence would contradict the owner's answer for the most common case of a drop |
| G16 | The idle timer and the end on the owner's behalf live in the session on the control thread, which hands over the newest tick, then queues a `gesture.end` with no value through the keeper; an end so made that is refused aborts the gesture; a stop of ear (SIGTERM) commits no open gesture | The session sees every message, so the timer needs no crossing; the end then takes the one write path every end takes. At SIGTERM edits are refused, and a stop is nobody's release |

## 15. Open items

- [U] Aggregate deduction of `TypeSpec` with designated initialisers and defaulted template parameters on GCC 14 (5.3,
  node-primitives.md U1); slice 3c compiles it first and step 5.2 extends the test; the fallback is a consteval
  `makeSpec`.
- [U] knobly reads `-3.5` and `-6` as one number word in a setting's place, and a directive name may hold `.`
  (`trimDb.1`) (5.1); steps 6.1 and 6.2 test both, with `trimDb-1` as the fallback.
- [U] `std::atomic<double>` is lock-free on arm64 (6.1); no arm64 build exists yet, and the `static_assert` will say.
- [U] The client's queue under status load: 30 notifications a second per node against the 256-message bound (12); step
  5.2's system test runs ten chatty nodes and a client that reads slowly.
- [U] Browsers' pointer capture on touch screens: whether `lostpointercapture` always follows a cancelled touch (8.3);
  checked by hand in step 6.5.
- [U] React Flow 12's `nodrag`, `nowheel`, `nopan` and `nokey` classes and its key filter (8.3); its source is not
  vendored, so step 6.5 checks them against the pinned package and its test proves Backspace on a knob removes nothing.
- [U] A `node.update` from today's editor round-trips decimals: it sends JSON numbers as JavaScript prints them
  (`-3.5`, `0.1`), the keeper writes them with `to_chars`; step 6.1's test sends both forms.
- [U] How knobly spells a boolean in a setting's place (G11); checked when a `flag` row is first wanted.
- The reach of a value defined through a chain of variables follows knobly's `definition()` (the chain's end); whether
  an operator expects that is checked with the owner on the first real file that does it.
- node-primitives.md's G14, G15 and G17 are answered (section 1): loops as spec handles, resources reached by handle
  inside `configure`, and the handle as `node.handle()`, so `Report::handle()`. node-primitives.md revision 4 and this
  page were rewritten with node-framework.md revision 6 in one change, and both again (revision 5) with its revision 7
  (section 16).
- **Taken by node-framework.md**, each now decided or an owner gate here: the `still` record (6.2; W4); `restart`
  ending an in-place change in flight (6.3, 8.4; its D-B6, absorbing X4); `.shapesPorts()` in 7.1's marks (X1); no
  `writer::thread` (8.1, 8.2; 5.2); `reason` kept, `lastFailure` beside it (8.1, 8.7; W2); `ports-in-use` and
  `dropRoutes` (6.7; X2, answered by the owner as recommended).
- [U] That the keeper writes `disabled true;` first in a node's block (making the block when there is none), replaces
  the node's own `disabled` line (R34) and removes the line and an emptied block, comments and layout kept (node-framework.md
  R29, D-F10; its 15 names the fallback, appending the line last); step L1's `ear_config_tests`. Settled in revision
  5.1: knobly parses `disabled true;` in a block as a directive with one value and reads it with its `boolean` reader
  (node-framework.md 15, D-F8 [V]). The framework diagnostics' codes stay this page's (4.6).
- [U] That knobly's `definition()` gives a source range from which the keeper can take the defining statement's whole
  lines for the related-save rule (7.3, G14), for a setting, a `@set`, a chain's end and a line inside a `@snippet`;
  step 6.4's `ear_config_tests`. If a snippet's use hides the range, the rule treats every save that changes the
  snippet or its `@use` line as related (it errs towards aborting).
- [U] A dry run's cost: a candidate (knobly edit on the keeper's thread) and a plan per Apply; the editor sends one per
  Apply, never per keystroke. Step L3's test times a dry run on the record's example file.
- [U] The keeper's view of a type's ports: `parse` and `ports` are pure (node-framework.md 7.4), so the keeper can run
  them for `dropRoutes`; that `NodeType` is safe to share with the keeper's thread (no mutable state) is checked in step
  L3 by a test that parses on the keeper's thread while the main loop does.
- [U] How long `rtp`'s hold lasts in practice (rtp-source's unload and load, node-framework.md 15), which is how long the
  meters on `fpp`'s outputs and the ports they feed read silence; step L3's system test measures it.
- `writer::companion` is named here and built by slice 8.
- `watch<row>(loop, fn)` for live rows off the realtime thread: reserved (X6).
- A `flag` setting row: deferred (G11).
- Timestamped parameter changes and events: reserved (node-framework.md 8.5, 15).
- `log.subscribe` on the wire: later, with no type changing (node-framework.md 8.8).
- `ear.realtime-overrun` (a code that ran past its cycle): reserved; measuring it needs a clock reading in the block
  wrapper, which slice 7's `GraphCycle::nextNs` makes possible.

## 16. Review record

Three reviews of revision 1 (2026-11-08): realtime audio (RA), API and protocol (AP), operator UX (UX). Each finding was
checked against the code before it was acted on. Outcomes: **fixed** (where), **rejected** (why), or **gate** (the
owner's call, section 14). Several findings were made by more than one reviewer; each is answered where it is fixed.

| # | Finding | Outcome |
|---|---|---|
| RA1 | 10 s idle aborts a fader held still, and an end queued behind a write | fixed: keep-alive every 2 s, 30 s idle per owner's messages, timer stopped once an end or cancel is read (6.5, 6.6, 8.1); G4 |
| RA2 | one message in flight per connection stalls ticks behind a slow request (`control_port.cpp:240` [V]) | fixed: read-ahead for gesture notifications (6.6); G12 |
| RA3 | `gesture.begin` through the keeper waits behind writes | fixed: begin on the main loop from the reach index (7.2); latency test in 6.4; G13 |
| RA4 | a reach with non-live keys or other ranges is undefined | fixed: refused unless all live, one kind and unit; ranges intersected, smallest decimals, own smoothing (4.2, 7.2); G1 |
| RA5 | `Smoothed` when the target moves is undefined | fixed: constant-time rule, ramp before `process` whether read or not, worked vector (5.4) |
| RA6 | no rule resets a client's status on a new run | fixed: reset sent at run close, higher `run` replaces, `run` on `node.state` (4.3, 4.4) |
| RA7 | stdout status pacing can lose a field's last value | fixed: per-field pacing with a trailing record (6.5, 9); G9 |
| RA8 | no off-realtime reaction to a live change | gate: G10; the limit is stated (2.4, 3.2, 7.4) |
| RA9 | a live row read at start goes stale | fixed: `get` on a live row does not compile; `initial<row>()` (5.1, 5.8) |
| RA10 | taper mapping unspecified; log with min <= 0 not refused | fixed: formulas (8.3); `log-taper-nonpositive`, `db-taper-range` (5.8) |
| RA11 | linear ramps for whole-number and log rows | fixed: whole-number live rows step (`live-whole-smoothed`); log rows ramp geometrically (5.1, 5.4) |
| RA12 | no switch row for mute | gate: G11 |
| RA13 | log records crowd events out of the ring; `t` not from the cycle | fixed: log refused past 192 of 256; cycle stamp in each record and `t` from it (6.1, 6.4, 9) |
| RA14 | diagnostics per code lose per-field flapping; client derives count | fixed: per code and subject, `active`, `count` sent (4.3, 4.6) |
| RA15 | a gang's slots change one at a time | fixed by stating it: bounded by one block of ramp; seqlock rejected (5.4, 13) |
| RA16 | ticks skip the type's `check` | fixed: `checkLive` runs the row and `check` per tick (5.1, 5.6, 6.6) |
| RA17 | restart of a lost block is a silent no-op | fixed: `not-restartable`, `blockLost` (4.5, 7.6) |
| RA18 | boundary with node-primitives.md: `TypeSpec`, slice 3's diagnostics and counters, realtime names | fixed: `TypeSpec` cited from node-primitives.md 6.12 (5.3); slice 3's five codes and the counters (4.6); names decided here and cited there (5.4); the boundary reconciled (below) |
| RA19 | owner's own change echoes; what arms the tick | fixed: no own `begin`/`change` echoes; `Gestures::move` arms the tick (4.3, 6.4, 6.6) |
| RA20 | begin on a node with no run | fixed: allowed, seeds the next run (4.2, 6.6); in `gestures.json` |
| RA21 | two wrong [V] citations | fixed: `graph_file_model.hpp:80`, `main_loop_side.hpp:24-30` with its real members (7.2, 7.3) |
| RA22 | rounding mode and negative zero unstated | fixed: half away from zero on the shortest text; zero as `0` (4.2, 5.1, G5) |
| RA23 | wheel steps per event | fixed: by `deltaY`, Shift fine, focused only (8.3) |
| AP1 | protocol number cannot tell old from new | fixed: absent is 0, this page 1, `features` (4.1); G7 |
| AP2 | new request members are not additive (`graph_json.cpp:33-42` [V]) | fixed: sent members additive, request members gated by feature; `errors.json` case (4.1) |
| AP3 | reach with non-live or mixed rows | fixed with RA4; begin reply carries the combined range (4.2) |
| AP4 | status reset not on the wire | fixed with RA6 (4.3) |
| AP5 | diagnostics per code lose the subject; settled code needs client logic | fixed with RA14; `ear.status-settled` dropped (4.6) |
| AP6 | catalogue misses slice 3's codes and counters | fixed with RA18 (4.6) |
| AP7 | held knob aborted by idle | fixed with RA1; `idle` has its own notice (8.3) |
| AP8 | one in flight vs direct ticks; `done` timing | fixed with RA2; `done` fires when stored (6.6) |
| AP9 | owner sees its `begin` before the reply | fixed: no own `begin`/`change` echoes (4.3) |
| AP10 | `invalid-settings` has no machine key (`command_error.hpp:12-15`, `command_error.cpp:44-45` [V]) | fixed: `key`, `detail` in `data`; `invalid-args` too; `data.pointer` on -32602 (4.2, 4.5) |
| AP11 | `writer::thread` against slice 3's no-threads rule | fixed: removed; `writer::loop(worker)` takes a loop handle of the spec (5.2, 5.7) |
| AP12 | three kind vocabularies; no decimal status | fixed: one wire vocabulary; `status::decimal`, `arg::flag` (4.1, 4.2, 5.2, 5.3) |
| AP13 | retune and restart indistinguishable | fixed: `applied` on `node.set`, `node.update`, `node.updated` (4.2, 4.3) |
| AP14 | `node.eventRaised` lacks `severity`; `t` of a realtime event | fixed (4.3, 6.4) |
| AP15 | restart of a lost block | fixed with RA17 |
| AP16 | "diagnostic" and `Severity` used twice (`protocol.ts:70`, `graph_file_state.hpp:19` [V]) | fixed: `NodeDiagnostic`, `EventSeverity`, stated in 3.1 |
| AP17 | Save aborts as `hand-edit` | fixed: `file-saved` with its notice (4.3, 7.3, 8.3) |
| AP18 | context-dependent defaults | fixed: `defaultFrom`; all-type test (4.2, 5.1, 5.9) |
| AP19 | `types` means two things; canonical hash | fixed: `typeSchemas`; opaque hash of the boot bytes (4.2) |
| AP20 | wrong refusal codes; no `node.set` fixture | fixed: `invalid-text` removed; `node-set.json`; every new code in `errors.json` (4.2, 4.5, 4.7) |
| AP21 | `protocol.ts`'s freeze rule names batch2.md's errata (`protocol.ts:5-6` [V]) | fixed: step 5.1 adds the erratum (10.3) |
| AP22 | removing `note` vs versioning | fixed: `v` versions shapes, kinds are open; `BREAKING CHANGE:` footer at 5.5 (9) |
| AP23 | `Report` checks its spec at run time | fixed: `ReadyOf<spec>::report()`, private constructor; compile-fail cases (5.3, 5.8) |
| AP24 | gesture edges: `seq`, ids, invalid ticks, invalid end | fixed: `seq` from 1, ids opaque and never reused, `tick-refused`, an invalid end leaves the gesture open (3.3, 4.2) |
| AP25 | realtime call names differ from node-primitives.md | fixed with RA18: this page's names, cited by node-primitives.md section 14 (5.4) |
| AP26 | two wrong [V] citations | fixed with RA21; the other 20 checked again and kept |
| UX1 | held fader aborted | fixed with RA1 (keep-alive is liveness only, 6.6) |
| UX2 | knob keys and wheel taken by React Flow (`App.tsx:138` [V]) | fixed: `nodrag nowheel nopan nokey`, stopped propagation, a test; class names [U] (8.3, 15) |
| UX3 | per-code diagnostics vs multiple flapping fields | fixed with RA14; node diagnostics survive a restart, said in 3.3 |
| UX4 | status reset never sent | fixed with RA6; order rule 5 (4.3) |
| UX5 | a choice's first value as initial is false | fixed: a choice starts `null`; `.initially`; box and callout fields hidden unless running and known (5.2, 8.4) |
| UX6 | per-key `paramChanged` overflows the queue | fixed: one `gesture.changed` per gesture (4.3). Its line citation was wrong: `maxQueuedMessages` is at `control_port.cpp:127` [V], as revision 1 said |
| UX7 | paced status records lose the flapping record | gate: G9 (summary record is option c); written to (a) |
| UX8 | refusal placement parses a prefix that does not exist (`command_error.cpp:44` [V]) | fixed with AP10 (8.2) |
| UX9 | knob mapping, fine adjust, reset to default, keyboard step left open | fixed: the control mapping (8.3) |
| UX10 | release or Escape before begin's reply; other refusals; disable up front | fixed: `gesture.ts`'s table (8.1); controls disabled on file errors or no connection (8.2) |
| UX11 | form drafts overwritten by updates (`Panels.tsx:80` [V]) | fixed: per-field drafts, `form.ts` (8.1, 8.2) |
| UX12 | hand-edit abort announces the old value; the chosen value is lost | fixed: no value on `hand-edit`; slots go straight to the new value; "Set again" (4.3, 7.3, 8.3) |
| UX13 | restart of a lost block (`node_lifecycle.hpp:29-31` [V]) | fixed with RA17; button disabled with its reason (8.4) |
| UX14 | restart reply order disagrees between 2.1 and 4.3 | fixed: reset, `starting`, reply, `running` everywhere (2.1, 4.3, 7.6) |
| UX15 | knob rule for families; 64 sliders | fixed: families never on the box; a strip above 8; count row marked "changes the ports" (8.2). A per-family "link" toggle is rejected: ganging is a `@set` in `ear.conf` (G1), one mechanism |
| UX16 | notice floods; activity lost on reload | fixed: notices coalesced and rate-limited; the activity list says it starts at connect (8.4) |
| UX17 | wheel over an unfocused slider changes a gain | fixed: focused only (8.3) |
| UX18 | two definitions of settled; thresholds hidden | fixed: one rule (2.1, 4.6); `flapsAbove` in the schema (4.2) |
| UX19 | boundary tables disagree | fixed with RA18; `typeSchemas` now matches (4.2) |
| UX20 | accessibility gaps | fixed: failed and flapping announced and rate-limited, focus rules, canvas-panel path, `role="alert"` (8.5) |
| UX21 | frozen-header rule for `protocol.ts` | fixed with AP21 (10.3) |

**Boundary reconciled with node-primitives.md revision 2** (2026-11-08), each fact given one home and cited from the
other page. Here: `ready.handle()` and `build` replace `Ready::run()` and the recipe (scope, 4.5, 5.1, 5.3, 5.7, 5.9,
10.1), and `Report::run()` is `Report::handle()`; `ReadyOf` is slice 3's facade, gaining `report()` (5.3); `TypeSpec`'s
member list moved to node-primitives.md 6.12 and is cited (5.3); `writer::loop(worker)` and `call<N, Args, Loop>` take a
loop handle of the spec, so a loop the spec lacks does not compile (2.4, 5.2, 5.5, 5.7, 5.8: `writer-loop-bad-name`
becomes `writer-loop-not-in-spec` and `call-loop-not-in-spec`); a queued call is answered at teardown step 1 through
`postFromOutside` and `closeToOthers` (5.5, 7.6); `ear.loop-source-left` and `ear.close-late` join 4.6 and step 5.2;
`Gain::start` takes `StartOf<spec>&` (5.7); the note-pacing and visible-change citations point at node-primitives.md
sections 5 and 8 (10.1, G6). The re-cut item of section 15 is gone: node-primitives.md section 14 now cites this page's
realtime calls and per-run ring, and both pages spell the claim wait `ear.claim-wait`. `writer::thread` stays absent:
node-primitives.md section 14 now says so.

### Revision 3 (2026-11-08): rebased onto node-framework.md revision 5

Revision 3 is written to the owner's words R11 to R18 through node-framework.md revision 5, and has not been reviewed.
What it changed, and where:

| # | Change | Why | Where |
|---|---|---|---|
| C1 | Health on the wire for every phase: `phase`, `since`, `cause`, `keys`, `next`, `holds`, `waitingFor`, with `run`; one `node.state` per phase change; `reason` kept; stopped stays `node.stopped` | R12, R16; node-framework.md 6.2 and gate A1 | 4.8; W1, W2, W3 |
| C2 | `applied` gains `reconfigure` and `reshape`, means the plan's outcome, and comes with `run`; the reply comes at the commit | R11, R17; node-framework.md 6.6 | 4.2, 4.9; X3, X7 |
| C3 | `dryRun` on `node.set`; `ports-in-use` and `dropRoutes` for a reshape; `ports`, `inputs`, `outputs` on `node.updated` | R17 ("might not be fully transparent"); batches are not atomic | 4.5, 4.9; X1, X2 |
| C4 | The schema's `change` (`live`, `inPlace`, `restart`) and `shapes` per row; `.shapesPorts()`, held to `ports()` by the all-type tests | R17: the node decides, and the operator should see it before a change | 4.2, 5.1, 5.9; X1 |
| C5 | Node edits and gestures against each kind of row and phase; retunes never wait; gestures only on live rows; `key-removed` | R11, R17 | 7.7, 7.3; X5 |
| C6 | `restart` as T8 by phase; it ends a reconfigure in flight; `not-restartable` gains `not-started`; `not-running` carries `phase` | node-framework.md 8.4, T8 | 7.6, 4.5; X4 |
| C7 | Companions on the wire (`companions`, `node.companionsChanged`), in the editor as badges; the framework's status counters and `frameworkStatus` gone; `ear.sink-dropped`, `ear.source-shortfall` | R14, R18; node-framework.md 9.6 and gate C2 | 4.6, 4.10, 8.7; K1, K2 |
| C8 | The per-run channels across the lifecycle: made at `setup`, bound at `build`, first used at T4, kept through a reconfigure, rebound at a code swap | R13: `start` after `build` | 5.6, 6.7 |
| C9 | `ReadyOf` and `StartOf` become node-framework.md's `BuildOf` and `SetupOf`; `Report` from `bx.report()` and `rx.report()`; `gain` writes `build`; `writer::resource` becomes `writer::companion`; `nodeRetuned` becomes `nodeChanged`'s `retune` | node-framework.md 6.4, 6.6, 7.3, 13.3 | 2.4, 5.2, 5.3, 5.7, 7.4 |
| C10 | The lifecycle's records: `run`, `phase`, `cause`, `keys`, `holds`, `afterMs`; `reconfiguring`, `restarting` and `still` | Q9 ("JSON with a msg field"); W4 | 9.1 |
| C11 | The editor in each phase: silence on purpose drawn as held, routes kept, a reshape redrawn in place, Apply asking first | R16, R17 | 8.2, 8.6, 8.5 |
| C12 | sendspin's `stream` and `sending` written by its run object from library and companion facts; a rename keeps the run | node-framework.md 13.3, 9.6 | 10.1 |
| C13 | The cut re-derived: L1, L2, L3 inside slices 3a, 3b, 3d (L1 bringing the protocol frame), `node.set` moved to L3, then 5.1 to 5.5, 6.1 to 6.5, R8 inside slice 8 | node-framework.md 13.1 and gate D1 | 11; O1, G6 |
| C14 | Every gate question has plain background and a scenario; G10 re-asked as X6 (in-place rows reach the run object), G6 restated beside O1 | the owner's request for revision 3 | 14 |
| C15 | The journeys gain a node's start, an in-place port change, a reshape with its routes, and an edit during a reconfigure; restart's journey follows T8 | P2: journeys first | 2.1, 2.2 |

Kept from revision 2 as it was: gestures, the reach index, read-ahead, the ramp's rule and worked vector, status, events,
actions, the realtime ring and bell, the keeper path, the editor's controls and accessibility, and every finding's fix
in the tables above. A review of revision 3 should be given sections 4.8 to 4.10, 6.7, 7.6, 7.7, 8.6, 9.1, 11 and 14
with node-framework.md sections 6 and 9.6, and judge at least: whether every phase and transition reaches a client and
standard output exactly once and in order with the commit's events; whether `applied` against `cause` is clear to a
script author; the dry run's and `dropRoutes`' place on the keeper's thread; and whether L1 to L3 keep slices 3a, 3b
and 3d buildable with today's editor.

### Revision 3, reconciled after node-framework.md's review round 3 (2026-11-08)

node-framework.md revision 5 went to two reviews (realtime audio, RA; API and developer experience, DX). Their
findings that touched this page, each checked against the code first:

| Finding | Outcome here |
|---|---|
| DX3: a run that dies after running fails `start-failed` | fixed: `reason.code` is node-framework.md 6.2's open set, with `run-lost`; `FailureCode` gains it in step L1 (4.8) |
| DX6: no spelling for a longer reconfigure deadline | fixed: `reconfigureMs` on each type's schema, from the spec's `.lifecycle` (4.2, 4.9, 5.3) |
| DX7: `applied: "reshape"` does not say whether the node restarts | fixed: `inPlace` on a reshape's reply, dry run and `node.updated`; the 2.2 journey shows it (2.2, 4.2, 4.9, 7.7) |
| DX8: `.shapesPorts()` had no home in the framework page | fixed there: node-framework.md 7.1 is the home of all four marks; 5.1 cites it; in place defined as every changed key `.inPlace()` or `.live` (4.2, 5.1, 7.7) |
| DX9: gates overturned across pages | fixed: node-framework.md 14.2 group E lists each; X4 moved to node-framework.md B6, asked once; X1 and X7 no longer use sendspin's listening/dialling switch as their scenario (it is in place under node-primitives.md G19) |
| DX10: two gesture designs | fixed there: node-framework.md 8.5 keeps the model and cites 4.2, 4.3 and 7.2 here |
| DX14: `--until` never exits on a retune | fixed: exits 0 at once on `retune` or `unchanged` with the node running; `--timeout` (2.2) |
| DX15: `TypeSpec`'s home cited as 6.12 | fixed: node-primitives.md 6.13 (scope, 5.3) |
| DX16: the harness shown three ways | fixed: node-framework.md 7.6 is its one home; every call here on `h.` (2.4, 5.9) |
| RA8: a reconfigure in flight cannot be overtaken | fixed there (node-framework.md T15, B6); here `keys` replaced on a supersession, `next` only after the commit point, `ear.reconfigure-superseded` (4.6, 4.8, 4.9, 7.7) |
| RA10: a retiring resource's loss fails the node | fixed there; here `ear.retiring-lost` (4.6) |

### Revision 4 (2026-11-08): rebased onto node-framework.md revision 6 (not reviewed)

The owner's answers of 2026-11-08 (node-framework.md R19 to R24) removed the run. This page now: carries no `run` on
health, status, companions, replies or records (the framework drops a closed configuration's messages itself), and
`--until` waits for the first `node.state` after the reply (2.2, 4.4, 4.8, 9.1); adds `node.start` and `node.stop` as
edits at the source of the wanted state and `wanted` on health (4.2, 7.6; node-framework.md gate F1); gives health the
new states (`added`, `configuring`, `configured`, `starting`, `stopped`, `removed`) with seven colours (4.8), the
editor's row for each (8.6) and their records, a removal's line becoming `removed dropped=` and its notification
`node.gone` (9.1, W5); states status and the per-node channels as the node's configuration's, kept through a stop and a
start (3.3, 4.3, 5.6, 6.7); replaces `build`, `BuildOf::report` and the run object with `configure`, `NodeOf::report`
and a loop's state (2.4, 5.3, 5.7); and re-sorts section 14 to the owner's new practice: five owner gates (W3, X2, G1,
G3, G4), rewritten to what an operator sees, and every other question decided by the designer (14.2). Revision 3's
history below keeps its words. A review is owed with node-framework.md's revision 6; it should judge at least the
ordering claim that replaces `run` (a session's one queue, I6's drop before sending), `node.gone` against today's
clients, and the editor's Start, Stop and Restart in every phase (8.6).

### Revision 5 (2026-11-08): the owner's answers to the eight gates (not reviewed)

The owner answered all eight gates of revision 4's three pages on 2026-11-08 (section 1, R25 to R32; 14.1 here;
node-framework.md 14.1, Q26 to Q28). This page now: has two resting states on the wire, `stopped` and `disabled`
(`state`, `phase` and `wanted` gain `disabled`; a node's description gains `disabled`; 4.3, 4.8); keeps `node.start` and
`node.stop` but makes them write nothing (ordered by the keeper, replied in the turn that applies them, an `action`
record on stdout, `node-disabled` on a disabled node; 4.2, 4.5, 7.6, W6, W10) and adds `node.disable` and
`node.enable` as edits at the source (`applied` `disable`, `enable` and, for any change to a disabled node, `stored`;
4.2, 4.9); drops `holds` and the "held" marks, keeping every port's meter, which is built, and adding only what the
lifecycle needs of it (8.6, W7, W8); decides when a save is related to a gesture and leaves an unrelated one going
(7.3, G14); ends a closed or silent session's gesture as released, committing its last value (6.5, 6.6, 7.2, 7.3, G15,
G16); records X2, G1 and C2 as answered as recommended. Journeys (2.1, 2.2), the editor's lifecycle table (8.6), the
gesture state machine and notices (8.1, 8.3), the records (9, 9.1), fixtures (4.7), the cut (L1, L3, 6.4, 6.5), costs
and rejected alternatives follow. No owner gate is open. The review owed for revision 4 now covers revision 5; it should
also judge the related-save rule's cases (7.3) and the end on a closed session's behalf against the session's queued
requests (6.6).

### Revision 5.1 (2026-11-08): `disabled` inside the node's braces (not reviewed)

Follows node-framework.md revision 7.1 and the owner's words (R29; revision 5.1 numbered it R32 here, before the pages shared one register): "the “disabled” key should be an
attribute within the node scope — not a keyword on the node name". `node.disable` writes `disabled true;` first in the
node's block and `node.enable` removes the line (node-framework.md D-F8 to D-F12, the spelling and rules the designer
chose); `disabled` is never a setting on the wire (W11): no schema lists it, `node.add` takes it beside `settings`, and
a `disabled` key inside `settings` is refused `invalid-settings`. Changed: the status, R29, 2.2's journey, 3's table,
the script examples, 4.2 (the table and its paragraph), the schema's list (4.2), 4.3's `wanted` and `disabled`, 4.8's
`wanted`, 7.6, step L1's cut and tests, W9, W10, W11 and section 15's knobly item. No wire member, notification, error
code or record changes; no owner gate is open.

### Round 4 (revisions 4 to 5.1), and revision 6 (2026-11-08)

node-framework.md's revisions 6 to 7.1 and this page's revisions 4 to 5.1 went to two reviewers (DX and RT; that page's
section 16, round 4, is the record of every finding, each checked in the code first). Those that touched this page,
and what changed here:

| Finding | Outcome here |
|---|---|
| M3 (the owner's R33, "Two versions") | `gain`'s class writes creation's `configure` on `CreateOf` (5.7); "the change version" where an in-place `configure` was named by its old signature (2.4) |
| M4: `node.state` only per phase change | Fixed: whenever any health member changes (4.3's table and rule 5, 4.8, W1); `lifecycle.json` gains `node.stop` on a failed node and on a configuring node |
| M5: `restart` an action from slice 5 | Fixed: `node.restart` under feature `lifecycle` in L1, `{applied: restart \| retry \| unchanged}` (4.2, 4.5, 7.6, 8.4, 9, 11; W12); removed from every type's `actions` (4.2's example) and from `node.invoke`; the name reserved (5.5, 5.8's `action-named-lifecycle`); `actions.json`'s restart cases move to `lifecycle.json`; step 5.4 keeps type actions only |
| M6 (the owner's R34, "Only this node") | Fixed: `node.disable` and `node.enable` edit the node's own `disabled` line (4.2's table and paragraph, L1; a `lifecycle.json` case and a system test) |
| M7: "run" on the wire | Fixed: `lifecycle.json`'s row, the snapshot row, 7.3, 7.7 (`setup`), 8.7's tooltip, R15's row, the R25-R28 row's "on the node's line"; `output-records.json`'s `"run": 2` goes as never emitted and `v` stays 1 (4.7) |
| M8: `node.stopped` for protocol 0 clients | Fixed: both sent within protocol 1, `node.stopped` deprecated (4.8, W5) |
| m1: R numbers | Fixed: one register, node-framework.md's; this page's R29, R30, R31 and R32 are now R30, R31, R32 and R29, and every citation moved (section 1) |
| m5, m9: `added` mapped two ways; `stopped` before resting | Fixed: `starting` until the node rests, "will rest stopped" in the editor; no `starting` record for a node added disabled (4.8, 8.6, 9.1; W13) |
| m6: gestures across a reconnect and a restart | Fixed: 8.3 and 7.3's `SIGTERM` row (W14) |
| m7: `action` records for requests that are not actions | Fixed: the record's definition (9; W15) |
| m10: ports changed on a disabled node in 3a | Fixed: `node.removed` and `node.added` until L3, `applied: "stored"` (4.3) |
| F1, F6: what a barrier costs; unbounded waits | Fixed: new diagnostics (4.6; W16) |

Revision 6 changes: the status, the scope's table, section 1 (one register, R33, R34, the R15 and R25-R28 rows), 2.2's
script, 3.1's Action row, 4.2, 4.3, 4.5, 4.6, 4.7, 4.8, 5.5, 5.7, 5.8, 7.3, 7.6, 7.7, 8.3, 8.4, 8.6, 8.7, 9, 9.1, 11
(L1 and 5.4), 14 (W1, W5, W12 to W16) and this record. No owner gate is open.
