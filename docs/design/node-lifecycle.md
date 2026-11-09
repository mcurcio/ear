# The node lifecycle, built: slice 3a's module design

Status: proposal, revision 2.9, 2026-11-09 (2.2: gaps found building steps L-1 and L-2; 2.3: building L-5's graph
file and desired graph; 2.4: S2 on CI; 2.5: building L-3a; 2.6: building L-3b; 2.7: building L-3c; 2.8: building L-7;
2.9: building L-3d; section 17). Revision 1 went to one
independent review; revision 2 answers it (section 17,
round 1: one blocker, nine major, fourteen minor and five nits, each checked in the code first). The module design
`docs/design/node-framework.md` (revision 8.2) asks for before slice 3a's code (its D-D2 and 13.1): the two pure
machines as tables, `NodeHost`'s rewrite, the asynchronous barriers and their bounds, the published position (3a
applies each change in its invoke; 3b's companions defer take-up to it), the interim sink and the legacy adapter for
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

- The block machine, the configuration machine and the aggregate (4), replacing `NodeLifecycle`
  (`node_lifecycle.hpp:8-41` [V]) through an erratum to `batch2.md` section 11.
- `NodeHost` rewritten on them (5): asynchronous barriers with tagged completions, the fade of every output the code
  writes and its acknowledgement, each change applied inside its invoke with its position published (D-L29), the three
  bounds (100 ms fade, 5 s barrier, 10 s close), the wedged data loop judged only on the loop's own sign of life,
  PipeWire's object changes batched per turn, T1's wait for an older block, the block end of a join by global id, a
  failed node torn down at once.
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
and their bounds, fades before every planned silence and a fade-in on every drive, the published position (`takenAt`:
in 3a the block applies each change inside its invoke, between two cycles, and publishes the position it takes effect
from; 3b's companions arm against it and need take-up deferred to a cycle, D-L29), the main-loop watchdog and the wedged
data loop.
It assigns holds, the code swap and the second layout to 3d, and companions' arming against the published position to
3b; this page leaves the seams for them (13) and plans 3d's reshape spike (10.4).

## 3. Journeys

**The operator stops a card for a minute.** `node.stop {"name":"desk"}` reaches the keeper, which writes nothing and
posts a step to the main loop (D-L19). There `NodeHost` sets `desk`'s stop: the node is `running`, so `stop()` is
called (the card type writes none), the block is asked to fade `play`'s mirror `to_card` and every public output to
zero over 5 ms (5.3), and the reply `{"applied":"stop"}` is posted after `node.state` `stopping` (cause `stop`). About
one cycle later the data loop acknowledges the fade; `NodeHost` posts the silence (`drive` with no code), which the
data loop applies between two cycles; its completion lands, `NodeHost` releases and prepares the code afresh (5.3), and
`desk` is `stopped`. The ALSA device stays open and plays the block's zeros. `node.start` posts a drive; `start()` runs
once the block applied it, and `desk` is `running` within a cycle and a turn.

**A hand edit disables a sendspin node.** The keeper reads `node sendspin porch { disabled true; port 8928; }`, the plan
names `porch`'s outcome `disable` (D-L4), and `NodeHost` tears `porch` down. `porch` is on the legacy adapter (5.10):
it has no `stop()`, its code is faded and silenced, its handoff closes on its own thread, its run object closes off
the main loop (the worker says goodbye), and its claim on 8928 goes with the run object. Then `porch` is `disabled`,
and its block and wires stay.

**A restart-only change on a recording node.** `node.update` changes `rec`'s `path`. The plan says `restart`; `rec`'s
code is faded and silenced between two cycles, with its sink unbound in the same invoke, so the sink took every cycle
the code drove and no other; the old recording's sink drains
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
factory asked, not yet STREAMING), `ready`, `lost` (final; it keeps the loss's detail, which the aggregate shows while
the node is not disabled, 4.3), `gone`.

| State | Event | Next | Actions | node-framework.md |
|---|---|---|---|---|
| — | node added while the data loop is wedged (checked first, 5.4) | `lost` (detail "the data loop is wedged") | nothing queued: making a block would call into the wedged loop (D-L10) | T1, I5a |
| — | node added, no older block of its name | `made` | queue `make` in this turn's flush (5.5) | T1 |
| — | node added, an older block of its name exists | `waiting` | `waitingFor` `older-node` after 2 s | T1 |
| `waiting` | the older block destroyed | `made` | queue `make` | T1 |
| `waiting` | removal | `gone` | nothing to destroy | T17 |
| `made` | `PortBlockEvents::ready` | `ready` | tell the aggregate (T5 may follow) | T5's precondition |
| `made`, `ready` | `PortBlockEvents::lost` | `lost` | keep the detail; tell the aggregate: T13 | T13 |
| `made`, `ready`, `lost` | the removal's silence completed, or no code was ever driven since the last silence (D-L7) | `gone` | queue `destroy` in this turn's flush; when it ran, tell every `waiting` block of the name (at most one: only the wanted node of a name waits), and the node is removed (row 1) only then (revision 2.6) | T17, D-R8 |
| `lost` | any but removal | `lost` | nothing: a lost block is never remade (I1) | T13 |

Destroying a block is PipeWire's blocking call (`pw_filter_destroy`, `port_blocks.cpp:140-148` [V]), so it is always
queued, never made inside an event handler (5.5).

### 4.2 The configuration machine

At most one per node (I7). It carries four facts beside its state: `started` (`start()` returned and `stop()` was not
called since; a `start()` that threw never sets it, so no `stop()` pairs with it), `driven` (a code has been handed to
the block since the last silence), `rested` (it reached `running` or `stopped`: set at T5 and at T4's rest; a later
`lost(detail)` or a hook that throws is the code `lost`, before it `start-failed`, D-L23), and `stopAsked` (a stop
arrived while the drive was in flight). `n` is the barrier number, raised for every request it posts (5.6).

States: `none`; `readying` (instance made, `configure` answered applied, resources readying); `prepared` (every
resource ready, code prepared, not driven); `arming` (a drive posted, its completion awaited, at most 5 s: revision 2.7); `driving` (applied,
`start()` called); `fading` (a fade posted, its acknowledgement awaited, at most 100 ms); `silencing` (a silence
posted, its completion awaited, at most 5 s); `tearing` (I5 steps 1's rest to 5, at most 10 s).

The machine has no shortcut: every `configure` enters `readying`, and when nothing is pending "every resource ready"
follows in the same turn, so T3 (a legacy recipe included, 5.10) always runs and `configuring` is always told (4.5).

| State | Event | Next | Actions | node-framework.md |
|---|---|---|---|---|
| `none` | configure (aggregate) | `readying` | new generation; make the instance; call creation's `configure`; ask the resources (5.8) | T2, T12, T15, T22 |
| `none` | configure; `configure` answered `failed(d)` or threw | `tearing` | fault `start-failed: d` (or `<hook> threw: <what>`); built as the row above, whose action calls the hook, then its answer as a fault in `readying` | T2 then T14 |
| `readying` | every resource ready (outside nodes linked, sink consumers begun; at once when none is pending) | `prepared` | legacy: run the recipe (5.10); prepare the code | T3 |
| `readying`, `prepared` | stop or start asked | unchanged | nothing: the aggregate's `stopHeld` decides at `prepared` (rule 1) | T4, 6.3 |
| `prepared` | rest (aggregate: wanted stopped, not `rested`) | `prepared` | `rested = true` | T4 |
| `prepared` | drive (aggregate: wanted running, block `ready`, no change pending) | `arming` | carry the latency plan; post `drive(code, faded, sinks)`, `n++` | T5, T8 |
| `arming` | `driven(n, P)` | `driving` | unless `stopAsked`: call `start()`, `started = true` once it returned; `rested = true`; a `start()` that threw undoes both (`start-failed`, not `lost`: it never reached `running`) | T5 |
| `arming` | `driven(n, P)` with `stopAsked` | `fading` | no `start()`; post a fade | 6.3 ("stop in starting") |
| `arming` | stop or teardown asked | `arming` | `stopAsked = true` (the intent is the aggregate's) | 6.3 |
| `arming` | start asked while `stopAsked` and the intent is `stop` | `arming` | `stopAsked = false` | 6.3 |
| `driving` | stop or teardown asked | `fading`, or `silencing` if no output is faded | if `started`: call `stop()`, `started = false`; post a fade (a 100 ms timer), else a silence | T6, T14, T17, T20 |
| `fading` | `faded(n)` | `silencing` | post a silence (5 s timer) | T6 |
| `fading` | the 100 ms bound, after `settle()` found no acknowledgement (5.4) | `silencing` | `ear.fade-unacknowledged`; post the silence unfaded | I5a |
| `fading`, `silencing`, `tearing` | stop, start or teardown asked | unchanged | nothing here: the aggregate's intent changes (4.3, rule 3), and `silenced` goes by the intent it finds | 6.3 |
| `silencing` | `silenced(n, P)`, intent `stop` | `prepared` | clear the latency plan; `driven = false`; `release` then `prepare` the code, so the next drive starts it afresh (D-L33) | T7 |
| `silencing` | `silenced(n, P)`, any other intent | `tearing` | clear the latency plan; `release`; queue the outside nodes' destruction; close the sinks and legacy handoffs; arm the 10 s close timer | T6, T14, T17, T20 then I5 |
| `silencing`, `arming` | the 5 s bound, after `settle()` found the invoke had not run (5.4; a drive's too, revision 2.7) | `none` (leaked) | `ear.barrier-overdue`; mark the data loop wedged; leak the configuration | I5a |
| `readying`, `prepared` | teardown asked (restart, disable, removal, fault) | `tearing` | no barrier (never driven); `release` if prepared; as above | I5 ("before ready") |
| `tearing` | every sink and handoff closed, then the legacy run object closed | `none` | on the main loop: the code, the instance, the recordings, in that order (I5 step 5) | I5 |
| `tearing` | the 10 s bound | `none` (leaked) | `ear.close-overdue`, its detail naming every sink, handoff and run object still open; leak (today's `closeOverdue`, `node_host.cpp:447-464` [V]) | I5a |
| `readying` … `driving` (not `arming` with `stopAsked`) | fault: a resource refused or lost, `lost(d)`, a hook threw | as "teardown asked" (intent `fail`) | the failure's code by the fact (below) | T14 |
| `fading`, `silencing`, `arming` with `stopAsked` | fault, intent `stop` | unchanged | held: the aggregate records it (`heldFault`) and applies T14 once the node rests (4.3, rule 1; D-L32) | T14 |
| `fading`, `silencing`, `tearing`, `arming` with `stopAsked` | fault, any other intent | unchanged | dropped (I6: facts are dropped once a teardown's step 1 began) | I6 |
| any but `none` | the block lost | `silencing` if driven, else `tearing` | no `stop()` (`started = false` without it), no fade; the silence completes by signal on a block with no data loop (5.3) | T13 |

**The failure's code, by fact** (node-framework.md 6.2's table, D-L23):

| Fact | Code |
|---|---|
| a sound card or RTP receiver refused or lost; the block refused or lost; the data loop wedged | `engine-refused`, whatever `rested` says |
| `configure` answered `failed(d)`; a sink consumer's `begin` said `lost`; a legacy recipe failed (all only before `rested`) | `start-failed` |
| `lost(detail)` from the node's handle; a hook (`configure`, `start()`, `stop()`) that threw | `start-failed` before `rested`, `lost` after |

### 4.3 The aggregate: wanted, intent, phase

The node (`NodeHost::Impl::Node`) holds the two machines and these facts: `disabled` (from its `NodeSpec`, 8),
`stopHeld` (the node's stop, in memory, D-F1), `intent` (what a stop or teardown in flight is for: `stop`, `restart`,
`disable`, `remove` or `fail`), `cause` (why, as the wire's `cause` shows it; it outlives the intent into
`configuring`), `queued` (the newest committed settings and their config), `changePending` (`queued` differs from the
configuration's config), `failure` (code and detail), `retryAsked` (a `start`, a `restart` or a committed change arrived
on a failed node, D-L31), `heldFault` (a fault that arrived during a stop's fade or silence, D-L32), `predecessor` (the
serial of an older `Node` of the name whose port-caused replace this node is, D-L36; cleared once that node closed
and its block was destroyed, in either order, so the successor goes from row 3 to `configuring` and never shows
`added`), `olderNode` (an older `Node` of the name is not yet removed, rule 1's "no older node of the name left";
cleared when the last one is: revision 2.6) and `configuredTold` (row 10 below). `wanted` is `disabled` when `disabled`, else `stopped` when `stopHeld`,
else `running`. A block's loss clears `failure`, `retryAsked` and `heldFault` (4.4's "reason replaced": row 4 then
shows the loss).

The phase is derived, first match wins:

| # | Condition | `phase` | `cause` |
|---|---|---|---|
| 1 | block `gone` and configuration `none`, intent `remove` | `removed` (not a phase on the wire: `node.gone`) | — |
| 2 | intent `remove` | `stopping` | `removing` |
| 3 | `predecessor` set (the older node of a port-caused replace still tears down) | `stopping` | `restarting` (`settings` with the changed keys; D-L36) |
| 4 | `failure` set, or the block `lost` and not `disabled` | `failed`, `reason` the failure's, else `engine-refused` with the block's loss detail (the teardown behind it is invisible, R20) | — |
| 5 | configuration `none`, `disabled` | `disabled` (`blockLost` when the block is lost) | — |
| 6 | configuration `fading`, `silencing` or `tearing`; or `arming` with `stopAsked` (a drive in flight that its completion fades and silences, so a restart or disable in `starting` shows `stopping` as D-L2 and 4.4 say); or `none` with intent `restart` (the teardown ended and `configure` follows in this turn) | `stopping` | by intent: `stop`, `restarting` (with its own kind: `settings` with keys, `action`), `disabling` |
| 7 | configuration `none`, not disabled (waiting for an older node) | `added` | — |
| 8 | `readying` | `configuring` | after a restart, a retry or an enable: `settings`, `action`, `retry`, `enabling` |
| 9 | `prepared`, `stopHeld` | `stopped` | — |
| 10 | `prepared`, not `stopHeld`, not `configuredTold`, not `rested` (a configuration that rested never shows `configured` again: a start after a stop tells `starting`, `running`, T8) | `configured`; `NodeHost` sets `configuredTold` once it told it, and derives again | — |
| 11 | `prepared`, not `stopHeld` (the block not `ready`, or a change pending) | `starting` (`waitingFor` `block` while the block is not `ready`) | — |
| 12 | `arming` | `starting` | — |
| 13 | `driving` | `running` | — |

`configuredTold` is cleared whenever the configuration leaves `prepared`.

The wire's `state` is the phase's colour: `added`, `configuring` and `configured` are `starting`; every other phase is
its own name (control-plane.md 4.8). Two enums carry them (D-L22): `NodePhase` (`added`, `configuring`, `configured`,
`starting`, `running`, `stopping`, `stopped`, `disabled`, `failed`; 3d adds `reconfiguring`, slice 8 `recovering`) and
`NodeState` (`starting`, `running`, `stopping`, `stopped`, `disabled`, `failed`), each with its status-line name.

**The aggregate's own rules.** They are what 6.3's "operations that arrive mid-transition" says, in three rules:

1. **Converge.** After every event the aggregate compares what is with what is wanted, in this order:
   - configuration `none`, not disabled, no `failure` unless `retryAsked` is set, block not lost, no older node of the
     name left and no `predecessor`, intent not `remove`, the data loop not wedged → configure with `queued`, clearing
     `failure`, `retryAsked` and `heldFault`; an intent `restart` ends here (its `cause` stays for `configuring`); the
     same with the data loop wedged → `failure` `engine-refused: the data loop is wedged` instead, as D-L10 for a new
     node (and a stop's or restart's silence overdue, whose configuration is leaked, fails the node so, since neither can
     end; a disable or removal goes on to its end);
   - `prepared` with `heldFault` → T14 (intent `fail`, `failure` from the held fault), applied at the rest itself, before
     health is told, so no `stopped` is told for a configuration with a fault against it;
   - `prepared` with `changePending` → restart (intent `restart`, cause `settings` with the changed keys), before any
     drive, so a change committed during a stop's teardown and a `start` after it restart first and drive the new
     settings (D-L41);
   - `prepared`, wanted running, block `ready` → drive;
   - `prepared`, wanted stopped, not `rested` → rest (T4);
   - `arming` or `driving`, wanted not running → stop (intent `stop`; a disable comes through its own outcome).
2. **A change restarts when the configuration is settled** (D-L3). A committed change to `queued` restarts at once
   (intent `restart`, cause `settings` with the changed keys) when the configuration is `readying`, `prepared`,
   `arming` or `driving`; when it is `fading`, `silencing` or `tearing` it only replaces `queued`, which the next
   `configure` takes, or, for a stop, which rule 1's `changePending` takes once the node rests. On a failed node it sets
   `retryAsked` (T15 once the failed configuration is gone). This is 6.3's "before the node first reached running or
   stopped, any change tears the configuration down" and "in stopping, a change waits for stopped" at once.
3. **Intent: its lifetime, and the newest wins** (D-L37). An intent is set when a stop or a teardown begins and lives
   only while it is in flight: `stop` ends when the node rests (T7) or a `start` cancels it in `arming`; `restart` ends
   at the next `configure` (T2); `disable` and `fail` when the configuration is `none`; `remove` when the node is
   removed. While one is in flight a newer one replaces it in this order only: `remove` over `disable` over `fail` over
   `restart` over `stop`. A disable during a stop or a restart's teardown goes on to `disabled`; an enable during a
   disable's teardown turns its intent to `restart` with cause `enabling`; a `start` during a stop clears `stopHeld`,
   so rule 1 drives again once the node rested (T7 then T8). A removal turns any intent to `remove`. Once an intent
   ended, the next operation starts afresh: a stop after a completed restart is a plain stop (T6, cause `stop`), never
   a teardown. `failure` is cleared when the node is configured again (rule 1, after a retry, a change or
   `node.restart`: T15) or disabled (T21), never before, so a retry asked while the failed configuration still tears
   down shows `failed` until its `configure` runs.

### 4.4 Every operation in every phase

The replies are control-plane.md 4.2 and 7.6's; this table is how 3a's machines produce them. "Restart" means intent
`restart`: the configuration's teardown (with `stop()`, the fade and the silence only where 4.2 calls for them), then
`configure` from empty with `queued`. A request naming a node being removed by an edit reaches the wanted node of that
name, if any (a successor of a port-caused replace is in the `stopping` (restarting) row), else it is `unknown-node`:
the node left the graph at the commit (control-plane.md 4.8). While `stopAll` runs, every operation is refused
`stopping`, whatever the phase. Two cells cannot arrive: `added` lasts only while an older block of the name exists, so
it has no block to lose; and `configured` lasts no input (row 10 is told and derived again at once), so its cells are
`configuring`'s.

| Phase | Restart-only change | `stop` | `start` | `restart` | `disable` | `enable` | removal | block lost | fault |
|---|---|---|---|---|---|---|---|---|---|
| `added` | replaces `queued` | `stopHeld` set: `stop` | cleared: `start` or `unchanged` | `not-restartable` (`not-started`) | T19 | `unchanged` | T17, no barrier | T13 | — |
| `configuring`, `configured` | restart now: `stopping` (`restarting`), no `stop()`, no barrier | set; it rests at T4: `stop` | cleared | restart (`action`): `restart` | T20, no `stop()` | `unchanged` | T17, no barrier | T13 | T14 (`start-failed` or `engine-refused`) |
| `starting` | restart now; a drive in flight is faded and silenced at its completion | set; at rest, or at the drive's completion faded and silenced, no `start()` or `stop()` | `unchanged` | restart: `restart` | T20, fade and silence only if a drive is in flight | `unchanged` | T17 likewise | T13 | T14, no `stop()` |
| `running` | T6 (`restarting: settings changed: <keys>`), T12 | T6 (`stop`), T7: `stop` | `unchanged` | T6 (`restart invoked`), T12: `restart` | T20 with `stop()` and the fade | `unchanged` | T17 with `stop()`, fade, silence | T13, no `stop()`, no fade | T14 with `stop()` and the fade |
| `stopping` (stop) | replaces `queued`; once it rests, a restart before any drive (rule 1) | `unchanged` | cleared; once it rests, T8: `start` | intent `restart`: `restart` | intent `disable`, stop dropped | `unchanged` | intent `remove` | T13 | held; T14 once it rests (D-L32) |
| `stopping` (restarting) | replaces `queued` | set; the next configuration rests: `stop` | cleared | `unchanged` | intent `disable` | `unchanged` | intent `remove` | T13, nothing configured again | dropped |
| `stopping` (disabling) | stored for the enable | `node-disabled` | `node-disabled` | `node-disabled` | `unchanged` | intent `restart` (`enabling`), T22 once gone | intent `remove` | T13 | dropped |
| `stopping` (removing) | — (not wanted) | `unknown-node` | `unknown-node` | `unknown-node` | `unknown-node` | `unknown-node` | — | nothing new | dropped |
| `stopped` | restart: `stopping` (`restarting`), no `stop()`, no barrier; rests `stopped` again | `unchanged` | T8: `start` | restart, rests `stopped`: `restart` | T20, no fade, stop dropped | `unchanged` | T17, no barrier | T13 | T14 (`lost`), no `stop()` (it ran at the stop) |
| `disabled` | `stored` (in 3a, a change of ports is a replace, D-R23) | `node-disabled` | `node-disabled` | `node-disabled` | `unchanged` | T22 | T17, no barrier | T13 | — |
| `failed`, block kept | `retryAsked`: T15 once the old configuration is gone | set, stays `failed`: `stop` | `retryAsked`, the stop cleared; T15: `retry` | `retryAsked`; T15: `retry` | T21 | `unchanged` | T17, no barrier | T13: reason replaced, final | — |
| `failed`, block lost | written; nothing runs (I1) | set: `stop` | `not-restartable` (`block-lost`) | `not-restartable` (`block-lost`) | T21 | back to `failed`, nothing configured | T17 | — | — |

Health is told after every machine transition that changes any derived member (5.2, D-L34), so a cell that walks several
phases in one turn sends one `node.state` per phase it passes (a restart of a `stopped` `white`: `stopping`
(restarting), `configuring` (`action`), `stopped`; a node resting stopped skips `configured`, row 9), and a cell that
changes only `wanted` sends one; each before the reply when the reply comes in the same turn (control-plane.md 4.8, its
rule 5; D-R19).

### 4.5 What each transition writes

The `node` records' sentences are node-framework.md 6.2's and their fields control-plane.md 9.1's. Health is told
after every transition that changes a derived member, with two exceptions: the `added` of a node added disabled is not
told when T19 follows in the same turn (a node added disabled shows only `disabled`), and a successor of a port-caused
replace starts in `stopping` (row 3), so no `added` is told for it (D-L36). `configuring` is always told, a node with
nothing pending included (4.2: no shortcut), so a normal start is `added`, `configuring`, `configured`, `starting`,
`running`, as control-plane.md 4.8 and `lifecycle.json` say. `StatusLog` writes a record when the derived state (the
colour) changes to `starting` (the `added`), `running`, `stopping`, `stopped`, `disabled`, `failed` or `removed`, and
`configuring` after a restart, a retry or an enable (`starting: <cause>`, W4); none for `configured` or the `starting`
phase inside the colour.

`waitingFor` is paced where health is made, in `NodeHost` (D-L42): it is absent until the phase has lasted 2 s (a
per-node timer armed on each phase change), then set, and after that changed in health at most once a second (a per-node
1 s timer coalesces changes in between, so the newest list is told when it fires). So the gateway's `node.state` (on
every health change) meets control-plane.md 4.8's "after 2 s, at most once a second" by construction, and `StatusLog`
writes a `still` record when told a health whose `waitingFor` first appears, then at most once per 10 s while it changes
(its own pacing, by the records' `t`). In 3a `waitingFor` has four kinds: `older-node`, `block`, `companion` (a card's
or a receiver's `<node>.<suffix>`), `consumer` (a sink's `begin`); `claim`, `store`, `recording` and `loop` are 3c's.
`since` is stamped by each listener when it is told, in the same turn (D-L22).

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

Revision 2's answers to its review changed both pages again, each by a minimal edit recorded in its review record
(node-framework.md revision 8.2, control-plane.md revision 6.2), so no two pages disagree:

- node-framework.md I4, 6.3's wedged data loop and 9.6: in 3a, with no companion, the block applies each change inside
  its invoke and publishes the position it takes effect from; take-up deferred to a published cycle begins with 3b's
  companions, and a block leaving STREAMING then applies what is pending at once; the loop is judged wedged only on its
  own sign of life, never on one block's silence (D-L29, D-L30).
- node-framework.md 6.2's failure codes: a hook that throws once the node rested is `lost`, as `lost(detail)` is (4.2's
  code by fact).
- control-plane.md 7.6: a restart of a node being removed by an edit is `unknown-node`, and every operation during
  ear's own stop is `stopping`; 4.8: a replace sends none of the old node's health, `node.stopped` or `node.gone`, since
  the client holds the new node of that name (D-L36).

## 5. `NodeHost`, rewritten

### 5.1 Its data

`NodeHost::Impl` keeps today's shape (listeners, `nodes` in arrival order, `generations`, `leaked`, `stopRequested`,
`allGone`; `node_host.cpp:495-508` [V]) and adds: `wedged` (the data loop judged wedged, 5.4), `flush` (this turn's
queued PipeWire changes, 5.5), `deferred` (inputs that arrived while the flush ran, 5.5), `serials` (a counter naming
each `Node` object for completions, 5.6), a `Clock` read through `Timers::now()` (D-L12), and `NodeOperations` (5.2)
implemented on it.

