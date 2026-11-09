# Changes in place and reshape: slice 3d of the node framework

Status: design, not built. Slice 3d follows slice 3c (`node-primitives.md`: it swaps claims and posts to loops, both
3c's) and comes before slice 6 (`control-plane.md`: a retune is one of the outcomes this slice makes). It starts with a
PipeWire spike (S4, 9.3). It changes the audio path: holds, code swaps and reshapes. The built lifecycle (states,
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
| R24 | "too technical for me to properly answer" (where reconfigure lives and how it reaches a type's threads) | 2026-11-08 | Hooks run on the main loop and reach a node's threads only by typed posts (D-A4); the rest of this page's choices are the designer's (section 12) |
| R30 (gate W3) | "I would like each port to show meter levels so I can see audio traversing the graph" | 2026-11-08 | A hold is seen as silence on the meters (no "held" marks); a reshape's new ports are metered from their first window (8.4) |
| R33 | Asked "Should configure come in two versions, each offering only its legal calls, so a wrong call won't compile?": "Two versions" | 2026-11-08 | `Changed configure(ChangeOf<spec>&, const Checked& old, const Checked& now)`, optional; absent, every change restarts (section 4) |
| X2 | Asked what happens to a change that removes ports that are wired: "(a)", as recommended: the editor lists the wires that would go before applying; Apply removes them and changes the setting in one step (one save of `ear.conf`) or neither; a script must ask (`dropRoutes: true`), else it is refused naming the wires | 2026-11-08 | 7.1, 8.3 |

## 2. What is built, and what this slice changes

As built (3a, [V]): the change version's signature is checked at compile time and ignored (the owner, 2026-11-09:
"Accept, ignore until 3d"); `ChangeOf<spec>` is declared for the signature only and `Changed::applied()` and
`restart(reason)` have trivial bodies (`lifecycle.hpp`). Every change of settings restarts the node
(`NodeOutcome { restart, disable, enable, stored }`, `desired_graph.hpp`), shown as `stopping` with cause `restarting`
(`settings`, with the changed keys) and then `configuring`. A change of ports is a replace: the desired graph tells
`nodeReplacing` then `nodeReplaced` with `NodeChange::portCaused`, and `NodeHost` carries the node's stop, its dropped
frames and one health stream into the successor, so a client sees `node.removed` and `node.added` and one `stopping`
(`restarting`) (D-L5, D-L36). `PortBlock` has `drive`, `fadeOut`, `settle`, `takenAt`, `blockId` and `carryLatency`;
no per-bank hold, no reshape. The fade of every output the code writes before every planned silence, and the fade-in
at every drive, are built.

This slice adds:

- rows marked `.inPlace()`, the plan's outcomes `reconfigure`, `reshape` and `unchanged`, and the state
  `reconfiguring`;
- the change version of `configure` with `ChangeOf`, `Changed`, `whenReady` and `ConfigureDone`, staged posts, and
  the spec's `.lifecycle`;
- holds with fades and their acknowledgement, make-before-break with retiring resources, break-first behind a
  framework hold, claims swapped, the code and its layout swapped, the deadline, supersession and the fallback restart;
- `SinkControl::pause` and `resume`;
- `PortBlock::reshape` (if the spike passes), turning the port-caused replace into a reshape;
- `rtp` and `sendspin` changing in place;
- on the wire (feature `node.set`, `reshape`, `dryRun`): `node.set`, `applied` with the new outcomes, `inPlace`,
  `ports`, `dryRun`, `dropRoutes`, `ports-in-use`, `keys` and `next` on `node.state`, the `fell-back` cause, three
  diagnostics, the `reconfiguring` record.

The transitions this page names are the framework's numbering: **T9** (`running` or `stopped` → `reconfiguring`, the
change version called), **T10** (the commit point: back to `running` or `stopped`), **T11** (the fallback: `reconfiguring`
→ `stopping`, cause `restarting`, then a restart with the newest settings), **T18** (a newer change supersedes one
before its commit point).

## 3. What a type declares, and what the plan decides

Every setting row is restart-only by default. A row the type can take while the node runs is marked in its table:

```cpp
inline constexpr auto portRow = setting::required("port", setting::number(1, 65535)).inPlace();
```

`.live(...)` (slice 6) implies `.inPlace()`. The marks are data in the table, so `plan` reads them and the type schema
(slice 5) carries them. `.shapesPorts()` (slice 5) is only the schema's promise; the plan compares the ports `ports()`
makes. Every row's mark is the one home of "how a change to it applies":

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

```cpp
// ear/core/lifecycle.hpp: ChangeOf<Spec> defined (declared for the signature in 3a)

/// An in-place change's token: move-only; done() or fail(detail) once, from any thread. Destroyed without either, it
/// fails the change ("<node>: the change was dropped"). After teardown began, both are ignored.
class ConfigureDone {
public:
	void done() &&;
	void fail(std::string detail) &&;
};
/// What configure answers for a change. A "later" answer is made only by whenReady, so a token and a later answer
/// cannot disagree. (Built with trivial bodies in 3a.)
class Changed {
public:
	static Changed applied();                         // the commit point comes once every replacement is ready
	static Changed restart(std::string reason);       // the type cannot take this change in place after all (T11)
};
/// The spec's lifecycle settings (TypeSpec's `.lifecycle`, node-primitives.md 6.11). Refused at compile time below 1 s
/// or above 60 s. The type schema carries it as `reconfigureMs`.
struct Lifecycle { std::chrono::milliseconds reconfigureDeadline = std::chrono::seconds(10); };

/// configure for a change (T9): replacements, holds, a swap, staged posts, a continuation. Valid only inside the
/// configure it is handed to.
template <const auto& Spec> class ChangeOf {
public:
	AudioFormat format() const;
	const NodeName& node() const;
	/// The code: always a swap, installed with its layout at the commit point. Optional, except in an in-place
	/// reshape, where the Io layout changed.
	void use(NodeCode code);
	/// Staged until the commit point; destroyed unrun on supersession (T18) or a fallback (T11).
	template <class Loop> void post(Loop, std::move_only_function<void(typename Loop::State&)> work);
	// Replacements: each replaces what the node holds for the same bank or key, make before break (5.3).
	template <class Bank> void soundCard(Bank, SoundCardShape, Order = Order::makeFirst);
	template <class Bank> void rtpReceiver(Bank, RtpReceiverShape);    // the same port: break-first, by the framework
	template <class Bank> SourceWriter& source(Bank, SourceShape);
	template <class Bank> SinkRequest& sink(Bank, SinkShape);
	void claim(claim::TcpListen);   void claim(claim::Udp);            // judged when the hook returns (6.1)
	void unclaim(claim::TcpListen); void unclaim(claim::Udp);          // released at the commit point, last
	int recording(RecordingRequest);                                   // a replacement, made before break
	template <class From, class To> void declareLatency(From, To, Latency);
	// Holds: from the next cycle, after a fade; released with a fade at the commit point.
	template <class... Banks> void hold(Banks...);      // routable output banks of Spec (static check)
	void holdAll();
	template <class... Banks> void release(Banks...);   // earlier than the end, if the node wants
	/// Once every acquisition this call made is ready (claims granted and probed, recordings open, replacement companions
	/// running and linked), posts work(state, token) to `loop` as the node's own work; the one later answer.
	template <class Loop>
	Changed whenReady(Loop, std::move_only_function<void(typename Loop::State&, ConfigureDone)> work);
};
```

The type's class gains, optionally, `Changed configure(ChangeOf<spec>&, const CheckedOf<spec>& old, const
CheckedOf<spec>& now)` (its signature already checked, as built). **A call the mode does not offer does not compile**
(R33): `CreateOf` has no holds, no `whenReady` and no `unclaim` (built), `ChangeOf` has no `loop`, `store` or
`executor` (a loop's state already holds them), and `Configured` has no `restart` nor `Changed` a `failed`.

**The rules a compiler cannot check**, each failing the node with the rule's text, as every built context rule does
(the built framework records the broken rule on the node and fails it at the hook's return, even when the type caught
the throw): a change gives the code at most once, and must give it when the ports changed; `unclaim` names a key the
node holds; `whenReady` is called at most once, and its `Changed` is what `configure` returns; a context is used only
inside its `configure`. A type bug found in a change fails the node `lost` (it had rested), never takes ear down.

**Staged posts** (D-R17). Work posted with `cx.post` is staged until the commit point: released there, in the order
posted, after the continuation's `done()` (if any), and destroyed unrun if the change is superseded (T18) or falls back
(T11), as an unposted `whenReady` continuation is. So nothing a superseded or fallen-back change asked of the node's
threads has run, and supersession leaves the applied configuration as it was.

**`whenReady`** joins "act once the new acquisitions are ready" and "act on the node's own thread" (I11, I14), so a
type writes neither a poll nor a second hook (G20):

- if an acquisition is refused or lost, or the deadline passes before the post, `work` is destroyed unrun on the main
  loop and the change falls back (T11);
- if a newer change supersedes it before the post, or a stop, a removal or `node.restart` ends it, `work` is destroyed
  unrun the same way, with every plain post the hook staged, and its acquisitions are released; the post is the commit
  point after which a newer change waits;
- once posted, `work` is the node's own work: it runs even if a stop, a restart or a removal began after the post,
  before the goodbye `stop()` posts behind it, so a goodbye always sees a whole state, old or new, never half a swap;
  its `done` is then ignored;
- a continuation may not swap the code or hold banks: those are `cx` calls, applied at the commit point.

**`use` is always a swap** (D-R18): the framework never compares codes, so a type whose code keeps state carries it into
the new code or does not call `use`. A code swap without holds is a discontinuity unless the type carries its state;
the framework adds no crossfade. The new code's `prepare` runs on the main loop while the old code still runs: it may
allocate and build the new code's own state, and may not touch the framework's realtime state (slots, ramps, ring
cursors) or anything the old code's `process` reads (D-R13).

## 5. What the framework does, in order

1. **Calls the change version** on the main loop (T9). The node is `reconfiguring`. Its configuration still applies the
   old settings; `now` is the change's target. A stopped node takes the change the same way, without audio: nothing is
   driven, so holds and the code swap cost nothing.
2. **Holds.** From the next cycle each held bank fades to zero over 5 ms and stays at zero (the block applies it after
   the code and before metering, so the code needs no branch and the meters show the silence, R30). The data loop
   acknowledges a hold in the cycle its fade reached zero (an atomic the block sets, a signal to the main loop, as the
   built fade-out completes); every step that would break a held path waits for that acknowledgement, at most 100 ms
   (the built fade bound: past it the step goes on, `ear.fade-unacknowledged`). "Null audio frames for a few seconds"
   (R17) is a hold.
3. **New acquisitions are readied beside the old ones**, in readiness order: claims first (a refusal ends the change,
   T11), then files, then companions. Each old resource being replaced is marked **retiring** at once: its loss is a
   diagnostic (`ear.retiring-lost`), never a failure of the node (a kept resource's loss still fails it). A replacement
   companion is linked to the same inside bank as the one it replaces; two sources into one inside input mix for a
   moment, which a hold covers [U: the port mixer loads, 13].
4. **The commit point** (T10), when the hook's work is done (`applied`, or the continuation's `done`) and every new
   resource is ready: the staged posts are released to their loops; in one data-loop invoke (if the node is driving)
   the new code and its layout are handed to the block, which takes them up at a position it publishes, so a replaced
   sink takes the cycles before it and its replacement the cycles from it, and a replaced source likewise
   (`companions.md` 3.4): no chunk lost or doubled at the swap; the latency plan is carried; the old code released.
   Then the retiring resources are released by the teardown's order for those resources only (a replaced source's
   writer closed; replaced companions destroyed; replaced sinks drained to their own consumers and finished; replaced
   recordings closed; `unclaim`ed claims released, last); then holds fade back in over 5 ms; the node is back in
   `running` or `stopped`, and its applied settings are `now`. The loops and their states go on (no loop step).
5. **Deadline**: 10 s from step 1 by default; a type may declare up to 60 s (`.lifecycle = Lifecycle{.reconfigureDeadline
   = 30s}`). Past it, on a replacement refused or lost, or on the token's `fail(detail)`: T11, a restart with the new
   settings, tearing the whole configuration down, old and new resources alike. A restart that then fails is a failed
   start like any other. A failure never rolls back to the old settings: the desired graph committed the new ones, and
   `ear.conf` holds them (D-B4).
6. **One change at a time, never stuck behind one** (D-B6): a newer change before the commit point supersedes it (T18):
   uncommitted replacements are released (their claims and companions go), an unposted continuation and every staged
   post destroyed unrun, its token ignored, holds kept; then the newest settings are classified afresh against the
   applied ones (a `reconfigure` calls the hook again with `old` the applied settings; a `restart` or `reshape` is
   T11), and the deadline restarts. After the commit point a newer change, `stop` or `start` waits until T10 or T11.
   `node.restart` ends a change at once, before or after the commit point (T11; a continuation already posted runs
   first). A `stop`, a disable or a removal ends it before the commit point as a newer change would, and waits for it
   after (D-A8).
7. **Two nodes swapping ports in one edit** (`a` from 8928 to 8929, `b` the other way) cannot each hold both: each new
   claim is held by the other node's current configuration, so it is refused, and both fall back to restarts; each
   restart's claim then finds the other's old configuration going and waits for it (`node-primitives.md` 6.8). Both
   come back on their new ports, after a restart each.

**Break before make**, for a thing that cannot be held twice (one ALSA device opened twice; an RTP receiver on a port
its node already binds): the old one is released before the new one is made. The framework first holds every public
output bank the inside bank feeds through the pass-through (or, for a type's own code, every public output bank) and
the inside output bank that feeds the companion being destroyed (a card's `to_card`, so the device is silent before it
closes), waits for the acknowledgement, and only then destroys the old resource; so a break-first replacement fades to
zero, stays silent, and fades back in, whether or not the type held anything. The type asks with `Order::breakFirst`
where only it can know; the same-port receiver is the framework's own case (G22).

**What arrives mid-change** (with the built rules for every other phase): in `reconfiguring` before the commit point, a
newer change supersedes (T18); `stop` or a removal ends it and then stops; a disable ends it and then disables; after
the commit point each of these waits for T10 or T11, then applies; `node.restart` is T11 at once. A retune (slice 6)
moves the slots at once in every phase and never waits for a change.

## 6. The primitives in a change

### 6.1 Claims swapped make-before-break

`cx.claim` in a change is judged when the hook returns, before any other new acquisition; a refusal ends the change
(T11) and destroys its continuation unrun. `cx.unclaim` names a key the node holds and releases it at the commit point,
after the replaced resources, last. So a node moving from 8928 to 8929 holds both for a moment, and another node can
take 8928 only once the first is `running` on 8929. **A key the node already holds** (a replacement asking its own key)
is granted at once, not probed (the probe would fail against the node itself) (G22). A claim wait inside a change
counts against the change's deadline, not its own 15 s; past it the fallback restart's `configure` waits afresh.

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

A consumer that writes into something its node swaps in place (sendspin's library client, 6.6; an RTP sender's socket)
needs a moment when no `consume` runs. Without it every such type writes a lock around its target on the sink's thread;
with it the framework owns the one hand-over (G21).

```cpp
class SinkControl {                                    // cx.sink(...).control(): a loop's state keeps it
public:
	/// Any of the node's loops. The sink's thread finishes a consume in flight, then discards chunks (counted as paused
	/// frames, not drops) until resume(); `paused` is posted to `loop` once no consume can run. Until resume(), the
	/// consumer's state may be used by the thread that runs `paused`: the post is the hand-over. Pausing a paused sink
	/// posts `paused` at once.
	virtual void pause(Loop& loop, std::move_only_function<void()> paused) = 0;
	/// The thread that paused it. The next chunk goes to consume, with its own position, so the consumer sees the gap.
	virtual void resume() = 0;
	virtual std::uint64_t pausedFrames() const noexcept = 0;
protected:
	~SinkControl() = default;
};
```

`SinkRequest` gains `SinkControl& control()`. Paused audio is discarded, not queued: a swap is a break in what the node
sends, and the chunks' positions say where. A paused sink at teardown is drained by discarding, then `finish` is called;
a consumer's `finish` must cope with a pause it never saw resumed. A replaced sink drains to its own consumer and
finishes at the commit point; its replacement's `begin` runs as it becomes ready, before the commit point. Slice 5
samples `pausedFrames` beside the companion's drops.

### 6.5 `rtp` in place

`rtp`'s `address`, `port`, `format`, `rate` and `latencyMs` are `.inPlace()`; `channels` makes its ports (a reshape,
restart-only).

```cpp
Changed Rtp::configure(ChangeOf<spec>& cx, const Checked&, const Checked& now) {
	cx.hold(stream);                                         // null audio until the new receiver runs
	cx.rtpReceiver(fromRtp, receiverFor(now, cx.format()));  // replaces it: a new port make-before-break (claims the new
	                                                         // UDP port, releases the old at the end); the same port
	                                                         // break-first, by the framework; it declares latencyMs
	return Changed::applied();                               // no use: the pass-through stays, nothing is swapped
}
```

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
		return Changed::applied();
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
2. `toServer_.pause(worker_, ...)`: the sink's thread finishes any `consume` in flight and discards until `resume`, so
   `write_audio` never reaches a client being destroyed (this replaces the worker's `clientMutex_`);
3. says goodbye on mDNS, then `client->stop()`: goodbye to the server, the library's threads joined, `on_stream_end`
   delivered (which clears the source, which fades out); the client destroyed; the old sync thread, the source's
   writer, joined, so the new one may write (`node-primitives.md` 6.4);
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
| `rtp` | `address`, `port`, `format`, `rate`, `latencyMs` (6.5) | `channels`: a reshape |
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
  replace told as `node.removed` and `node.added`, `applied: "stored"`, D-R23.)
- **In place**, when every changed key is `.inPlace()` or `.live`: a reconfigure. The framework adds the new ports to
  the engine first, calls the change version, which must swap the code (its `Io` layout changed; a hook that does not
  fails the node with the rule's text), and removes the vanished ports after the swap's barrier. A type whose channel
  count is `.inPlace()`, going from 2 to 3 channels, keeps channels 1 and 2 playing. Companions on resized inside banks
  are replaced make-before-break.

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
- **Companions** (`companions.md` 3.7) gain `retiring: true` on the old entry during a make-before-break, listed before
  its replacement, which shares its name for a moment.
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

| Built piece | Change |
|---|---|
| `NodeOutcome`, `NodeChange`, `DesiredNodeListener` (`desired_graph.hpp`) | outcomes `reconfigure`, `reshape`, `unchanged`; `NodeChange` gains the old and new ports and `inPlace`; a change of ports with the same type is told by `nodeChanged`, not as a replace; `portCaused` goes |
| `DesiredGraph::plan` | reads each changed key's mark; a dry run is `plan` without `commit` (as built) |
| `ConfigurationMachine`, `NodeAggregate` (`node_lifecycle.hpp`) | a `changing` state; T9, T10, T11, T18; `NodePhase` and `NodeState` gain `reconfiguring` |
| `lifecycle.hpp` | `ChangeOf` defined; `Changed` with real bodies; `ConfigureDone`; `Lifecycle`; `SinkControl` |
| `NodeHost` | the in-place path: holds and their acknowledgement, retiring resources, the commit point, the deadline, supersession, the fallback; the block's reshape between teardown and `configure`; claims swapped (`node-primitives.md`'s registry) |
| `PortBlock`, `FilterPortBlocks` | `hold(banks)`, `release(banks)` with acknowledgement; `reshape(next)` with layouts swapped whole; levels re-registered |
| The sink companion | `pause` and `resume` on its thread |
| `rtp`, `sendspin` | rows marked `.inPlace()`; their change versions |
| `TypeHarness` | `change()` returns the outcome and steps an in-place change (holds, replacements, the continuation on `runLoops()`, the commit); `held(bank)`, `paused(bank)`, `pausedFrames(bank)`; `state()` stays `reconfiguring` until the token's `done` and every replacement is ready |
| `ear_config` | `node.set`'s merge; `dropRoutes` and `ports-in-use` in the candidate; dry runs posted and dropped |
| `ear_control`, `web/` | features `node.set`, `reshape`, `dryRun`; `keys`, `next`, the new causes; 8.3's editor |

## 10. Tests

- **`ear_core_tests`**: in-place, fallback (each `reason`), deadline, supersession (a newer change before and after the
  commit point; `restart` and `stop` during a change), a retiring resource lost, holds acknowledged before a break,
  break-first behind a framework hold, the code swap (a code swap at one position, a sink and its replacement splitting
  the cycles exactly), staged posts destroyed on supersession and fallback, a continuation run only once every
  acquisition is ready and on the loop named, a token dropped failing the change, a reshape in place by a test type
  (channels 1 and 2 keep playing); claims swapped make-before-break (both held, the old released last), a refused new
  claim destroying the continuation and restarting, two nodes swapping ports both restarting and running, a same-key
  replacement granted without a probe and made break-first; `keys` and `next` on `node.state`.
- **`ear_pipewire_tests`**: S4; reshape (a port added and removed beside a running ramp, links of surviving ports kept,
  the layout swapped whole, the transitional layout zero-filling new outputs); holds (the fade, then zeros, then the
  fade in); a reshape's new port metered from its first window; two sources into one inside input mixed (the port
  mixer loads).
- **`ear_config_tests`**: `node.set`'s merge; a dry run changes nothing and tells no one; `dropRoutes` removes exactly
  the stranded route lines in one write; a route line inside a snippet refused `shared-definition`; `ports-in-use`
  naming every route.
- **The sink pause**: waits for a consume in flight, discards and counts, posts `paused`; a paused sink at teardown
  finishes; a writer at full rate while pausing and resuming 10^4 times (U15).
- **Types**: `rtp`'s and `sendspin`'s in-place cases through the harness; the all-type tests run a change of every
  in-place row through the change version, ending `running` with the same configuration (no status reset) or in a
  restart whose cause is `fell-back` with `restart-asked`.
- **System tests**: an `rtp` port changed in place (`reconfigure`, `node.state` `reconfiguring` with `keys: ["port"]`
  and back, the WAV it feeds silent and then the tone with no `stopping` record, `levels` showing `fpp:stream.1` and
  `rec:record.left` silent during the change and back after); an `rtp` format changed on the same port (a fade to
  exactly 0, silence, a fade in, no step above the fade's slope); a card's `playback` from 2 to 1 on the test card with
  `dropRoutes`, the surviving route never `pending`; a `sendspin` port, name and server changed in place
  (`reconfiguring` and never `stopping`, the same client id, the key file not read again, the connection dropped and
  back), a change to a held port restarting and failing with today's text, dialling to listening and back.
- **Wire fixtures**: `test/protocol/node-set.json` (merge, `null`, a missing required key, each `applied`, `dryRun` for
  each outcome, a reshape refused `ports-in-use` then with `dropRoutes`); the reconfigure and reshape cases of
  `lifecycle.json`; `companions.json`'s `retiring`.

## 11. Order of work

1. S4, the spike, and the pure machines' new states and transitions (tables).
2. `ChangeOf`, `Changed`, staged posts, holds and their acknowledgement, the commit point, the deadline, supersession,
   the fallback, with a test type; `rtp` in place.
3. The primitives in a change: claims swapped, `whenReady`, `ConfigureDone`, the sink's pause, the same-port rule.
4. `PortBlock::reshape` (or, if S4 fails, the replace kept), layouts, levels.
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
| D-R18 | In place, `cx.use` is always a swap; the framework never compares codes | One rule; comparing would silently skip or add a swap |
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

- **S4** (7.4): whether PipeWire 1.4.2 adds and removes ports on a connected realtime filter safely. Fallback: replace.
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
- **U15.** That the sink's thread can stop calling a consumer between chunks without a lock on the ring's hot path.
- **Tuning.** The fade (5 ms), the hold acknowledgement's bound (100 ms) and the in-place deadline (10 s, at most 60 s)
  are first choices, to be tuned on the MOTU.
- **Risks.** A wider lifecycle: the number of interleavings (a change during a reconfigure, a loss during a reshape)
  grows; each is a state-table test. `applied` is not what happened (a client must read `node.state`); a dry run may be
  stale by the real edit. A claim wait in a change can cost up to its deadline and then the restart's own 15 s, about
  25 s before the node fails. `sendspin`'s in-place change still drops the connection and the stream (Music Assistant
  reconnects in seconds). A pause discards what the node sends during a swap.
