# Changes in place and reshape: slice 3d of the node framework

Status: design; steps 1 and 2 of section 11 built, and of step 3 claims in a change, make-before-break for an RTP
receiver, `whenReady` and `ConfigureDone`, and the sink's pause (section 2 says what and where). Slice 3d follows slice 3c (built: it swaps claims and posts to loops, both
3c's, `docs/writing-a-node-type.md#claims`, `#loops`) and comes before slice 6 (`control-plane.md`: a retune is one of the outcomes this slice makes). Its PipeWire
spike (S4) is run: a reshape goes ahead as designed (7.4). It changes the audio path: holds, code swaps and reshapes. The built lifecycle (states,
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
| X2 | Asked what happens to a change that removes ports that are wired: "(a)", as recommended: the editor lists the wires that would go before applying; Apply removes them and changes the setting in one step (one save of `ear.conf`) or neither; a script must ask (`dropRoutes: true`), else it is refused naming the wires | 2026-11-08 | 7.1, 8.3 |

## 2. What is built, and what this slice changes

Built (steps 1 and 2 of section 11), described where they live and not repeated here:

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
- on the wire: `reconfiguring` with `keys` and `next`, the `fell-back` cause, `applied: "reconfigure"`, the
  standard-output records and the three diagnostics ([`docs/control-port.md#node-health`](../control-port.md#node-health),
  [`docs/output-records.md#node`](../output-records.md#node)).

This page still designs:

- the plan's outcomes `reshape` and `unchanged`, and a change of ports with the same type no longer a replace (3, 7);
- recordings, sinks, sources and cards replaced in a change (4, 6);
- make-before-break for the other kinds, and `Order::breakFirst` (5);
- `PortBlock::reshape` (7);
- `sendspin` in place (6.6);
- on the wire: features `node.set`, `reshape` and `dryRun`, `dropRoutes`, `ports-in-use`, and the editor (8).

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
| `unchanged` | the same settings | nothing; nothing written | as it was |
| `retune` (slice 6) | every changed key is `.live` | the parameter slots move; no hook | as it was |
| `reconfigure` | every changed key is `.live` or `.inPlace()`, at least one `.inPlace()`, ports unchanged | the change version of `configure` | `reconfiguring`, then back to `running` or `stopped` |
| `restart` (built) | any changed key is restart-only, ports unchanged | torn down, configured from empty, back to the wanted state | `stopping` (`restarting`), `configuring`, then as before |
| `reshape` | the same type, the ports differ | section 7; in place (`inPlace: true`) when every changed key is `.inPlace()` or `.live`, else a restart with the block reshaped between configurations | `reconfiguring` when in place, else as `restart` |
| `replace` (built) | another type under the same name | removed and added | `stopping`, removed, then a new node |
| `disable`, `enable`, `stored` (built) | `disabled` changed, or any change to a node that stays disabled | as built; a `stored` change of ports reshapes the block alone (7.2) | as built |

An edit that changes several kinds of row takes the strongest outcome (`retune` < `reconfigure` < `restart` <
`reshape`). A node that has not yet rested (`added`, `configuring`, `configured`, `starting`) takes any change as a
restart of its configuration (D-A7, built for every change in 3a). **How the outcome reaches `NodeHost`**: `NodeOutcome`
gains `reconfigure`, `reshape` and `unchanged` (and `retune` in slice 6), told through the built
`DesiredNodeListener::nodeChanged`, whose `NodeChange` gains the old and new ports; a change of ports with the same
type is no longer a replace (`nodeReplacing`/`nodeReplaced` stay for another type only, and `portCaused` goes). Its
default still calls `nodeUpdated`, so every listener but `NodeHost` and the gateway is unchanged.

**Restart-only by default, and why.** A node's code, instance and loop states were made from the old settings; only the
type knows which of its members a key reaches. So nothing is in place unless the type says so, and the hook may still
answer `restart` for a value it cannot take. **A type cannot refuse a change** (D-B2): by the time the change version
is called the change is committed, and a refusal at plan time would make a valid `ear.conf` unreachable from a running
graph though the same file boots fine, against "hand edits win".

## 4. The change version of `configure`

Built: `ChangeOf`'s `format`, `node`, `applied()`, `applied(code)`, `post` (staged), `attach(bank, RtpReceiverShape)`
(break-first), `declareLatency`, `hold`, `holdAll` and `release`; `Changed::restart`; `Lifecycle`; the rules a compiler
cannot check, each failing the node `lost`; the new code's `prepare` beside the old code
([`docs/writing-a-node-type.md#the-change-version`](../writing-a-node-type.md#the-change-version)). Still designed, added
to `ChangeOf`:

```cpp
template <const auto& Spec> class ChangeOf {
public:
	// Replacements of the other kinds, made before break (5.3).
	template <class Bank> void attach(Bank, SoundCardShape, Order = Order::makeFirst);
	template <class Bank> SourceWriter& attach(Bank, SourceShape);
	template <class Bank> SinkControl& attach(Bank, SinkShape, /* its consumer, in creation's three forms */);   // 6.4
	int recording(RecordingRequest);                                   // a replacement, made before break
};
```

Built: `whenReady` and `ConfigureDone`
([`docs/writing-a-node-type.md#whenready-and-configuredone`](../writing-a-node-type.md#whenready-and-configuredone)).

**A code in the answer is always a swap** (D-R18): the framework never compares codes, so a type whose code keeps state
carries it into the new code or answers `cx.applied()` with none. A code swap without holds is a discontinuity unless the type carries its state;
the framework adds no crossfade. The new code's `prepare` runs on the main loop while the old code still runs: it may
allocate and build the new code's own state, and may not touch the framework's realtime state (slots, ramps, ring
cursors) or anything the old code's `process` reads (D-R13).

## 5. What the framework does, in order

Built: the order (the change version, holds, the commit point, the deadline, supersession and what arrives
mid-change: [`docs/architecture.md#changes-in-place`](../architecture.md#changes-in-place)). Still designed:

- **New acquisitions readied beside the old ones**, in readiness order: claims first, then files, then companions, each
  old resource marked retiring at once. A replacement engine companion linked to the same inside bank as the one it
  replaces; two capture cards into one inside input mix for a moment, which a hold covers [U: the port mixer loads,
  13]. A replacement ring companion (a sink or a source) served by the block itself and swapped in the commit point's
  invoke (`docs/architecture.md#companions`), so two never play into one bank, a replaced sink taking the cycles before
  the swap and its replacement those from it.
- **At the commit point**, the retiring resources released by the teardown's order for those resources only (a
  replaced source's writer closed; replaced companions destroyed; replaced sinks drained to their own consumers and
  finished; replaced recordings closed; `unclaim`ed claims released, last), after the continuation's `done`.

**Make-before-break** is built for an RTP receiver, and break-first on its same port
([`docs/writing-a-node-type.md#make-before-break`](../writing-a-node-type.md#make-before-break)); still designed: a type
asking for it with `Order::breakFirst` where only it can know (a card: one ALSA device opened twice), holding the inside
output bank that feeds the companion being destroyed (a card's `to_card`), and every other replacement made before
break.

## 6. The primitives in a change

### 6.1 Claims swapped make-before-break

Built: [`docs/writing-a-node-type.md#claims-in-a-change`](../writing-a-node-type.md#claims-in-a-change).

**A same-port RTP receiver** (`rtp` changing `format` on 5004) asks `udp:5004`, which its configuration holds: its own.
Two rtp-source modules bound to one port with SO_REUSEADDR would split or duplicate the datagrams [U13], so the
framework replaces such a companion break-first without the type asking (5, "Break before make").

### 6.2 Recordings

`cx.recording({newPath})` in a change is claimed and opened beside the old one; the old is closed at the commit point.
No built type uses it (`file`'s `path` stays restart-only; a gapless switch is possible later).

### 6.3 Loops and their states

A loop and its state are the node's, unchanged by a change. The change version may post to it (staged, section 4) and
continue on it (`whenReady`). Slice 5's action calls keep running on the node's loops during a change.

### 6.4 A sink paused while the type swaps what it writes into

Built ([`docs/writing-a-node-type.md#pausing-a-sink`](../writing-a-node-type.md#pausing-a-sink)). Still designed: a
replaced sink drains to its own consumer and finishes at the commit point; its replacement's `begin` runs as it becomes
ready, before the commit point (6.2's kinds, section 5). Slice 5 samples `pausedFrames` beside the companion's drops.

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

No built-in type reshapes in place; this slice proves an in-place reshape with a test type in `ear_core_tests` and the
spike's engine test (D-R24).

## 7. A change of shape: reshape

Ports, banks and member counts are made by `ports(checked)` at parse; a change of them is a **reshape** (R17: "might not
be fully transparent").

### 7.1 How the desired graph judges it

As built, every wanted route must name a port the new ports declare (a plan keeping a route to `desk:play.4` while
`desk` goes to two playback channels is refused `unknown-port`). Through the control port it becomes `ports-in-use`,
naming every such route, and `dropRoutes: true` removes those route lines in the same commit (X2, the owner's answer);
a hand edit or `graphFile.set` that keeps one is refused `unknown-port` as today. By the time a reshape commits, no wanted
route names a port that vanishes. Wanted routes may name ports that appear; they link once the ports are live.

### 7.2 What the framework does

- **By default** (not in place): a restart with the block reshaped between the old configuration's teardown and the new
  one's `configure`. The block is kept. Ports that vanish are removed (no wanted route names them; routes are removed
  before nodes change in a commit, as built); ports that appear are added, silent, and registered for levels.
  Surviving ports keep their links throughout and carry zeros while the node restarts. Inside banks are resized with
  the rest, and their companions are made afresh by the new `configure`.
- **On a disabled node** (no configuration): the block alone is reshaped at the commit, with no hook called; the
  outcome is `stored`, with its `ports`; an enable later configures it with the ports the block already has. (Built: a
  replace told as `node.removed` and `node.added`, `applied: "stored"`.)
- **In place**, when every changed key is `.inPlace()` or `.live`: a reconfigure. The framework adds the new ports to
  the engine first, calls the change version, which must swap the code (its `Io` layout changed; a hook that does not
  fails the node with the rule's text), and removes the vanished ports after the swap's barrier. A type whose channel
  count is `.inPlace()`, going from 2 to 3 channels, keeps channels 1 and 2 playing. Companions on resized inside banks
  are replaced make-before-break: the new companion is handed over in one invoke, so a bank never holds two at once
  (the block's binding storage is one per inside bank, `docs/architecture.md#companions`).

### 7.3 Two layouts while a reshape is in flight

The block's ports are two flat lists by direction, public then inside, and `Io` resolves banks to indices in `prepare`;
adding `in.3` before an inside bank moves that bank's indices, so the old code keeps the layout it was prepared with
until the swap. **Each layout is one object, swapped whole** (D-R5): everything the realtime side reads for a block is
fixed-size today (the port handles, the spans, the scratch, the meters, the latency arrays, and the levels'
`RegisteredBlock` with its `LevelMailbox`, sized to the public ports, `port_blocks.cpp` [V]), and none is resized in
place. A **layout** holds all of them for one set of ports; it is built whole on the main loop (allocation included)
and the block reads it through one pointer. An in-place reshape uses three: the old; a **transitional** one, built once
the new ports exist in PipeWire, that hands the old code the spans of its own ports and lists the new ports' outputs to
zero-fill and their inputs to ignore; and the new one, for the new code. Each is installed by one pointer swap in a
barrier invoke, the second together with the new code; its `RegisteredBlock` is swapped into the levels' list under
`Levels::mutex`; and the layout it replaced is freed only after a second barrier completed. Vanished ports are removed
from PipeWire only after the new layout's swap. A restart's reshape (between two configurations) needs one new layout,
swapped before the new code first drives.

### 7.4 The mechanism, and its spike

`PortBlock::reshape(const Ports& next)` on `pw_filter_add_port` and `pw_filter_remove_port` on a connected
`PW_FILTER_FLAG_RT_PROCESS` filter. **S4, the spike, first** (planned in 3a's design, D-L43): a test type with two input
banks and an inside bank, a running ramp through a second block beside it, then `pw_filter_add_port` in the middle of
the input list and `pw_filter_remove_port` of a linked port, 500 times. Pass: the beside ramp never loses a sample; new
ports appear in the registry and link; a removed port's links go; a port added while links exist carries audio; and a
code reading of PipeWire 1.4.2's `filter.c` (`pw_filter_add_port`, `pw_filter_remove_port`, the port list the data
loop's `impl_node_process` walks, and how each change reaches the data loop) shows the list is changed only inside an
invoke on the data loop or before the port is visible to it, cited in the test's header. ThreadSanitizer is not a
criterion: the image's libpipewire is not instrumented. **Fallback** (D-B5): a reshape stays the built port-caused
replace, and only the UX claim (surviving links stay) is lost.

**S4, run (2026-11-22; `spikes_test.cpp`, tag `[s4]`; what it found of PipeWire is
`docs/engine.md#the-one-data-loop-and-invokes`).** Every functional criterion passed, in 19 runs of 500 rounds of each of its two cases, six
of them beside the whole suite: the ramp beside lost nothing, the new port appeared, linked and carried audio, a removed
port's link went, and a port linked throughout kept its link and its audio. **The code reading did not pass as the
criterion is written:** `filter.c` appends to and unlinks from the port list its data loop walks on the main loop,
with no invoke. It is safe in practice for the reasons that page gives: on x86-64 by the order of the append's two
stores, and for a remove on any architecture. So **the fallback is not taken, and `PortBlock::reshape` is built on
`pw_filter_add_port` and `pw_filter_remove_port`**, on these terms:

- **A vanishing port leaves the block's layout behind a barrier before `pw_filter_remove_port`**, as 7.3 already
  orders it: the handle is the port's memory, freed when the call returns.
- **The add is made plain, never with the data loop held** (the owner, 2026-11-22, binding: the graph is changed
  without interrupting it). It is safe on x86-64, the only architecture ear builds. The held add the spike also ran is
  not built: it makes the realtime thread wait on an ordinary one, a priority inversion with no bound (50 to 300 us
  in most runs, 2.1 ms in one).
- **An arm64 image waits on this.** Without total store order the plain add is not safe, so arm64
  (`docs/architecture.md#not-built-yet`) needs the append made on the data loop: by PipeWire (an upstream change to
  `alloc_port`, not yet reported), or by ear owning its node's port list instead of using `pw_filter` (a port block
  written on the node interface, its list swapped by the invoke ear already has; not scoped).
- **A new port's first cycles after its link is active may bring no buffer, or silence.** The transitional layout
  (7.3) already ignores a new input; a test of an in-place reshape waits for the audio, never counts cycles from
  `linkActive`.
- **The safety rests on PipeWire 1.4.2's code, not on a contract of `pw_filter`.** A PipeWire bump reads `alloc_port`,
  `free_port` and `pw_impl_port_remove` again (the `pipewire-engine` skill's bump steps), and S4 runs on every build.

## 8. On the wire and in the editor

The wire's frame (protocol 1, `features`, open enums) and the lifecycle's wire are built (`docs/control-port.md`). This
slice adds the features `node.set`, `reshape` and `dryRun`.

### 8.1 Requests

| Method | Feature | Params | Result | Refusals (`data.code`) |
|---|---|---|---|---|
| `node.set` | `node.set` | `{"name", "settings": {key: value \| null}, "dryRun"?, "dropRoutes"?}` | `{"applied"}`; on a reshape also `inPlace`, `ports` and `routesDropped`; with `dryRun`, `{"applied", "inPlace"?, "ports"?, "routes"?}` | `unknown-node`, `invalid-settings`, `shared-definition`, `file-invalid`, `ports-in-use`, `stopping`, the desired graph's (slice 6 adds `parameter-held`) |

- **`node.set`** sets the named keys and leaves the others: a JSON merge patch of the node's settings (`null` removes a
  key, which then reads its default; removing a required key is `invalid-settings` with detail `missing`). It is
  `node.update` with the node's current settings merged, made at the source like every edit. A `disabled` key in it is
  refused `invalid-settings`, as built for `node.update`.
- **`applied`** on `node.set`, `node.update` and `node.updated` gains `reconfigure`, `reshape` and `unchanged` (built:
  `restart`, `disable`, `enable`, `stored`). It is the plan's outcome at the commit, never rewritten by what the node
  then does (X7): a node not yet rested takes any change as a restart, and `configure` may answer `restart`; both show
  in `node.state` (`stopping` with cause `restarting`), the one place for what happened. `replace` is never an
  `applied`. A `reshape` also carries `inPlace`.
- **The reply comes at the commit** (X3), never when the node runs again: the keeper takes no next request until the
  reply's step is done, so holding it for a 10 s change would stall every client. A client that must wait for audio
  waits for the first `node.state` after the reply (`rpc-call.mjs --until`, built), since a session's replies and
  notifications share one ordered queue.
- **`dryRun: true`**: the keeper makes the candidate as for a real edit and posts it; the main loop plans it and
  answers the outcome without committing, telling, writing or changing the working copy. Every refusal a real edit
  would get, it gets. A dry run is advice: the real edit is judged afresh.
- **`dropRoutes: true`** (on `node.set` and `node.update`, feature `reshape`): an edit whose new ports drop a port that
  a wanted route names is refused `ports-in-use` (`data.ports`, `data.routes`, each `{from, to}`) unless it carries
  `dropRoutes`; then the keeper removes those route lines in the same candidate (as `node.remove` removes a node's
  route lines), so the routes and the reshape are one commit and one write (X2). The keeper knows the new ports on its
  thread: a type's `parse` and `ports` are pure. A route line inside a snippet another route or node uses is refused
  `shared-definition`. The reply and `node.updated` then carry `ports` (`{"added", "removed"}`, whole port names) and
  the node's new `inputs` and `outputs`, the reply `routesDropped`.

```json
{"jsonrpc":"2.0","id":8,"method":"node.set","params":{"name":"desk","settings":{"playback":1},"dryRun":true}}
{"jsonrpc":"2.0","id":8,"result":{"applied":"reshape","inPlace":false,"ports":{"removed":["play.2"],"added":[]},"routes":[{"from":"kitchen:from.left","to":"desk:play.2"}]}}
{"jsonrpc":"2.0","id":9,"error":{"code":-32000,"message":"ports-in-use desk: play.2 (1 route)","data":{"code":"ports-in-use","ports":["play.2"],"routes":[{"from":"kitchen:from.left","to":"desk:play.2"}]}}}
```

### 8.2 Health, notifications and records

- **`node.state`**: `state` and `phase` gain `reconfiguring` (open values, so additive within protocol 1); `keys`
  (`reconfiguring` only: the keys this change applies) and `next` (keys changed since its commit point, which the node
  takes when it ends). A supersession sends a new `node.state` `reconfiguring` with the new `keys` (or a `stopping` with
  cause `restarting` if the newest change needs one). `cause.kind` of a restart gains `reshape` (`added`, `removed`) and
  `fell-back` (`reason`: `restart-asked`, `deadline`, `resource-refused`, `resource-lost`, `failed`,
  `restart-requested`; `detail`). A retune (slice 6) changes no health.
- **`node.updated`** carries `ports` on a reshape (handles redrawn in place); a change of ports with the same type is no
  longer `node.removed` and `node.added`.
- **Companions**: `retiring: true` is built ([`docs/control-port.md#companions`](../control-port.md#companions)).
- **Order**: an in-place change sends `node.updated` (`applied: "reconfigure"`), then `node.state` `reconfiguring`, then
  the reply, in the commit's turn; then `node.state` `running` (or `stopped`). A reshape sends the dropped routes'
  `route.removed`, then `node.updated` with `ports`, then the restart's or the reconfigure's sequence; the block's new
  ports reach `route.status` only after `node.updated` named them.
- **Standard output**: `node <n> reconfiguring: <keys, comma-separated>` (phase `reconfiguring`, `keys`); the `running`
  that ends it carries `afterMs`; `stopping: restarting (<restart>)` gains `ports changed` and `could not apply <keys>
  in place: <detail>` (`detail` the hook's reason, the refusal, `deadline 10s passed` or the loss).
- **Diagnostics** (`diagnostic` records; on the wire with slice 5's `node.diagnostic`): `ear.reconfigure-fell-back`
  (`reason`, `keys`, `detail`, `elapsedMs`; just before its `stopping` record), `ear.reconfigure-superseded` (`keys`, the
  abandoned ones; `next`, the newest), `ear.retiring-lost` (subject the companion, `detail`).

### 8.3 The editor

- **Apply asks first** (feature `dryRun`): non-live rows are edited in a form and sent together with one `node.set`.
  Apply sends it with `dryRun: true`, and when the answer is `restart` or `reshape` (or names routes) shows what will
  happen before sending ("Restarts desk: its audio stops for a moment." or "Restarts desk and removes 1 route:
  kitchen:from.left → desk:play.2", with Apply and Cancel); `reconfigure` is sent at once, since nothing stops. The
  confirmed send carries `dropRoutes: true` exactly when the dry run named routes; if the real edit is refused
  `ports-in-use` anyway (another edit landed between), the prompt is shown again with the refusal's routes. (Per-field
  hints, "applies in place" or "restarts the node", need the schema's per-row `change`, slice 5's `types.get`.)
- **`reconfiguring`** has its own style (not a warning colour), its line from `keys` ("reconfiguring port"; "; latencyMs
  next" from `next`). Stop says "ends the change and stops", Disable "ends the change and disables", Restart "ends the
  change and restarts". An edit to a key in `keys` or `next` says "applies after this change".
- **A held bank**: no "held" mark (R30). Its meters fall to silence, its wires thin with their level, and the meters of
  the ports they feed fall with them, while the box says "reconfiguring port" in words.
- **A reshape** redraws the box's handles in place when `node.updated` brings `ports`: surviving handles keep their
  wires (which never go `pending`), new handles appear unwired, and the box keeps its position.
- **A fallback** also raises a notice: "fpp could not apply port in place, so it restarted: <detail>". The panel keeps
  the new value: `ear.conf` holds it.

### 8.4 Levels

A reshape registers the block's new public ports for levels and drops the vanished ones at its barrier (7.3), so a new
handle's meter moves from its first window (today a block's metered ports are fixed when it is made). A held bank's
output meter, the wire's level and the fed input's meter all read silence during the hold, since the hold applies after
the code and before metering.

## 9. What it changes in the built system

What steps 1 and 2 changed is built (section 2). What is left:

| Built piece | Change |
|---|---|
| `NodeOutcome`, `NodeChange`, `DesiredNodeListener` (`desired_graph.hpp`) | outcomes `reshape`, `unchanged`; `NodeChange` gains the old and new ports and `inPlace`; a change of ports with the same type is told by `nodeChanged`, not as a replace; `portCaused` goes |
| `lifecycle.hpp` | the other kinds' replacements |
| `NodeHost` | make-before-break for the other kinds; the block's reshape between teardown and `configure` |
| `PortBlock`, `FilterPortBlocks` | `reshape(next)` with layouts swapped whole; levels re-registered |
| `sendspin` | rows marked `.inPlace()`; its change version |
| `ear_config` | `node.set`'s merge; `dropRoutes` and `ports-in-use` in the candidate; dry runs posted and dropped |
| `ear_control`, `web/` | features `node.set`, `reshape`, `dryRun`; 8.3's editor beyond the `reconfiguring` style and line (built) |

## 10. Tests

Built: what `docs/testing.md` lists for changes in place. Still to write with what they test:

- **`ear_core_tests`**: the code swap with a sink and its replacement splitting the cycles exactly, a reshape in place by
  a test type (channels 1 and 2 keep playing).
- **`ear_pipewire_tests`**: S4 (built: 7.4); reshape (a port added and removed beside a running ramp, links of surviving
  ports kept, the layout swapped whole, the transitional layout zero-filling new outputs); a reshape's new port metered
  from its first window; two sources into one inside input mixed (the port mixer loads).
- **`ear_config_tests`**: `node.set`'s merge; a dry run changes nothing and tells no one; `dropRoutes` removes exactly
  the stranded route lines in one write; a route line inside a snippet refused `shared-definition`; `ports-in-use`
  naming every route.
- **Types**: `sendspin`'s in-place cases through the harness.
- **System tests**: an `rtp` format changed on the same port (a fade to exactly 0, silence, a fade in, no step above the
  fade's slope), and `levels` showing `fpp:stream.1` and `rec:record.left` silent during an `rtp` change and back after
  (the port change itself is built); a card's `playback` from 2 to 1 on the test card with `dropRoutes`, the surviving
  route never `pending`; a `sendspin` port, name and server changed in place (`reconfiguring` and never `stopping`, the
  same client id, the key file not read again, the connection dropped and back), a change to a held port restarting and
  failing with today's text, dialling to listening and back.
- **Wire fixtures**: `test/protocol/node-set.json` (merge, `null`, a missing required key, each `applied`, `dryRun` for
  each outcome, a reshape refused `ports-in-use` then with `dropRoutes`); the reshape cases of `lifecycle.json` (its
  reconfigure cases are built).

## 11. Order of work

1. S4, the spike (run: 7.4), and the pure machines' new states and transitions (built:
   [`docs/architecture.md#changes-in-place`](../architecture.md#changes-in-place)).
2. `ChangeOf`, `Changed`, staged posts, holds and their acknowledgement, the commit point, the deadline, supersession,
   the fallback, with a test type; `rtp` in place (built, section 2: with an RTP receiver replaced always break-first,
   the part of step 3 `rtp` needed).
3. The primitives in a change: claims swapped make-before-break, `whenReady`, `ConfigureDone`, the sink's pause,
   make-before-break for an RTP receiver and `rtp` on a new port (built, section 2); the other kinds' replacements
   (cards with `Order`, sources, sinks with the cycles split at the commit's invoke, recordings: sections 4, 5, 6.2,
   6.4) still to build.
4. `PortBlock::reshape` on the plain add (7.4), the 32-port question of section 13 settled first; layouts, levels.
5. The wire and the editor (features `node.set`, `reshape`, `dryRun`).
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

- **[U] A block with more than 32 ports of one direction.** `pw_filter` keeps each direction's ports in a map that
  grows 32 slots at a time (`filter.c:1247`), and `alloc_port` grows it on the main loop (`pw_map_insert_new`,
  `filter.c:342`), which moves its storage. The data loop looks a port up by id only in `impl_port_reuse_buffer`
  (`filter.c:964`), reached from the port's tee (`impl-port.c:282`); whether a DSP link ever calls it is not
  established. Settled by reading `impl-link.c`'s buffer negotiation for a 32-bit float mono port, or by S4 run with
  40 inputs, **before `PortBlock::reshape` is built** (section 11, step 4). If it can be called, the data loop is
  never held for it (7.4): a reshape that would cross a multiple of 32 ports in one direction is a replace, until
  the append is made on the data loop.
- **arm64** is not open here: it waits on the append being made on the data loop (7.4).
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
