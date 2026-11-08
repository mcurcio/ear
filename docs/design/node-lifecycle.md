# The node lifecycle, built: slice 3a's module design

Status: proposal, revision 1, 2026-11-08, not reviewed. The module design `docs/design/node-framework.md` (revision 8.1)
asks for before slice 3a's code (its D-D2 and 13.1): the two pure machines as tables, `NodeHost`'s rewrite, the
asynchronous barriers and their bounds, take-up by graph position, the interim sink and the legacy adapter for
`sendspin`, the spikes 3a needs first, the tests, and the cut into steps that each merge green. It also plans slice 3d's
reshape spike, which 13.1 puts here. Written by the code architect after reading every file it names.
Layer: module design, beneath `docs/design/node-framework.md` (the system design of the node framework) and beside
`docs/design/node-primitives.md` (slice 3c) and `docs/design/control-plane.md` (whose step L1 is 3a's wire).

**What this page is the home of, and what it is not.** It owns the cut and the how: what 3a builds, in what order, how
each built file changes, what stays interim and how it goes. It is the home of the two machines' full tables (4.1, 4.2),
the aggregate's derivation (4.3), the table of every operation against every phase (4.4), `PortBlock`'s 3a interface
(5.3), the batching rule (5.5), the erased instance and the legacy adapter (5.9, 5.10) and the interim sink (5.11). It
is not the home of the type author's API: every class a type writes against (`CreateOf`, `NodeOf`, `Configured`, the
3a interim calls, `SinkRequest`) is declared whole in node-framework.md 6.10 and only cited here. It is not the home of
the wire either: every method, member, error code and record is control-plane.md's (4.2, 4.5, 4.6, 4.8, 7.6, 9.1),
and this page says only which of them 3a builds and where in the code.

[V] = verified in the source named, as of `cc02fee` (ear) or `74ad445` (knobly, `KNOBLY_COMMIT`); [U] = unverified,
so a step's test or spike checks it (section 10, 16).

## 1. The owner's words

One register: the R numbers are node-framework.md's section 1, and this page adds none. The ones 3a builds:

| # | The owner's words (node-framework.md section 1) | What it means in 3a |
|---|---|---|
| R6 | "in a general purpose sources-and-sinks graph, its always just 'running'" | No transition of one node stops a block processing, its own included: a silent block still processes and meters (5.3) |
| R12, R16 | "each node deserves a dedicated state machine"; "it should be per-node. Some node stopping/reconfiguring/failing does not effect the graph globally" | Two pure machines per node and one aggregate (4); every barrier is on the node's own block; one node's wait never holds another's (5.2, 5.5) |
| R13 | "why must mailboxes be built before the run object exists?" | The code drives only at T5, after the instance exists (5.9); `file`'s consumer is a member made in `configure` (7.4) |
| R14 | "The node audio threads should be concerned with reading inputs and writing outputs" | `file`'s `Recorder` goes; its code is a pass-through onto the interim sink, whose ring is framework code (5.11) |
| R17 | "even if it must output null audio frames for a few seconds" | Every planned silence is faded and every drive fades in (5.3); in-place changes are 3d's |
| R19, R23, R33 | one `configure`, three hook names; "Two versions" | 3a builds creation's `configure`, `start()` and `stop()`; the change version is 3d's (node-framework.md 6.4, 6.10) |
| R20, R21 | "A “run” can’t fail — only a node can fail"; "I meant “asynchronously”" | No run in any new name; a failed node is torn down at once and keeps its block and wires; every operation's reply comes in the turn that applies it, never after the node's slow work (5.2) |
| R22 | a short `state` and a `phase` | `NodePhase` and `NodeState` are two enums (4.3) |
| R25 to R28 | "Disabled"; "Keeps everything"; "Both words"; "Only disabled in file" | `stopped` keeps the configuration and is held by `NodeHost` in memory; `disabled` releases it and lives in `ear.conf` (4.3, 8) |
| R29, R34 | `disabled` inside the node's braces; "Only this node" | The graph file's reserved key, read before the type's settings and edited on the node's own line (8) |
| R30 | "each port to show meter levels" | Blocks meter their public ports in every phase, silent ones included, as today (`port_blocks.cpp:421-422` [V]) |

## 2. Scope