Each `Node` holds: its `name` and `serial`; `block` (`BlockMachine` plus `std::unique_ptr<PortBlock>`, its `Ports`, its
`PortBlockEvents`); `config` (`ConfigurationMachine` plus, when not `none`, a `Configuration`); 4.3's facts
(`disabled`, `stopHeld`, `intent`, `cause`, `queued` as spec and `std::shared_ptr<const NodeConfig>`, `failure`,
`retryAsked`, `heldFault`, `predecessor`, `configuredTold`); its health as last told; its timers (the 2 s phase timer
and the 1 s `waitingFor` timer, 4.5; the fade's, the barrier's, the close's); `dropped` (frames its sinks and legacy
handoffs refused, summed for `removed dropped=`, and carried into a port-caused replace's successor, 5.7).

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
4. **Tell.** After every machine transition in 2 and 3, not once per input, derive the health; if any member changed,
   tell every `NodeHealthListener` (`StatusLog`, the gateway), in subscribe order, before going on (D-L34; the two
   exceptions are 4.5's). So a commit's `node.updated` (told by the gateway as an observer before `NodeHost`,
   node-framework.md 6.6) precedes the `node.state`s it causes, every phase a turn passes through is told in order,
   and an operation's `node.state`s precede its reply, which the keeper's main-loop side posts after `NodeHost`
   returned.

Nothing in 1 to 4 waits for another thread. The flush runs once at the end of the turn's inputs (5.5).

`NodeOperations` is a new interface in `node_host.hpp` (an interface lives with its consumer's package, AGENTS.md, but
its consumer `MainLoopSide` is in `ear_config`, which sees `ear_core`, so it lives beside `NodeHost`):
`LifecycleReply start(const NodeName&)`, `stop`, `restart`, where `LifecycleReply` is `std::expected<LifecycleApplied,
LifecycleRefusal>`, both core's own types with no wire spelling (D-L35):

```cpp
enum class LifecycleApplied { start, stop, restart, retry, unchanged };
struct LifecycleRefusal {
	enum class Code { unknownNode, nodeDisabled, notRestartable, stopping } code;
	enum class Reason { none, notStarted, blockLost } reason = Reason::none;   // notRestartable only
};
```

`MainLoopSide` (`ear_config`, which sees both `ear_core` and `ear_control`) maps them onto control-plane.md 4.2's
`applied` values and 4.5's codes (`unknown-node`, `node-disabled`, `not-restartable` with `reason`, `stopping`) when it
builds the `CommandResult`; `ear_core` never names a wire code.

### 5.3 Barriers, fades and the published position: `PortBlock` in 3a

Today `drive(AudioCode*)` is a blocking invoke (`port_blocks.cpp:159-166`, `341-347` [V]) whose function stores the
code between two cycles. In 3a `PortBlock` (core's interface, `node_host.hpp`, through an erratum to its header note)
becomes:

```cpp
/// What a drive hands the block: the code (null: silence), which outputs its fade touches, and the interim sinks the
/// block writes after the code (5.11). All of it outlives the next drive's completion or the block.
struct DriveRequest {
	AudioCode* code;
	std::span<const bool> faded;            // one per output (outputs then inside outputs, as process's spans)
	std::span<SinkTap* const> sinks;        // interim (L-4b on): each with its inside output bank's first port, count
};
class PortBlock {
public:
	virtual ~PortBlock() = default;
	virtual void carryLatency(const LatencyPlan& plan);   // as today: synchronous, never batched (5.5)
	/// Main thread; returns at once. The data loop applies the request inside its invoke, between two cycles, whatever
	/// the block's state: the code, the sinks and a reset fade-in gain together, any pending fade cancelled. From the
	/// next cycle no process() of the previous code runs. `done(P)` then runs on the main loop, never inside this call,
	/// with P the position the request takes effect from (the next cycle's expected position). A block with no data
	/// loop stores the request on the main thread and still completes by signal.
	virtual void drive(DriveRequest request, std::move_only_function<void(std::uint64_t taken)> done) = 0;
	/// Main thread; returns at once. From the next cycle every faded output ramps to zero over 5 ms and stays there
	/// (the code still runs); `done()` runs on the main loop once a cycle ended at zero. A block that is not processing
	/// (not STREAMING, or no data loop) completes it by signal, never inline.
	virtual void fadeOut(std::move_only_function<void()> done) = 0;
	/// Main thread: run every `done` whose request the data loop already completed, as the event callback would. Called
	/// by NodeHost before it judges any bound (5.4), so a main-loop stall never mistakes a late dispatch for a late loop.
	virtual void settle() = 0;
	/// Any thread: the position from which the block runs what it was last given (for 3b's companions, and tests).
	virtual std::uint64_t takenAt() const noexcept = 0;
	/// Main thread: the block's engine node's global id, read when first STREAMING (5.7); SPA_ID_INVALID before.
	virtual std::uint32_t blockId() const noexcept = 0;
};
```

How `FilterPortBlock` (`port_blocks.cpp`) does it:

- **Posting, one invoke in flight per block** (D-L38). `drive` and `fadeOut` push the request onto a small
  single-producer, single-consumer ring of the block's (8 entries, main thread to data loop) and post
  `pw_loop_invoke(dataLoop, ..., block = false)` only when no invoke of this block is in flight; that invoke's function
  drains every request the ring holds, in order. A request pushed while one is in flight is drained by it or by the
  invoke the completion posts. So the loop's invoke queue holds at most one entry per block, whatever the main loop
  posts, and a full ring (eight requests a block never completed) is a stalled loop, judged at 5.4's bound, never a
  post that waits. The invoke may run inline when the data thread has not yet entered its loop (node-framework.md's
  "Slice 7 as built", denormals) [V]; that is harmless, because the function only sets data-loop state and signals
  (below), never calls `done`. A block with no data loop (refused, never connected, `port_blocks.cpp:153-158` [V])
  drains the ring on the main thread and signals, as today's `drive` stores the pointer (`port_blocks.cpp:159-163`
  [V]).
- **Applying, inside the invoke** (D-L29). Invokes run on the data loop between two cycles, never during a `process`
  (today's `drive` already relies on it, `port_blocks.cpp:341-347` [V], and S1 checks it again under load), so I4
  holds without waiting for a cycle: the function swaps the code (today's `fresh_` discontinuity stays,
  `port_blocks.cpp:345` [V]), binds or unbinds the interim sinks' taps, resets the fade gain to the fade-in's start for
  the faded outputs (a silence's gain is moot), cancels a pending fade-out, publishes `takenAt` as the next expected
  position (`nextPosition_`, the position the next cycle on this driver will carry; one atomic word, release) and
  signals completion. The next cycle, whatever its position, runs the new code with its taps, so the code and the sink
  change together and the sink takes exactly the cycles the code drove, by construction. A block that has stopped
  processing (PAUSED, or its driver gone) is still applied at once: nothing in 3a waits for a cycle but a fade. 9.6's
  take-up deferred to a published cycle exists for companions, which are other engine nodes dispatched separately; it
  begins with them in 3b (13), and then a block leaving STREAMING posts an invoke that applies whatever it deferred.
- **Fading.** One gain for the block's faded outputs, applied after the code and before metering, so levels show the
  silence: a linear ramp of `rate * 5 / 1000` frames (240 at 48 kHz, inside one 256-frame cycle), either way. Sink
  banks are never faded: the sink is bound and unbound with the code (5.11). A fade-out's acknowledgement is signalled
  at the end of the first cycle that ended at gain 0; a cycle too large to run (`frames > maxBlock`, which writes
  zeros, `port_blocks.cpp:387-393` [V]) counts as one that ended at 0. A fade-out whose invoke finds the block not
  processing (an atomic the main loop sets from `onStateChanged`, read by the data loop) is acknowledged in that
  invoke, by signal; and on every transition out of STREAMING the main loop posts one more invoke that acknowledges a
  fade still pending. So a fade completes within one cycle, or at once when no cycle comes, and the 100 ms bound only
  ever fires for a block that is STREAMING and silent, never judged as the loop.
- **Built in L-2** (revision 2.2): `DriveRequest` has no `sinks` until L-4b, which brings `SinkTap`. `blockId()` is a
  member of `PortBlock` (main thread, read in `onStateChanged` at the first STREAMING), which `NodeHost` reads once the
  block is ready (L-3b), so `PortBlockEvents::ready()` keeps no argument; `pw_filter_get_node_id` is set when the filter's proxy is bound (`filter.c` `proxy_bound_props`), before
  STREAMING [V]. A request the ring has no room for waits in a main-thread queue behind it and enters as `done`s run, so
  no request is refused and none waits on the data loop; a ring slot is free once its request's `done` ran (the main
  thread reads the result from it). A fade-out a later drive cancelled has its `done` dropped, never run: running it
  would tell the caller its outputs are silent when the drive has just faded them in. A fade-out is linear in frames,
  frame `k` at `k / N` in and `(N - k) / N` out (`N = rate * 5 / 1000`), so exact at both ends; a fade-out's first frame
  is still at full gain and frame `N` on is zero. `takenAt` is the *expected* position: when a stall longer than the
  driver's resync bound (480 frames, 10 ms) makes the dummy driver reset, the next cycle carries another position (L-2's
  fade test found it), forward or back (10.5: after a step back the swap waits until the position reaches `takenAt`
  again, up to `resync.ms` late); nothing in 3a reads it but tests, and 3b's companions must allow for it.
- **Completing.** The data loop writes the completed request's number into an atomic (`appliedSeq`, `fadedSeq`) and
  calls `pw_loop_signal_event` on an event source the block made on the main loop at construction (as the handoff
  does from `process` today, `handoffs.cpp:54` [V], and as the block already does for refusals, `port_blocks.cpp:106`
  [V]). The main-loop callback, and `settle()`, read the atomics and run every `done` up to them, in order; signals
  coalesce, the numbers do not. Never an invoke towards the main loop (D-R7). As built (L-2) the two numbers are one,
  `completed`: requests complete in the order they were made, a drive at once in its invoke and a fade-out at its zero,
  at once when not processing, or cancelled by the drive behind it, so the requests applied but not completed are
  always pending fade-outs and one number says which `done`s may run.
- **Destroying.** An invoke of the block still queued (posted, not run) would run on a freed block, so the destructor
  first posts one blocking no-op invoke to the loop it posted to, which runs after it (posting order), when one is in
  flight (L-2; a wedged loop's block is leaked, never destroyed, 5.4, so this never waits on one). The event source is
  destroyed in the destructor after `pw_filter_destroy`, so a completion for a
  destroyed block runs nothing.

`NodeHost` wraps each `done` with the node's serial, its generation and the barrier number (5.6) before passing it.
The barrier is skipped when the block has had no code since its last silence (D-L7): nothing the realtime thread can
reach would be freed.

### 5.4 Deadlines, and the wedged data loop

Timers are `NodeHost`'s (`Timers`, as the close deadline is today), never the block's (D-L24): the fade's 100 ms, the
silence's 5 s, the teardown's 10 s (I5a), and 4.5's 2 s and 1 s. Every bound that judges the data loop first calls the
block's `settle()` (D-L30): a main loop that stalled past a bound dispatches its timers and the block's event in either
order, so the timer reads the atomics itself, and a request that completed meanwhile completes now, as if its event
had come first. Only what is still not done is judged.

- **Fade unacknowledged** (100 ms, after `settle()`): `ear.fade-unacknowledged` (control-plane.md 4.6), then the
  silence is posted as if the fade had ended. A late `faded` is stale (5.6) and dropped. This judges one block, and
  never the loop.
- **Barrier overdue** (5 s, after `settle()`): a barrier is applied inside its invoke (5.3), so a barrier not done at
  5 s means the invoke itself never ran: the loop ran no invoke for 5 s, a fact about the loop and never about one
  block's processing (D-L30). `ear.barrier-overdue` names the node, and `ear.data-loop-wedged` is written once through
  `EngineDiagnosticListener` (below). From then on `wedged` is set and no PipeWire call is made on that loop again: the
  flush does nothing; every configuration and block in a teardown, and every one after, is leaked (counted in
  `leaked()`, renamed from `leakedRuns`); rule 1 configures nothing; a node added afterwards is checked before T1 and
  fails `engine-refused: the data loop is wedged` without a block (4.1, D-L10); edits still commit and the keeper
  still writes. NodeHost is not the only caller: making or destroying a route's link, an outside node tearing itself
  down and a latency update also reach the data loop through a blocking invoke (PipeWire 1.4.2's `impl-port.c`
  `port_set_io` releases a mix with `pw_loop_invoke(..., true)`), so `main.cpp` marks the engine when
  `ear.data-loop-wedged` is said (`ear/pipewire/data_loop_wedge.hpp`) and `ear_pipewire`'s adapters stop too: `Links`
  makes no link (each is refused "the data loop is wedged", so a route added or relinked after the wedge is `refused`
  with that detail; the wire's statuses are unchanged) and leaks every link proxy it would destroy, an outside node
  leaks its links and body, and a port block tells no latency. Levels, live ports, handoffs and the control thread make
  no call that waits on the data loop (revision 2.7). `stopAll` reaches `allGone` at once for leaked nodes, and `main.cpp` already leaves through
  `std::_Exit` once anything is leaked, after the keeper's last write (`main.cpp:307-315` [V]). From 3b, whose
  companions defer take-up to a cycle, a barrier overdue first posts a no-op probe invoke and judges the loop wedged
  only if the probe has not run within a further 1 s; 3a does not need it (13).
- **Close overdue** (10 s): today's path, kept (`node_host.cpp:447-464` [V]).

**Diagnostics with no node** (D-L44). `ear.data-loop-wedged` names no node (control-plane.md 4.6), so it does not go
through `NodeDiagnosticListener`. `node_host.hpp` gains `EngineDiagnosticListener::engineDiagnostic(EngineDiagnostic)`,
where `EngineDiagnostic` is `{std::string_view code; std::string detail; std::vector<DiagnosticField> fields;}` (revision
2.7: `DiagnosticField` is `{std::string_view key; std::int64_t value;}`, control-plane.md 4.6's Fields, here `ms`, and
`NodeDiagnosticListener::nodeDiagnostic` gains the same `fields`; a record carries them between `code` and `detail`) (the detail names the node whose barrier
tripped it and the time waited); `StatusLog` implements it with a `diagnostic` record without `node`, and
`ear.main-loop-stalled` and `ear.main-loop-resumed` (5.12) reach the same encoder from `main.cpp`.

The three bounds are tuned against CI's stalls by 11.6's test (node-framework.md 15: a loaded host reports a stall but
never judges the loop wedged, since a stalled data thread still runs its invokes once it runs at all).

### 5.5 PipeWire's own calls, batched per turn

Making and destroying a block and making and destroying an outside node (its body and links) are PipeWire object
changes, and each makes a blocking invoke into the data loop inside PipeWire (node-framework.md 6.3). `NodeHost` never
makes them inside an input's handling: it appends them to `flush` and, on the first append of a turn, posts one `flush`
task to the main-loop `Executor` (D-L8). The task runs after the turn's queued inputs, in one go:

1. every queued destruction (outside nodes, then blocks), each after its node's silence completed;
2. every queued making (blocks, then outside nodes, whose adapters need their block's ports, `node_host.hpp:150` [V]).

Destroys come first so a name added back (T1) gets its block in the same flush as its older block's destruction.
`carryLatency` is not batched: it is a params update on the main loop (`pw_filter_update_params`,
`port_blocks.cpp:328-335` [V]), not one of the four, and it must precede its drive, so it stays synchronous, as today.

**Re-entrancy** (D-L39). The flush iterates a snapshot of `flush` taken when it starts; an item a flush item causes
(a waiting block whose older block was just destroyed, made "in the same flush") is appended behind the snapshot and
runs in the same flush, after the current item, never inside it. An event an adapter tells synchronously inside a
PipeWire call the flush makes (an outside node's `ready` or `lost` told from inside its making, a block's refusal) is
not handled there: `NodeHost` appends it to `deferred` and handles it as an input of its own, through 5.2's steps,
once the current flush item returned; inputs it causes that queue PipeWire changes go behind the snapshot as above.
Each call sets `MainLoopActivity` (5.12) to name itself and clears it when it returns, so a stall inside it is
reported with its name and a stall after it is not blamed on it. A block's refusal is told through its event
(`port_blocks.cpp:202-213` [V]), so it never arrives inside the flush in the first place.

### 5.6 Generations and stale completions

Each `configure` from empty takes `++generations` (global, as today, `node_host.cpp:505` [V]). Every completion
`NodeHost` receives is checked against `(serial, generation, barrier)`: the node with that serial must still exist
(nodes are looked up by serial, never by pointer), its configuration's generation must match, and the barrier number
must be the one in flight. Anything else is dropped. `Channel` remains the generation for what the node's own threads
say (detached on every path that ends a configuration), so I6 needs no number compared there. The generation never
leaves `NodeHost` (D-A11): `LogEntry::run` goes (9).

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

**A port-caused replace keeps the node's stop and its health** (D-L36). Until 3d a change of ports is a replace
(D-R23, D-L5): `nodeRemoved` then `nodeAdded` of the same name. D-F4 says a hand edit that keeps a node (same name and
type) keeps its stop whatever else it changes, and a change of ports keeps both. So the plan marks a replace whose type
is unchanged as port-caused (`NodeChange` carries it, 8.4), and `NodeHost`, told that remove and add in one commit:

- gives the new `Node` the old one's `stopHeld` and `predecessor` (the old serial); its block waits for the old block
  (T1), its configuration for the old node to be gone (rule 1), and its phase is `stopping` (restarting: `settings`
  with the keys that changed the ports) until then (4.3, row 3), then `configuring` with the same cause, then it rests
  `stopped` or runs, as any restart;
- tells the old `Node`'s teardown to nobody: no health, no `node` record and no `nodeGone`; its `stop()`, fade, silence
  and teardown run as a removal's, and its `dropped` frames are added to the new node's. So the wire sees
  `node.removed`, `node.added` (at the commit, from the gateway) and then one health stream for the name, and standard
  output sees `node <n> stopping: restarting (settings changed: <keys>)` (control-plane.md 9.1's form), then `starting`, `running` or `stopped`, as for
  any restart.

Any other name added back while an older `Node` of the name still tears down (a type changed, a remove and add of
unrelated nodes in one plan) keeps today's records on standard output (`stopping: removing`, `removed dropped=`), but
from the commit that adds the newer node the older one's health goes to `StatusLog` only, never to the gateway, and
no `node.stopped` or `node.gone` is sent for it: the wire keys health by name, and the client holds the newer node of
that name (control-plane.md 4.8, revision 6.2).

The join links the block end by global id (I9): `OutsideTarget` gains `blockId`, the block's node id, which
`FilterPortBlock` reads when STREAMING (`pw_filter_get_node_id`) and `NodeHost` reads from `PortBlock::blockId()` when it
makes the outside node in the flush, the block being ready; `joinOutsideNode` writes it in `link.output.node` or
`link.input.node` instead of the name (`outside_nodes.cpp:73-76` [V]). A target with no id (`0xFFFFFFFF`) cannot be
linked: the outside node runs alone, then is lost ("the `<kind>`'s port block has no engine id"), as a link to a block
that does not exist was (revision 2.6).

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
/// Both contexts NodeHost implements on one set of internals, for the one call that needs either (n3).
struct InstanceContexts {
	CreateContext& create;    // what SpecInstance hands the type, as CreateOf<Spec>
	StartContext& legacy;     // what LegacyInstance hands config.start (5.10); interim until 3c
};
class ErasedInstance {
public:
	virtual ~ErasedInstance() = default;
	virtual Configured configure(InstanceContexts& cx) = 0;   // creation's configure (or the legacy start, 5.10)
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
fails `start-failed: <hook> threw: <what>` (or `lost` once rested, 4.2's code by fact), and its configuration is torn
down. The edges (D-L45):

- **`configure` throws, or answers `failed`, after it asked for resources.** Everything it asked is torn down as for
  any configuration "before ready": a recording opened is closed after the instance; a sink asked with `cx.sink` and
  never given a consumer, or given one whose `begin` never ran, closes with nothing to finish (`ThreadSinks::close`
  calls `finish` only on a consumer that began, 5.11); `file`'s `WavWriter::finish` tolerates a null `SNDFILE*`, as
  today's `close` does (`file.cpp:34-38` [V]). The check that exactly one consumer was given (5.9's table) runs only
  on an `applied` answer, so a failing `configure` is never also a `std::logic_error`.
- **`start()` throws.** `started` stays false, so no `stop()` pairs with it (6.4's pairing is with a `start()` that
  returned); the fault fails the node (by `rested`) and the configuration is faded, silenced and torn down.
- **`stop()` throws.** During a stop (intent `stop`) it is a fault, held and applied once the node rests (D-L32). During
  a teardown (restart, disable, removal, failure) facts are dropped (I6), so it is only written: a `log` record from
  source `ear` at level `error`, `stop() threw: <what>`, attributed to the node; the teardown goes on.

### 5.10 The legacy adapter

`NodeConfig::make`'s default (in `ear_core`, not virtual-overridden by any legacy config) returns a `LegacyInstance`
over the config's `start(StartContext&)`; `start` itself keeps its signature and gains a default that throws
`std::logic_error` ("a type on the new SPI has no start"), so `SpecConfig` stops defining it (D-L13). `sendspin`'s
`SendspinConfig` (`sendspin_type.cpp:72-117` [V]) and every hand-made `NodeTypeOf` config in the tests run through it
unchanged:

| Machine step | `LegacyInstance` |
|---|---|
| T2, `configure` | calls `config.start(cx.legacy)`, where `cx.legacy` is today's `StartContext`, implemented by `NodeHost` on the same internals as `CreateContext` and handed in `InstanceContexts` (5.9) (outside requests, handoffs, recordings, latency); a failed `StartResult` is `Configured::failed`; else it keeps the code (unprepared, undriven) and the recipe |
| T3, every resource ready | runs the recipe (`Ready&`) and keeps the run object; a failed recipe is a fault (`start-failed`); then the code is prepared, as for any type. As built (L-3a): `NodeHost` runs the recipe when it finds every resource ready, *before* it tells the machine, and tells `resourcesReady` only if the recipe succeeded, else the fault in `readying`; the machine's `runRecipe` action is then a marker. Told after, a failed recipe would meet a configuration already `prepared` (and `arming`, its drive asked in the same converge) with no drive to complete |
| T5 | the code is driven, after the recipe, never before (today it drove at once, `node_host.cpp:305-306` [V]) |
| `start()`, `stop()` | none: a legacy type has no hooks; a stopped legacy node keeps its run object, connection included (13.1, D-L27) |
| I5 step 3 | its handoffs close on their threads, as today (`node_host.cpp:399-402` [V]) |
| I5 step 4 | `RunObject::close`, off the main loop as today (`node_host.cpp:412-420` [V]) |

`sendspin`'s run builds its threads in `start`, and its recipe only hands them over; it asks no outside node, so its T3
falls in the same turn as T2 and a recipe holding threads is never destroyed unclosed ("Slice 1 as built" [V]): a
teardown before T3 would destroy the recipe, and the threads it captured, without a `close`. The adapter makes that rule
a check, not prose (D-L40): a legacy `StartResult` that carries a recipe and whose start asked for an outside node is a
`std::logic_error` at T2 ("a legacy type with a recipe may ask for no outside node: node-lifecycle.md 5.10"), thrown out
of `NodeHost` as today's type bugs are (`node_host.cpp:126-127` [V]). A recipe cannot say whether it holds threads,
so the check refuses every such pair; no built type has one (`card` and `rtp` return no recipe, `sendspin` asks no
outside node), and the slice 2 cases of `node_host_test.cpp` that pair them are rewritten in L-3a to a recipe without
an outside node, or to an outside node without a recipe (11.1).

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
  accepted frames}` onto the stamp ring; what the ring refuses is dropped and counted (the node's `dropped=`). The two
  rings drop together: the tap first checks the stamp ring for one free entry; with none, it writes no frames and counts
  the whole chunk dropped, so no frames are ever in the ring without their stamp; with one, it writes what the frame
  ring has room for and stamps exactly that many accepted frames.
- **Arming.** The taps ride in the `DriveRequest` (5.3), so the block binds them in the very invoke that applies the
  code and unbinds them in the one that applies the silence: the sink takes exactly the cycles the code drove, by
  construction, with no companion to arm. The block writes each tap after the code, from the inside output bank's
  spans (an unlinked inside output writes into `scratch_`, `port_blocks.cpp:410-413` [V], which the tap reads).
- **Begin** (readiness 4'). When `configure` returned, `NodeHost` asks each sink to begin; the sink's thread calls
  `consumer.begin()`, then posts `begun` through the main-loop `Executor`, the same FIFO `Channel` posts through, so a
  `lost(detail)` the consumer said inside `begin` always lands first (D-L14).
- **Chunks.** The sink's thread pairs stamps with frames and calls `consume(Chunk)` (node-framework.md 9.6's struct) one
  chunk per cycle, or what the ring held of it; `upstreamFrames` is 0 until 3b (D-L15). A chunk whose frames wrap the
  ring's end is consumed in two calls, the second with its stamp's `position` advanced by the first's frames and
  `discontinuity` false, so positions stay exact.
- **Stop and teardown.** At a stop nothing is closed: the sink's thread drains what the ring holds, and the next drive
  binds the same tap again. At a teardown (I5 step 3) `close` drains, calls `finish` on the sink's thread, then reports
  its drops on the main loop, as `Handoff::close` does today (`handoffs.cpp:44-49`, `86-91` [V]). `close` works on a
  sink with no consumer (`configure` failed or threw before `consumedBy`) or whose `begin` failed: it drains nothing and
  calls `finish` only on a consumer that began (5.9's edges).
- **The order of I5's steps in 3a.** node-framework.md I5 closes companions (step 2) before handoffs and sinks (step
  3); in 3a the outside nodes' destruction is queued for the turn's flush while the sinks close on their threads at
  once, so step 3 may finish first. That is harmless: an outside node and a sink never reach each other (a sink reads
  only the block's inside output, which the silence already stopped writing), and step 4 waits for both.

### 5.12 Hooks timed, and the main-loop watchdog

Each hook call is timed with `Timers::now()`; past 20 ms it is `ear.hook-slow` with `node`, `hook` and `ms`, through
`NodeDiagnosticListener` (control-plane.md 4.6 is the record's home).

The watchdog (D-L9) is `StallWatch` in `ear_output`, which depends on nothing of EAR's: a thread that reads an atomic
heartbeat every 250 ms and, past 500 ms without a beat, calls `stalled(ms, where)` once, then `resumed(ms)` when it
moves. `StallWatch` does not know where the main loop is: its constructor takes a `std::function<std::string()>`
`where`, which it calls from its own thread when it reports, and `main.cpp` passes a lambda over
`MainLoopActivity::current()` (`ear_core`), so `ear_output` still sees nothing of EAR's. `main.cpp` arms a 100 ms
main-loop timer that beats it and points its callbacks at `StatusLog`, which encodes `ear.main-loop-stalled` and
`ear.main-loop-resumed` straight onto the output queue from the watchdog's thread (its encoder is safe from many
threads, tested today as "one order from many threads"). As built (revision 2.8): the beat stores the steady clock's
time, so `ear.main-loop-stalled` carries `ms` (the heartbeat's age at the look that saw it) and `in` (control-plane.md
4.6's name: `where`'s text, absent when empty), and `ear.main-loop-resumed` carries `ms` from the last beat before the
stall to the first after it, which that first beat measures itself (a beat that follows the last by more than the bound
records the pair for the watchdog), so neither the look's period nor later beats inflate it. Looks are 250 ms apart, so
the guarantee is: a stall longer than bound + period (750 ms) is always reported, one between 500 and 750 ms may not be;
the detail says what was seen ("no main-loop heartbeat for <ms> ms"). `in` is text, which `DiagnosticField` (whole numbers) cannot carry, so `StatusLog` has
`mainLoopStalled(ms, in)` and `mainLoopResumed(ms)` rather than going through `EngineDiagnosticListener` (a main-thread
interface). The watchdog judges nothing until the first beat (boot before the loop runs is no stall), and `main.cpp`
stops it as soon as `Engine::run` returns (the keeper's last write is no stall). `MainLoopActivity` (`ear/core/main_loop_activity.hpp`, its
own header, landing in step L-3b with the flush that first writes it) holds a string literal naming the call
(`configure`, `pw_filter_destroy`) and a fixed 96-byte copy of the node's name, which `NodeHost` writes before each
hook and each flush item and clears when it returns, under a sequence counter (a seqlock, as sendspin's `Mailbox`,
`mailbox.hpp` [V]), so the watchdog's thread reads a consistent pair without a lock and never follows a pointer into
a node; empty means "between calls" (or, revision 2.8, unknown: a reader yields between tries and gives up after
1,000 of them, so the watchdog never spins on a writer preempted mid-write).

## 6. File by file

| File | Change | Step (12) |
|---|---|---|
| `src/core/include/ear/core/node_lifecycle.hpp`, `node_lifecycle.cpp` | replaced: `BlockMachine`, `ConfigurationMachine`, the aggregate (`NodeAggregate`, its `derive`), `NodePhase`, `NodeState`, `NodeCause`, names; erratum in `batch2.md` section 11. L-1 adds them beside it, in `node_machines.hpp` and `.cpp` (namespace `ear::lifecycle`, since today's `NodeState` keeps its name while `NodeHealth` carries it), tested by `node_machines_test.cpp`; L-3d, which changes `NodeHealth`, removes `NodeLifecycle`, today's `NodeState` and `node_lifecycle_test.cpp`, and moves these into `ear` | L-1, L-3d |
| `src/core/include/ear/core/node_host.hpp` | `PortBlock` as 5.3 (L-2); `OutsideTarget::blockId`, read from `PortBlock::blockId()` (L-3b); `EngineDiagnosticListener`, `leakedRuns` becomes `leaked` (L-3c); `NodeHealth` gains `phase`, `wanted`, `cause`, `waitingFor`, `NodeOperations` with `LifecycleApplied` and `LifecycleRefusal` (L-3d); `SinkAdapter`, `SinkTap`, `Sink`, `ResourceRegistry` takes the sink adapter (L-4b); header note re-cut | L-2 to L-4b |
| `src/core/node_host.cpp` | rewritten (5): the aggregate and the machines' actions with async drive (L-3a); the flush, T1's wait, deferred inputs (L-3b); fades, the three bounds, `settle()`, the wedge (L-3c); health members, `waitingFor` pacing, `NodeOperations`, port-caused replace (L-3d); `CreateContext` and `StartContext` on one set of internals, hooks timed (L-4a); sinks (L-4b) | L-3a to L-4b |
| `src/core/include/ear/core/main_loop_activity.hpp`, `main_loop_activity.cpp` (new) | `MainLoopActivity` (5.12) | L-3b |
| `src/core/include/ear/core/node_machines.hpp`, `node_machines.cpp` | `AddFacts::olderNode` and `NodeAggregate::olderNodeGone()` (4.3's `olderNode`, revision 2.6) | L-3b |
| `src/core/include/ear/core/lifecycle.hpp` (new) | 6.10's 3a classes: `NodeOf`, `CreateOf`, `Configured`, `SinkRequest` (3a members), `SinkShape`, `NodeCode`; `ErasedInstance`, `InstanceContexts`, `CreateContext`, `NodeContext` | L-4a |
| `src/core/include/ear/core/node_type.hpp` | `FailureReason::Code::lost`, `failureCode` (L-3d); `NodeConfig::make` with the legacy default, `start` with a throwing default, `RunCode` renamed `NodeCode` (an alias kept until 3c for `sendspin`) (L-4a); `Chunk`, `SinkConsumer` (as node-primitives.md 6.6) (L-4b) | L-3d, L-4a, L-4b |
| `src/core/include/ear/core/type_spec.hpp` | `SpecConfig::make`, `SpecInstance`, the class's static checks (5.9); `StartResult` construction from a spec code kept for `NodeCode` | L-4a |
| `src/core/include/ear/core/settings_table.hpp` | `Row`'s consteval constructor refuses the key `disabled` ("disabled is the framework's key: no type may declare it"), beside "a setting's key is empty" (`settings_table.hpp:104-108` [V]) | L-5 |
| `src/core/include/ear/core/graph_spec.hpp` | `NodeSpec::disabled` (`bool`, default false), compared by `operator==`; erratum in `m1-white-to-file.md` | L-5 |
| `src/core/include/ear/core/desired_graph.hpp`, `desired_graph.cpp` | `NodeChange {outcome, keys, portCaused}` (L-3d marks the port-caused replace, D-L36); `DesiredNodeListener::nodeChanged` with a default calling `nodeUpdated`; the plan's outcome per node (`restart`, `disable`, `enable`, `stored`; a change of ports or type stays a replace, D-R23, D-L5); `Plan::outcomes()` for the reply (L-5): every node kept by name whose spec changes, a replaced one included with its `disabled` change's outcome (D-R23, D-L5, D-L46) | L-3d, L-5 |
| `src/core/include/ear/core/timers.hpp` | `Timers::now()` (D-L12) | L-3a |
| `src/core/include/ear/core/log.hpp` | `LogEntry::run` removed | L-3d |
| `src/core/include/ear/core/graph_view.hpp` | `NodeView` gains `phase`, `wanted`, `cause`, `waitingFor`, `disabled`, `blockLost` | L-6 |
| `src/core/include/ear/testing/type_harness.hpp`, `node_harness.hpp`, `testing/node_harness.cpp` | rewritten over a real `NodeHost` with in-memory fakes (11.3), 7.6's class whole; `NodeHarness` keeps the by-name calls for legacy types (L-4a); the sink fake (L-4b) | L-4a, L-4b |
| `src/core/include/ear/core/run_requests.hpp` | removed (11.3) | L-4a |
| `src/pipewire/port_blocks.cpp`, `port_blocks.hpp` | 5.3: asynchronous `drive` applied in its invoke, `fadeOut`, `settle()`, one invoke in flight, the fade gain, completions through the block's event source, `takenAt`, `blockId` at STREAMING (L-2); the blocking `drive` removed (L-3a); the interim sinks' taps (L-4b) | L-2, L-3a, L-4b |
| `src/pipewire/handoffs.cpp`, `handoffs.hpp` | `ThreadSinks` beside `ThreadHandoffs` (5.11) | L-4b |
| `src/pipewire/outside_nodes.cpp`, `outside_nodes.hpp`, `sound_cards.cpp`, `rtp_receivers.cpp` | the block end of each link by `blockId` (5.7) | L-3b |
| `src/pipewire/engine.conf` | `context.num-data-loops = 1` in `context.properties` (D-R9) | L-0 |
| `src/pipewire/engine.cpp` | its comment on the data loop (`engine.cpp:227` [V]) says it is set, no longer defaulted | L-0 |
| `src/pipewire/main_loop_executor.cpp` | `now()` | L-3a |
| `src/output/` | `StallWatch` with its `where` callback (5.12) | L-7 |
| `src/nodes/white`, `card`, `rtp` | the class (7) | L-4a |
| `src/nodes/file` | the class (7.4); its spec gains the inside bank `writer` | L-4b |
| `src/nodes/sendspin` | nothing (5.10) | — |
| `src/config/graph_file_model.cpp`, `.hpp` | the reserved key read and written (8): `readNode`, `addNode`, `updateNode` leaving `disabled` alone, `disableNode`, `enableNode`, `dropBlock` | L-5 |
| `src/config/keeper.cpp`, `keeper.hpp`, `main_loop_side.cpp`, `.hpp` | `DisableNode` and `EnableNode` as edits (L-6, with `ear_control`'s `Command`s they carry; L-5 built the model's `disableNode` and `enableNode` they call); `StartNode`, `StopNode`, `RestartNode` as a new step kind needing no file (D-L19), answered by `NodeOperations` through `MainLoopSide`, which maps `LifecycleApplied` and `LifecycleRefusal` to the wire's values (L-6) | L-5, L-6 |
| `src/control/` | `StatusLog`: the new diagnostics and the node-less `diagnostic` record (L-3c); `node` records with `phase` and `cause`, the new sentences, `still`, `LogEntry::run`'s line removed (L-3d); `ear.hook-slow` (L-4a); commands, results with `applied`, the two new refusal codes, `node.state`'s members, `node.updated`'s `applied` and `disabled`, `node.gone`, the protocol frame, `action` records (L-6); the stall records (L-7) | L-3c to L-7 |
| `test/protocol/` | each fixture changes in the step that changes its encoder (9): `output-records.json`'s diagnostics (L-3c), its `node` records and `"run": 2` removed (L-3d), `ear.hook-slow` (L-4a), `action` records (L-6), stall records (L-7); `lifecycle.json`, `unknown-values.json`, `errors.json`, `snapshot.json`, `edits.json` (L-6) | L-3c to L-7 |
| `test/system/` | the seven removal lines and `sendspin_connections_test.cpp:334`'s search (11.7) | L-3d |
| `src/main.cpp` | `leaked()` (L-3c); the sink adapter (L-4b); `NodeOperations` to `MainLoopSide` (L-6); `StallWatch` and its `where` lambda (L-7) | L-3c, L-4b, L-6, L-7 |
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
section 6). Its code counts a gap of positions up to its feed's capacity (about 43 ms) as frames played and skips them
in the feed (`missedFrames`, `sendspin_code.cpp:13-17` [V]), so a stop and a start within 43 ms would have told the
library that frames nobody heard were played. So every silence completed for a stop releases and prepares the code
afresh (4.2's T7 row, D-L33): `prepare` clears `started_` (`sendspin_code.cpp:9` [V]), and the first cycle after the
next drive skips nothing and plays what the feed holds: at most about 43 ms written before the stop.
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
| `node.disable` (new `disableNode`) | the node's own rendered `disabled` setting, if any: refused `shared-definition` when its statement makes more than one rendered directive (`Editing::shared`, D-F11); else its values are set to `true` (`written_directive::set`, so a comment the line owns stays; `replace_with_text` would drop it), which turns `disabled $quiet;` into `disabled true;` and leaves `$quiet`'s `@set` alone (R34). With no such setting: refused `shared-definition` when the node's own statement makes more than one rendered directive (a node line in a snippet used twice, as `updateNode`'s added key is); else `disabled true;` goes first in the node's block as 8.3 pins it (`insertFirst`); with no block, today's `addBlock` makes `{ disabled true; }`. Already disabled: `unchanged` (no candidate) |
| `node.enable` (new `enableNode`) | the node's own `disabled` statement: `shared-definition` as above; else removed, with the comments it owns (knobly's `remove`: a comment on the lines just above it goes with it, as with any removed statement); then, on a fresh load of that text, a block left with no statement and no comment is dropped by `dropBlock`, `addBlock`'s inverse: the `{ ... }` replaced by `;`, comments outside it kept (D-L21). No such line, or one that reads false: `unchanged` |

Each makes a candidate as every edit does, so the plan judges it and the keeper writes it behind.

### 8.3 The knobly layout, and its test

What was unverified was the layout: `insert_statements` puts statements "on lines of their own, indented as the
block's first statement" (`written.hpp:508-509` [V]), so a one-line block (`node card porch { card USB2; playback 2; }`)
may become several lines. The first test of step L-5, before the reader changes, pins what knobly gives for: a
multi-line block, a one-line block, a block whose first statement is a `@use`, a block with a leading comment, and a
node with no block; and that comments survive each. If knobly's result for a one-line block is not acceptable to read
(the test shows it), the designer appends the line last in the block instead, as node-framework.md 15 already allows,
and records it there; no owner question follows, since the meaning is the same.

**As built (L-5, `graph_file_disabled_test.cpp`'s first case) [V].** For a block whose first statement starts on a
line after the `{` (a multi-line block, its first statement with or without a comment above it, or an empty `{` and `}`
on two lines), `insert_statements(0, ...)` writes `disabled true;` on a line of its own, indented as that statement,
above any comment the statement owns; a comment after the `{` stays on its line. For a block whose first statement
starts on the `{`'s line (a one-line block, one that starts with `@use`), knobly breaks the line after the new
statement and leaves the old first statement at column 1 (`node card porch { disabled true;` then `card USB2; playback
2; }`), which is not acceptable to read. So `insertFirst` uses knobly there and, on the `{`'s line, writes the line
textually before the first statement (or before the `}` of an empty one-line block), the node's statement replaced as
`addBlock` replaces it: `node card porch { disabled true; card USB2; playback 2; }`. The line stays first, as D-F10
says, so node-framework.md 15 needs no change. Comments survive in every case.

### 8.4 The plan

`NodeSpec::disabled` is part of the desired state, so `DesiredGraph::plan` sees a changed `disabled` as a change
(`desired_graph.cpp:158-166` [V] compares whole specs). It classifies each changed node (D-L4): type or ports changed →
a replace (as today; on a disabled node, D-R23: told as `node.removed` then `node.added` with `disabled`, the reply
`applied: "stored"`; on an enabled node D-L5: told the same, the reply `applied: "restart"`); `disabled` false to true
→ `disable`; true to false → `enable`; true to true → `stored`; else `restart` (3a has no in-place rows). `commit` tells
`nodeChanged(spec, config, {outcome, keys})`; its default calls `nodeUpdated`, so `StatusLog` and every test listener
keep working; `NodeHost` and the gateway override it.

A replace whose type is unchanged (only the ports changed) is marked `portCaused` on the removal and addition it tells
(as built, revision 2.9: `nodeReplacing` and `nodeReplaced`, which every replace is told through, defaulting to
`nodeRemoved` and `nodeAdded`) (`NodeChange::portCaused`, read by `NodeHost` alone, 5.7; D-L36). An edit that changes the ports and
`disabled` at once (D-L46) is still a replace, the new node added with the new `disabled`, and the reply carries the
`disabled` change's outcome, since that is what the operator asked: ports changed with false to true → `applied:
"disable"` (the new node is added disabled and never configured; its stop, if any, is dropped as a disable drops it);
true to false → `applied: "enable"` (the new node configures afresh and runs; no stop to carry, the disable dropped
it); true to true → `stored`. `NodeHost` carries `stopHeld` into a port-caused successor only when both are enabled.

## 9. The wire in 3a

control-plane.md step L1 is 3a's wire, whole; this page builds it in steps L-3c and L-3d (records), L-4a
(`ear.hook-slow`), L-6 (methods) and L-7 (the stall records), and adds no member. Every fixture changes in the step that
changes its encoder, so each step's `ear_control_tests` (field for field against the fixtures) stay green. What changes
where:

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
- **Fixtures** (`test/protocol/`), by step: `output-records.json` gains `ear.fade-unacknowledged`,
  `ear.barrier-overdue` and the node-less `ear.data-loop-wedged` in L-3c; its `node` records gain `phase` and `cause`,
  one case per 9.1 row 3a has and per `cause.kind`, and its log case loses `"run": 2` (`output-records.json:235` [V])
  in L-3d, with `LogEntry::run` and `status_log.cpp:197`'s line; `ear.hook-slow` in L-4a; the `action` records in
  L-6; `ear.main-loop-stalled` and `ear.main-loop-resumed` in L-7. New `lifecycle.json` (control-plane.md 4.7's list,
  the phases 3a has: everything but `reconfiguring`, `recovering`, `keys` and `next`), `unknown-values.json`, and
  changed `errors.json` (the two new codes), `snapshot.json` and `edits.json` (regenerated: `protocol`, `features`,
  the health members, `disabled`, `applied`) all in L-6, with the gateway's encoders.
- **`web/`**: control-plane.md 8.6 is the home; 3a's minimum is `protocol.ts`'s open enums and new members
  (`protocol.ts:13`, `15` [V]), `graph.ts` folding `node.gone` as it ignores `node.stopped` (`graph.ts:77` [V]) and
  `node.state`'s members, `health.ts` (each phase's line, style and enabled actions), styles for `state-stopped`,
  `state-disabled` (`components/EarNode.tsx:28` builds the class [V]), and the five buttons in the node panel.

## 10. The spikes 3a needs first

Each is a test in `ear_pipewire_tests` (kept afterwards as a regression), on the real in-process engine, merged as step
L-0 before any code depends on it. S1 and S3 gate 3a; S2 gates 3b's companions only, since 3a applies every change in
its invoke (D-L29), and runs in L-0 because its probes are reused by L-2's tests.

### 10.1 S1: asynchronous `pw_loop_invoke` from the main loop

Method: a block whose code counts cycles and flags "inside process"; the main loop posts 10,000 invokes with `block =
false`, in bursts of 1,000 and singly, each carrying a number; each invoked function records its thread, whether
"inside process" was set, and its number, then sets an atomic and signals a main-loop event made beforehand. A malloc
hook on the data thread, as `handoff_test.cpp`'s "write allocates nothing" [V]. Then two stalled phases, the case a
real stall makes: (a) the block's `process` sleeps 2 s once, while the main loop goes on posting single invokes every
millisecond and bursts of 1,000; (b) the data loop is stopped (`pw_data_loop_stop` on a test engine of its own) while
the main loop posts until a post is refused, then started again. Each post's duration is measured on the main thread.

Pass: every call returns a non-negative result before its function ran (once the data thread entered its loop); every
function runs on the data thread, never while any block's `process` runs, in posting order; every completion reaches
the main loop through the event, none lost though signals coalesce; nothing allocates on the data thread; a burst of
1,000 costs the main loop under 5 ms in all. In the stalled phases every post returns within 1 ms; a full queue is
reported by a negative result (its value recorded: the spike pins what PipeWire 1.4.2 returns), never by a post that
waits or allocates on the main loop beyond the queue PipeWire documents; and after the stall every accepted invoke
runs, in order. With 5.3's one invoke in flight per block (D-L38) ear never fills the queue in normal use, but the
spike pins what happens if it does. Fail (a refused post that is not reported, a post that blocks, allocation on the
data thread, reordering): ear's barriers stay blocking (today's `drive`), but batched: one blocking invoke per turn
applies every block's request of that turn, so the main loop waits on the data loop once per turn; the 5 s bound
becomes the watchdog's (a barrier that never returns is a main-loop stall, reported, and ear's stop cannot leave)
(D-L26). The cap of one invoke in flight per block stays in either case.

### 10.2 S2: take-up by position, under an invoke storm (for 3b)

Method: block B (a ramp code), an upstream probe U and a downstream probe D (test-only `pw_filter`s in `ear.graph`,
always processing, in a test-support file of `ear_pipewire_tests` that L-2's `port_block_test.cpp` reuses), linked
U → B → D. B implements 9.6's deferred take-up (a test-only variant of 5.3: see a request in cycle `P - 1`, publish
`P`, apply at `P`). A second thread posts no-op invokes to the data loop as fast as it can for the whole test. The main
loop drives and silences B alternately, 1,000 times, at random intervals of 1 to 30 cycles. Each of U, B and D records,
per cycle, `clock.position`, an atomic sequence number taken on entry, and, in D, the samples received and B's
`takenAt`.

Pass: the three record identical position sequences (a block and its peers see one `clock.position` per cycle); in
every cycle U's entry precedes B's and B's precedes D's; D receives the ramp from exactly its first sample at each
take-up to exactly its last before each silence's take-up, with no gap and no trailing zero. Fail: 3a is unaffected
(its sink is inside the block and binds in the same invoke as the code, 5.11); 3b keeps every ring companion's
realtime side inside the block (the sink as 3a builds it, the source as today's feed moved into framework code), and
9.6's arming by position is dropped (D-L25). Recorded in node-framework.md 15.

### 10.3 S3: one data loop

Method: `context.num-data-loops = 1` in `engine.conf`; make two blocks, a test card (`clocklessCardProperties`) and an
RTP receiver, and wait until each runs. Read the context's `context.num-data-loops` property; list the process's
threads (`/proc/self/task/*/comm`) and count those PipeWire names `data-loop.<n>`; record the thread id each block's
`process` runs on. Pass: the property reads 1, exactly one `data-loop` thread exists once all four run, and both
blocks' `process` run on it. A node's `node.loop.name` property is not relied on, since whether PipeWire 1.4.2 writes
it on an in-process node's info is itself unverified; with one data-loop thread in the process every node necessarily
runs on it. Fail (two data-loop threads): name the loop explicitly (`node.loop.name`) on every adapter's and module's
properties, and keep the test.

### 10.4 S4 (slice 3d, planned here): ports on a connected filter

node-framework.md 6.7 states what it checks; the plan: a test type with two input banks and an inside bank, a running
ramp through a second block beside it, then `pw_filter_add_port` in the middle of the input list and
`pw_filter_remove_port` of a linked port, 500 times. Pass: the beside ramp never loses a sample; new ports appear in
the registry and link; a removed port's links go; a port added while links exist carries audio; and a code reading of
PipeWire 1.4.2's `filter.c` (`pw_filter_add_port`, `pw_filter_remove_port`, the port list the data loop's `process`
walks, and how each change reaches the data loop) shows the list is changed only inside an invoke on the data loop or
before the port is visible to it, its citations written into the test's header comment (D-L43). ThreadSanitizer is
not a criterion: the image's libpipewire is not instrumented, so TSan cannot see `filter.c`'s accesses, and a
PipeWire built with TSan in a throwaway image is a cost 3d may choose if the code reading leaves a doubt. Fail: a
reshape stays a replace (D-B5).

### 10.5 Results (step L-0, 2026-11-08)

Built as `src/pipewire/tests/spikes_test.cpp` (the malloc hook moved to `allocation_count.cpp`, shared with
`port_block_test.cpp`), each run 10 times on the dev image; the whole suite 603/603. PipeWire 1.4.2's sources read for
the facts below: `spa/plugins/support/loop.c`, `pipewire/context.c`, `pipewire/filter.c`.

| Spike | Verdict | Measured |
|---|---|---|
| S1 | Pass; D-L26 not needed | 13,000 invokes (bursts, singles, and 3,000 during a 2 s stall) ran in posting order, on the data thread, never inside any `process`; every post returned the async code, never the function's result; completions all reached the main loop (about 2,000 coalesced events); no allocation on the data thread; a burst of 1,000 cost the main loop 0.3 to 0.9 ms; the slowest post during the stall 0.04 to 0.81 ms |
| S2 | Take-up and sample exactness pass; the literal "identical position sequences" fails under xruns | 1,000 requests a run under a storm of about 490,000 invokes a second: every one taken up once, in order, `takenAt` one cycle after publication; U, B, D entered in link order in every cycle they all ran (0 misordered of about 17,845); D's ramp exact at every take-up and silence. But 1 to 17 cycles a run were run by only an upstream prefix (U, or U and B), at driver resyncs and xruns. On a loaded host (CI, 2026-11-08; locally with 6 busy loops on 4 cores) 500 to 4,600 cycles a run were cut short, and clock.position stepped back up to 11 times a run, by up to 438 frames, with the whole graph running positions again (below) |
| S3 | Pass in substance | the property reads 1; one `data-loop.0` thread in the process; a filter's `pw_filter_get_data_loop` is that loop; adapters and the rtp module name `data-loop.0` in their info, while `pw_filter`s name no loop (so `node.loop.name` is set only by a node's maker) |

What the spikes changed in this page's criteria (the kept tests assert the corrected ones):

- **A full invoke queue is never refused.** In 1.4.2 a full 32 KiB queue (about 455 items) makes the *posting* thread
  `calloc` an overflow queue and go on; `flush_all_queues` keeps posting order across queues. So S1's "a full queue is
  reported by a negative result" cannot happen. Corrected: posts never fail or wait; a full queue costs the posting
  thread one allocation per 32 KiB; the data thread allocates nothing.
- **"Returns before its function ran" was ill-posed**: the data thread runs concurrently, and 169 to 1,286 of 13,000
  functions had run before their post returned. Corrected: the post's result is the async code, and the function never
  runs on the poster. A loop whose thread is not running (not started, or stopped) runs the function inline on the
  caller and returns its result: a second test pins this, and it is why completions are posted, never run inline
  (D-L30).
- **An unbounded producer wedges the data loop.** S2's storm "as fast as it can" outran the flush: the data loop spun at
  100% CPU in `flush_all_queues`, never ran `process` again, and memory grew. The kept storm posts bursts of 100 with at
  most 2,000 outstanding. This confirms D-L38 (one invoke in flight per block) as a requirement, not a nicety: ear
  bounds every producer of invokes.
- **A cycle cut short by an xrun is run by an upstream prefix only.** S2's corrected criterion: in every cycle all three
  run, they see one position, in link order; a cycle cut short is run only by an upstream prefix (U, or U and B), never
  by a downstream node without its upstream. For 3a this changes nothing (the sink is inside the block). For 3b it
  means a *source* companion, upstream of its block, can run a cycle its block never runs, so a source must not count
  a chunk as played until its block took it up (its `takenAt`); 3b's design either adopts that rule or takes D-L25
  (ring companions' realtime side inside the block). Recorded as open in 16 and node-framework.md 15.
- **clock.position steps back, and the whole graph runs a position again.** Found on CI (2026-11-08), where S2 failed
  twice (8 D cycles "heard zeros, wanted the ramp"), and reproduced with busy loops beside it. A starved data loop reads
  the dummy driver's timer late; past `resync.ms` (480 frames) behind the raw clock the driver resets the position
  forward to it (`node-driver.c` `on_timeout`), but its next timeout, computed from the late one, is due at once, and
  the timeouts it fires back to back each add a block until the position is `resync.ms` *ahead* of the raw clock,
  when it resets back to it (the log shows the pair: `err -665 ... resetting`, then `err 519 ... resetting` 1 ms
  later). U, B and D had run those run-ahead positions as whole cycles, so after the step back they run positions
  below ones they already ran, and B's swaps happen at positions below those cycles. The first S2 compared D's samples
  with the swaps by position, which put a swap made after a step back in force in cycles that ran before it: the model
  was wrong, not the take-up (B and D agreed sample for sample in every cycle). Corrected criterion: cycles are told
  apart by the order they ran in (the shared entry sequence: a cycle is U's number, then B's, then D's, straight after
  each other, at one position), never by position; prefix semantics for cycles cut short as above; D hears the ramp
  from exactly the cycle of each drive's swap to exactly the cycle before the next silence's swap, in the order the
  cycles ran; D sees `takenAt` in the cycle B published it, when D runs that cycle; and no cycle starts more than
  `resync.ms` ahead of the raw clock (the bound the driver keeps). Exact everywhere else: no tolerance. For 3a: a
  take-up published just before a step back waits until the position reaches `takenAt` again (30 to 215 of 1,000 a
  run were swapped after `takenAt`, by a jump forward or back, under that load), so a drive's take-up is late by up
  to `resync.ms` plus a block, never early. For 3b: a companion that keys anything on `clock.position` (a chunk played "at" a position) sees positions
  repeat, and must key on the cycle instead.
- **S3's literal "every node names the loop" fails for `pw_filter`s.** Corrected: the property reads 1; the process has
  one data-loop thread; a block's filter loop is the context's loop; any node that names a loop names that one. The
  fallback (naming the loop on every adapter and module) is not needed.
- **Three deviations, all in the test's favour of running on CI:** S2 runs at a 64-frame block (24 s instead of 95 s;
  at 256 frames its load made two load-sensitive sendspin clock tests fail in the parallel suite), its storm is
  bounded as above, and it runs alone: its CTest test has `RUN_SERIAL` (`ear_catch_tests(... SERIAL [s2])`,
  `test/CMakeLists.txt`), since even at 64 frames it kept a CI core busy while the realtime-timed tests ran beside it
  (a sendspin system test, stable before S2 was added, lost 10 ms of its ramp in the same suite on 2026-11-08); it adds about 25 s to the suite's wall time.
- After a 2 s data-loop stall the dummy driver resyncs (an error of about -96,000 frames) and then resets about 70
  times within 7 ms (`node-driver.c on_timeout ... resetting`); 5.4's bounds already treat a stall as no wedge while
  the loop answers.

## 11. Tests

Names are the test executables of AGENTS.md; today's counts are `TEST_CASE`s [V]. 13.2 of node-framework.md says each
built group's fate; this section names the cases.

### 11.1 `ear_core_tests`

- `node_lifecycle_test.cpp` (7, rewritten; in L-1 the new cases are `node_machines_test.cpp`, renamed at L-3d, 6):
  "Every event in every block state moves the block machine as its table says" (4.1, one table row per case row);
  "Every event in every configuration state ... as its table says" (4.2);
  "The phase, state, wanted and cause derived for every combination of the machines and facts" (4.3); "Every
  operation in every phase answers and moves as section 4.4 says"; "The newest intent wins: remove over disable over
  fail over restart over stop, only while one is in flight"; "A stop after a completed restart is a plain stop"; "A
  change restarts a settled configuration and waits for an unsettled one"; "A failed node configures again only when a
  retry is asked"; "A fault during a stop's fade is held and fails the node once it rests, and is dropped during a
  teardown"; "A block lost with no failure derives failed, and disabled with blockLost on a disabled node"; "Every
  configure enters readying, so configuring is derived even with nothing pending"; "The failure code is chosen by the
  fact: engine-refused always for the engine, start-failed or lost by rested for the node"; "Phases, states, causes and
  failure codes have their names".
- `node_host_test.cpp` (47): as node-framework.md 13.2 says, with these new cases: "A drive's completion arrives later
  than the call, and start() runs only then"; "A stale completion (an older generation, an older barrier, a removed
  node) changes nothing"; "Every output the code writes is faded before a silence, and a sink's bank never"; "A fade
  unacknowledged for 100 ms is silenced unfaded, with ear.fade-unacknowledged"; "A barrier overdue for 5 s marks the
  data loop wedged: every later teardown is leaked and no PipeWire call is made"; "A node added after the wedge fails
  engine-refused without a block"; "PipeWire's object changes of one turn run together, destroys first"; "A name added
  back gets its block only once the older block is destroyed, in the same flush"; "Removal silences before it destroys
  the block"; "stop() runs exactly once after every start(), for a stop, a disable, a restart, a removal and a failure,
  and never after the block's loss"; "A stop keeps the configuration: no hook, no resource released, and start drives it
  again"; "A node configured while stopped never calls start()"; "stop in starting with a drive in flight ends stopped
  without start() or stop()"; "start during a stop starts again once stopped"; "start on a failed node retries and
  clears the stop; stop on a failed node changes only wanted"; "disable tears the whole configuration down, keeps the
  block and drops the stop; enable configures afresh"; "A node added disabled is never configured"; "start, stop and
  restart on a disabled node are refused node-disabled"; "node.restart in every phase replies as control-plane.md 7.6's
  table"; "A failed node is torn down at once and keeps its block"; "lost(detail) is start-failed before the node rested
  and lost after"; "A hook that throws fails the node and the host goes on"; "A hook past 20 ms is ear.hook-slow"; "A
  sink with no consumer or two at configure's return is a type bug"; "A sink's consumer begins before the node is
  configured, and a lost said in begin fails it start-failed"; "A legacy type runs under the new machines: start at T2,
  recipe at T3, driven at T5, stopped keeping its run object, disabled and removed with its close at step 4"; "A health
  notification for every change of any member, in order with the commit's events"; "A still record after 2 s, then at
  most every 10 s while waitingFor changes"; "waitingFor reaches health only after 2 s in the phase, then changes at
  most once a second" (D-L42); "Stopping 16 nodes posts every fade at once". Revision 2 adds: "A block that leaves
  STREAMING with a drive or a fade in flight still completes it, and the loop is not judged wedged" (B1: the fake block
  goes PAUSED mid-request); "A bound that fires before its completion was dispatched completes it from settle() and
  judges nothing" (M1: the fake block completes, the event is held, the timer fires first); "start on a failed node
  configures it again once the failed configuration is gone; so do restart and a committed change" (M2); "Every phase a
  turn passes is told, in order: a restart of a stopped node tells stopping, configuring and stopped" (M3); "A card lost
  during a stop's fade fails the node once it rests, without a card; lost during a disable's teardown it is dropped"
  (M5); "A change of ports keeps the node's stop and one health stream for the name, the old node telling nothing" and
  "A hand edit that keeps a node keeps its stop; one that renames it drops it" (M6, m13: moved here from
  `ear_config_tests`, since the stop is `NodeHost`'s); "A stop after a completed restart fades and rests, never tears
  down" (M7); "A change committed during a stop's teardown, then start: the restart runs before any drive" (m10);
  "stopAll after the wedge reaches allGone at once and makes no PipeWire call" (m5); "An outside node ready told inside
  its making, during the flush, is handled after that flush item, and a block it frees is made in the same flush" (m6);
  "A legacy type with a recipe that asks for an outside node is a std::logic_error at T2" (5.10, D-L40); "A restart from
  configuring, configured and starting goes through stopping"; "A node removed by an edit refuses start, stop and
  restart unknown-node; during stopAll every operation is stopping" (m4); "configure that throws after cx.sink tears
  down with no consumer to finish; start() that throws gets no stop(); stop() that throws during a teardown is a log
  record" (m9); "A drive after a stop's silence runs on a code released and prepared afresh" (m8). The two order tests,
  "A node acquires resources and its events only inside start" and the two loss cases are rewritten as 13.2 says; the
  handoff cases move to sinks and stay for the legacy adapter.
- `desired_graph_test.cpp` (17): "disabled is part of a node's spec: false to true is disable, true to false enable,
  true to true stored"; "A change of ports on a disabled node is a replace with the outcome stored"; "nodeChanged is
  told with its outcome and keys, and its default calls nodeUpdated".
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

`TypeHarness<spec, Type>` and `NodeHarness` run one real `NodeHost` over in-memory fakes (D-L11): a manual executor for
the main loop (pumped by each harness call), a fake clock and `Timers`, a fake `PortBlockFactory` whose block runs the
driven code over the harness's buffers (outputs NaN-filled first, as today), applies the fade, writes the interim sinks'
taps and completes requests as 5.3 says, `settle()` included (at once with `autoComplete`, else on `acknowledgeFade()`
and `completeBarrier()`), fake card and receiver adapters by bank, the test's `RecordingAdapter`
(`Options::recordings`), and a fake sink adapter that calls `begin` at readiness and hands each chunk straight to its
consumer on the test's thread. So the harness and `NodeHost` cannot disagree; slice 2's shared `run_requests.hpp`
(`node_host.cpp:21` [V]) becomes unnecessary and goes.

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
make no version, and work while the file has errors"; "LifecycleApplied and every LifecycleRefusal map to the wire's
applied values and codes" (through a fake `NodeOperations`). The stop kept through a hand edit is `NodeHost`'s and is
tested in `node_host_test.cpp` (11.1).

### 11.5 `ear_control_tests`

`lifecycle.json` against the encoders and the gateway; `output-records.json`'s new cases field for field and in order;
the five commands decoded and their replies encoded; `node.gone` after `node.stopped`; `unknown-values.json`;
`errors.json`'s new codes; `StatusLog`'s `still` pacing; the stall records written from another thread in order with
the main thread's.

### 11.6 `ear_pipewire_tests` and `ear_output_tests`

S1 to S3 as regression tests; `port_block_test.cpp` (13): "drive returns at once and its completion arrives on the main
loop" (today's "drive returns only when the old code can no longer run" is rewritten: "after the completion no process
of the old code runs"), "drive on a block whose engine node is gone completes by signal, never inline", "A block that is
PAUSED takes a drive in its invoke and completes it", "A fade-out reaches exactly 0 over 5 ms and is acknowledged in the
cycle it did", "A fade-out on a block that is not processing, or that leaves STREAMING mid-fade, is acknowledged by
signal", "A cycle too large to run counts as one that ended at zero", "Every drive fades in over 5 ms and cancels a
pending fade-out", "The published position is where the new code's first sample is", "settle() runs a completion whose
event has not been dispatched", "A thousand requests to one block keep one invoke in flight" (D-L38).
`sound_card_test.cpp` (6): "to_card's last 5 ms before a silence decay to zero, at a stop, a restart, a disable and a
removal". `engine_test.cpp` (7): "One data-loop thread runs every block, card and receiver" (S3), "Stopping 16 nodes
costs about one fade, one cycle and one batch of destroys" (measured and printed; bounded at 2 s). `sink_test.cpp` (new,
from `handoff_test.cpp`'s five, which stay for the legacy handoff): order, drops counted, close drains then finishes, no
allocation on the tap, begin before the first consume, and "A ramp through a block's sink starts at the first driven
sample and ends at the last, across a stop and a start, under an invoke storm". `ear_output_tests`: "StallWatch reports
a heartbeat older than its bound once, then its resumption" (and its judge on a clock stepped by hand, steady beats,
before the first beat, after stop); `main_loop_executor_test.cpp`: "The main-loop watchdog reports a stall of the
engine's main loop with where it was, then its resumption" (node-framework.md 13's row for the engine tests).

### 11.7 `ear_system_tests`, and the editor

node-framework.md 13.2's and control-plane.md L1's system tests, whole; the seven removal lines wait for `node <n>
removed dropped=0` (`white_to_file_test.cpp:52`, `graph_file_test.cpp:103`, `rtp_test.cpp:116`,
`sendspin_connections_test.cpp:181`, `sendspin_floods_test.cpp:180`, `sendspin_pairing_test.cpp:66`,
`sendspin_streams_test.cpp:75` [V]), and `sendspin_connections_test.cpp:334`'s search for `node ma stopped` becomes
`node ma removed`, all changed in L-3d with the record; and
`control_port_test.cpp:183-193`'s `node.stopped` waits also see `node.gone` after it. Vitest: `lifecycle.json` through
the reducer; `health.ts` for every phase.

## 12. The cut: steps that each merge green

Each step changes every fixture and system-test line its own change of an encoder or record breaks (9, 11.7), so every
step's `ctest` and `make web-test` pass on their own.

| Step | What | Risk |
|---|---|---|
| L-0 | S1, S2, S3 (10.1 to 10.3) as tests; `context.num-data-loops = 1` | S1 fails: the fallback is designed (D-L26) and changes 5.3's posting and 5.4's bound before L-2 starts. S2 fails: 3b's design changes (D-L25), 3a does not |
| L-1 | The pure machines and the aggregate's derivation in `ear_core`, tested by table; `NodeLifecycle` still used by `NodeHost` | Low: no wiring; a table found wrong against node-framework.md 6.3 is fixed there first |
| L-2 | `PortBlock`'s asynchronous `drive` applied in its invoke, `fadeOut`, `settle()`, one invoke in flight per block, completions, `takenAt`, `blockId`, in `FilterPortBlock`; today's blocking `drive(AudioCode*)` kept beside them, used by the old `NodeHost` | The fade's ramp and acknowledgement on the data loop, and a fade on a block leaving STREAMING; `port_block_test.cpp`'s new cases, with S2's probes reused |
| L-3a | `NodeHost` on the machines with the asynchronous drive, every type on the old SPI through the legacy adapter (5.10, its recipe check included); `Timers::now()`; a failed node torn down at once (the machines' T14 does it, so it lands here, not in L-3c); no fades, no flush, no wedge, the old `NodeHealth` and records: behaviour identical to today's, the blocking `drive` removed | Every node's start and teardown now waits for a completion; a missed one hangs a node (no bound yet). Mitigated: L-1's tables and the rewritten `node_host_test` cases; every system test unchanged and green |
| L-3b | The per-turn flush (destroys first, re-entrancy, deferred inputs), T1's wait for an older block, the join by `blockId`, `MainLoopActivity` written | A name added back and a ready told inside a make (m6); the flush's cases in `node_host_test`, `sound_card_test` and `rtp_receiver_test` linking by id |
| L-3c | Fades before every planned silence and the fade-in, the three bounds with `settle()`, the wedge and `EngineDiagnosticListener`; `leaked()`; `StatusLog`'s three new diagnostics with their `output-records.json` cases | A bound tuned too tight for CI (11.6's 16-node test measures it); a fade on a block that stops processing (B1's cases) |
| L-3d | `NodeHost` honours `disabled` (from L-5: `nodeChanged` overridden for `disable`, `enable`, `stored`; a node added disabled is never configured); `NodeHealth`'s members, health told per transition, `waitingFor` pacing, `NodeOperations` (core's types, not yet on the wire), the port-caused replace's continuity (with `NodeChange::portCaused` from the plan), the failure code `lost`; `StatusLog`'s new sentences (`removed dropped=`, `stopping: <cause>`, `stopped`, `disabled`, `still`); `LogEntry::run` removed; `output-records.json`'s `node` cases and `"run": 2`; the seven system-test removal lines | The records every script reads change; the fixtures and system lines change in this same step |
| L-4a | The new SPI: `lifecycle.hpp`, `InstanceContexts`, `CreateContext`, `SpecInstance`, hooks timed (`ear.hook-slow` and its fixture case), the harness over `NodeHost`; `white`, `card`, `rtp` moved; the compile-fail cases; `run_requests.hpp` removed | The harness and `NodeHost` must agree (they are one); `card`'s system test on the test card is the gate |
| L-4b | The interim sink: `SinkAdapter`, `SinkTap`, `ThreadSinks`, `Chunk`, `SinkConsumer`, the harness's sink fake; `file` moved | `file`'s recording path is the first audio path through new code: its system test (exact samples) and `sink_test.cpp`'s ramp are the gate |
| L-5 | `disabled`: the `Row` refusal, `NodeSpec::disabled`, the reader and writer (8.3's pin first), the plan's outcomes, `nodeChanged`. `NodeHost` honouring `disabled` (a hand edit disables and enables) moved to L-3d (revision 2.3): L-5 was built while L-3a rewrote `NodeHost` | knobly's layout (8.3); the plan's outcome reaching listeners in the commit's order |
| L-6 | The wire (9): commands, the keeper's no-file step, `MainLoopSide` mapping `NodeOperations`' replies, the gateway's members, the protocol frame, `node.gone`, `action` records, `lifecycle.json`, `unknown-values.json`, `errors.json`, `snapshot.json`, `edits.json`, the batch2.md erratum, `rpc-call.mjs --until`, `image-smoke.sh`, the editor's minimum | A frozen header and the web's frozen `protocol.ts` change through their errata; the reducer must take unknown values (`unknown-values.json`) |
| L-7 | `StallWatch` with its `where` callback, wired in `main.cpp` over `MainLoopActivity`; the stall records and their fixture cases | A false stall report on CI's loaded host; the bound is tuned by 11.6's measurement |
| L-8 | 3a's remaining system tests (stop, disable across a restart of ear, `$quiet`, restart of a card on the test card, a held UDP port's `still` record); AGENTS.md, the two skills; node-framework.md's "Slice 3a as built" | Flaky timing under load; every test is event-gated (AGENTS.md) |

## 13. What stays interim, and how it goes

| Interim | Until | How it goes, exactly |
|---|---|---|
| The legacy adapter: `LegacyInstance`, `NodeConfig::start` and its throwing default, `NodeConfig::make`'s default, `StartContext`, `StartResult`, `RunRecipe`, `Ready`, `RunObject`, `HandoffRequest`, `RtWriter`, `HandoffConsumer`, `HandoffAdapter`, `ThreadHandoffs`, `ResourceRegistry`'s handoff slot, the `RunCode` alias, `NodeTypeOf`'s config path | 3c, when `sendspin` moves onto the class in one step | each deleted; `NodeConfig::make` becomes pure; the legacy rows of `node_host_test.cpp` and `sendspin`'s `notYetInHarness` entry go; `handoff_test.cpp` goes, its cases already in `sink_test.cpp` |
| The interim sink's realtime side inside the block (`DriveRequest::sinks`, `SinkTap`, the block's writes after the code) | 3b | `ThreadSinks`' ring moves behind a ring companion's `pw_filter` on `joinOutsideNode`; `DriveRequest` loses `sinks`; the companion compares its cycle's position with the block's `takenAt` (S2); `cx.sink`, `consumedBy`, `SinkConsumer`, the harness's sink fake and `file` do not change |
| Every change applied inside its invoke, and the wedge judged on a barrier's invoke alone (D-L29, D-L30) | 3b | a block with a companion defers take-up to the cycle it publishes (9.6): its invoke records the request and the next `process` publishes `P` and applies at `P`; on every transition out of STREAMING the main loop posts an invoke that applies whatever is deferred; a barrier overdue posts a no-op probe invoke and the loop is wedged only if the probe has not run within 1 s more. `PortBlock`'s interface does not change |
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
| D-L6 | `PortBlock` in 3a is 5.3's: asynchronous `drive` and `fadeOut`, each change applied inside its invoke (D-L29), completions through the block's own event source and an atomic, `settle()` | Meets I4 and D-R7 with one code path; a lost or idle block can never be judged a wedged loop (revised in revision 2: revision 1 deferred take-up to a cycle, round 1's B1) |
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
| D-L29 | In 3a the block applies every request inside its invoke, between two cycles, binding the sinks' taps in the same function, and publishes `takenAt` as the next expected position; take-up deferred to a published cycle is 3b's, for companions, and then a block leaving STREAMING applies what is pending (B1) | Invokes run between cycles, so I4 holds without waiting for one; with the sink inside the block, the code and the tap change in one step and the sink is exact; a block that stops processing (PAUSED) can no longer park a request. Position-based arming (node-framework.md 9.6, F3) exists because companions are separate engine nodes, which 3a does not have |
| D-L30 | Every bound first calls `settle()`; the data loop is judged wedged only when a barrier's invoke has not run in 5 s, never on one block's silence; from 3b a probe invoke confirms (B1, M1) | A main-loop stall can fire a timer before the completion's event; a barrier applied in its invoke is a fact about the loop; a fade waits for cycles and judges only its block |
| D-L31 | A failed node configures again only when `retryAsked` is set, by `start`, `restart` or a committed change (M2) | Rule 1 must not configure a failed node by itself (slice 8's recovery does that), and must configure it when asked |
| D-L32 | A fault during a stop's fade or silence is held and applied once the node rests; during a teardown it is dropped (M5) | A stop keeps the configuration, so a fault against it must not vanish (a card unplugged during a stop would rest `stopped` with no card); a teardown discards the configuration anyway (I6) |
| D-L33 | Every silence completed for a stop releases and prepares the code afresh (m8) | `AudioCode`'s contract allows it (prepare before the first process, release after the last); a code that keeps state across cycles (sendspin's `missedFrames`) never takes a stop's gap for frames played |
| D-L34 | Health is told after every machine transition that changes a derived member; `configuring` is always told, since every `configure` enters `readying`; the exceptions are T19 in the same turn and a port-caused successor (M3) | control-plane.md 4.8: one notification for every change, never coalesced, and a normal start is four `node.state`s; telling once per input hid intermediate phases |
| D-L35 | `NodeOperations` replies with core's `LifecycleApplied` and `LifecycleRefusal`; `MainLoopSide` maps them to the wire (M4) | `ear_core` depends on nothing but nlohmann/json and names no wire code (AGENTS.md) |
| D-L36 | A replace whose type is unchanged is marked port-caused; `NodeHost` carries the stop, `dropped` and one health stream into the successor, the predecessor telling nothing; any other predecessor of a name added back tells `StatusLog` only (M6) | D-F4: a node of the same name and type keeps its stop whatever changed; the wire keys health by name, so two streams for one name would corrupt the client's node |
| D-L37 | An intent lives only while its stop or teardown is in flight; priority applies only between intents in flight (M7) | A stop after a completed restart must be a stop, not a teardown |
| D-L38 | At most one invoke in flight per block, requests in an 8-entry SPSC ring the invoke drains (M9) | The loop's invoke queue then holds at most one entry per block whatever the main loop does, so a stalled loop never fills it |
| D-L39 | The flush iterates a snapshot; items caused during it run after the current item in the same flush; adapter events told inside a flush item are deferred to an input of their own (m6) | No input is handled inside a PipeWire call, and a block freed by a destroy is still made in the same flush |
| D-L40 | A legacy `StartResult` with a recipe whose start asked for an outside node is a `std::logic_error` at T2 (5.10). **Superseded by node-lifecycle-modules.md D-M3 (L-4a review, 2026-11-09):** it fails the node `start-failed` with the same text, never thrown out of `NodeHost` | A teardown before T3 would destroy the recipe's threads unclosed; a recipe cannot say whether it holds threads, so the pair is refused; no built type has it |
| D-L41 | At rest, a pending change restarts before any drive (m10) | A change committed during a stop's teardown and a `start` after it must drive the new settings, not the old code |
| D-L42 | `waitingFor`'s 2 s delay and once-a-second pacing are `NodeHost`'s, so every health listener gets the same paced value; `StatusLog`'s `still` paces itself at 10 s (m11) | control-plane.md 4.8's rule is about health, which `NodeHost` makes; one place, one test |
| D-L43 | S4's criterion is functional checks plus a cited code reading of `filter.c`; ThreadSanitizer is not a criterion (m12) | TSan cannot see accesses inside the image's uninstrumented libpipewire, so a clean TSan run would prove nothing |
| D-L44 | Diagnostics with no node go through `EngineDiagnosticListener` (5.4) (m5) | `NodeDiagnosticListener` is per node; `ear.data-loop-wedged` is control-plane.md 4.6's node-less record |
| D-L45 | A `stop()` that throws during a teardown is a `log` record; during a stop it is a held fault; a `start()` that threw gets no `stop()` (m9) | I6 drops facts once a teardown began, but the throw must still be seen; 6.4 pairs `stop()` with a `start()` that returned |
| D-L46 | An edit changing the ports and `disabled` at once is a replace whose reply carries the `disabled` change's outcome (n5) | What the operator asked is the disable or enable; the ports change is invisible beyond `node.removed` and `node.added` |

## 15. Owner gates

None. Every choice here is internal or follows an owner's answer already given (R25 to R34); the wire-visible ones
(D-L2, D-L5, D-L36's one health stream per name, D-L46's reply) follow control-plane.md 7.6, 4.8 and D-R23, and D-F4.

## 16. Open items

- [V] S1 and S3 (10.1, 10.3, results in 10.5): asynchronous invokes pass, a full queue is never refused (the poster
  allocates an overflow queue), one data-loop thread. Step L-0 done; 3a may proceed.
- Open for 3b (10.5): S2 passed take-up and sample exactness, but a cycle cut short by an xrun is run by an upstream
  prefix only, so a source companion can run a cycle its block never runs. 3b's design either counts a source's chunk
  as played only once its block took it up, or takes D-L25 (ring companions' realtime side inside the block). And
  `clock.position` steps back on a loaded host, the whole graph running positions again, so nothing may key on a
  position as unique.
- [V] knobly's layout of `insert_statements` into a one-line block (8.3): not acceptable on the `{`'s line, so that
  case is written textually; pinned by L-5's first test.
- [V] `pw_filter_get_node_id` returning the block's global id once STREAMING, in PipeWire 1.4.2 (5.7): L-2's test finds
  it among the registry's ids for the name and a link made by it (link-factory, as `joinOutsideNode` makes one) reaches
  that block and not an older block of the same name.
- [V] That PipeWire runs a loop's invokes while a node on it is PAUSED or its driver gone, so a request applied in its
  invoke completes without a cycle (5.3, D-L29): L-2's test suspends a block through its node proxy (SUSPENDED, its
  filter PAUSED, no cycle of it while other blocks run) and its drives and fade-out complete, two drives at the same
  position.
- [U] The three bounds against CI's stalls (5.4; node-framework.md 15). Step L-3c and 11.6's 16-node test.
- [U] That `start` after `stop` drives again within a cycle and one turn, with the sink rebound in the same request
  (node-framework.md 15). Step L-4b's `sink_test.cpp` ramp.
- The fade (5 ms), the hook bound (20 ms), the watchdog's (500 ms) and the per-block request ring (8) are first
  choices, as node-framework.md 15 says.
- A legacy `sendspin` node started again plays up to about 43 ms its feed held from before the stop (7.5), since the
  legacy feed is not cleared at a stop; accepted until 3c moves it onto a source, which is. It never reports that gap
  as played (D-L33).
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

### Round 1 (revision 1)

One independent review. Every finding was checked against the code at `cc02fee` first; each is fixed or declined with
its reason.

| # | Finding | Severity | Outcome |
|---|---|---|---|
| B1 | Deferred take-up parks a request when the block leaves STREAMING (PAUSED, not a loss): no completion, the 5 s bound, the whole data loop judged wedged | blocker | Fixed: 3a applies every request inside its invoke, sinks bound in the same function, `takenAt` the next expected position (5.3); deferred take-up kept for 3b's companions, with an invoke applying what is pending on leaving STREAMING (13); the wedge judged only on the loop's own sign of life (5.4). node-framework.md I4, 6.3 and 9.6 follow (revision 8.2) (D-L29, D-L30) |
| M1 | A main-loop stall can fire the 100 ms and 5 s timers before the completion's event is dispatched: a false wedge | major | Fixed: `PortBlock::settle()`, called by every bound before it judges (5.3, 5.4; D-L30) |
| M2 | Rules 1 and 3 deadlock a failed node's retry: it never reconfigures | major | Fixed: `retryAsked`, set by `start`, `restart` and a committed change on a failed node; rule 1 configures on it and clears it with `failure` (4.3, 4.4; D-L31) |
| M3 | Health told once per input loses intermediate phases (a restart of a stopped `white` sends no `node.state`) | major | Fixed: told after every transition that changes health, with T19's and a port-caused successor's exceptions; `configuring` always told, the `none` to `prepared` shortcut removed (4.2, 4.3, 4.5, 5.2; D-L34) |
| M4 | `NodeOperations` in `ear_core` returned `ear_control`'s refusal | major | Fixed: core's `LifecycleApplied` and `LifecycleRefusal`, mapped to the wire by `MainLoopSide` (5.2; D-L35) |
| M5 | A fault during a stop's fade was dropped while the configuration survives (a card unplugged during a stop rests `stopped` with no card) | major | Fixed: held during a stop and applied once rested; dropped only during a teardown (4.2, 4.3, 4.4; D-L32) |
| M6 | A change of ports in 3a is a replace and dropped the node's stop, against D-F4 | major | Fixed: the plan marks a port-caused replace; `NodeHost` carries the stop, `dropped` and one health stream into the successor (5.7, 8.4; D-L36); control-plane.md 4.8 follows for the predecessor's notifications (revision 6.2) |
| M7 | The intent's lifetime was undefined, and a stop after a completed restart would tear down as a restart | major | Fixed: each intent ends with its stop or teardown; priority only between intents in flight (4.3, rule 3; D-L37) |
| M8 | L-3 was not green (`output-records.json`'s `"run": 2`, field-for-field encoders, fixtures only in L-6), the removal lines were seven, and L-3 and L-4 were too large | major | Fixed: each fixture changes in the step changing its encoder (9); seven lines with `white_to_file_test.cpp:52` (11.7); L-3 split into L-3a to L-3d and L-4 into L-4a and L-4b (12) |
| M9 | S1 lacked the stalled loop (a full queue; `block = false` may block, fail or allocate) | major | Fixed: S1's two stalled phases and their pass and fail (10.1); and one invoke in flight per block whatever S1 finds (5.3; D-L38) |
| m1 | A block-lost node with no failure derived `added` | minor | Fixed: rows 4 and 5 of 4.3 (failed with the loss's detail; disabled with `blockLost`) |
| m2 | Gaps in 4.2's table; the shortcut skipping the legacy recipe; `rested` never set when configured while stopped; `started` never cleared; row 10's `waitingFor` | minor | Fixed: the missing rows, no shortcut, `rested` at T4's rest, `started` cleared at `stop()`, `waitingFor` `block` on `prepared` (4.2, 4.3) |
| m3 | The failure code was not given per fact | minor | Fixed: 4.2's code-by-fact table; node-framework.md 6.2's `lost` row names a hook that threw after rest (revision 8.2) |
| m4 | Operations on a node removed by an edit, and during `stopAll` | minor | Fixed: `unknown-node`, and `stopping` during `stopAll` (4.4); control-plane.md 7.6's row follows (revision 6.2) |
| m5 | Rule 1 lacked "not wedged"; the wedge check before T1; no path for node-less diagnostics | minor | Fixed: rule 1 (4.3), 4.1's first row, `EngineDiagnosticListener` (5.4; D-L44) |
| m6 | Re-entrancy: an outside node's `ready` told inside its making during the flush | minor | Fixed: snapshot iteration, items appended behind it, adapter events deferred (5.5; D-L39); a test (11.1) |
| m7 | Fade details: every drive resets the gain and cancels a fade; a fade on a block not STREAMING; take-up on the `frames > maxBlock` path | minor | Fixed: 5.3's Applying and Fading (with the invoke there is no take-up in `process` to miss) |
| m8 | Legacy `sendspin` after a quick stop and start reports unheard frames as played (`missedFrames`, `sendspin_code.cpp:13-17`) | minor | Fixed: the code released and prepared at every stop's silence (4.2's T7 row, 7.5; D-L33). The reviewer's "before every drive after a silence" is the same rule, done at the silence so Start stays one invoke |
| m9 | A hook throwing after `cx.sink`; `stop()` after a `start()` that threw; `stop()` throwing in a teardown; the leaked record | minor | Fixed: 5.9's edges, 5.11's `close`, 4.2's close-overdue row naming sinks (D-L45) |
| m10 | A change committed during a stop's teardown, then `start` | minor | Fixed: the restart runs first (4.3, rule 1; D-L41) |
| m11 | Where `waitingFor`'s pacing lives | minor | Fixed: `NodeHost` (4.5; D-L42), with a test |
| m12 | S2 as a test-only filter; S3's loop check; S4's TSan not well-posed | minor | Fixed: S2's probes a test-support file L-2 reuses, and S2 now gates 3b only (10.2); S3 counts data-loop threads (10.3); S4's TSan dropped for a cited code reading (10.4; D-L43) |
| m13 | "A hand edit keeps a node's stop" was an `ear_config_tests` case | minor | Fixed: moved to `node_host_test.cpp` (11.1, 11.4) |
| m14 | Tests for B1, M1, M2, M5, M6, M7, stopAll after a wedge, ready inside make, the legacy recipe check | minor | Fixed: each named in 11.1 and 11.6; the legacy rule is a check (5.10; D-L40) |
| n1 | `carryLatency` listed as batched, then synchronous | nit | Fixed: removed from the batched list (5.5) |
| n2 | `ear_output` cannot read `MainLoopActivity`; its home step disagreed; the activity never cleared | nit | Fixed: a `where` callback from `main.cpp`, its own header in L-3b, cleared after each item (5.12, 6, 12) |
| n3 | How `LegacyInstance` gets a `StartContext` | nit | Fixed: `InstanceContexts` (5.9, 5.10) |
| n4 | Frame and stamp rings' drops; a wrapping chunk's position; I5's steps 2 and 3 in 3a | nit | Fixed: 5.11 |
| n5 | An enable with a change of ports in one edit | nit | Fixed: 8.4 (D-L46) |

None was declined.

### Revision 2 (2026-11-08, after round 1)

Answers round 1 above. Changed: the status; 2; 3; 4.1 to 4.6; 5.1 to 5.5, 5.7, 5.9 to 5.12; 6; 7.5; 8.4; 9; 10; 11.1,
11.4, 11.6, 11.7; 12; 13; 14 (D-L6 revised, D-L29 to D-L46); 15; 16; this record. node-framework.md moves to revision
8.2 (I4, 6.2's failure codes, 6.3's wedged data loop, 9.6, 13.1; its rows 136 to 139) and control-plane.md to revision
6.2 (4.8, 7.6), each by a minimal edit. No owner gate is open. Step L-1 may start once L-0's S1 and S3 pass; a second
look at 4.2 to 4.4 against node-framework.md 6.3, and at 5.3 against S1's stalled phases, is worth having before L-3a.

### Revision 2.2 (2026-11-08, found building L-2)

Step L-2 built 5.3 in `FilterPortBlock` (`port_block_test.cpp`'s cases of 11.6, with the probes moved to
`src/pipewire/tests/probes.hpp`). 5.3 gains what building it settled: `blockId()` on `PortBlock`; `sinks` waiting for
L-4b; requests beyond the ring queued on the main thread; a cancelled fade-out's `done` dropped; the fade's exact
shape; `takenAt` as an expected position, wrong after a driver reset; one completion number for both kinds; the
destructor's flush of a queued invoke. 16's two [U] for L-2 are verified.

### Revision 2.1 (2026-11-08, step L-0's results)

10.5 records S1, S2 and S3 as built (`src/pipewire/tests/spikes_test.cpp`) and the criteria they corrected: a full
invoke queue is never refused in PipeWire 1.4.2; "returns before its function ran" is ill-posed; an unbounded producer
of invokes wedges the data loop, which makes D-L38 a requirement; a cycle cut short by an xrun is run by an upstream
prefix only, an open item for 3b's sources; `pw_filter`s name no loop. 16 updated. 3a's gates (S1, S3) pass.

### Revision 2.2 (2026-11-08, found building L-1)

Step L-1 built 4.1 to 4.4 as `NodeAggregate` over the two machines (`src/core/include/ear/core/node_machines.hpp`) and
tested every cell by table. Writing the tables found these gaps, each fixed by a minimal edit; none disagrees with
node-framework.md 6.2 or 6.3, which needed no change:

- 4.3's row 6 showed a restart or disable in `starting` with a drive in flight as `starting` (the configuration stays
  `arming` until the completion), against D-L2 and 4.4's `starting` row: `arming` with `stopAsked` is now `stopping`.
- Row 10 showed `configured` again whenever a rested configuration was `prepared` without the stop, so a start after a
  stop would tell `configured`: it now needs "not `rested`".
- 4.4's "reason replaced" on a block lost after a failure against row 4's "the failure's, else the block's": the loss
  clears `failure`.
- Rule 1 had no outcome for a node that would configure on a wedged loop (a restart's teardown ending after the wedge,
  a retry), nor 4.2 for a stop's silence overdue (its configuration leaked, the stop never ending): both fail the node
  `engine-refused: the data loop is wedged`, as D-L10 does a new node.
- A held fault applied "once the node rests" would tell `stopped` for a moment: it is applied at the rest, before
  health is told.
- `predecessor`'s end: the older node's configuration closing and its block's destruction can land in either order;
  the successor waits for both, so it never shows `added`.
- 4.2: a `start()` that threw undoes `rested` as well as `started` (so its code is `start-failed`); the composite
  "configure answered failed" row is the `configure` row then a fault in `readying`.
- 4.4: two cells cannot arrive (`added`'s block lost; `configured`'s, which lasts no input).
- 6 and 11.1: L-1 cannot replace `node_lifecycle.hpp` while `NodeHost` and `NodeHealth` still use it, so the machines
  are in `node_machines.hpp` under `ear::lifecycle` until L-3d.

### Revision 2.3 (2026-11-08, found building L-5)

Step L-5's graph file and desired graph part built 8 (`graph_file_model.cpp`, `desired_graph.cpp`, `settings_table.hpp`;
`graph_file_disabled_test.cpp`, `desired_graph_test.cpp`, two compile-fail cases). Gaps found, each fixed by a minimal
edit:

- 8.3's pin: knobly's insert into a block whose first statement is on the `{`'s line breaks the line and leaves that
  statement at column 1; that case is written textually, still first in the block (8.3, "As built"). 16's item is [V].
- 8.2's Disable replaced the whole line with `replace_with_text`, which drops a comment the line owns: it sets the
  line's values instead. With no line, a node statement that makes several nodes is refused `shared-definition`, as an
  update's added key is (8.2 said nothing).
- 8.2's Enable: knobly's `remove` takes the comments the line owns (those just above it), as every removal does; only a
  comment it does not own (after the `{`) keeps a block. `dropBlock` runs on a fresh load of the text the removal left.
- 6: `Plan::outcomes()` carries a replaced node too, so the reply's `applied` for D-R23, D-L5 and D-L46 comes from one
  place. `NodeChange` has `outcome` and `keys`; `portCaused` stays L-3d's.
- 6 and 12: the keeper's `DisableNode` and `EnableNode` need `ear_control`'s `Command`s, which are L-6's (9), so they
  move to L-6. `NodeHost` honouring `disabled` moves to L-3d, since `NodeHost` was being rewritten (L-3a) while L-5 was
  built. Until then `nodeChanged`'s default tells `NodeHost` `nodeUpdated`, so any changed `disabled` restarts the node
  with its config and a disabled node runs (today's behaviour), and the gateway sends `node.updated` as for an update.
  L-3d overrides `nodeChanged` in `NodeHost` (`disable`: T20 or T21, the stop dropped; `enable`: T22; `stored`: the
  config kept, nothing runs; `restart`: as today) and makes a node added with `spec.disabled` rest `disabled` without
  configuring, a port-caused replace following D-L46.
- The node line's hint is appended to today's message after `; ` ("...; to disable a node, write disabled true; inside
  its braces"); the wire's text for a `disabled` key in settings is control-plane.md 4.2's, refused by the model's
  `updateNode` and `addNode` before any edit.
- `NodeSpec` and `desired_graph.hpp` are frozen interfaces: errata in m1-white-to-file.md and batch2.md section 11.

### Revision 2.4 (2026-11-09, S2 on CI)

S2 failed twice on CI's parallel suite, and a sendspin system test beside it lost 10 ms of its ramp. 10.5 records why: on a
loaded host the dummy driver steps `clock.position` back after a run of late timeouts, and the whole graph runs
positions again, so S2's model of D, which compared swaps by position, was wrong; and its check that steps back lie
near a jump B saw kept only B's first 256 jumps. S2 now tells cycles apart by the order they ran in, asserts the
corrected criterion exactly, and runs alone (`RUN_SERIAL`). 5.3's `takenAt` and 16's open item for 3b name the step
back.

### Revision 2.5 (2026-11-09, found building L-3a)

Step L-3a built `NodeHost` on `NodeAggregate` with the asynchronous drive (`node_host.cpp`), every type through the
legacy adapter, `Timers::now()`, and the blocking `PortBlock::drive(AudioCode*)` removed (engine tests use
`ear::testing::driveNow`, `ear/testing/drive_now.hpp`: the asynchronous drive, then `settle()` until it completed).
`node_host_test.cpp` has 52 cases: the 47 rewritten (the slice 2 cases that paired a recipe with a card now give the
card no recipe) and five new (a drive's completion later than the call, a stale completion, a restart with a drive in
flight, removal silencing before the block goes, and, after review, a fault during a restart's drive in flight). What
building it settled, each a minimal edit or recorded here:

- 12: a failed node is torn down at once in L-3a, not L-3c: the machines' fault rows (4.2, T14) tear the
  configuration down, and L-3a runs on them. Externally a failed node's code is silenced and its outside nodes, handoffs
  and run object go at the failure, where before they ran on until an update or removal; no record changes.
- 5.10's T3: the recipe runs before the machine hears `resourcesReady` (the row says why).
- 4.5's records against the aggregate: a removal that ends in its own input (no configuration, or one already gone)
  derives `removed` at once, so the aggregate's `tell` emits `Gone` with no `stopping` before it. Today's records (and
  L-3a's) say `stopping` first, so `NodeHost` tells it before a `Gone`. L-3d, which changes the records, decides
  whether 4.5's `removed` alone is meant there.
- T1 (5.7) is L-3b's, but a name added back must still wait for its older node: L-3a makes its block at once, as
  today, and adds the node with the aggregate's `predecessor` set, so rule 1 configures it only once the older node is
  gone (`predecessorGone`, then the block being already made). Row 3 derives `stopping` (`restarting`) for it, which
  L-3a's old health shows as `starting`, as today.
- The old health from the derived one (until L-3d): `stopping` only with cause `removing`; `failed` with the failure's
  reason, the code `lost` shown as `start-failed`, or with the block's loss; `running`; `starting` for everything else
  (a restart's `stopping` included, as today's restart said `starting`). A listener is told only when that changes.
- 5.1's `queued`: `NodeHost` keeps the newest committed config beside its version for the next `configure`, and drops
  it at removal and at a block's loss, which never configure again, so a config goes with its configuration's step 5
  as before.
- 5.2's inputs: `NodeHost` queues an input that arrives while another is handled (a resource ready at once, a run
  object closed inside `close()`, an outside node ready inside its making) and handles it after, in arrival order,
  before the call returns; nothing is handled inside an action. Every input names its node by serial.
- A configuration whose `start` threw (a type bug) asks for nothing more: no outside node is made for it once its block
  is ready, as before.
- `Timers::now()` is `std::chrono::steady_clock::time_point` (CLOCK_MONOTONIC, the clock the main loop's timers count
  on).

Then an independent review of the L-3a commit found these, each fixed in a second commit:

- 4.2: a fault in `arming` with `stopAsked` (a restart, disable or removal asked while the drive is in flight, which
  4.3's row 6 already shows as `stopping`) went through the `driving` row and raised intent `fail` over `restart`, so
  the restart's queued config never configured. Such a drive is a stop or teardown already begun: the rows now treat it
  as `fading` (held during a stop, dropped otherwise), and `NodeHost` marks the configuration's channel closing then,
  as at its silence.
- 5.2: an input that throws (a type bug) no longer clears the inputs queued behind it, which may be other nodes' (a
  stop's `allGone` would never run): they are handled, then the first exception is rethrown.
- The records' order, now as before: a removal tells `stopping` before anything it does (a never-driven removal used to
  release and destroy the block first), and an update of a node whose block stands tells `starting` at once, before
  its teardown, a failed node's included (its retry waits for the failed configuration to go; the old health shows
  `starting` while `retryAsked` is set).
- `node_host.hpp`: an outside node may be destroyed inside its own `lost()` (`NodeHost` tears a failed configuration
  down in that call); not inside `ready()`. `sound_card_test.cpp` checks it on a card that cannot open.
- An outside node's `ready` or a handoff's `closed` queued while its configuration tears down, or after it, lands on
  nothing (the generation, and the outside nodes already cleared).
- `driveNow` is `[[nodiscard]]` and every engine test REQUIREs it; destructors use `silenceOrAbort`, which waits up to a
  minute and aborts rather than free a code the data loop may still run.

### Revision 2.6 (2026-11-09, found building L-3b)

Step L-3b built 5.5's flush in `node_host.cpp` (one task posted on a turn's first change; a snapshot sorted outside
destroys, block destroys, block makes, outside makes; an item an input causes runs behind the snapshot in the same flush;
an input told inside an item, an adapter's `ready` among them, waits for the item to return), T1 (a name added back
waits `waiting` for the older block's destruction and is made in that flush), outside nodes made and destroyed in the
flush, the join by the block's id (`outside_nodes.cpp`, `OutsideJoin::blockId` in place of `blockNode`), and
`MainLoopActivity` (`main_loop_activity.hpp`, written around each flush item and each legacy `start` and recipe:
`make block`, `destroy block`, `make outside node`, `destroy outside node`, `configure`, `recipe`; one process-wide
seqlock, since there is one main loop). `node_host_test.cpp` has 59 cases: the 52 kept (their logs now show blocks and
outside nodes made and destroyed at the turn's end) and seven new (one flush with destroys first, a block removed in the
turn it was queued never made, a name added back gets its block in the flush that destroyed the older one, a name added
back three times in one turn, a card ready inside its making with a freed block made in the same flush, the outside
node made with its block's id, the activity named in each hook). `sound_card_test.cpp` and `rtp_receiver_test.cpp` each
join a block beside an older block of the same name and reach only the newer. What building it settled, each a minimal
edit or recorded here:

- 4.3's rule 1 says "no older node of the name left", but L-1's aggregate had only `predecessor` (a port-caused
  replace, L-3d's) for it, which L-3a borrowed. The aggregate gains the fact `olderNode` (`AddFacts::olderNode`,
  `olderNodeGone()`): `NodeHost` sets it when an older `Node` of the name is not yet removed and clears it when the last
  one is. `predecessor` is left to L-3d. Row 7 (`added`) shows such a node, mapped to today's `starting`.
- 4.1 and 5.7: the block machine is `gone` when its destruction is queued, but the node is removed, told gone
  (`nodeGone`, the `removed` record) and dropped from `health()` only once the flush ran the destruction, so `stopAll`'s
  `allGone` follows the last block's destruction. An older block "exists" for T1 while its making or destruction is
  still queued. A destruction queued while the block's making is still queued (a node added and removed in one turn)
  cancels the making: the block is never made.
- 4.1's "tell every `waiting` block of the name": at most one waits (only the wanted node of a name waits, and a waiting
  node removed is `gone` at once), and it is told only once no block of the name is left.
- 5.5's "outside nodes, then blocks" orders the flush; the teardown queues the outside nodes' destruction and closes
  the handoffs in the same action, so a legacy handoff is told to close before its configuration's outside nodes are
  destroyed (at the turn's end), never after they finished. No record changes.
- 5.7: `PortBlockEvents::ready()` keeps no argument: `NodeHost` reads `PortBlock::blockId()` when it makes the outside
  node, the block being ready (6's table said `ready(blockId)`). `OutsideTarget` keeps `blockNode` for its names and
  details. A target with no id is lost at link time, never linked by name.
- What a flush item or an input during it throws (a type bug) strands nothing: the flush ends, then the first exception
  is rethrown out of the main-loop task, as 5.2's inputs do.
- Externally: a node's block is made at the end of the turn that added it (its `starting` record before it, as before);
  a name added back gets its block, and so its ports and routes, only once the older block is destroyed, where L-3a made
  both at once; a removed node's `removed` record follows its block's destruction in the same turn's flush. Every
  record's text is unchanged.

### Revision 2.7 (2026-11-09, found building L-3c)

Step L-3c built 5.3's fades and 5.4's bounds and wedge in `node_host.cpp`: every drive carries one faded flag per output
the code writes (public outputs, then inside outputs, all true: 3a has no sink), so `Queued.fades` is true for a node
with any output and a node with none is silenced unfaded; every planned silence fades first; the fade's bound (100 ms),
the barrier's (5 s) and the close's (10 s, as before) are `Timers`, each judging only after the block's `settle()`;
`EngineDiagnosticListener`, `leaked()` (renamed from `leakedRuns()`), and `StatusLog`'s `ear.fade-unacknowledged`,
`ear.barrier-overdue` and node-less `ear.data-loop-wedged` with their `output-records.json` cases. `node_host_test.cpp`
has 68 cases: the 59 kept (a node with an output now logs its fade before its silence) and nine new (the faded flags, a
fade unacknowledged, M1's completion settled from a bound after a 6 s stall, B1's block leaving STREAMING, the wedge, a
node added after it, `stopAll` after it, 16 fades at once, a factory or adapter throwing in the flush); review added
four (the grace after a settle that ran a completion, the grace after a late timer, a leaked block's node kept, a card's
playback bank faded at a restart and a removal), and the B1 case is renamed for what the fake can show, pointing to
`port_block_test.cpp`'s real ones.
`sound_card_test.cpp` follows to_card's fade-in and its last 5 ms to the card's samples. What building it settled, each
a minimal edit or recorded here:

- 4.2: a drive is a barrier too (5.6 numbers it as one, and it is applied inside its invoke), so `arming` has the 5 s
  bound and the same row as `silencing`'s: a drive whose invoke never ran is the loop's fact, and without it a node
  starting when the loop wedged would never end, nor would `stopAll`. L-1's table test gains the cell.
- 5.4's bound, as built: when its timer fires the block is settled in an input of its own and the request judged in the
  input behind whatever that settle ran, so a completion the settle found is heard first (M1); the judgement also
  checks the configuration and the barrier number, since the fade's bound must not judge the silence posted after it.
  After review: when that settle ran a `done` of the block (it may then have posted the request's invoke only now,
  behind a completion whose event had not been dispatched), or the timer fired more than 250 ms past its due time (the
  whole process was stalled, frozen or paused, not the data loop alone), the request gets a grace (1 s for a barrier,
  100 ms for a fade) instead of a judgement, and its diagnostic's `ms` counts the whole wait.
- 5.4's wedge beyond NodeHost (review): `Links`, the outside nodes and the port blocks consult the engine's mark, as 5.4
  now says. Without it a route removed after the wedge hung the main loop for good, SIGTERM included (an engine test
  without the mark hangs and ignores SIGTERM; with it the removal returns and the loop turns). A `Node` whose block was
  leaked is never freed (the block still holds it as its `PortBlockEvents`), and `~NodeHost` leaks its whole state on a
  wedged loop.
- 11.6: `engine_test.cpp`'s "Stopping 16 nodes costs about one fade, one cycle and one batch of destroys" runs the real
  `NodeHost` over the real engine (12 to 17 ms here, bounded at 2 s); the bounds against CI's stalls stay [U] until CI
  has run it under load.
- 5.4's wedge, as built: `ear.barrier-overdue` (with `ms`), then `ear.data-loop-wedged` once (with `ms`, its detail
  naming the node); then every node hears it as an input of its own, and nothing is settled again (`settle()` may post
  an invoke). A configuration `readying` or `prepared` fails `engine-refused: the data loop is wedged` and tears down
  with no barrier (its latency is no longer carried); one `arming`, `fading` or `silencing` is leaked at once, its
  bound's diagnostics unsaid; one `driving` runs on (the loop is wedged anyway) until its stop or removal, whose fade and
  silence are then judged overdue at once and leak it. Leaked configurations keep their outside nodes, handoffs (never
  closed: their writer may still run) and faded flags. The flush still runs (it is a main-loop task, not a PipeWire
  call), but a destruction leaks what it would destroy, a block's making makes nothing (the node is lost "the data loop
  is wedged") and an outside node's making is skipped, so every removal still ends and `stopAll`'s `allGone` runs.
  `~NodeHost` releases, never destroys, what a wedged loop may reach. `leaked()` counts configurations (with the outside
  nodes they hold), and blocks and outside nodes leaked alone.
- 5.12: `MainLoopActivity::Scope` saves what was written and restores it, so scopes nest (a hook inside a flush item);
  `prepare`, `release`, `close run` and `free configuration` are named too (L-3b's review, N1 and N2).
- 5.5: a flush item that throws strands nothing: a block's factory that throws leaves the node's block `lost` ("making
  the port block threw: <what>"), never `made` with none; an outside adapter that throws drops the half-made entry and
  fails the node as an outside node refused; the item's object goes inside its activity whether or not it threw; and the
  flush entered inside an input or another flush (an executor that ran a post inline) is a `std::logic_error` (L-3b's
  review, N3 to N5).
- Externally: a node with an output (every type but `file`) fades its outputs over 5 ms at every start and before every
  restart, removal, failure and ear's stop, its card's playback included, so its block's teardown waits one more
  completion (about one cycle); no record or wire change but the three new diagnostics. A route linked just after a node
  runs hears its fade-in: `control_port_test.cpp`'s recording RMS skips its first 5 ms and its levels case reads the
  second window with noise, since those recordings last only 20 to 60 ms.

### Revision 2.8 (2026-11-09, found building L-7)

Step L-7 built 5.12's watchdog: `StallWatch` in `ear_output` (`stall_watch.hpp`, `.cpp`), a thread (every signal
blocked) that wakes every 250 ms, reads one atomic heartbeat and reports through two callbacks; its judgement is
`StallJudge`, a plain class told the last beat and the time, so its rules are tested on a clock stepped by hand. `main.cpp`
beats it from a 100 ms `Timers::after` timer armed again each time, passes `where` as a lambda over
`MainLoopActivity::current()` and points the callbacks at `StatusLog`. What building it settled, each a minimal edit
or recorded here:

- 5.12's records: `ear.main-loop-stalled` is `{"code", "ms", "in"?, "detail"}`, `in` being `where`'s text (absent
  between calls), detail "no main-loop heartbeat for <ms> ms[, in <in>]"; `ear.main-loop-resumed` is `{"code", "ms",
  "detail"}`, detail "the main-loop heartbeat came back after a gap of <ms> ms". Both are `diagnostic` records with no `node`,
  `output-records.json`'s last three cases (seq 57 to 59). `in` is text, so they are not `EngineDiagnostic`s (whose
  `DiagnosticField`s are whole numbers, and whose listener is the main thread's): `StatusLog` gains
  `mainLoopStalled(ms, in)` and `mainLoopResumed(ms)`, called from the watchdog's thread.
- The heartbeat is the steady clock's time of the last beat, not a counter; and a beat that follows the last by more
  than the bound also stores that pair (from, to) before the beat, so `resumed`'s `ms` is the gap the first beat after
  the stall measured, not the distance to whatever beat a look later sees (which overstated it by up to a period and a
  beat interval; review's F1). A pair that does not start at the stall's last beat is an older stall's, and the look's
  own beats are used instead. `stalled`'s `ms` is the heartbeat's age when a look saw it (500 to 750 ms at the first
  look, with the 250 ms period); so a stall is reported once it is older than bound + period, and one between the
  bound and bound + period may go unreported (review's F3; control-plane.md 4.6 says the same).
- "Past 500 ms" is strictly older than the bound. A stall is reported once and its resumption once; a second stall is
  a second pair. A stall still open when the watchdog stops is not resumed.
- No false report at either end: nothing is judged before the first beat (the first timer fires 100 ms after
  `Engine::run` starts, so boot's synchronous work is no stall), and `main.cpp` stops the watchdog as soon as
  `Engine::run` returns, before the keeper's last write and the `_Exit` path. A stall inside ear's own stop, while the
  loop still runs, is reported: it is one.
- 11.6's test, plus one over the real engine (node-framework.md 13's row for the engine tests asks for "the main-loop
  watchdog's diagnostic on a stalled main loop"): `main_loop_executor_test.cpp` beats it from the executor's timers,
  sleeps one main-loop turn 900 ms inside `MainLoopActivity::Scope("configure", "desk")`, and sees one stall naming
  `configure desk` and one resumption of at least 900 ms. `ear_pipewire_tests` links `ear_output` for it (tests only).
  No system test: 11.7 lists none, and stalling the real binary's loop would need a hook ear does not have.
- The bound stays [U] against CI's loaded host (16): a host that stalls the whole process for 500 ms is reported, which
  is true; nothing is judged on the report.
- After review: `main.cpp`'s heartbeat is not `const` (its timer re-arms itself, F2); the engine test quits only on the
  resumption that follows its own stall, tolerating a loaded host's earlier pairs (F4); `MainLoopActivity::current()`
  yields between tries and gives up after 1,000, empty (F5); `maskedThread` leaves the fault signals (SIGSEGV, SIGBUS,
  SIGFPE, SIGILL) unblocked on ear_output's threads, since one blocked when its thread faults is delivered with its
  default action and skips the crash handler's last words (F6); the judge test where the beat moves twice between looks,
  and the thread test bounding the resumed `ms` from both sides.
### Revision 2.9 (2026-11-09, found building L-3d)

Step L-3d built 12's L-3d row: `node_machines.hpp` and `.cpp` became `node_lifecycle.hpp` and `.cpp` in `ear` (6's
plan), replacing `NodeLifecycle` and today's `NodeState`; `node_machines_test.cpp` became `node_lifecycle_test.cpp`.
`FailureReason` gained the code `lost` and `operator==`; the machines' `FailureCode` and `Failure` are its aliases.
`NodeHealth` carries `phase`, `wanted`, `cause`, `waitingFor` (`WaitingFor {kind, name}`) and `blockLost`, told after
every transition that changes any of them; `NodeHost` implements `NodeOperations` and overrides `nodeChanged`;
`StatusLog` writes 9.1's sentences; `LogEntry::run` and `output-records.json`'s `"run": 2` are gone; the seven removal
lines and `sendspin_connections_test.cpp`'s search wait for `removed`. `node_host_test.cpp` has 86 cases: the 72 kept
(their logs now say `stopping` for a restart, D-L2, and show each health after its transition's actions, as 5.2's
steps 2 then 4 order them) and fourteen new (a normal start's five phases, a stop kept and started, the card's bank
faded at a stop and a disable, disable and enable with a change stored between, a node added disabled, a restart of a
stopped node, start and stop on a failed node, `lost` after rest, operations refused unknown-node and stopping,
`waitingFor`'s delay and pace, an older node waited for, a change of ports keeping the stop, a hand edit keeping the
stop and a rename dropping it, a replace of another type heard by standard output alone). What building it settled,
each a minimal edit or recorded here:

- 4.5 and 5.2 against today's records: health is told after the transition's actions, so a restart's `stopping` follows
  its fade's post and a restart's `starting: <cause>` follows `configure`. Revision 2.5's open question is decided:
  every removal says `stopping` (cause `removing`) before anything it does, and before `removed` when it ends in its
  own input, since 4.8 says every node shows `stopping` at ear's stop.
- 5.7 and 9.1 disagreed on the restart's sentence (`stopping: restarting: settings changed: <keys>` against
  `stopping: restarting (settings changed: <keys>)`); 9.1 is the records' home (this page's header says so), so 5.7
  follows it. Keys are joined with `, `; a restart by settings with no keys says `settings changed`; a configuring cause
  says `settings changed: <keys>`, `restart invoked`, `retry` or `enabling`. The `removed` record's `state` is
  `stopping`, as 9.1's example has it.
- 4.5's records: one when the state (the colour) changes, and also when a `failed` node's reason changes (a block's
  loss replacing a failure: today's record, kept). `starting` is written for the `added` phase and `starting: <cause>`
  for `configuring` with a cause; a start from `stopped` (T8, the `starting` phase, no cause) writes none, then
  `running`. `still`'s `<s>` and its 10 s pace are counted on `StatusLog`'s steady pace clock, not the records' `t`.
- 8.4's `portCaused` "on the `nodeRemoved` and `nodeAdded`": those calls carry no change, so `DesiredNodeListener`
  gains `nodeReplacing(name, change)` and `nodeReplaced(spec, config, change)`, through which `commit` tells every
  replace (`NodeChange::portCaused` true when the type is unchanged), defaulting to `nodeRemoved` and `nodeAdded`, so
  the gateway and `StatusLog` change nothing (batch2.md's frozen header, through this page).
- D-L36's "health goes to `StatusLog` only, never to the gateway": `NodeHealthListener::hearsSuperseded()` (default
  true; the gateway's false). A node is superseded from the commit that replaces it (another type) or adds its name
  back; the older node of a port-caused replace tells nobody, and its successor takes its dropped frames.
- D-L46's corners: a port-caused replace whose new node is disabled is not given the aggregate's `predecessor` (it
  shows `disabled` at once, its block waiting for the older one's, which tells nothing), but `NodeHost` still holds its
  `removed` until the older node is gone and reports the older node's dropped frames in it (review); one whose old node was disabled and new node is
  enabled is given `predecessor`, so it shows `stopping` (`restarting`, `settings`) and `configuring` (`settings`), not
  `enabling`, though the reply says `enable`.
- 4.5's `waitingFor`, as built: `older-node` (its own name) in `added`; in `configuring`, `block` while the block is not
  ready and outside nodes are asked, then each outside node not yet ready (`companion`, `<node>.<suffix>`); `block` in
  `starting`; none in `arming`. The 2 s timer is armed on every phase told, the 1 s pace on every list shown.
- 5.2's `LifecycleApplied` and `LifecycleRefusal` stay in `node_lifecycle.hpp`, which `node_host.hpp` includes, since
  the aggregate returns them; they are still core's own and named by no wire code.
- Until L-6 the gateway sends `node.state` only when a node's `state` or `reason` changes (the wire has neither `phase`
  nor the other members yet), so a normal start is still `starting` then `running` on the wire; `stopped`, `disabled`
  and a restart's `stopping` are its new values. `NodeOperations` reaches nothing on the wire yet.
- After review: a port-caused replace's new node is told gone only once its older node is gone too (its own block
  destroyed and its older node closed, in either order), with the older node's dropped frames, so a removal during the
  older node's teardown no longer says `removed` while the older configuration still closes (5.7, D-L36). `StatusLog`
  writes a `still` record for a list told inside its 10 s pace at the first tell after the pace, if it differs from the
  list last written. A superseded node's health carries `superseded` and its end is `supersededGone` (default
  `nodeGone`), told first when it is superseded, so `StatusLog` keeps it apart from the newer node of its name.
- `sendspin_streams_test.cpp`'s `CHECK_FALSE(ear.hasLine("node ma stopping"))` matched the whole line, so the new
  `stopping: <cause>` would pass it vacuously; it checks the prefix now.
