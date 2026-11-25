# Changes in place and reshape: slice 3d of the node framework

Status: design; steps 1 to 5 of section 11 built (step 5 but the editor's Apply with a dry run) (section 2 says what and where). Slice 3d follows slice 3c (built: it swaps claims and posts to loops, both
3c's, `docs/writing-a-node-type.md#claims`, `#loops`) and comes before slice 6 (`control-plane.md`: a retune is one of the outcomes this slice makes). Its PipeWire
spike (S4) is run and the reshape built on it. It changes the audio path: holds, code swaps and reshapes. The built lifecycle (states,
operations, teardown, fades, barriers and their bounds) is described in `docs/architecture.md` and
`docs/writing-a-node-type.md`; the built wire in `docs/control-port.md`; this page cites them and says only what 3d adds.

[V] = verified in the code named, as of 2026-11-09; [U] = unverified, with the test that checks it (section 13).

## 1. The owner's words that bind this slice

The R numbers are the register the node framework's design used.

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R11 | "a Run should not be bounded by config changes. I want config and graph changes to be supported at runtime while audio is running" | 2026-11-08 | A node keeps running through a settings change it can take in place; a restart is one outcome of a change, not the only one |
| R16 | "I like the basic shape of the state machine proposal. And most importantly: it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | 2026-11-08 | An in-place change, its holds and its swap touch only its node's block and companions |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. For example, changing a network port on a node is something that a node can handle on its own — even if it must output null audio frames for a few seconds. But if reconfiguring changes the shape of the input/output bus, then that might not be fully transparent" | 2026-11-08 | The type declares what it can change in place and its change version of `configure` decides; the framework holds outputs silent, readies new resources before releasing old ones, swaps claims and enforces a deadline (sections 3 to 6); a change of ports is a reshape with its own rules (section 7) |
| R21 | "I meant “asynchronously”" (operations are asynchronous and ordered) | 2026-11-08 | An edit's reply comes at its commit with `applied`; it never waits for the change to finish |
| R24 | "too technical for me to properly answer" (where reconfigure lives and how it reaches a type's threads) | 2026-11-08 | Hooks run on the main loop and reach a node's threads only by typed posts (built: [`docs/writing-a-node-type.md#loops`](../writing-a-node-type.md#loops), "Why loops"); the rest of this page's choices are the designer's (section 12) |
| R30 (gate W3) | "I would like each port to show meter levels so I can see audio traversing the graph" | 2026-11-08 | A hold is seen as silence on the meters (no "held" marks); a reshape's new ports are metered from their first window (8.4) |
| R33 | Asked "Should configure come in two versions, each offering only its legal calls, so a wrong call won't compile?": "Two versions" | 2026-11-08 | `Changed configure(ChangeOf<spec>&, const Checked& old, const Checked& now)`, optional; absent, every change restarts (section 4) |
| X2 | Asked what happens to a change that removes ports that are wired: "(a)", as recommended: the editor lists the wires that would go before applying; Apply removes them and changes the setting in one step (one save of `ear.conf`) or neither; a script must ask (`dropRoutes: true`), else it is refused naming the wires | 2026-11-08 | 7, 8.3 |

## 2. What is built, and what this slice changes

Built (steps 1 to 4 of section 11), described where they live and not repeated here:

- the `.inPlace()` mark and the plan's `reconfigure` outcome
  ([`docs/writing-a-node-type.md#settings-one-table-of-typed-rows`](../writing-a-node-type.md#settings-one-table-of-typed-rows),
  [`docs/architecture.md#restarts-and-changes-of-settings`](../architecture.md#restarts-and-changes-of-settings));
- the pure machines' change states, and NodeHost's change in place: the change version through `ChangeOf`, `Changed`,
  staged posts, holds and their acknowledgement, a new code swapped at one position, the commit point, the deadline and
  the spec's `.lifecycle`, supersession, the fallback with its reasons and its diagnostics
  ([`docs/architecture.md#changes-in-place`](../architecture.md#changes-in-place),
  [`docs/writing-a-node-type.md#the-change-version`](../writing-a-node-type.md#the-change-version));
- `PortBlock::hold`, `release` and `swap` ([`docs/engine.md#holds-and-swaps`](../engine.md#holds-and-swaps));
- an RTP receiver replaced in a change behind a framework hold, made before break on a new port and break-first on the
  same one, the old one listed `retiring` until the commit point destroys it, its claims judged first and a key no
  longer asked released per key at the commit point
  ([`docs/writing-a-node-type.md#make-before-break`](../writing-a-node-type.md#make-before-break)); `rtp` on a new
  port made before break
  ([`docs/writing-a-node-type.md#engine-companions`](../writing-a-node-type.md#engine-companions));
- `rtp` in place ([`docs/node-types.md#rtp`](../node-types.md#rtp));
- `cx.claim` and `cx.unclaim` in a change: judged as the change version returns, before anything else, the old port
  released at the commit point, last, and two nodes swapping ports in one edit both coming back after a restart each
  ([`docs/writing-a-node-type.md#claims-in-a-change`](../writing-a-node-type.md#claims-in-a-change));
- `whenReady` and `ConfigureDone`: the continuation posted once every acquisition is ready, the commit point after its
  `done()`, a newer change waiting from the post
  ([`docs/writing-a-node-type.md#whenready-and-configuredone`](../writing-a-node-type.md#whenready-and-configuredone));
- `SinkControl::pause`, `resume` and `pausedFrames`, attaching a sink returning its `SinkControl&`, and the harness's
  `paused` and `pausedFrames` ([`docs/writing-a-node-type.md#pausing-a-sink`](../writing-a-node-type.md#pausing-a-sink));
  U15 is settled there (the tap never reads the pause);
- the other kinds' replacements: a sound card made before break or, by `Order::breakFirst`, break-first behind a hold
  on what its bank carries ([`docs/writing-a-node-type.md#make-before-break`](../writing-a-node-type.md#make-before-break));
  sinks and sources made beside the old ones and swapped in the commit's invoke, splitting the cycles exactly
  ([`docs/writing-a-node-type.md#sinks-and-sources-in-a-change`](../writing-a-node-type.md#sinks-and-sources-in-a-change));
  a recording opened beside the old one, which closes once the sinks the change replaced finished
  ([`docs/writing-a-node-type.md#recordings-in-a-change`](../writing-a-node-type.md#recordings-in-a-change));
- on the wire: `reconfiguring` with `keys` and `next`, the `fell-back` cause, `applied: "reconfigure"`, the
  standard-output records and the three diagnostics ([`docs/control-port.md#node-health`](../control-port.md#node-health),
  [`docs/output-records.md#node`](../output-records.md#node));
- the plan's outcomes `reshape` and `unchanged`, a change of ports with the same type told by `nodeChanged` with the
  ports before and after (no longer a replace, `portCaused` gone), a reshape as a restart with the block reshaped
  between the configurations, a disabled node's block reshaped alone, and in place with a test type
  ([`docs/architecture.md#reshapes`](../architecture.md#reshapes)); `PortBlock::reshape` on the plain add, its layouts
  swapped whole, its levels re-registered, the 32-port question of section 13 settled (a block may cross 32 ports:
  [`docs/engine.md#reshapes`](../engine.md#reshapes),
  [`#the-one-data-loop-and-invokes`](../engine.md#the-one-data-loop-and-invokes));
- on the wire, feature `reshape`: `applied: "reshape"`, `node.updated` with `inPlace`, `ports`, `inputs` and `outputs`
  in place of the replace, the cause `reshape` (`added`, `removed`) and its `ports changed` record, `ports-in-use` and
  `node.update`'s `dropRoutes` ([`docs/control-port.md#changes-of-ports`](../control-port.md#changes-of-ports));
  features `node.set` and `dryRun`: the merge, `applied` with `inPlace`, `ports` and `routesDropped`, a dry run of every
  outcome naming the routes a reshape would remove
  ([`docs/control-port.md#nodeset-and-dry-runs`](../control-port.md#nodeset-and-dry-runs));
- in the editor: a reshape redrawn in place, Stop, Restart and Disable saying they end a change, a fallback's toast
  ([`docs/editor.md#the-node-box`](../editor.md#the-node-box), [`#toasts`](../editor.md#toasts)).

This page still designs:

- a reshape in place of a configuration with companions: the companions on resized inside banks replaced make-before-break
  (7; built, such a node restarts);
- `sendspin` in place (6.6);
- the editor's Apply with a dry run, with slice 5's settings form (8.3).

The transitions this page names are the framework's numbering: **T9** (`running` or `stopped` → `reconfiguring`, the
change version called), **T10** (the commit point: back to `running` or `stopped`), **T11** (the fallback: `reconfiguring`
→ `stopping`, cause `restarting`, then a restart with the newest settings), **T18** (a newer change supersedes one
before its commit point), all built.

## 3. What a type declares, and what the plan decides

Built: the `.inPlace()` mark and `reconfigure`
([`docs/writing-a-node-type.md#settings-one-table-of-typed-rows`](../writing-a-node-type.md#settings-one-table-of-typed-rows)).
`.live(...)` (slice 6) implies `.inPlace()`. `.shapesPorts()` (slice 5) is only the schema's promise; the plan compares
the ports `ports()` makes. The outcomes, the built and those still designed:

| Outcome | When | What the node does | Its state |
|---|---|---|---|
| `unchanged` (built) | the same settings | nothing; nothing written | as it was |
| `retune` (slice 6) | every changed key is `.live` | the parameter slots move; no hook | as it was |
| `reconfigure` (built) | every changed key is `.live` or `.inPlace()`, at least one `.inPlace()`, ports unchanged | the change version of `configure` | `reconfiguring`, then back to `running` or `stopped` |
| `restart` (built) | any changed key is restart-only, ports unchanged | torn down, configured from empty, back to the wanted state | `stopping` (`restarting`), `configuring`, then as before |
| `reshape` (built) | the same type, the ports differ | section 7; in place (`inPlace: true`) when every changed key is `.inPlace()` or `.live`, else a restart with the block reshaped between configurations | `reconfiguring` when in place, else as `restart` |
| `replace` (built) | another type under the same name | removed and added | `stopping`, removed, then a new node |
| `disable`, `enable`, `stored` (built) | `disabled` changed, or any change to a node that stays disabled | as built; a `stored` change of ports reshapes the block alone | as built |

An edit that changes several kinds of row takes the strongest outcome (`retune` < `reconfigure` < `restart` <
`reshape`). A node that has not yet rested (`added`, `configuring`, `configured`, `starting`) takes any change as a
restart of its configuration (D-A7, built for every change in 3a). How the outcome reaches `NodeHost` is built
([`docs/architecture.md#restarts-and-changes-of-settings`](../architecture.md#restarts-and-changes-of-settings));
slice 6 adds `retune`.

**Restart-only by default, and why.** A node's code, instance and loop states were made from the old settings; only the
type knows which of its members a key reaches. So nothing is in place unless the type says so, and the hook may still
answer `restart` for a value it cannot take. **A type cannot refuse a change** (D-B2): by the time the change version
is called the change is committed, and a refusal at plan time would make a valid `ear.conf` unreachable from a running
graph though the same file boots fine, against "hand edits win".

## 4. The change version of `configure`

Built, every call of it ([`docs/writing-a-node-type.md#the-change-version`](../writing-a-node-type.md#the-change-version)).

**A code in the answer is always a swap** (D-R18): the framework never compares codes, so a type whose code keeps state
carries it into the new code or answers `cx.applied()` with none. A code swap without holds is a discontinuity unless the type carries its state;
the framework adds no crossfade. The new code's `prepare` runs on the main loop while the old code still runs: it may
allocate and build the new code's own state, and may not touch the framework's realtime state (slots, ramps, ring
cursors) or anything the old code's `process` reads (D-R13).

## 5. What the framework does, in order

Built, the replacements of every kind included ([`docs/architecture.md#changes-in-place`](../architecture.md#changes-in-place)).
Two capture cards, or two receivers, into one inside input mix for a moment while one is made before break; a hold
covers it, but it relies on the port mixer loading [U: section 13].

## 6. The primitives in a change

### 6.1 Claims swapped make-before-break

Built: [`docs/writing-a-node-type.md#claims-in-a-change`](../writing-a-node-type.md#claims-in-a-change).

**A same-port RTP receiver** (`rtp` changing `format` on 5004) asks `udp:5004`, which its configuration holds: its own.
Two rtp-source modules bound to one port with SO_REUSEADDR would split or duplicate the datagrams [U13], so the
framework replaces such a companion break-first without the type asking (5, "Break before make").

### 6.2 Recordings

Built ([`docs/writing-a-node-type.md#recordings-in-a-change`](../writing-a-node-type.md#recordings-in-a-change)): a
replacement names the recording it replaces. No built type uses it (`file`'s `path` stays restart-only; a gapless
switch is possible later).

### 6.3 Loops and their states

A loop and its state are the node's, unchanged by a change. The change version may post to it (staged, section 4) and
continue on it (`whenReady`). Slice 5's action calls keep running on the node's loops during a change.

### 6.4 A sink paused while the type swaps what it writes into

Built ([`docs/writing-a-node-type.md#pausing-a-sink`](../writing-a-node-type.md#pausing-a-sink), and a sink replaced
in a change: [`#sinks-and-sources-in-a-change`](../writing-a-node-type.md#sinks-and-sources-in-a-change)). Slice 5
samples `pausedFrames` beside the companion's drops.

### 6.5 `rtp` in place

Built ([`docs/node-types.md#rtp`](../node-types.md#rtp)): made before break on a new port, break-first on the same one.

### 6.6 `sendspin` in place

Decided (G19): `sendspin`'s `name`, `port` and `server` change in place, the listening and dialling switch included.
sendspin-cpp fixes its configuration when a client is made (`include/sendspin/client.h:333-335` [V]), so a name or port
change makes a new client inside the node's worker; a `server` change between two URLs with the same name is a redial
of the same client (`connect_to`, `client.h:389` [V]). The audio drops either way (the connection drops and the stream
ends); in place keeps the node's configuration (instance, worker state, loops, companions, status), its identity
without re-reading the key file, and both ports claimed for a moment. If the client swap's unknowns (U11, U12) fail,
the rows stay restart-only and this step is skipped.

```cpp
Changed Sendspin::configure(ChangeOf<spec>& cx, const Checked& old, const Checked& now) {
	SendspinSettings next = settingsFrom(cx.node(), now);
	if (old.get<rows::server>() && now.get<rows::server>() && settingsFrom(cx.node(), old).name == next.name) {
		cx.post(worker, [url = *next.server](Worker& w) { w.redial(url); });   // staged until the commit point
		return cx.applied();
	}
	if (!old.get<rows::server>()) cx.unclaim(claim::TcpListen{old.get<rows::port>()});   // released at the end
	if (!now.get<rows::server>()) cx.claim(claim::TcpListen{now.get<rows::port>()});     // judged now
	return cx.whenReady(worker, [next = std::move(next)](Worker& w, ConfigureDone done) mutable {
		w.swapClient(std::move(next), std::move(done));
	});
}
```

`swapClient` runs on the worker, the library's main loop, which the library's `start()` and `stop()` require
(`client.h:337`, `362` [V]):

1. posts to the notifier, which destroys its `onWake` source (no `notify_audio_played` reaches the old client), and back;
2. `toServerSink_.pause(worker_, ...)`, on the `SinkControl&` that attaching the sink returned and the worker's state
   keeps (given to `cx.state` after the attach): the sink's thread finishes any `consume` in flight and discards until
   `resume`, so
   `write_audio` never reaches a client being destroyed (this replaces the worker's `clientMutex_`);
3. says goodbye on mDNS, then `client->stop()`: goodbye to the server, the library's threads joined, `on_stream_end`
   delivered (which clears the source, which fades out); the client destroyed; the old sync thread, the source's
   writer, joined, so the new one may write (one writing thread at a time: `docs/writing-a-node-type.md#sources`);
4. makes the new client over the same key store, so the identity and pairings stay (U11), with `log_context` the same
   node handle; `start()`; on `false`, `done.fail("the Sendspin client did not start")`;
5. re-arms the notifier's `onWake`, `toServer_.resume()`, opens the advertiser on the new port, and calls `done.done()`.

What the operator sees for `kitchen` moving from 8928 to 8929: `node kitchen reconfiguring: port`; notes
`disconnected`, `stream-ended`, then `listening 8929 as ear-kitchen-<id>` and `id <same client id>`; `node kitchen
running`; then Music Assistant finds it again (`connected "Music Assistant" paired`). Its failures, each a fallback to a
restart with the new settings: 8929 held by another running node (refused at once, the continuation destroyed, the
restart's claim refused: `failed start-failed: cannot listen on port 8929: held by node porch`); held by a node still
closing (`ear.claim-wait` against the change's deadline; a newer edit meanwhile supersedes it); the new client not
starting (`done.fail`, then the restart); a stop or removal during the swap (a continuation not yet posted is destroyed;
one already posted runs, and the goodbye behind it stops whichever client exists).

### 6.7 The built-in types

| Type | In place | Restart-only, and why |
|---|---|---|
| `white` | — | no settings |
| `file` | — | `path`: a new file is a new recording (a gapless switch is possible later) |
| `card` | — | `card`: the same device cannot be opened twice, so break-first only, a restart in all but name; `playback`, `capture`: a reshape |
| `rtp` | `address`, `port`, `format`, `rate`, `latencyMs` (built: 6.5) | `channels`: a reshape |
| `sendspin` | `name`, `port`, `server` (6.6) | — |

No built-in type reshapes in place (D-R24); a test type proves it (built:
[`docs/architecture.md#reshapes`](../architecture.md#reshapes)).

## 7. A change of shape: reshape

Built: [`docs/architecture.md#reshapes`](../architecture.md#reshapes) (the plan, the restart, the disabled node, in
place), [`docs/engine.md#reshapes`](../engine.md#reshapes) (`PortBlock::reshape`, the layouts, the plain add, levels;
S4 and the 32-port answer: [`#the-one-data-loop-and-invokes`](../engine.md#the-one-data-loop-and-invokes)). Still
designed:

- **In place with companions**: companions on resized inside banks replaced make-before-break, the new companion
  handed over in one invoke, so a bank never holds two at once (the block's binding storage is one per inside bank,
  `docs/architecture.md#companions`). Built, a configuration with any companion takes a reshape as a restart.
- **arm64** waits on the append being made on the data loop: by PipeWire (an upstream change to `alloc_port`, not yet
  reported), or by ear owning its node's port list instead of using `pw_filter` (a port block written on the node
  interface, its list swapped by the invoke ear already has; not scoped).

## 8. On the wire and in the editor

The wire's frame (protocol 1, `features`, open enums) and the lifecycle's wire are built (`docs/control-port.md`). This
slice adds the features `node.set`, `reshape` (built: [`docs/control-port.md#changes-of-ports`](../control-port.md#changes-of-ports)) and `dryRun`.

### 8.1 Requests

Built ([`docs/control-port.md#nodeset-and-dry-runs`](../control-port.md#nodeset-and-dry-runs),
[`#changes-of-ports`](../control-port.md#changes-of-ports)). The reply comes at the commit (X3) and `applied` is the
plan's outcome (X7), as designed.

### 8.2 Health, notifications and records

Built: `reconfiguring`, `keys`, `next`, the causes `fell-back` and `reshape`, `node.updated` with a change's ports, the
order, the standard-output records and the diagnostics ([`docs/control-port.md#node-health`](../control-port.md#node-health),
[`#changes-of-ports`](../control-port.md#changes-of-ports), [`docs/output-records.md#node`](../output-records.md#node)).

### 8.3 The editor

Built ([`docs/editor.md#a-nodes-health`](../editor.md#a-nodes-health), [`#the-node-box`](../editor.md#the-node-box),
[`#toasts`](../editor.md#toasts)): `reconfiguring`'s style and line, Stop, Restart and Disable saying they end the
change, a held bank seen as silence on the meters (no "held" mark, R30), a reshape redrawing the box's handles in place,
and a fallback's toast. Still designed, waiting for slice 5's settings form (`control-plane.md`; the editor edits a node
by its knobly text, `node.setText`, which has no dry run):

- **Apply asks first** (feature `dryRun`): non-live rows are edited in a form and sent together with one `node.set`.
  Apply sends it with `dryRun: true`, and when the answer is `restart` or `reshape` (or names routes) shows what will
  happen before sending ("Restarts desk: its audio stops for a moment." or "Restarts desk and removes 1 route:
  kitchen:from.left → desk:play.2", with Apply and Cancel); `reconfigure` is sent at once, since nothing stops. The
  confirmed send carries `dropRoutes: true` exactly when the dry run named routes; if the real edit is refused
  `ports-in-use` anyway (another edit landed between), the prompt is shown again with the refusal's routes. (Per-field
  hints, "applies in place" or "restarts the node", need the schema's per-row `change`, slice 5's `types.get`.)
- **An edit to a key in `keys` or `next`** says "applies after this change".

### 8.4 Levels

Built ([`docs/engine.md#reshapes`](../engine.md#reshapes)): a reshape's new public port is metered from the first window
of the layout that has it. A held bank's output meter, the wire's level and the fed input's meter all read silence
during the hold, since the hold applies after the code and before metering.

## 9. What it changes in the built system

What steps 1 to 4 changed is built (section 2). What is left:

| Built piece | Change |
|---|---|
| `NodeHost` | a reshape in place of a configuration with companions (7) |
| `sendspin` | rows marked `.inPlace()`; its change version |
| `web/` | 8.3's Apply with a dry run, in slice 5's settings form |

## 10. Tests

Built: what `docs/testing.md` lists for changes in place. Still to write with what they test:

- **`ear_pipewire_tests`**: two sources into one inside input mixed (the port mixer loads).
- **Types**: `sendspin`'s in-place cases through the harness.
- **System tests**: an `rtp` format changed on the same port (a fade to exactly 0, silence, a fade in, no step above the
  fade's slope), and `levels` showing `fpp:stream.1` and `rec:record.left` silent during an `rtp` change and back after
  (the port change itself is built); a card's `playback` from 2 to 1 on the test card with `dropRoutes`, the surviving
  route never `pending`; a `sendspin` port, name and server changed in place (`reconfiguring` and never `stopping`, the
  same client id, the key file not read again, the connection dropped and back), a change to a held port restarting and
  failing with today's text, dialling to listening and back.

## 11. Order of work

1. S4, the spike (run: [`docs/engine.md#the-one-data-loop-and-invokes`](../engine.md#the-one-data-loop-and-invokes)), and the pure machines' new states and transitions (built:
   [`docs/architecture.md#changes-in-place`](../architecture.md#changes-in-place)).
2. `ChangeOf`, `Changed`, staged posts, holds and their acknowledgement, the commit point, the deadline, supersession,
   the fallback, with a test type; `rtp` in place (built, section 2: with an RTP receiver replaced always break-first,
   the part of step 3 `rtp` needed).
3. The primitives in a change: claims swapped make-before-break, `whenReady`, `ConfigureDone`, the sink's pause, and
   every kind's replacement (built, section 2).
4. `PortBlock::reshape` on the plain add, the 32-port question settled first; layouts, levels (built, section 2).
5. The wire and the editor (features `node.set`, `reshape`, `dryRun`): built, but the editor's Apply with a dry run,
   which waits for slice 5's settings form (8.3).
6. `sendspin` in place (skipped if U11 or U12 fails).
7. Words: AGENTS.md and the library docs; this page deleted.

## 12. Decisions already taken

Each is the designer's (R24); the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| D-A7 | Any change before the node first rested restarts its configuration; in-place changes apply only from then on | Nothing has started, so nothing is gained; one rule is easier to test |
| D-A8 | A `stop` or a removal during an in-place change ends it as a newer change would before its commit point, and waits for it after | The newest intent wins at once; a type's own work is never cut in half |
| D-B1 | Each row says whether it changes in place (`.inPlace()`, `.live`), restart-only by default; the plan reads the marks; `configure` may still answer `restart` | Data in the one settings table; nothing is in place by accident |
| D-B2 | A type cannot refuse a change it cannot make in place; restart is the fallback | A refusal would make a valid `ear.conf` unreachable from a running graph |
| D-B3 | A type asks for the outputs it wants silent (`cx.hold`); the framework fades them out and in over 5 ms, and holds itself before a break-first replacement | Only the node knows which outputs a change breaks; fades in framework code keep type code free of realtime work |
| D-B4 | An in-place change that fails or passes its deadline (10 s, up to 60 s declared) restarts with the new settings; it never rolls back | A rollback would leave the audio on a value `ear.conf` does not hold |
| D-B5 | A change of ports keeps the block and the links of surviving ports (a reshape), restarting unless every changed key is in place; if S4 fails, a reshape stays a replace | Surviving wires never blink; the fallback loses only that |
| D-B6 | A newer change, `stop`, removal or `restart` supersedes an in-place change before its commit point; after it, they wait (`restart` ends it at once) | The newest intent wins within milliseconds, and nothing is left half-done |
| D-R5 | An in-place reshape swaps whole layout objects (old, transitional, new) built on the main loop, one pointer each in a barrier invoke | Every array the data thread reads is fixed-size; resizing any in place is a use-after-free |
| D-R13 | The new code's `prepare` may not touch the framework's realtime state or anything the old code's `process` reads | It runs while the old code still runs |
| D-R17 | In place, `cx.post` work is staged until the commit point and destroyed unrun on supersession or fallback | Else work could run for a change that never commits |
| D-R18 | In place, a code given in the answer (`cx.applied(code)`) is always a swap; the framework never compares codes | One rule; comparing would silently skip or add a swap |
| D-R24 | An in-place reshape is proved with a test type; no built-in type reshapes in place | No built-in type needs it yet |
| D-L43 | S4's criterion is functional checks plus a cited code reading of `filter.c`, not ThreadSanitizer | TSan cannot see inside the uninstrumented libpipewire |
| G19 | `sendspin`'s `name`, `port` and `server` change in place, the listen/dial switch included | R17 names a port change as the case a node handles itself; the operator hears the same drop either way |
| G20 | `whenReady` is the only later answer of a change | No fourth hook, no callback per kind |
| G21 | `SinkControl::pause` and `resume`: the framework hands a sink's consumer to a loop and back | One hand-over, tested once, instead of a lock in every type |
| G22 | A replacement asking a key its own node holds is granted unprobed; a same-port RTP receiver is replaced break-first by the framework | The type cannot know how PipeWire binds; the gap is a fade, silence and a fade in |
| X2 (owner) | `ports-in-use`, and `dropRoutes` removing the stranded routes in the same commit | The owner's answer, as recommended |
| X3 | An edit's reply comes at the commit, with `applied`; a client waits for `node.state` | R21; holding replies would stall every client |
| X7 | `applied` is the plan's outcome; what the node did is `node.state`'s `cause` | Known at the commit, the same for a dry run and every client |
| new | A rule a type breaks in the change version fails the node (`lost`) with the rule's text, as every built context rule does | Consistent with 3a as built: rules fail the node, never ear |

## 13. Open questions and risks

- **arm64** is not open here: it waits on the append being made on the data loop (7).
- **[U] Two sources into one inside input** are mixed while a replacement is made before the old one goes; `engine.conf`
  maps no `audio.mixer*` library, so this relies on the port mixer for DSP client-node ports loading through its
  `library.name` fallback.
- **[U] How long rtp-source takes to unload and load** on a port change, which is how long `rtp` holds its outputs.
- **U11.** That a second `SendspinClient` made in the same process over the same persistence provider, after the first
  was stopped and destroyed, has the same client id and pairings (`client.h:347-348`, `config.h:171-174` [V] say a
  `start()` after `stop()` keeps them and the id comes from the persisted keypair; a new instance is not stated).
- **U12.** How long a client swap takes against the 10 s deadline (`stop()` is bounded by the transports' send and
  close, `client.h:351-354` [V]); sendspin may declare a longer one.
- **U13.** What Linux does with unicast datagrams for two sockets bound to one UDP port with SO_REUSEADDR; the
  break-first rule makes it moot, and the RTP test shows no doubled audio.
- **Tuning.** The fade (5 ms), the hold acknowledgement's bound (100 ms) and the in-place deadline (10 s, at most 60 s)
  are first choices, to be tuned on the MOTU.
- **Risks.** A wider lifecycle: the number of interleavings (a change during a reconfigure, a loss during a reshape)
  grows; each is a state-table test. `applied` is not what happened (a client must read `node.state`); a dry run may be
  stale by the real edit. A claim wait in a change can cost up to its deadline and then the restart's own 15 s, about
  25 s before the node fails. `sendspin`'s in-place change still drops the connection and the stream (Music Assistant
  reconnects in seconds). A pause discards what the node sends during a swap.