**In 3a** (node-framework.md 13.1's row, which this page follows to the letter):

- The block machine, the configuration machine and the aggregate (4), replacing `NodeLifecycle` (`node_lifecycle.hpp:8-41`
  [V]) through an erratum to `batch2.md` section 11.
- `NodeHost` rewritten on them (5): asynchronous barriers with tagged completions, the fade of every output the code
  writes and its acknowledgement, take-up by graph position, the three bounds (100 ms fade, 5 s barrier, 10 s close),
  the wedged data loop, PipeWire's object changes batched per turn, T1's wait for an older block, the block end of a
  join by global id, a failed node torn down at once.
- The wanted state: `disabled` from `ear.conf` (8) and the node's stop in `NodeHost`; the plan outcomes `restart`,
  `disable`, `enable`, `stored` and `unchanged`, told through `nodeChanged` (D-L4).
- The type's class with creation's `configure`, `start()` and `stop()`, `CreateOf` with 6.10's 3a members and interim
  calls, `NodeOf` with today's `Channel` as its handle, `Configured` (5.9); `white`, `card`, `rtp` and `file` moved (7).
- The interim sink (5.11) and the legacy adapter that keeps `sendspin` on today's SPI (5.10).
- The harness over the real `NodeHost` (11.3), the compile-fail cases for the class.
- The wire of control-plane.md step L1 (9), the main-loop watchdog, `ear.hook-slow`, `context.num-data-loops = 1`.

**Not in 3a:** companions as engine nodes and their readiness deadlines (3b; a card whose PCM never runs still leaves
its node `configuring`, node-framework.md I3); the source; `upstreamFrames` in chunks (0 until 3b, D-L15); loops,
stores, claims, deferred recordings, the node handle (3c); the change version of `configure`, holds, `reconfiguring`,
reshape, `.inPlace()` (3d); `recovering` (8); status, events, actions (5); opening cards off the main loop (later).

**Which 3b and 3d parts are here.** node-framework.md 13.1 assigns to 3a, and this page designs: asynchronous barriers
and their bounds, fades before every planned silence and a fade-in on every drive, take-up by position (published by
the block, read by the interim sink in 3a and by companions from 3b), the main-loop watchdog and the wedged data loop.
It assigns holds, the code swap and the second layout to 3d, and companions' arming against the published position to
3b; this page leaves the seams for them (13) and plans 3d's reshape spike (10.4).

## 3. Journeys

**The operator stops a card for a minute.** `node.stop {"name":"desk"}` reaches the keeper, which writes nothing and
posts a step to the main loop (D-L19). There `NodeHost` sets `desk`'s stop: the node is `running`, so `stop()` is
called (the card type writes none), the block is asked to fade `play`'s mirror `to_card` and every public output to
zero over 5 ms (5.3), and the reply `{"applied":"stop"}` is posted after `node.state` `stopping` (cause `stop`). About
one cycle later the data loop acknowledges the fade; `NodeHost` posts the silence (`drive` with no code); one cycle
later its completion lands, and `desk` is `stopped`. The ALSA device stays open and plays the block's zeros. `node.start`
posts a drive; `start()` runs once the block took the code up, and `desk` is `running` within two cycles and a turn.

**A hand edit disables a sendspin node.** The keeper reads `node sendspin porch { disabled true; port 8928; }`, the plan
names `porch`'s outcome `disable` (D-L4), and `NodeHost` tears `porch` down. `porch` is on the legacy adapter (5.10):
it has no `stop()`, its code is faded and silenced, its handoff closes on its own thread, its run object closes off
the main loop (the worker says goodbye), and its claim on 8928 goes with the run object. Then `porch` is `disabled`,
and its block and wires stay.

**A restart-only change on a recording node.** `node.update` changes `rec`'s `path`. The plan says `restart`; `rec`'s
code stops at a position the block publishes, so its sink took every cycle up to it; the old recording's sink drains
and finishes its WAV on its own thread; the new configuration opens the new path, its consumer begins (writing the
header) on the sink's thread, and the code drives again from a published position with a fade-in.

**ear stops.** `stopAll` posts every node's fade at once and, as each acknowledges, its silence; every block's
destruction is made in the turns' flushes, so the stop costs about one fade, one cycle and one batch of PipeWire's
destroys, not one blocking barrier per node in turn (node-framework.md 15). The test of 11.6 stops 16 nodes and
measures it.

**A type author writes `white`.** A class with a static `ports`, and `configure` that gives its code and answers
`Configured::applied()` (7.1). The harness runs it: `h.add()`, `h.blockReady()`, `h.process()`, `h.stop()`.

## 4. The two machines, and the node they make

Both machines are pure, as `NodeLifecycle` is today (`node_lifecycle.hpp:10-15` [V]): each takes an event and returns
its next state and a list of actions, which `NodeHost` carries out; neither calls a hook, PipeWire or a timer. So every
state against every event is one table-driven test (11.1). node-framework.md 6.1 names them coarsely (the block's made,
ready, lost; the configuration's configuring, configured, driving, changing, going); these tables refine those names,
and every row cites the transition of node-framework.md 6.3 it builds. `changing` is 3d's and has no row here.

### 4.1 The block machine

One per node, for the node's life (I1). States: `waiting` (an older block of the name still exists, T1), `made` (the
factory asked, not yet STREAMING), `ready`, `lost` (final), `gone`.

| State | Event | Next | Actions | node-framework.md |
|---|---|---|---|---|
| — | node added, no older block of its name | `made` | queue `make` in this turn's flush (5.5) | T1 |
| — | node added, an older block of its name exists | `waiting` | `waitingFor` `older-node` after 2 s | T1 |
| `waiting` | the older block destroyed | `made` | queue `make` | T1 |
| `waiting` | removal | `gone` | nothing to destroy | T17 |
| `made` | `PortBlockEvents::ready` | `ready` | tell the aggregate (T5 may follow) | T5's precondition |
| `made`, `ready` | `PortBlockEvents::lost` | `lost` | tell the aggregate: T13 | T13 |
| `made`, `ready`, `lost` | the removal's silence completed, or no code was ever driven since the last silence (D-L7) | `gone` | queue `destroy` in this turn's flush; when it ran, tell every `waiting` block of the name | T17, D-R8 |
| `lost` | any but removal | `lost` | nothing: a lost block is never remade (I1) | T13 |

Destroying a block is PipeWire's blocking call (`pw_filter_destroy`, `port_blocks.cpp:140-148` [V]), so it is always
queued, never made inside an event handler (5.5).

### 4.2 The configuration machine

At most one per node (I7). It carries four facts beside its state: `started` (`start()` was called and `stop()` not
yet), `driven` (a code has been handed to the block since the last silence), `rested` (it reached `running` or
`stopped`: a later `lost(detail)` is the code `lost`, before it `start-failed`, D-L23), and `stopAsked` (a stop arrived
while the drive was in flight). `n` is the barrier number, raised for every request it posts (5.6).

States: `none`; `readying` (instance made, `configure` answered applied, resources readying); `prepared` (every
resource ready, code prepared, not driven); `arming` (a drive posted, its completion awaited); `driving` (taken up,
`start()` returned); `fading` (a fade posted, its acknowledgement awaited, at most 100 ms); `silencing` (a silence
posted, its completion awaited, at most 5 s); `tearing` (I5 steps 1's rest to 5, at most 10 s).

| State | Event | Next | Actions | node-framework.md |
|---|---|---|---|---|
| `none` | configure (aggregate) | `readying`, or `prepared` if nothing is pending | new generation; make the instance; call creation's `configure`; ask the resources (5.8); if none pending: prepare the code | T2, T12, T15, T22 |
| `none` | configure; `configure` answered `failed(d)` or threw | `tearing` | fault `start-failed: d` (or `<hook> threw: <what>`) | T2 then T14 |
| `readying` | every resource ready (outside nodes linked, sink consumers begun) | `prepared` | legacy: run the recipe (5.10); prepare the code | T3 |
| `prepared` | drive (aggregate: wanted running, block `ready`) | `arming` | carry the latency plan; post `drive(code, faded, sinks)`, `n++` | T5, T8 |
| `arming` | `driven(n, P)` | `driving` | unless `stopAsked`: call `start()`, `started = true`; `rested = true` | T5 |
| `arming` | `driven(n, P)` with `stopAsked` | `fading` | no `start()`; post a fade | 6.3 ("stop in starting") |
| `arming` | stop or teardown asked | `arming` | `stopAsked = true` (and the intent recorded) | 6.3 |
| `driving` | stop or teardown asked | `fading`, or `silencing` if no output is faded | if `started`: call `stop()`; post a fade (a 100 ms timer), else a silence | T6, T14, T17, T20 |
| `fading` | `faded(n)` | `silencing` | post a silence (5 s timer) | T6 |
| `fading` | the 100 ms bound | `silencing` | `ear.fade-unacknowledged`; post the silence unfaded | I5a |
| `silencing` | `silenced(n, P)`, intent stop | `prepared` | clear the latency plan; `driven = false`; `rested = true` | T7 |
| `silencing` | `silenced(n, P)`, any other intent | `tearing` | clear the latency plan; `release`; queue the outside nodes' destruction; close the sinks and legacy handoffs; arm the 10 s close timer | T6, T14, T17, T20 then I5 |
| `silencing` | the 5 s bound | `none` (leaked) | `ear.barrier-overdue`; mark the data loop wedged (5.4); leak the configuration | I5a |
| `readying`, `prepared` | teardown asked (restart, disable, removal, fault) | `tearing` | no barrier (never driven); `release` if prepared; as above | I5 ("before ready") |
| `prepared` | stop asked | `prepared` | nothing: it rests | T4 |
| `tearing` | every sink and handoff closed, then the legacy run object closed | `none` | on the main loop: the code, the instance, the recordings, in that order (I5 step 5) | I5 |
| `tearing` | the 10 s bound | `none` (leaked) | `ear.close-overdue`; leak (today's `closeOverdue`, `node_host.cpp:447-464` [V]) | I5a |
| `readying` … `driving` | fault: a resource refused or lost, `lost(d)`, a hook threw | as "teardown asked" | the failure's code by `rested` (D-L23) | T14 |
| `fading`, `silencing`, `tearing` | fault | unchanged | dropped (I6: facts are dropped once step 1 began) | I6 |
| any | the block lost | `silencing` if driven, else `tearing` | no `stop()`, no fade; the silence completes by signal on a block with no data loop (5.3) | T13 |

### 4.3 The aggregate: wanted, intent, phase

The node (`NodeHost::Impl::Node`) holds the two machines and five facts: `disabled` (from its `NodeSpec`, 8),
`stopHeld` (the node's stop, in memory, D-F1), `intent` (what a teardown in flight is for: `stop`, `restart`,
`disable`, `remove` or `fail`), `queued` (the newest committed settings and their config) and `failure` (code and
detail). `wanted` is `disabled` when `disabled`, else `stopped` when `stopHeld`, else `running`.

The phase is derived, first match wins:

| # | Condition | `phase` | `cause` |
|---|---|---|---|
| 1 | block `gone` and configuration `none`, intent `remove` | `removed` (not a phase on the wire: `node.gone`) | — |
| 2 | intent `remove` | `stopping` | `removing` |
| 3 | `failure` set | `failed` (the teardown behind it is invisible, R20) | — |
| 4 | configuration `none`, `disabled` | `disabled` | — |
| 5 | configuration `fading`, `silencing` or `tearing` | `stopping` | by intent: `stop`, `restarting` (with its own kind: `settings` with keys, `action`), `disabling` |
| 6 | configuration `none`, not disabled (waiting for an older node, or between a teardown and the next `configure` in the same turn) | `added` | — |
| 7 | `readying` | `configuring` | after a restart, a retry or an enable: `settings`, `action`, `retry`, `enabling` |
| 8 | `prepared`, `stopHeld` | `stopped` | — |
| 9 | `prepared`, not `stopHeld` | `configured` on entry from `readying` (one notification), then `starting` in the same turn | — |
| 10 | `arming` | `starting` (`waitingFor` `block` while the block is not `ready`) | — |
| 11 | `driving` | `running` | — |

The wire's `state` is the phase's colour: `added`, `configuring` and `configured` are `starting`; every other phase is
its own name (control-plane.md 4.8). Two enums carry them (D-L22): `NodePhase` (`added`, `configuring`, `configured`,
`starting`, `running`, `stopping`, `stopped`, `disabled`, `failed`; 3d adds `reconfiguring`, slice 8 `recovering`) and
`NodeState` (`starting`, `running`, `stopping`, `stopped`, `disabled`, `failed`), each with its status-line name.

**The aggregate's own rules.** They are what 6.3's "operations that arrive mid-transition" says, in three rules:

1. **Converge.** After every event the aggregate compares what is with what is wanted: configuration `none`, not
   disabled, no failure, block not lost, no older node of the name left, intent not `remove` → configure with `queued`;
   `prepared`, wanted running, block `ready` → drive; `driving`, wanted not running → stop (intent `stop` or
   `disable`).
2. **A change restarts when the configuration is settled** (D-L3). A committed change to `queued` restarts at once
   (intent `restart`, cause `settings` with the changed keys) when the configuration is `readying`, `prepared`,
   `arming` or `driving`; when it is `fading`, `silencing` or `tearing` it only replaces `queued`, which the next
   `configure` takes, or, for a stop, which the restart rule takes once the node rests. This is 6.3's "before the node
   first reached running or stopped, any change tears the configuration down" and "in stopping, a change waits for
   stopped" at once.
3. **The newest intent wins, in this order: `remove` over `disable` over `fail` over `restart` over `stop`.** A disable
   during a stop or a restart's teardown goes on to `disabled`; an enable during a disable's teardown turns its intent
   to `restart` with cause `enabling`; a `start` during a stop clears `stopHeld`, so rule 1 drives again once the node
   rested (T7 then T8). A removal turns any intent to `remove`. `failure` is cleared when the node is configured
   again (a retry, a change, `node.restart`: T15) or disabled (T21), never before, so a retry asked while the failed
   configuration still tears down shows `failed` until its `configure` runs.

### 4.4 Every operation in every phase

The replies are control-plane.md 4.2 and 7.6's; this table is how 3a's machines produce them. "Restart" means intent
`restart`: the configuration's teardown (with `stop()`, the fade and the silence only where 4.2 calls for them), then
`configure` from empty with `queued`. A request naming a node being removed reaches the wanted node of that name, if
any, else it is `unknown-node`.

| Phase | Restart-only change | `stop` | `start` | `restart` | `disable` | `enable` | removal | block lost | fault |
|---|---|---|---|---|---|---|---|---|---|
| `added` | replaces `queued` | `stopHeld` set: `stop` | cleared: `start` or `unchanged` | `not-restartable` (`not-started`) | T19 | `unchanged` | T17, no barrier | T13 | — |
| `configuring`, `configured` | restart now: `stopping` (`restarting`), no `stop()`, no barrier | set; it rests at T4: `stop` | cleared | restart (`action`): `restart` | T20, no `stop()` | `unchanged` | T17, no barrier | T13 | T14 (`start-failed` or `engine-refused`) |
| `starting` | restart now; a drive in flight is faded and silenced at its completion | set; at rest, or at the drive's completion faded and silenced, no `start()` or `stop()` | `unchanged` | restart: `restart` | T20, fade and silence only if a drive is in flight | `unchanged` | T17 likewise | T13 | T14, no `stop()` |
| `running` | T6 (`restarting: settings changed: <keys>`), T12 | T6 (`stop`), T7: `stop` | `unchanged` | T6 (`restart invoked`), T12: `restart` | T20 with `stop()` and the fade | `unchanged` | T17 with `stop()`, fade, silence | T13, no `stop()`, no fade | T14 with `stop()` and the fade |
| `stopping` (stop) | replaces `queued`; applied once it rests | `unchanged` | cleared; once it rests, T8: `start` | intent `restart`: `restart` | intent `disable`, stop dropped | `unchanged` | intent `remove` | T13 | dropped (I6) |
| `stopping` (restarting) | replaces `queued` | set; the next configuration rests: `stop` | cleared | `unchanged` | intent `disable` | `unchanged` | intent `remove` | T13, nothing configured again | dropped |
| `stopping` (disabling) | stored for the enable | `node-disabled` | `node-disabled` | `node-disabled` | `unchanged` | intent `restart` (`enabling`), T22 once gone | intent `remove` | T13 | dropped |
| `stopping` (removing) | — (not wanted) | — | — | `stopping` | — | — | — | nothing new | dropped |
| `stopped` | restart: `stopping` (`restarting`), no `stop()`, no barrier; rests `stopped` again | `unchanged` | T8: `start` | restart, rests `stopped`: `restart` | T20, no fade, stop dropped | `unchanged` | T17, no barrier | T13 | T14 (`lost`), no `stop()` (it ran at the stop) |
| `disabled` | `stored` (in 3a, a change of ports is a replace, D-R23) | `node-disabled` | `node-disabled` | `node-disabled` | `unchanged` | T22 | T17, no barrier | T13 | — |
| `failed`, block kept | T15 once the old configuration is gone | set, stays `failed`: `stop` | T15, clears: `retry` | T15: `retry` | T21 | `unchanged` | T17, no barrier | T13: reason replaced, final | — |
| `failed`, block lost | written; nothing runs (I1) | set: `stop` | `not-restartable` (`block-lost`) | `not-restartable` (`block-lost`) | T21 | back to `failed`, nothing configured | T17 | — | — |

Every cell that changes `wanted`, `phase`, `cause` or `reason` sends one `node.state` (control-plane.md 4.8, D-R19),
before the reply when the reply comes in the same turn (its rule 5).

### 4.5 What each transition writes

The `node` records' sentences are node-framework.md 6.2's and their fields control-plane.md 9.1's. `StatusLog` writes
one when the derived phase changes to `added` (unless T19 follows in the same turn), `running`, `stopping`, `stopped`,
`disabled`, `failed` or `removed`, and `configuring` after a restart, a retry or an enable (`starting: <cause>`, W4);
it writes a `still` record for a node 2 s in `added`, `configuring`, `configured` or `starting`, then at most once per
10 s while `waitingFor` changes. In 3a `waitingFor` has four kinds: `older-node`, `block`, `companion` (a card's or a
receiver's `<node>.<suffix>`), `consumer` (a sink's `begin`); `claim`, `store`, `recording` and `loop` are 3c's. The
gateway sends `node.state` on every health change; `since` is stamped by each listener when it is told, in the same
turn (D-L22).

### 4.6 Gaps this page found in node-framework.md, fixed there

- A restart before the node first rested (from `configuring`, `configured`, `starting`) had no drawn edge; it now goes
  through `stopping` (`restarting`), as removal and disable do.
- A restart of a `stopped` node went straight to `configuring` (6.2's diagram, T12's From) while control-plane.md 7.6
  says `node.state` `stopping` first, and the teardown takes up to 10 s; it now goes through `stopping` (`restarting`),
  with no fade or `stop()`, as T20 does for a disable.
- `removed`'s wire cell said `node.removed`, which is the commit's; the end is `node.stopped` and `node.gone`.
- `nodeChanged` was 3d's in 13.2 while 3a needs the outcomes `disable`, `enable` and `stored` on the reply and in
  `NodeHost`; it lands in 3a with 3a's outcomes (D-L4). control-plane.md 10.3's `desired_graph.hpp` row follows.
- Section 15's knobly item: `written_block::insert_statements(index, text)` exists at `KNOBLY_COMMIT` [V], so what
  stays [U] is only the layout knobly gives (8.3).

Each is recorded in node-framework.md's review record (revision 8.1).

## 5. `NodeHost`, rewritten

### 5.1 Its data

`NodeHost::Impl` keeps today's shape (listeners, `nodes` in arrival order, `generations`, `leaked`, `stopRequested`,
`allGone`; `node_host.cpp:495-508` [V]) and adds: `wedged` (the data loop judged wedged, 5.4), `flush` (this turn's
queued PipeWire changes, 5.5), `serials` (a counter naming each `Node` object for completions, 5.6), a `Clock` read
through `Timers::now()` (D-L12), and `NodeOperations` (5.2) implemented on it.

Each `Node` holds: its `name` and `serial`; `block` (`BlockMachine` plus `std::unique_ptr<PortBlock>`, its `Ports`, its
`PortBlockEvents`); `config` (`ConfigurationMachine` plus, when not `none`, a `Configuration`); `disabled`,
`stopHeld`, `intent`, `cause`, `queued` (spec and `std::shared_ptr<const NodeConfig>`), `failure`; its health as last
told; its timers (2 s `still`, the fade's, the barrier's, the close's); `dropped` (frames its sinks and legacy
handoffs refused, summed for `removed dropped=`).

A `Configuration` replaces today's `Run` (`node_host.cpp:229-247` [V]), members in reverse teardown order:
`generation`; `config` (shared, the parse's); `channel` (today's `Channel`, `node_host.cpp:173-226` [V], unchanged: it
is I6's generation and `NodeOf::handle()` in 3a); `recordings` (outlive everything that writes them); `instance`
(`std::unique_ptr<ErasedInstance>`, 5.9); `code` (`std::unique_ptr<AudioCode>`, freed before the instance it may
read); `latency` (the pairs and what `configure` declared); `sinks` (5.11) and `legacyHandoffs`; `outsideRequests`
and `outside` (as today); `faded` (one flag per output port: every public output, and every inside output bank a
sound card joins; never a sink's bank); flags `started`, `driven`, `rested`, `stopAsked`; `barrier` (the number of the
request in flight, 5.6).

### 5.2 One main-loop turn

Everything reaches `NodeHost` on the main loop, one input at a time: a committed change (`nodeAdded`, `nodeChanged`,
`nodeRemoved`, from `DesiredGraph::commit` after its observers, as today), an operation (`start`, `stop`, `restart`,
through `NodeOperations` from the keeper's main-loop side, D-L19), a block's or an outside node's event, a sink's
`begun`, a `Channel` post (`say`, `lost`), a barrier's completion, a timer. For each input, in this order:

1. **Judge.** Feed the event to the machine it concerns; refuse an operation the phase does not take (4.4) with its
   error, changing nothing.
2. **Act synchronously.** Call the hooks the transition names (`configure`, `start()`, `stop()`), each timed (5.12);
   post ear's own barriers (asynchronous invokes, 5.3), each with its deadline timer; queue PipeWire's own object
   changes into the turn's flush (5.5).
3. **Converge** (4.3, rule 1), which may repeat 2 for the same node; never for another node.
4. **Tell.** Derive the health; if any member changed, tell every `NodeHealthListener` (`StatusLog`, the gateway), in
   subscribe order, before returning. So a commit's `node.updated` (told by the gateway as an observer before
   `NodeHost`, node-framework.md 6.6) precedes the `node.state` it causes, and an operation's `node.state` precedes its
   reply, which the keeper's main-loop side posts after `NodeHost` returned.

Nothing in 1 to 4 waits for another thread. The flush runs once at the end of the turn's inputs (5.5).

`NodeOperations` is a new interface in `node_host.hpp` (an interface lives with its consumer's package, AGENTS.md, but
its consumer `MainLoopSide` is in `ear_config`, which sees `ear_core`, so it lives beside `NodeHost`):
`LifecycleReply start(const NodeName&)`, `stop`, `restart`, where `LifecycleReply` is `std::expected<Applied,
ControlRefusal>` with control-plane.md 4.2's `applied` values and 4.5's refusals (`unknown-node`, `node-disabled`,
`not-restartable` with its reason, `stopping`).

### 5.3 Barriers, fades and take-up by position: `PortBlock` in 3a

Today `drive(AudioCode*)` is a blocking invoke (`port_blocks.cpp:159-166`, `341-347` [V]). In 3a `PortBlock` (core's
interface, `node_host.hpp`, through an erratum to its header note) becomes:

```cpp
/// What a drive hands the block: the code (null: silence), which outputs its fade touches, and the interim sinks the
/// block writes after the code (5.11). All of it outlives the next drive's completion or the block.
struct DriveRequest {
	AudioCode* code;
	std::span<const bool> faded;            // one per output (outputs then inside outputs, as process's spans)
	std::span<SinkTap* const> sinks;        // interim (3a): each with its inside output bank's first port and count
};
class PortBlock {
public:
	virtual ~PortBlock() = default;
	virtual void carryLatency(const LatencyPlan& plan);   // as today
	/// Main thread; returns at once. The block takes the request up at the start of a cycle whose position it published
	/// one cycle before (P), fading the new code in over 5 ms; then `done(P)` runs on the main loop, never inside this
	/// call. From P on no process() of the previous code runs. A block that is not STREAMING, or has no data loop, takes
	/// it at once and `done` carries the position it last saw.
	virtual void drive(DriveRequest request, std::move_only_function<void(std::uint64_t taken)> done) = 0;
	/// Main thread; returns at once. From the next cycle every faded output ramps to zero over 5 ms and stays there
	/// (the code still runs); `done()` runs on the main loop once a cycle ended at zero. Not STREAMING: done at once.
	virtual void fadeOut(std::move_only_function<void()> done) = 0;
	/// Any thread: the position from which the block runs what it was last given (for companions, 3b, and tests).
	virtual std::uint64_t takenAt() const noexcept = 0;
};
```

How `FilterPortBlock` (`port_blocks.cpp`) does it:

- **Posting.** `drive` and `fadeOut` store the request in a main-loop-owned slot and post `pw_loop_invoke(dataLoop, ...,
  block = false)` carrying only its number. A block with no data loop (refused, never connected, `port_blocks.cpp:153-
  158` [V]) or not STREAMING (an atomic the main loop sets from `onStateChanged`) has the invoke's function apply it at
  once. The invoke may run inline when the data thread has not yet entered its loop (node-framework.md's "Slice 7 as
  built", denormals) [V]; that is harmless, because the function only sets data-loop state and signals (below), never
  calls `done`.
- **Taking up.** The function puts the request in the data loop's `pending`. The next `onProcess` that sees `pending`
  publishes `takenAt = clock.position + clock.duration` (one atomic word, release) and runs the old code once more;
  the cycle whose `clock.position` equals `takenAt` swaps the code (today's `fresh_` discontinuity stays,
  `port_blocks.cpp:345` [V]), starts the 5 ms fade-in on the faded outputs, binds the interim sinks, and signals
  completion. A cycle that jumped past `takenAt` (a resync) takes it up at once. 9.6's "a start costs one cycle more
  than an invoke would" is this.
- **Fading.** One gain for the block's faded outputs, applied after the code and before metering, so levels show the
  silence: a linear ramp of `rate * 5 / 1000` frames (240 at 48 kHz, inside one 256-frame cycle), either way. Sink
  banks are never faded: the sink is cut by position (5.11). A fade-out's acknowledgement is signalled at the end of
  the first cycle that ended at gain 0.
- **Completing.** The data loop writes the completed request's number into an atomic (`appliedSeq`, `fadedSeq`) and
  calls `pw_loop_signal_event` on an event source the block made on the main loop at construction (as the handoff
  does from `process` today, `handoffs.cpp:54` [V], and as the block already does for refusals, `port_blocks.cpp:106`
  [V]). The main-loop callback reads the atomics and runs every `done` up to them, in order; signals coalesce, the
  numbers do not. Never an invoke towards the main loop (D-R7).
- **Destroying.** The event source is destroyed in the destructor after `pw_filter_destroy`, so a completion for a
  destroyed block runs nothing.

`NodeHost` wraps each `done` with the node's serial, its generation and the barrier number (5.6) before passing it.
The barrier is skipped when the block has had no code since its last silence (D-L7): nothing the realtime thread can
reach would be freed.

### 5.4 Deadlines, and the wedged data loop

Timers are `NodeHost`'s (`Timers`, as the close deadline is today), never the block's (D-L24): the fade's 100 ms, the
silence's 5 s, the teardown's 10 s (I5a), the `still` record's 2 s and 10 s.

- **Fade unacknowledged** (100 ms): `ear.fade-unacknowledged` (control-plane.md 4.6), then the silence is posted as if
  the fade had ended. A late `faded` is stale (5.6) and dropped.
- **Barrier overdue** (5 s): `ear.barrier-overdue` names the node, and `ear.data-loop-wedged` is written once. From
  then on `wedged` is set and no PipeWire call is made on that loop again: the flush does nothing; every configuration
  and block in a teardown, and every one after, is leaked (counted in `leaked()`, renamed from `leakedRuns`); a node
  added afterwards fails `engine-refused: the data loop is wedged` without a block (D-L10); edits still commit and the
  keeper still writes. `stopAll` reaches `allGone` at once for leaked nodes, and `main.cpp` already leaves through
  `std::_Exit` once anything is leaked, after the keeper's last write (`main.cpp:307-315` [V]).
- **Close overdue** (10 s): today's path, kept (`node_host.cpp:447-464` [V]).

The three bounds are tuned against CI's stalls by 11.6's test (node-framework.md 15: a loaded host reports a stall but
never judges the loop wedged).

### 5.5 PipeWire's own calls, batched per turn

Making and destroying a block, making and destroying an outside node (its body and links), and carrying latency to a
block whose ports' params change (`pw_filter_update_params`) are PipeWire object changes; the first four each make a
blocking invoke into the data loop inside PipeWire (node-framework.md 6.3). `NodeHost` never makes them inside an
input's handling: it appends them to `flush` and, on the first append of a turn, posts one `flush` task to the
main-loop `Executor` (D-L8). The task runs after the turn's queued inputs, in one go:

1. every queued destruction (outside nodes, then blocks), each after its node's silence completed;
2. every queued making (blocks, then outside nodes, whose adapters need their block's ports, `node_host.hpp:150` [V]).

Destroys come first so a name added back (T1) gets its block in the same flush as its older block's destruction. Each
call sets `MainLoopActivity` (5.12) to name itself, so a stall inside it is reported with its name. A block's refusal
is still told later through its event (`port_blocks.cpp:202-213` [V]), never inside the flush. `carryLatency` stays
synchronous: it is a params update on the main loop, not an invoke, and it must precede its drive.

### 5.6 Generations and stale completions

Each `configure` from empty takes `++generations` (global, as today, `node_host.cpp:505` [V]). Every completion `NodeHost`
receives is checked against `(serial, generation, barrier)`: the node with that serial must still exist (nodes are
looked up by serial, never by pointer), its configuration's generation must match, and the barrier number must be the
one in flight. Anything else is dropped. `Channel` remains the generation for what the node's own threads say
(detached on every path that ends a configuration), so I6 needs no number compared there. The generation never leaves
`NodeHost` (D-A11): `LogEntry::run` goes (9).

### 5.7 Removal, and a name added back

Removal (T17) is ear's barrier first, then PipeWire's: `stop()` if `started`; the fade; the silence; at its completion
the block's destruction is queued (5.5) and the configuration tears down in parallel; the node is `removed` once the
block is destroyed and the configuration is `none` (gone or leaked), and `nodeGone(name, dropped)` is told. Today the
block is destroyed first, as the barrier (`node_host.cpp:90-97` [V]); that goes with D-R8.

A name added back (a remove and an add in one plan, a rename, a replace) makes a new `Node` whose block machine starts
`waiting` while an older `Node` of the name has a block; when the older block is destroyed, every waiting block of the
name is made in the same flush. Its configuration waits for the older `Node` to be wholly gone (rule 1's "no older node
of the name left"), as today's `olderGoing` (`node_host.cpp:571-579` [V]). So two blocks never share a `node.name`
(I9), and `LiveGraph`'s lookups by name stay unambiguous.

The join links the block end by global id (I9): `OutsideTarget` gains `blockId`, the block's node id, which
`FilterPortBlock` reads when STREAMING (`pw_filter_get_node_id`) and hands with `ready()`; `joinOutsideNode` writes it
in `link.output.node` or `link.input.node` instead of the name (`outside_nodes.cpp:73-76` [V]).

### 5.8 Readiness in 3a

node-framework.md 6.4's readiness steps, as far as 3a has them: steps 1 to 3 (claims, stores and recordings, loops)
do not exist yet (3c; the interim `openRecording` opens at once inside `configure`); step 3' is the outside nodes
(cards, receivers), made in the flush once the block is `ready`; step 4' is every sink consumer's `begin`, asked when
`configure` returned. The configuration is `prepared` when every outside node is ready and every consumer has begun.
A legacy configuration has 3' only (today's handoffs are ready at once, `node_type.hpp:195` [V]).

### 5.9 The erased instance, and `CreateContext`

`NodeConfig` (`node_type.hpp:308-315` [V]), the parse's result, gains one virtual: `std::unique_ptr<ErasedInstance>
make(NodeContext&) const`, which makes the type's instance. `ErasedInstance`, core-internal (`ear/core/lifecycle.hpp`,
beside 6.10's classes), is what `NodeHost` calls:

```cpp
class ErasedInstance {
public:
	virtual ~ErasedInstance() = default;
	virtual Configured configure(CreateContext& cx) = 0;   // creation's configure (or the legacy start, 5.10)
	virtual void ready(Ready& ready) {}                     // legacy only: the recipe, at T3
	virtual void start() {}
	virtual void stop() {}
	/// I5 step 4: legacy only, RunObject::close; the new SPI has nothing to close until 3c's loops.
	virtual void close(std::move_only_function<void()> closed) { closed(); }
};
```

`SpecConfig` (`type_spec.hpp:318-338` [V]) implements `make` with `spec_detail::SpecInstance<Spec, Type>`, which holds
the type's object (constructed with `NodeOf<Spec>&` or with nothing, found by `std::constructible_from`), calls its
`configure(CreateOf<Spec>&, const CheckedOf<Spec>&)` with the config's checked settings, and its `start()` and
`stop()` when it has them. `SpecNodeType`'s static checks (`type_spec.hpp:350-357` [V]) move from `start` to the class:
`ports` static and exact, `configure` exact, `start` and `stop` exactly `void()` when present, one of the two
constructors, and no static `start(StartContext&, ...)` (a type on `makeNodeType` writes `configure`, D-L13). Each is a
compile-fail case (11.2).

`CreateContext` is the erased context (as `StartContext` is today, `node_type.hpp:184-223` [V]); `CreateOf<Spec>`
(6.10) is a thin template over it that turns bank handles into names, checks direction and reach statically, and erases
the code (`NodeCode`, today's `RunCode` renamed, D-L13). `NodeHost` implements `CreateContext` on the node, valid only
inside `configure` (else `std::logic_error`, as today, `node_host.cpp:126-127` [V]). Its 3a calls and what each does:

| Call (6.10) | `NodeHost` does |
|---|---|
| `use(NodeCode)` | stores the code; a second `use`, or none at `configure`'s return, is a `std::logic_error` (6.10's rule table) |
| `soundCard(Bank, SoundCardRequest)` | fills `request.bank` from the handle (a non-empty, different `bank` is a `std::logic_error`; so is a `direction` that disagrees with the bank's: playback on an inside output, capture on an inside input), records it as today's `askOutside` does (`node_host.cpp:261-268` [V]), and marks the bank `faded` when it is an output (D-L16) |
| `rtpReceiver(Bank, RtpReceiverRequest)` | likewise; never faded (an inside input) |
| `openRecording(RecordingRequest)` | today's recording acquire, synchronous, through `RecordingAdapter` (`node_host.cpp:137-141` [V]); the file outlives the code and instance (I5 step 5) |
| `sink(Bank, SinkShape)` | asks the `SinkAdapter` for an interim sink on the inside output bank (5.11), its consumer named by `consumedBy`; exactly one consumer at `configure`'s return, else `std::logic_error` |
| `declareLatency(From, To, Latency)` | records the latency on the pair's output bank in today's per-bank `LatencyPlan::internal` (D-L17), refused twice or for a bank the node lacks, as today (`node_host.cpp:117-123` [V]) |
| `format()`, `node()` | as today |

`NodeContext` is what `NodeOf<Spec>` wraps: the format, the node's name, and `handle()`, which returns the
configuration's `Channel` as `InsidesEvents&` in 3a (6.10's "3a: today's Channel"; 3c makes it `NodeHandle&`). Since the
instance is made per configuration, its `NodeOf` is valid for its life.

`Configured::failed(detail)` fails the node `start-failed: <detail>` (D-A5); a hook that throws is caught, the node
fails `start-failed: <hook> threw: <what>` (or `lost` once rested), and its configuration is torn down.

### 5.10 The legacy adapter

`NodeConfig::make`'s default (in `ear_core`, not virtual-overridden by any legacy config) returns a `LegacyInstance`
over the config's `start(StartContext&)`; `start` itself keeps its signature and gains a default that throws
`std::logic_error` ("a type on the new SPI has no start"), so `SpecConfig` stops defining it (D-L13). `sendspin`'s
`SendspinConfig` (`sendspin_type.cpp:72-117` [V]) and every hand-made `NodeTypeOf` config in the tests run through it
unchanged:

| Machine step | `LegacyInstance` |
|---|---|
| T2, `configure` | calls `config.start(ctx)`, where `ctx` is today's `StartContext`, implemented by `NodeHost` on the same internals as `CreateContext` (outside requests, handoffs, recordings, latency); a failed `StartResult` is `Configured::failed`; else it keeps the code (unprepared, undriven) and the recipe |
| T3, every resource ready | runs the recipe (`Ready&`) and keeps the run object; a failed recipe is a fault (`start-failed`); then the code is prepared, as for any type |
| T5 | the code is driven, after the recipe, never before (today it drove at once, `node_host.cpp:305-306` [V]) |
| `start()`, `stop()` | none: a legacy type has no hooks; a stopped legacy node keeps its run object, connection included (13.1, D-L27) |
| I5 step 3 | its handoffs close on their threads, as today (`node_host.cpp:399-402` [V]) |
| I5 step 4 | `RunObject::close`, off the main loop as today (`node_host.cpp:412-420` [V]) |

`sendspin`'s run builds its threads in `start`, and its recipe only hands them over; it asks no outside node, so its T3
falls in the same turn as T2 and a recipe holding threads is never destroyed unclosed ("Slice 1 as built" [V]). The
adapter keeps that rule: a legacy type whose recipe holds threads must ask for no outside node.

### 5.11 The interim sink

The sink's API lands in 3a (`cx.sink`, `consumedBy` with a consumer given whole, `SinkConsumer` with `begin`, stamped
chunks; node-primitives.md 6.6 is `SinkConsumer`'s home); its realtime side is inside the block until 3b moves it into a
ring companion, with the same API. Its parts:

- **`SinkAdapter`**, a new kind in `ResourceRegistry` (`node_host.hpp:177-196` [V]) beside `HandoffAdapter`, which
  stays for `sendspin` until 3c. `ear_pipewire` implements it as `ThreadSinks` (`handoffs.cpp`), reusing
  `ThreadHandoff`'s `FrameRing`, wake and `pw_thread_loop` (`handoffs.cpp:21-112` [V]) with three additions: a stamp
  ring beside the frame ring; `begin`; and `SinkTap`, the realtime end.
- **`SinkTap`** (core, `node_host.hpp`) is framework-internal: no type sees it. `write(std::span<const std::span<const
  float>> bank, std::uint32_t frames, const ChunkStamp&) noexcept` interleaves the bank's spans into the ring (as
  `RtWriter::write` does, `handoffs.cpp:51-56` [V]) and pushes `{position, startNs, startRawNs, discontinuity,
  accepted frames}` onto the stamp ring; what the ring refuses is dropped and counted (the node's `dropped=`).
- **Arming.** The taps ride in the `DriveRequest` (5.3), so the block binds them in the very cycle it takes the code
  up and unbinds them in the cycle it takes the silence up: the sink takes exactly the cycles the code drove, by
  construction, with no companion to arm. The block writes each tap after the code, from the inside output bank's
  spans (an unlinked inside output writes into `scratch_`, `port_blocks.cpp:410-413` [V], which the tap reads).
- **Begin** (readiness 4'). When `configure` returned, `NodeHost` asks each sink to begin; the sink's thread calls
  `consumer.begin()`, then posts `begun` through the main-loop `Executor`, the same FIFO `Channel` posts through, so a
  `lost(detail)` the consumer said inside `begin` always lands first (D-L14).
- **Chunks.** The sink's thread pairs stamps with frames and calls `consume(Chunk)` (node-framework.md 9.6's struct) one
  chunk per cycle, or what the ring held of it; `upstreamFrames` is 0 until 3b (D-L15).
- **Stop and teardown.** At a stop nothing is closed: the sink's thread drains what the ring holds, and the next drive
  binds the same tap again. At a teardown (I5 step 3) `close` drains, calls `finish` on the sink's thread, then reports
  its drops on the main loop, as `Handoff::close` does today (`handoffs.cpp:44-49`, `86-91` [V]).

### 5.12 Hooks timed, and the main-loop watchdog

Each hook call is timed with `Timers::now()`; past 20 ms it is `ear.hook-slow` with `node`, `hook` and `ms`, through
`NodeDiagnosticListener` (control-plane.md 4.6 is the record's home).

The watchdog (D-L9) is `StallWatch` in `ear_output`, which depends on nothing of EAR's: a thread that reads an atomic
heartbeat every 250 ms and, past 500 ms without a beat, calls `stalled(ms, where)` once, then `resumed(ms)` when it
moves. `main.cpp` arms a 100 ms main-loop timer that beats it and points its callbacks at `StatusLog`, which encodes
`ear.main-loop-stalled` and `ear.main-loop-resumed` straight onto the output queue from the watchdog's thread (its
encoder is safe from many threads, tested today as "one order from many threads"). `where` is
`MainLoopActivity::current()` in `ear_core`: a string literal naming the call (`configure`, `pw_filter_destroy`) and a
fixed 96-byte copy of the node's name, which `NodeHost` writes before each hook and each flush item under a sequence
counter (a seqlock, as sendspin's `Mailbox`, `mailbox.hpp` [V]), so the watchdog's thread reads a consistent pair
without a lock and never follows a pointer into a node.

## 6. File by file

| File | Change | Step (12) |
|---|---|---|
| `src/core/include/ear/core/node_lifecycle.hpp`, `node_lifecycle.cpp` | replaced: `BlockMachine`, `ConfigurationMachine`, the aggregate's `derivePhase`, `NodePhase`, `NodeState`, `NodeCause`, names; erratum in `batch2.md` section 11 | L-1 |
| `src/core/include/ear/core/node_host.hpp` | `NodeHealth` gains `phase`, `wanted`, `cause`, `waitingFor`; `PortBlock` as 5.3; `PortBlockEvents::ready(std::uint32_t blockId)`; `OutsideTarget::blockId`; `SinkAdapter`, `SinkTap`, `Sink`; `ResourceRegistry` takes the sink adapter; `NodeOperations`; `leakedRuns` becomes `leaked`; header note re-cut | L-2, L-3, L-4 |
| `src/core/node_host.cpp` | rewritten (5): the aggregate, the machines' actions, `CreateContext` and `StartContext` on one set of internals, the flush, the timers, the wedge, `NodeOperations` | L-3, L-4 |
| `src/core/include/ear/core/lifecycle.hpp` (new) | 6.10's 3a classes: `NodeOf`, `CreateOf`, `Configured`, `SinkRequest` (3a members), `SinkShape`, `NodeCode`; `ErasedInstance`, `CreateContext`, `NodeContext`; `MainLoopActivity` | L-4 |
| `src/core/include/ear/core/node_type.hpp` | `NodeConfig::make` with the legacy default, `start` with a throwing default; `RunCode` renamed `NodeCode` (an alias kept until 3c for `sendspin`); `Chunk`, `SinkConsumer` (as node-primitives.md 6.6); `FailureReason::Code::lost`; `failureCode` | L-4 |
| `src/core/include/ear/core/type_spec.hpp` | `SpecConfig::make`, `SpecInstance`, the class's static checks (5.9); `StartResult` construction from a spec code kept for `NodeCode` | L-4 |
| `src/core/include/ear/core/settings_table.hpp` | `Row`'s consteval constructor refuses the key `disabled` ("disabled is the framework's key: no type may declare it"), beside "a setting's key is empty" (`settings_table.hpp:104-108` [V]) | L-5 |
| `src/core/include/ear/core/graph_spec.hpp` | `NodeSpec::disabled` (`bool`, default false), compared by `operator==`; erratum in `m1-white-to-file.md` | L-5 |
| `src/core/include/ear/core/desired_graph.hpp`, `desired_graph.cpp` | `NodeChange {outcome, keys}` and `DesiredNodeListener::nodeChanged` with a default calling `nodeUpdated`; the plan's outcome per node (`restart`, `disable`, `enable`, `stored`; a change of ports or type stays a replace, D-R23, D-L5); `Plan::outcomes()` for the reply | L-5 |
| `src/core/include/ear/core/timers.hpp` | `Timers::now()` (D-L12) | L-3 |
| `src/core/include/ear/core/log.hpp` | `LogEntry::run` removed | L-3 |
| `src/core/include/ear/core/graph_view.hpp` | `NodeView` gains `phase`, `wanted`, `cause`, `waitingFor`, `disabled`, `blockLost` | L-6 |
| `src/core/include/ear/testing/type_harness.hpp`, `node_harness.hpp`, `testing/node_harness.cpp` | rewritten over a real `NodeHost` with in-memory fakes (11.3), 7.6's class whole; `NodeHarness` keeps the by-name calls for legacy types | L-4 |
| `src/pipewire/port_blocks.cpp`, `port_blocks.hpp` | 5.3: asynchronous `drive` and `fadeOut`, take-up by position, the fade gain, completions through the block's event source, `takenAt`, the interim sinks' taps, `blockId` at STREAMING | L-2, L-4 |
| `src/pipewire/handoffs.cpp`, `handoffs.hpp` | `ThreadSinks` beside `ThreadHandoffs` (5.11) | L-4 |
| `src/pipewire/outside_nodes.cpp`, `outside_nodes.hpp`, `sound_cards.cpp`, `rtp_receivers.cpp` | the block end of each link by `blockId` (5.7) | L-3 |
| `src/pipewire/engine.conf` | `context.num-data-loops = 1` in `context.properties` (D-R9) | L-0 |
| `src/pipewire/engine.cpp` | its comment on the data loop (`engine.cpp:227` [V]) says it is set, no longer defaulted | L-0 |
| `src/pipewire/main_loop_executor.cpp` | `now()` | L-3 |
| `src/output/` | `StallWatch` (5.12) | L-7 |
| `src/nodes/white`, `card`, `rtp`, `file` | the class (7); `file`'s spec gains the inside bank `writer` | L-4 |
| `src/nodes/sendspin` | nothing (5.10) | — |
| `src/config/graph_file_model.cpp`, `.hpp` | the reserved key read and written (8): `readNode`, `addNode`, `updateNode` leaving `disabled` alone, `disableNode`, `enableNode`, `dropBlock` | L-5 |
| `src/config/keeper.cpp`, `keeper.hpp`, `main_loop_side.cpp`, `.hpp` | `DisableNode` and `EnableNode` as edits; `StartNode`, `StopNode`, `RestartNode` as a new step kind needing no file (D-L19), answered by `NodeOperations` through `MainLoopSide` | L-5, L-6 |
| `src/control/` | commands, results with `applied`, `ControlRefusal` codes, `node.state`'s members, `node.updated`'s `applied` and `disabled`, `node.gone`, the protocol frame; `StatusLog`'s `node` records with `phase` and `cause`, the new sentences, `still`, `LogEntry::run`'s line removed | L-3, L-6 |
| `src/main.cpp` | `NodeOperations` to `MainLoopSide`; the sink adapter; `StallWatch`; `leaked()` | L-3, L-4, L-6, L-7 |
| `web/src/protocol.ts`, `graph.ts`, `components/EarNode.tsx`, `editor.css`, `health.ts` (new), `components/Panels.tsx` | open enums, the new members, `node.gone`, the new states' styles, the phase line and Stop, Start, Restart, Disable and Enable (control-plane.md 8.6 is the home) | L-6 |
| `scripts/rpc-call.mjs`, `scripts/image-smoke.sh` | `--until`; `protocol` and `features` checked | L-6 |
| `AGENTS.md`, `.claude/skills/pipewire-engine`, `control-port` | the new states, records, methods and traps | L-8 |

## 7. The types, before and after

### 7.1 `white`

Before (`white_spec.hpp:35-38` [V]): `static StartResult start(StartContext&, const CheckedOf<spec>&) { return
WhiteCode{}; }`. After:

```cpp
struct White {
	static Members<spec> ports(const CheckedOf<spec>&) { return {noise(members("mono"))}; }
	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
		cx.use(WhiteCode{});
		return Configured::applied();
	}
};
```

`WhiteCode` is unchanged; its fade is the framework's.

### 7.2 `card`

Before (`card_spec.hpp:43-51` [V]): `start` acquires one `SoundCardRequest` per direction with `bank` spelled from the
handle, and returns the pass-through. After:

```cpp
Configured configure(CreateOf<spec>& cx, const Checked& s) {
	if (s.get<playbackRow>() > 0)
		cx.soundCard(toCard, SoundCardRequest{.card = s.get<cardRow>(), .direction = CardDirection::playback});
	if (s.get<captureRow>() > 0)
		cx.soundCard(fromCard, SoundCardRequest{.card = s.get<cardRow>(), .direction = CardDirection::capture});
	cx.use(passThrough({bankPair(play, toCard), bankPair(fromCard, capture)}));
	return Configured::applied();
}
```

`to_card` is faded with the public outputs, so a stop never clicks the card's own speaker (D-R4). `ports` and `check`
are unchanged.

### 7.3 `rtp`

Before (`rtp_spec.hpp:49-60` [V]): `start` acquires the receiver and declares `latencyMs` on `stream`. After:

```cpp
Configured configure(CreateOf<spec>& cx, const Checked& s) {
	cx.rtpReceiver(fromRtp, RtpReceiverRequest{.address = s.get<addressRow>(),
	                                           .port = static_cast<std::uint16_t>(s.get<portRow>()),
	                                           .encoding = s.get<formatRow>(),
	                                           .rate = s.get<rateRow>().value_or(cx.format().rate),
	                                           .latencyMs = s.get<latencyRow>()});
	const std::uint32_t buffered = s.get<latencyRow>() * cx.format().rate / 1000;
	cx.declareLatency(fromRtp, stream, Latency{buffered, buffered});   // on stream, as today (D-L17)
	cx.use(passThrough({bankPair(fromRtp, stream)}));
	return Configured::applied();
}
```

### 7.4 `file`

Before (`file.cpp:47-87` [V]): `start` opens the recording, `sf_open_fd`s it on the main loop, acquires a handoff on the
routable bank `record` with a consumer owned by a `FileRun` run object, and returns `Recorder`, which writes the
handoff from `process`. After: the spec gains `inline constexpr auto writer = bank::insideOutput<"writer">;`, `.banks =
Banks<record, writer>{}`, and `ports` gives `writer(members("left", "right"))`. The class:

```cpp
class File {
public:
	explicit File(NodeOf<spec>& node) : node_(node) {}
	static Members<spec> ports(const Checked&);              // record and writer, left and right
	static std::expected<void, std::string> check(const Checked&);   // unchanged
	Configured configure(CreateOf<spec>& cx, const Checked& s);      // file.cpp
private:
	NodeOf<spec>& node_;
};

// file.cpp
Configured File::configure(CreateOf<spec>& cx, const Checked& s) {
	const std::string path = s.get<pathRow>();
	const auto descriptor = cx.openRecording(RecordingRequest{path});   // 3a interim: synchronous
	if (!descriptor) return Configured::failed("cannot open " + path + ": " + descriptor.error());
	cx.sink(writer, SinkShape{.frames = ringSeconds * cx.format().rate})
		.consumedBy(std::make_unique<WavWriter>(*descriptor, path, cx.format(), node_.handle()));
	cx.use(passThrough({bankPair(record, writer)}));
	return Configured::applied();
}
```

`WavWriter` becomes a `SinkConsumer`: `begin()` calls `sf_open_fd` on the sink's thread (the header is written there,
not on the main loop) and, on failure, `handle.lost("cannot open <path>: <sf_strerror>")`, which fails the node
`start-failed` before it is configured (D-L18); `consume(chunk)` writes `chunk.interleaved`; `finish()` closes.
`Recorder` and `FileRun` go. The descriptor is the framework's and is closed after the instance (I5 step 5).

### 7.5 `sendspin`

Unchanged: its `NodeTypeOf` type, `SendspinConfig::start`, its recipe, its `RunObject`, its `HandoffRequest` on `to`
and its `declareLatency("from", ...)` (`sendspin_type.cpp:81-110` [V]) run through the legacy adapter (5.10). What
changes for it is the framework around it: its code drives only at T5 and fades in and out; `node.stop` silences it and
keeps its run object and connection; a disable, a restart or a failure closes its run object at I5 step 4. While
stopped nothing reads its feed, so the library's writer waits up to 20 ms a write and then drops (`sendspin.md`
section 6). A stop keeps its code prepared, so at Start the code sees a position jump larger than its feed, skips
nothing (`sendspin_code.cpp:13-17` [V]) and plays what the feed holds: at most about 43 ms written before the stop.
3b's source clears its ring at a stop (node-framework.md 6.2); the legacy feed does not, an interim cost until 3c. It
stays in `notYetInHarness`.

## 8. The graph file: `disabled`

### 8.1 Reading

`readNode` (`graph_file_model.cpp:193-230` [V]) takes the reserved key out of the block before the type's settings:

- a setting named `disabled` is read with knobly's `boolean()` (`written.hpp:950` [V]); `true` sets
  `NodeSpec::disabled`, `false` leaves it clear; neither enters `settings`;
- a bare `disabled;`, two values or a block: `ear/bad-directive` "disabled takes true or false: write disabled true;"
  (replacing, for this key alone, `oneValueNoBlock`'s generic text, `graph_file_model.cpp:165-170` [V]);
- a word other than `true` or `false`: knobly's E4006; a quoted or list value: E4002, both from `boolean()`, placed on
  the value (D-F12);
- a second `disabled` line: "setting disabled is given twice", as any repeated setting (`graph_file_model.cpp:209`
  [V]), tracked by a flag since the key is not in `settings`;
- a `node` line with three values, one of them the word `disabled` (`node card porch disabled { ... }`, `node disabled
  card porch`): today's "node takes a type and a name" (`graph_file_model.cpp:196` [V]) plus the hint "to disable a
  node, write disabled true; inside its braces".

So `parse`, `check`, `settingKinds()` and every hook never see the key, and the type schema never lists it under a type.

### 8.2 Writing

| Edit | How (`graph_file_model.cpp`) |
|---|---|
| `node.add` with `"disabled": true` | `addNode` writes `node <t> <n> { disabled true; <settings> }`, the key first (`graph_file_model.cpp:628-641` [V]) |
| `node.update` | `updateNode` skips any rendered setting named `disabled`: today it would remove a key absent from the new settings (`graph_file_model.cpp:598` [V]), which would enable the node. A `disabled` key inside the given settings is refused `invalid-settings` with D-F12's wire text before any edit (D-L20) |
| `node.disable` (new `disableNode`) | the node's own rendered `disabled` setting, if any: refused `shared-definition` when its statement makes more than one rendered directive (`Editing::shared`, D-F11); else `replace_with_text("disabled true;")` on that statement (a `disabled $quiet;` line is replaced and `$quiet`'s `@set` is left alone, R34). With no such setting: `insert_statements(0, "disabled true;")` on the node's block (knobly `written.hpp:516`, spec `spec.md:1071` [V]); with no block, today's `addBlock` (`graph_file_model.cpp:438-452` [V]) makes `{ disabled true; }`. Already disabled: `unchanged` (no candidate) |
| `node.enable` (new `enableNode`) | the node's own `disabled` statement: `shared-definition` as above; else removed; a block left with no statement and no comment is dropped by `dropBlock`, `addBlock`'s inverse: the `{ ... }` replaced by `;`, comments outside it kept (D-L21). No such line, or one that reads false: `unchanged` |

Each makes a candidate as every edit does, so the plan judges it and the keeper writes it behind.

### 8.3 The knobly [U], and its test

What remains unverified is the layout: `insert_statements` puts statements "on lines of their own, indented as the
block's first statement" (`written.hpp:508-509` [V]), so a one-line block (`node card porch { card USB2; playback 2; }`)
may become several lines. The first test of step L-5, before the reader changes, pins what knobly gives for: a
multi-line block, a one-line block, a block whose first statement is a `@use`, a block with a leading comment, and a
node with no block; and that comments survive each. If knobly's result for a one-line block is not acceptable to read
(the test shows it), the designer appends the line last in the block instead, as node-framework.md 15 already allows,
and records it there; no owner question follows, since the meaning is the same.

### 8.4 The plan

`NodeSpec::disabled` is part of the desired state, so `DesiredGraph::plan` sees a changed `disabled` as a change
(`desired_graph.cpp:158-166` [V] compares whole specs). It classifies each changed node (D-L4): type or ports changed →
a replace (as today; on a disabled node, D-R23: told as `node.removed` then `node.added` with `disabled`, the reply
`applied: "stored"`; on an enabled node D-L5: told the same, the reply `applied: "restart"`); `disabled` false to true
→ `disable`; true to false → `enable`; true to true → `stored`; else `restart` (3a has no in-place rows). `commit` tells
`nodeChanged(spec, config, {outcome, keys})`; its default calls `nodeUpdated`, so `StatusLog` and every test listener
keep working; `NodeHost` and the gateway override it.

## 9. The wire in 3a

control-plane.md step L1 is 3a's wire, whole; this page builds it in steps L-3 (records) and L-6 (methods), and adds no
member. What changes where:

- **`ear_control`.** `Command` gains `StartNode`, `StopNode`, `RestartNode`, `DisableNode`, `EnableNode`; `AddNode`
  gains `disabled`; `CommandResult` gains `Applied` (control-plane.md 4.2's values) and the refusals `node-disabled`
  and `not-restartable` (4.5). `node.state` carries `phase`, `since`, `wanted`, `cause`, `waitingFor` (4.8; `keys` and
  `next` stay absent until 3d); snapshots and `node.added` the same members plus `disabled` and `blockLost`;
  `node.updated` gains `applied` and `disabled` (4.3); after a removal ear sends `node.stopped` then `node.gone`, the
  first deprecated (4.8). `graph.get` gains `protocol: 1` and `features: ["lifecycle"]` (4.1). The `node` record gains
  `phase` and `cause` and the sentences of 9.1; `ear.fade-unacknowledged`, `ear.barrier-overdue`,
  `ear.data-loop-wedged`, `ear.main-loop-stalled`, `ear.main-loop-resumed`, `ear.hook-slow` are `diagnostic` records
  (4.6); `node.start`, `node.stop` and `node.restart` write an `action` record (9); `LogEntry::run`'s line goes
  (`status_log.cpp:197` [V]).
- **Fixtures** (`test/protocol/`): new `lifecycle.json` (control-plane.md 4.7's list, the phases 3a has: everything
  but `reconfiguring`, `recovering`, `keys` and `next`), `unknown-values.json`; changed `output-records.json` (one case
  per 9.1 row 3a has, per `cause.kind`, the six diagnostics, the `action` records; the `"run": 2` member removed),
  `errors.json` (the two new codes), `snapshot.json` and `edits.json` (regenerated: `protocol`, `features`, the health
  members, `disabled`, `applied`).
- **`web/`**: control-plane.md 8.6 is the home; 3a's minimum is `protocol.ts`'s open enums and new members
  (`protocol.ts:13`, `15` [V]), `graph.ts` folding `node.gone` as it ignores `node.stopped` (`graph.ts:77` [V]) and
  `node.state`'s members, `health.ts` (each phase's line, style and enabled actions), styles for `state-stopped`,
  `state-disabled` (`components/EarNode.tsx:28` builds the class [V]), and the five buttons in the node panel.

## 10. The spikes 3a needs first

Each is a test in `ear_pipewire_tests` (kept afterwards as a regression), on the real in-process engine, merged as step
L-0 before any code depends on it.

### 10.1 S1: asynchronous `pw_loop_invoke` from the main loop

Method: a block whose code counts cycles and flags "inside process"; the main loop posts 10,000 invokes with `block =
false`, in bursts of 1,000 and singly, each carrying a number; each invoked function records its thread, whether
"inside process" was set, and its number, then sets an atomic and signals a main-loop event made beforehand. A malloc
hook on the data thread, as `handoff_test.cpp`'s "write allocates nothing" [V].

Pass: every call returns a non-negative result before its function ran (once the data thread entered its loop); every
function runs on the data thread, never while any block's `process` runs, in posting order; every completion reaches
the main loop through the event, none lost though signals coalesce; nothing allocates on the data thread; a burst of
1,000 costs the main loop under 5 ms in all. Fail (a refused or blocking post when the queue fills, allocation on the
data thread, reordering): ear's barriers stay blocking (today's `drive`), but batched: one blocking invoke per turn
applies every block's request of that turn, so the main loop waits on the data loop once per turn; the 5 s bound
becomes the watchdog's (a barrier that never returns is a main-loop stall, reported, and ear's stop cannot leave) (D-L26).

### 10.2 S2: take-up by position, under an invoke storm

Method: block B (a ramp code), an upstream probe U and a downstream probe D (test-only `pw_filter`s in `ear.graph`,
always processing), linked U → B → D. B implements 5.3's take-up and publishes `takenAt`. A second thread posts no-op
invokes to the data loop as fast as it can for the whole test. The main loop drives and silences B alternately, 1,000
times, at random intervals of 1 to 30 cycles. Each of U, B and D records, per cycle, `clock.position`, an atomic
sequence number taken on entry, and, in D, the samples received and B's `takenAt`.

Pass: the three record identical position sequences (a block and its peers see one `clock.position` per cycle); in
every cycle U's entry precedes B's and B's precedes D's; D receives the ramp from exactly its first sample at each
take-up to exactly its last before each silence's take-up, with no gap and no trailing zero. Fail: 3a is unaffected
(its sink is inside the block and binds with the code, 5.11); 3b keeps every ring companion's realtime side inside the
block (the sink as 3a builds it, the source as today's feed moved into framework code), and 9.6's arming by position is
dropped (D-L25). Recorded in node-framework.md 15.

### 10.3 S3: one data loop

Method: `context.num-data-loops = 1` in `engine.conf`; make blocks, a test card (`clocklessCardProperties`) and an RTP
receiver; read the context's properties and each node's `node.loop.name` from its info. Pass: the property reads 1 and
every node names the one loop. Fail: name the loop explicitly on every adapter's and module's properties, and keep
the test.

### 10.4 S4 (slice 3d, planned here): ports on a connected filter

node-framework.md 6.7 states what it checks; the plan: a test type with two input banks and an inside bank, a running
ramp through a second block beside it, then `pw_filter_add_port` in the middle of the input list and
`pw_filter_remove_port` of a linked port, 500 times, run under ThreadSanitizer (a build option of the dev image's GCC,
`-fsanitize=thread`, for `ear_pipewire_tests` alone). Pass: the beside ramp never loses a sample; new ports appear in
the registry and link; a removed port's links go; TSan reports no race on `filter.c`'s port list; a port added while
links exist carries audio. Fail: a reshape stays a replace (D-B5).

## 11. Tests

Names are the test executables of AGENTS.md; today's counts are `TEST_CASE`s [V]. 13.2 of node-framework.md says each
built group's fate; this section names the cases.

### 11.1 `ear_core_tests`

- `node_lifecycle_test.cpp` (7, rewritten): "Every event in every block state moves the block machine as its table
  says" (4.1, one table row per case row); "Every event in every configuration state ... as its table says" (4.2);
  "The phase, state, wanted and cause derived for every combination of the machines and facts" (4.3); "Every
  operation in every phase answers and moves as section 4.4 says"; "The newest intent wins: remove over disable over
  fail over restart over stop"; "A change restarts a settled configuration and waits for an unsettled one"; "Phases,
  states, causes and failure codes have their names".
- `node_host_test.cpp` (47): as node-framework.md 13.2 says, with these new cases: "A drive's completion arrives later
  than the call, and start() runs only then"; "A stale completion (an older generation, an older barrier, a removed
  node) changes nothing"; "Every output the code writes is faded before a silence, and a sink's bank never"; "A fade
  unacknowledged for 100 ms is silenced unfaded, with ear.fade-unacknowledged"; "A barrier overdue for 5 s marks the
  data loop wedged: every later teardown is leaked and no PipeWire call is made"; "A node added after the wedge fails
  engine-refused without a block"; "PipeWire's object changes of one turn run together, destroys first"; "A name added
  back gets its block only once the older block is destroyed, in the same flush"; "Removal silences before it
  destroys the block"; "stop() runs exactly once after every start(), for a stop, a disable, a restart, a removal and
  a failure, and never after the block's loss"; "A stop keeps the configuration: no hook, no resource released, and
  start drives it again"; "A node configured while stopped never calls start()"; "stop in starting with a drive in
  flight ends stopped without start() or stop()"; "start during a stop starts again once stopped"; "start on a failed
  node retries and clears the stop; stop on a failed node changes only wanted"; "disable tears the whole configuration
  down, keeps the block and drops the stop; enable configures afresh"; "A node added disabled is never configured";
  "start, stop and restart on a disabled node are refused node-disabled"; "node.restart in every phase replies as
  control-plane.md 7.6's table"; "A failed node is torn down at once and keeps its block"; "lost(detail) is start-failed
  before the node rested and lost after"; "A hook that throws fails the node and the host goes on"; "A hook past 20 ms
  is ear.hook-slow"; "A sink with no consumer or two at configure's return is a type bug"; "A sink's consumer begins
  before the node is configured, and a lost said in begin fails it start-failed"; "A legacy type runs under the new
  machines: start at T2, recipe at T3, driven at T5, stopped keeping its run object, disabled and removed with its close
  at step 4"; "A health notification for every change of any member, in order with the commit's events"; "A still record
  after 2 s, then at most every 10 s while waitingFor changes"; "Stopping 16 nodes posts every fade at once". The two
  order tests, "A node acquires resources and its events only inside start" and the two loss cases are rewritten as
  13.2 says; the handoff cases move to sinks and stay for the legacy adapter.
- `desired_graph_test.cpp` (17): "disabled is part of a node's spec: false to true is disable, true to false enable,
  true to true stored"; "A change of ports on a disabled node is a replace with the outcome stored"; "nodeChanged is told
  with its outcome and keys, and its default calls nodeUpdated".
- `type_spec_test.cpp` (8): the shape cases move to the class: "The class's constructor takes NodeOf or nothing";
  "configure, start and stop are found by name and called"; "A spec code given with use is bound to the node's
  ports".
- `settings_table_test.cpp`: unchanged.

### 11.2 Compile-fail cases

`settings_table_bad.cpp`: `key-reserved-disabled` ("disabled is the framework's key"). `type_spec_bad.cpp`:
`configure-wrong-signature`, `configure-not-a-member`, `start-wrong-signature`, `stop-wrong-signature`,
`constructor-neither`, `legacy-start-on-spec-type`, `create-has-no-hold`, `create-has-no-when-ready`,
`create-has-no-unclaim`, `configured-has-no-restart`, `sound-card-on-public-bank`, `rtp-receiver-on-output-bank`,
`sink-on-input-bank`, `declare-latency-bank-the-spec-lacks`, each with the reason it is refused, matched to GCC 14's
wording as today's cases are. node-framework.md 13.2's later cases (an action named for the lifecycle, loops, a
`Lifecycle` deadline, a hold of an input) wait for their slices.

### 11.3 The harness, and `ear_node_*_tests`

`TypeHarness<spec, Type>` and `NodeHarness` run one real `NodeHost` over in-memory fakes (D-L11): a manual executor
for the main loop (pumped by each harness call), a fake clock and `Timers`, a fake `PortBlockFactory` whose block runs
the driven code over the harness's buffers (outputs NaN-filled first, as today), applies the fade, writes the interim
sinks' taps and completes requests by position (at once with `autoComplete`, else on `acknowledgeFade()` and
`completeBarrier()`), fake card and receiver adapters by bank, the test's `RecordingAdapter` (`Options::recordings`),
and a fake sink adapter that calls `begin` at readiness and hands each chunk straight to its consumer on the test's
thread. So the harness and `NodeHost` cannot disagree; slice 2's shared `run_requests.hpp` (`node_host.cpp:21` [V])
becomes unnecessary and goes.

The four types' tests are rewritten to the harness's operations with the same assertions, plus: `white`: "Stop
silences noise.mono after a 5 ms fade and start brings it back with a fade-in"; `card`: "to_card is faded with the
capture outputs at a stop", "A card lost while running fails the node engine-refused and tears it down"; `rtp`:
"Disable releases the receiver; enable asks for a new one"; `file`: "Every cycle the code drove reaches the sink,
none after the stop's position and none before the start's"; "A path it cannot open is Configured::failed and asks for
no sink"; "A WAV libsndfile cannot open fails the node start-failed from begin"; "A restart finishes the old WAV and
opens the new one". `test/nodes/harness_test.cpp` walks every type but `notYetInHarness` through add, ready, start,
stop, start, restart, disable, enable and remove, checking every output sample of every cycle and `stop()`'s pairing.

### 11.4 `ear_config_tests`

node-framework.md 13.2's list, whole, plus: "node.update leaves a disabled line alone"; "node.update with a disabled key
in its settings is refused invalid-settings naming node.disable"; "disable's insert into a one-line block, a block that
starts with @use or a comment, and a node with none keeps every comment" (8.3's pin); "enable drops a block it empties
and keeps a comment outside it"; "start, stop and restart reach the main-loop side in order with edits, write nothing,
make no version, and work while the file has errors"; "A hand edit that keeps a node keeps its stop" (through a fake
`NodeOperations`).

### 11.5 `ear_control_tests`

`lifecycle.json` against the encoders and the gateway; `output-records.json`'s new cases field for field and in order;
the five commands decoded and their replies encoded; `node.gone` after `node.stopped`; `unknown-values.json`;
`errors.json`'s new codes; `StatusLog`'s `still` pacing; the stall records written from another thread in order with
the main thread's.

### 11.6 `ear_pipewire_tests` and `ear_output_tests`

S1 to S3 as regression tests; `port_block_test.cpp` (13): "drive returns at once and its completion arrives on the main
loop" (today's "drive returns only when the old code can no longer run" is rewritten: "from the published position no
process of the old code runs"), "drive on a block whose engine node is gone completes by signal, never inline", "A
block not streaming takes a change at once", "A fade-out reaches exactly 0 over 5 ms and is acknowledged in the cycle it
did", "Every drive fades in over 5 ms", "The published position is where the new code's first sample is". `sound_card_test.cpp` (6):
"to_card's last 5 ms before a silence decay to zero, at a stop, a restart, a disable and a removal". `engine_test.cpp`
(7): "Every block and companion names the one data loop" (S3), "Stopping 16 nodes costs about one fade, one cycle and
one batch of destroys" (measured and printed; bounded at 2 s). `sink_test.cpp` (new, from `handoff_test.cpp`'s five,
which stay for the legacy handoff): order, drops counted, close drains then finishes, no allocation on the tap, begin
before the first consume, and "A ramp through a block's sink starts at the first driven sample and ends at the last,
across a stop and a start, under an invoke storm". `ear_output_tests`: "StallWatch reports a heartbeat older than its
bound once, then its resumption".

### 11.7 `ear_system_tests`, and the editor

node-framework.md 13.2's and control-plane.md L1's system tests, whole; the six removal lines wait for `node <n>
removed dropped=0` (`graph_file_test.cpp:103`, `rtp_test.cpp:116`, `sendspin_connections_test.cpp:181`,
`sendspin_floods_test.cpp:180`, `sendspin_pairing_test.cpp:66`, `sendspin_streams_test.cpp:75` [V]; and
`sendspin_connections_test.cpp:334`'s search for `node ma stopped` becomes `node ma removed`), and
`control_port_test.cpp:183-193`'s `node.stopped` waits also see `node.gone` after it. Vitest: `lifecycle.json` through
the reducer; `health.ts` for every phase.

## 12. The cut: steps that each merge green

| Step | What | Risk |
|---|---|---|
| L-0 | S1, S2, S3 (10.1 to 10.3) as tests; `context.num-data-loops = 1` | S1 or S2 fails: the fallbacks are designed (D-L25, D-L26); S1's changes 5.3's posting and 5.4's bound before L-2 starts |
| L-1 | The pure machines and the aggregate's derivation in `ear_core`, tested by table; `NodeLifecycle` still used by `NodeHost` | Low: no wiring; a table found wrong against node-framework.md 6.3 is fixed there first |
| L-2 | `PortBlock`'s asynchronous `drive` and `fadeOut`, take-up by position, completions, `takenAt`, `blockId`, in `FilterPortBlock`; today's blocking `drive(AudioCode*)` kept beside them, used by the old `NodeHost` | The position and fade logic on the data loop; caught by S2's harness reused in `port_block_test.cpp` |
| L-3 | `NodeHost` on the machines, every type still on the old SPI through the legacy adapter: the aggregate, asynchronous barriers, fades, the three bounds, the wedge, the flush, T1's wait, the join by `blockId`, a failed node torn down at once, `NodeOperations` (not yet on the wire), `NodeHealth`'s members, `StatusLog`'s new records (`removed dropped=`, `stopping: <cause>`, `stopped`, `disabled`, `still`), `LogEntry::run` removed, `Timers::now()`; the blocking `drive` removed; system tests' removal lines | The largest step and the one that changes every node's teardown; a missed completion hangs a node until its bound. Mitigated: L-1's tables, the 47 rewritten `node_host_test` cases, the system tests unchanged but for their lines |
| L-4 | The new SPI: `lifecycle.hpp`, `CreateContext`, `SpecInstance`, the interim sink and `ThreadSinks`, the harness over `NodeHost`; `white`, `card`, `rtp`, `file` moved; the compile-fail cases; `run_requests.hpp` removed | `file`'s recording path is the first audio path through new code: its system test (exact samples) is the gate |
| L-5 | `disabled`: the `Row` refusal, `NodeSpec::disabled`, the reader and writer (8.3's pin first), the plan's outcomes, `nodeChanged`; `NodeHost` honours `disabled` (a hand edit disables and enables) | knobly's layout (8.3); the plan's outcome reaching listeners in the commit's order |
| L-6 | The wire (9): commands, the keeper's no-file step, `MainLoopSide` to `NodeOperations`, the gateway's members, the protocol frame, `node.gone`, the fixtures, the batch2.md erratum, `rpc-call.mjs --until`, `image-smoke.sh`, the editor's minimum | A frozen header and the web's frozen `protocol.ts` change through their errata; the reducer must take unknown values (`unknown-values.json`) |
| L-7 | `StallWatch` and `MainLoopActivity`, wired in `main.cpp` | A false stall report on CI's loaded host; the bound is tuned by 11.6's measurement |
| L-8 | 3a's remaining system tests (stop, disable across a restart of ear, `$quiet`, restart of a card on the test card, a held UDP port's `still` record); AGENTS.md, the two skills; node-framework.md's "Slice 3a as built" | Flaky timing under load; every test is event-gated (AGENTS.md) |

## 13. What stays interim, and how it goes

| Interim | Until | How it goes, exactly |
|---|---|---|
| The legacy adapter: `LegacyInstance`, `NodeConfig::start` and its throwing default, `NodeConfig::make`'s default, `StartContext`, `StartResult`, `RunRecipe`, `Ready`, `RunObject`, `HandoffRequest`, `RtWriter`, `HandoffConsumer`, `HandoffAdapter`, `ThreadHandoffs`, `ResourceRegistry`'s handoff slot, the `RunCode` alias, `NodeTypeOf`'s config path | 3c, when `sendspin` moves onto the class in one step | each deleted; `NodeConfig::make` becomes pure; the legacy rows of `node_host_test.cpp` and `sendspin`'s `notYetInHarness` entry go; `handoff_test.cpp` goes, its cases already in `sink_test.cpp` |
| The interim sink's realtime side inside the block (`DriveRequest::sinks`, `SinkTap`, the block's writes after the code) | 3b | `ThreadSinks`' ring moves behind a ring companion's `pw_filter` on `joinOutsideNode`; `DriveRequest` loses `sinks`; the companion compares its cycle's position with the block's `takenAt` (S2); `cx.sink`, `consumedBy`, `SinkConsumer`, the harness's sink fake and `file` do not change |
| `cx.openRecording`, synchronous | 3c | replaced by `cx.recording()`, claimed and opened on the I/O executor; `file`'s `configure` changes that call, and its failure becomes a readiness failure instead of `Configured::failed` |
| `cx.soundCard(Bank, SoundCardRequest)`, `cx.rtpReceiver(Bank, RtpReceiverRequest)` | 3b | the shapes (`SoundCardShape`, `RtpReceiverShape`); `card` and `rtp` change those calls; readiness deadlines added |
| `rtp`'s `declareLatency` of `latencyMs` | 3b | the receiver companion declares it; `rtp`'s line goes |
| `NodeOf::handle()` as `InsidesEvents&` over `Channel` | 3c | `NodeHandle&` (node-primitives.md 5); `file`'s `WavWriter` takes the new type |
| `Chunk::upstreamFrames` always 0 | 3b | sampled from the block's carried latency |
| Replace for a change of ports (D-R23, D-L5) | 3d | `PortBlock::reshape`; `node.updated` with `ports` (L3) |

## 14. Decided by the designer

Each is technical or internal (node-framework.md R24's practice); the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| D-L1 | Two pure machines with refined states (4.1, 4.2) and the node as the aggregate holding five facts; the machines return actions and `NodeHost` carries them out | Every state against every event is one table test; no hook or PipeWire call hides inside a transition |
| D-L2 | Every restart shows `stopping` (`restarting`), from `configuring`, `configured`, `starting` and `stopped` too (4.6) | The teardown can take seconds; an honest phase, and one sequence a client learns; control-plane.md 7.6 already said so |
| D-L3 | A change restarts a settled configuration at once and is queued behind an unsettled one; the next `configure` takes the newest | One rule gives every mid-transition change of 6.3 |
| D-L4 | `nodeChanged` and the plan's outcome land in 3a with `restart`, `disable`, `enable`, `stored`; 3d adds `retune`, `reconfigure`, `reshape` | 3a's reply must carry `applied` and `NodeHost` must tell a disable from a restart; one mechanism, not a 3a comparison replaced in 3d |
| D-L5 | Until 3d, a change of ports on an enabled node is told `node.removed` then `node.added`, the reply `applied: "restart"` | As D-R23 for a disabled node: the node does restart, and L3 makes it `reshape` |
| D-L6 | `PortBlock` in 3a is 5.3's: asynchronous `drive` and `fadeOut`, take-up by position, completions through the block's own event source and an atomic, a block not STREAMING taking a change at once | Meets I4 and D-R7 with one code path; a lost or idle block can never be judged a wedged loop |
| D-L7 | The barrier is skipped when the block had no code since its last silence | Nothing would be made unreachable; a disabled or failed node's removal costs no data-loop round |
| D-L8 | PipeWire's object changes are queued and run in one flush per turn, posted to the main-loop executor on the first append, destroys before makes | One wait on the data loop per turn (node-framework.md 6.3); a name added back gets its block in the same flush |
| D-L9 | The watchdog is `StallWatch` in `ear_output`, beaten by a main-loop timer, its "where" from `MainLoopActivity` (a literal and a copy of the node's name under a seqlock) | `ear_output` already owns threads and depends on nothing of EAR's; the watchdog reads without a lock and never touches a node |
| D-L10 | After the wedge, a new node fails `engine-refused: the data loop is wedged` without a block | Making a block calls into the wedged loop and would hang the main loop for good |
| D-L11 | The harnesses run the real `NodeHost` over in-memory fakes | The harness and the host cannot disagree on any rule; slice 2 already shared what it could |
| D-L12 | `Timers` gains `now()` | Hook timing and `waitingFor` need a clock tests can turn; core reads no clock of its own |
| D-L13 | The legacy path is `NodeConfig::make`'s default; `start` gets a throwing default; `RunCode` is renamed `NodeCode` with an alias until 3c; a spec type with a static `start` does not compile | One adapter, for one type, deleted whole in 3c; a half-migrated type is a compile error |
| D-L14 | The interim sink is `ThreadHandoff`'s ring, wake and thread behind a new `SinkAdapter`; its taps ride in the drive request; `begun` is posted through the main-loop executor | The sink takes exactly the driven cycles by construction; a `lost` from `begin` always lands before `begun` |
| D-L15 | `Chunk::upstreamFrames` exists in 3a and is 0 | The consumer's code does not change in 3b |
| D-L16 | `soundCard` fills `request.bank` from the handle and checks the direction against the bank; a playback card's bank is faded | One source of the bank; a card's speaker never clicks (D-R4) |
| D-L17 | `declareLatency(From, To, L)` puts L on the pair's output bank, in today's per-bank plan | Exactly today's `rtp` behaviour; the per-pair carrying a delay line would want waits for a type that has one |
| D-L18 | `file` opens its WAV in its consumer's `begin` on the sink's thread; only the descriptor is opened in `configure` | No libsndfile I/O on the main loop (I11); a failure is still `start-failed` before the node is configured |
| D-L19 | `node.start`, `node.stop` and `node.restart` are a new keeper step that needs no file, answered by `NodeOperations` through `MainLoopSide` | `node.invoke`, the precedent node-framework.md names, does not exist yet; ordering with edits and working while the file has errors both follow |
| D-L20 | `node.update` never touches a `disabled` line; a `disabled` key in given settings is refused before any edit | Today's update would remove the line, silently enabling the node |
| D-L21 | Enable drops an emptied block by a textual rewrite, `addBlock`'s inverse | `node white noise { disabled true; }` must come back as `node white noise;` (control-plane.md L1's test) |
| D-L22 | `NodePhase` and `NodeState` are two enums; `NodeHealth` gains `phase`, `wanted`, `cause`, `waitingFor`; listeners stamp `since` | The colour and the exact state cannot drift; core reads no wall clock |
| D-L23 | A configuration's `rested` flag decides `lost` against `start-failed` | node-framework.md 6.2's failure codes by whether the node had reached `running` or `stopped` |
| D-L24 | The fade's, the barrier's and the close's bounds are `NodeHost`'s timers | The block stays mechanism; tests turn one fake clock |
| D-L25 | If S2 fails, ring companions keep their realtime side inside the block (3b) | 3a is unaffected, and the API does not change |
| D-L26 | If S1 fails, ear's barriers stay blocking, batched into one invoke per turn, bounded by the watchdog | Correct, and the main loop waits once per turn rather than once per node |
| D-L27 | A stopped legacy (`sendspin`) node keeps its run object and connection; its code is silenced | node-framework.md 13.1; a legacy type has no `stop()` to say goodbye with |
| D-L28 | `waitingFor` in 3a has `older-node`, `block`, `companion` and `consumer` | The waits 3a has; 3c adds its own |

## 15. Owner gates

None. Every choice here is internal or follows an owner's answer already given (R25 to R34); the wire-visible ones
(D-L2, D-L5) follow control-plane.md 7.6 and D-R23.

## 16. Open items

- [U] S1, S2, S3 (10): asynchronous invokes, one position per cycle under an invoke storm, one data loop. Step L-0.
- [U] knobly's layout of `insert_statements` into a one-line block (8.3). Step L-5's first test.
- [U] `pw_filter_get_node_id` returning the block's global id once STREAMING, in PipeWire 1.4.2 (5.7). Step L-2's test
  links an outside node by it.
- [U] The three bounds against CI's stalls (5.4; node-framework.md 15). Step L-3 and 11.6's 16-node test.
- [U] That `start` after `stop` drives again within two cycles and one turn, with the sink rebound in the same request
  (node-framework.md 15). Step L-4's `sink_test.cpp` ramp.
- The fade (5 ms), the hook bound (20 ms) and the watchdog's (500 ms) are first choices, as node-framework.md 15 says.
- A legacy `sendspin` node started again plays up to about 43 ms its feed held from before the stop (7.5), since the
  legacy feed is not cleared at a stop; accepted until 3c moves it onto a source, which is.
- S4, slice 3d's reshape spike (10.4), is planned here and runs with 3d.

## 17. Review record

### Revision 1 (2026-11-08, not reviewed)

Written to node-framework.md revision 8 after reading every built file it names (`src/core`, `src/pipewire`,
`src/nodes`, `src/config`, `src/control`, `web/src`, the tests) and knobly's written layer at `KNOBLY_COMMIT`. It found
five gaps in node-framework.md and fixed them there (4.6; node-framework.md's revision 8.1 record), with one row of
control-plane.md 10.3 following. No owner gate. A review is owed before step L-1, together with node-framework.md
revision 8's; it should judge at least: 4.4's table against node-framework.md 6.3 cell by cell, 5.3's take-up against
I4 when a block stops processing mid-request, the flush's ordering against a name added back in the same turn, the
legacy adapter's T3 for a legacy type with outside nodes, and whether L-3 is small enough to review.
