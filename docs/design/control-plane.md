# The control plane and parameters: one schema, one write path, one wire

Status: proposal, revision 2, 2026-11-08, after three independent reviews (section 16 records each finding and its
outcome). Nothing here is built. Layer: module design for slices 5 (control plane) and 6 (parameters) of
`docs/design/node-framework.md` (revision 4), beneath that page. It refines that page's sections 8.1 to 8.7, its section
9.2 where parameters touch the audio contract, and its section 13's slices 5 and 6; the table "What this page changes"
below says exactly where. It also lands slice 4's deferred node-facing primitives (8.8's erratum: event calls on the run
handle, and the realtime coded ring). It changes the wire of `docs/design/batch2.md` section 6 additively (protocol 0 to
1, section 4.1), and the standard-output schema of `test/protocol/output-records.json`.

[V] = verified in the source named, as of `f677a50`; [U] = unverified, with the step (section 11) whose test checks it,
or section 15 keeps it open. knobly's citations (`/opt/knobly/...`) were checked in the dev image.

## Scope, and what this page changes

Slices 5 and 6 are designed together because they share six things: one type schema (a parameter is a settings row
marked `.live`), one write path for what lasts (the graph file keeper: a gesture commits at its end into `ear.conf`, and
a hand edit aborts it; actions and restart are ordered by the keeper too), one wire vocabulary and protocol number, one
line drawn between parameter, status and event, one family of realtime channels (wait-free parameter slots into the
audio thread, the realtime coded ring out of it), and one editor model that renders controls, status and diagnostics
from the schema.

**The boundary with slice 3** (`docs/design/node-primitives.md`; its section 14 states the same boundary from its
side). Slice 3 owns loops (strands, dedicated), feeds, mailboxes, wakes, the I/O executor, stores, claims, and **the run
handle**: `RunHandle` (its section 5), one per run, generation-bound and shared-owned (node-framework.md I6), with
`node()`, `generation()`, `lost`, `log` and an interim paced `say`, reached by `ready.handle()` in the behaviour's
`build` (node-primitives.md 6.1; G15 and G17 there, written to their recommendations and pending the owner's answers).
This page adds to that handle and reshapes none of it: the typed `Report<spec>` over it (status writes and events), the
realtime ring and its bell, parameter slots, `Smoothed`, triggers, actions, and every wire and keeper change; it removes
`say` once sendspin's notes are split. Action calls and `writer::loop` fields run on loops, which slice 3 declares in the
spec as handles (its G14). So slices 5 and 6 are built after slice 3 (G6). Where the two pages name one thing, one page
is its home and the other cites it: `TypeSpec`'s whole member list and order is node-primitives.md 6.12; slice 3's
diagnostic codes and their texts are node-primitives.md's (6.4, 6.9, 6.10), put on the wire by 4.6 here; the realtime
calls (`io.set`, `io.raise`, `io.log`) and the ring the framework makes per run are this page's (5.4, 6.1).

| node-framework.md | What it says | What this page decides |
|---|---|---|
| 8.1 table, Parameters row | request `node.setParam`; notification `node.paramChanged`; stdout at commit only | requests `node.set` (a merge of named keys, for any row) and `gesture.begin`, `gesture.change`, `gesture.end`, `gesture.cancel`; one notification per gesture, `gesture.changed`, never echoed to the gesture's own session for `begin` and `change`; a `param` record on stdout for each committed live change and each abort (4.3, 9) |
| 8.1 table, Actions row | request `node.action`; nothing on stdout | request `node.invoke`; an `action` record on stdout (4.3, 9) |
| 8.1, status on stdout | every change | every change, paced per node and field, with the field's last value always written (6.5, 9; G9) |
| 8.2, text fields | written under a mutex per field | every off-realtime write goes into one bounded queue per run, under one mutex, drained by the tick; one FIFO per writer gives 8.1's "an event first flushes the status" for free (6.3) |
| 8.3, diagnostics | the framework's, per code | kept per code and **subject** (a field, a loop, a claim), each with `active` and the server's `count` (4.3, 4.6) |
| 8.4 | the handler's result is the reply | the reply of a batch holding an action waits for the handler (deadline 10 s), but the keeper never waits (7.6); `restart` replies once the teardown began, and is refused `not-restartable` on a lost block; a second kind of action, a **trigger**, goes to the run's code wait-free (5.5) |
| 8.5 | one method with a `gesture` field; `begin` refused when the row is on a line that makes other nodes | separate `gesture.*` methods; `begin` answered on the main loop from a **reach index** the keeper posts with each version (7.2); a gesture's **reach** is every node and key whose value is defined at the same place, all live and of one kind and unit, and the gesture moves them all (G1); any hand edit aborts every open gesture (G3); the committed value is rounded to the reach's `decimals` (G5) |
| 8.5, hand edit of a live row | moves the slot without a restart | a new `DesiredNodeListener::nodeRetuned`, defaulting to `nodeUpdated`, so `NodeHost` retunes and every other listener is unchanged (7.4) |
| 8.7 | `graph.get` gains `typeSchemaHashes`, a hash per type | one opaque hash for the whole set (`typeSchemaHash`); types are compiled in, so they change only with ear (4.2) |
| 8.7 | a higher `protocol` shows a banner | an absent `protocol` is 0 (today's wire), this page is 1; additive growth is announced in `features`, which clients gate each method on (4.1, G7) |
| 9.2 | `io.param(gainRow)` | `io.param<gainRow>()`, as `s.get<row>()` reads a row today; `Io` also gains `set`, `raise`, `log` and `triggered` (5.4); the ramp's rule is fixed with a worked vector (5.4) |
| 13, slices 5 and 6 | one paragraph each | ten buildable steps (section 11) |

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

`gain` is the new type of slice 6 (5.7): `channels` (default 2), a live `gainDb` and a live indexed family `trimDb.<i>`.

### 2.1 The operator, in the editor

**Turns a gain knob, and sees and hears it.**

1. The operator selects `amp`. The side panel draws its settings from the type schema (4.2): `channels` as a number
   field marked "restarts the node", `gainDb` as a fader from -inf to +24 dB showing `-6.0 dB`, `trimDb.1` and
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

**A hand edit lands mid-gesture.** The operator is holding `amp`'s knob at `-3.5`; a colleague saves `ear.conf` from
`vi` with `gainDb -10;` and a new comment.

1. The keeper's inotify watch fires; it reads the file and offers it (cause `disk`).
2. On the main loop, before the plan, every open gesture is closed as aborted (G3): every client gets
   `gesture.changed {"gesture":"g4","phase":"aborted","reason":"hand-edit"}`, with no value. The slots stay where the
   hand left them for the rest of this turn. Then `graphFile.changed` (cause `disk`), the plan, the retune of `amp` to
   `-10` with `node.updated`; last in the turn every slot the gesture reached is set to the value now running. So the
   audio ramps once, from -3.5 straight to -10 (7.3).
3. The operator's editor snaps the knob to `-10`, ignores the pointer until it is released, and shows a notice: "ear.conf
   was edited by hand while you held amp gainDb; it now says -10." with a button "Set -3.5 again" (a `node.set`).
   Releasing sends nothing (the gesture is gone). The audio never stayed on a value `ear.conf` did not hold.

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
4. If the run ends (a restart, a failure), the server sends the run's status reset (4.3): the callout and the box's
   status line go at once, and they stay hidden while the node is not `running` (8.4). A client never shows a pairing
   code from a run that is gone.

**Invokes restart.** `desk` is `failed engine-refused: card lost` after a USB unplug; the card is back.

1. The panel shows every action the schema lists for `card`: only the framework's `restart`. The operator clicks it.
2. The editor sends `node.invoke {"name":"desk","action":"restart"}`. The keeper orders it with every other request;
   on the main loop `NodeHost` tears the run down and starts it again behind the same block, without touching
   `ear.conf`. The client gets the run's status reset, `node.state` `starting` (with the new `run`), then the reply
   `{}`, then later `node.state` `running` (4.3's order rule 5).
3. Standard output has `node desk restart invoked` (an `action` record) before the `node` records. The button is
   disabled while the node is `stopping`, while the request is in flight, and while the node's block is lost
   (`"blockLost": true` in the snapshot), its description then saying why: "the node's engine block is gone; remove and
   add the node, or restart ear". A script that tries anyway is refused `not-restartable`.

### 2.2 The script author, with `scripts/rpc-call.mjs`

```bash
U=ws://127.0.0.1:7777/rpc
# Set one key, leaving the others as they are. A live row retunes; any other row restarts the node.
node scripts/rpc-call.mjs $U node.set '{"name":"amp","settings":{"gainDb":-12}}'
{"applied":"retune"}
# Remove a key (back to its default): null.
node scripts/rpc-call.mjs $U node.set '{"name":"amp2","settings":{"trimDb.2":null}}'
{"applied":"retune"}
# Read a status field.
node scripts/rpc-call.mjs $U graph.get | jq -r '.nodes[] | select(.name=="kitchen") | .status.pairingCode'
123-456
# Restart a node.
node scripts/rpc-call.mjs $U node.invoke '{"name":"desk","action":"restart"}'
{}
# What does a type take?
node scripts/rpc-call.mjs $U types.get | jq '.typeSchemas[] | select(.name=="gain") | .settings[] | {key, kind, min, max, unit, live}'
```

A refusal is printed to standard error and exits 1, with the same `data.code` grammar as today, and the key it is
about, so a tool never parses the message:

```json
{"code":-32000,"message":"invalid-settings amp: gainDb: must be a number of dB from -90 to 24","data":{"code":"invalid-settings","key":"gainDb","detail":"must be a number of dB from -90 to 24"}}
```

To wait for something (a pairing code, a node running), step 5.2 adds `--follow` to `rpc-call.mjs`: it subscribes to the
graph and prints every notification as one line, so `| jq --unbuffered 'select(.method=="node.statusChanged")'` waits
on what it needs. A script that holds a gesture must send a tick at least every 30 s (6.5); scripts rarely need one,
since `node.set` changes a live row without a gesture.

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
4. **Fold notifications into the snapshot** (4.4). A node carries `run`, `status`, `gestures`, `diagnostics` and
   `blockLost`. A `node.statusChanged` with `reset: true`, or with a `run` above the node's, *replaces* the status; any
   other merges. `node.diagnostic` replaces the entry of its code and subject, and carries the server's `count`.
   `node.eventRaised` changes nothing (an event has no lasting value) and goes to an activity list that starts when you
   connect. Order is today's: one ordered stream per client, a change's notification before the statuses it causes
   (batch2.md section 6).
5. **Gestures.** Begin, tick with notifications (keep-alive every 2 s while held), end. You get no `begin` or `change`
   echo for your own gestures, so any `begin` you see is another session's. On `phase: "aborted"` stop the control,
   show `value` when it is given, else wait for the `node.updated` that follows, and tell the person (8.3).

### 2.4 The node type author

The author adds, to one type, a live parameter, a status field, an event and an action. The complete type is `gain`
(5.7); this is the part that matters, against slice 2's interface (`type_spec.hpp`, `settings_table.hpp`) and the
additions of section 5.

```cpp
// src/nodes/gain/gain_spec.hpp
namespace ear::nodes::gain {
inline constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 64, "channels"), 2);
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
`s.get<gainRow>()` in `start` (where it would go stale at the first retune; `s.initial<gainRow>()` says so in its name),
and it does not compile (5.8).

Two limits a type author should know. A live row reaches only the run's realtime code, through `io.param`; a run
object or an outside resource cannot react to a live change in this slice, so a value that must be applied off the
realtime thread (a sound card's mixer volume, a volume sent to a server) is a non-live row that restarts the node until
the hook of G10 is built. And status written off the realtime thread is written from a loop of the spec the field
names (`writer::loop(worker)`, 5.2), never from a thread of the type's own: slice 3 gives a type no threads.

The type's tests drive it all through the harness (5.9):

```cpp
TEST_CASE("gain follows a gesture and latches clipping", "[gain]") {
	ear::testing::TypeHarness<gain::spec, gain::Gain> h;
	REQUIRE(!h->parse({{"channels", 1}, {"gainDb", 0.0}}));
	REQUIRE(!h->start().failed());
	std::ranges::fill(h.input(gain::in, 0), 2.0F);
	h->process(256);
	CHECK(h.status<gain::clipping>());
	CHECK(h->events().at(0).code == "clipped");
	h.live<gain::gainRow>(-12.0); // as a gesture tick
	h->process(256);              // ramping: 20 ms is 960 frames at 48 kHz (5.4's vector)
	h.trigger<gain::resetClip>();
	h->process(256);
	CHECK_FALSE(h.status<gain::clipping>());
}
```

## 3. The domain model

### 3.1 The nine words

| Concept | What it is | Declared in | Written by | Kept | Owner of the truth | On the wire | On stdout |
|---|---|---|---|---|---|---|---|
| **Setting** | What the operator wants, surviving restarts | a `SettingsTable` row | the keeper (from `ear.conf`, `node.*`, `graphFile.set`) | `ear.conf` | `ear.conf`, judged by the desired graph | node `settings`; `node.updated` | `graph-file` record |
| **Parameter** | A setting declared `.live`: it also has a realtime value that moves without a restart | a row with `.live(smoothing)` | committed: the keeper; live: a gesture's ticks | committed: `ear.conf`; live: memory, only during a gesture | committed: `ear.conf`; live: the gesture's owner | `gestures`; `gesture.changed` | `param` record at commit and abort |
| **Status** | A fact the node observes that has a current value | a `StatusTable` field with one writer | that writer, through `Report` or `Io`; the framework's own `ear.` fields | memory, per run; reset when the run closes | the declared writer | node `status`; `node.statusChanged` | `status` record per change, paced per field (6.5) |
| **Event** | Something that happened at an instant, with no lasting value | an `EventTable` code | the run, through `Report` or `Io` | not kept | the run | `node.eventRaised` | `event` record |
| **Action** | A command into a node that changes no desired state | an `ActionTable` entry, or the framework's `restart` | a client, ordered by the keeper | not kept | the node | `node.invoke` | `action` record |
| **Node diagnostic** | What the framework observes about a node's machinery | the framework's code list (4.6) | the framework alone | per code and subject: the latest, whether active, a count | the framework | node `diagnostics`; `node.diagnostic` | `diagnostic` record |
| **Log** | Free text for people debugging | levels; `LogCodes` for the realtime thread | any node code; captured output | not kept | whoever wrote it | not yet (`log.subscribe`, later) | `log` record |
| **Note** | Retired. Today's `InsidesEvents::say` sentence; about 11 of sendspin's 17 are state, the rest edges | `notes.hpp` | sendspin | not kept | — | — | `note` record, until step 5.5 removes the last |
| **Health** | Whether the node is up: state, reason, whether its block is lost (and from slice 8 the attempt and last failure) | the framework's state machine | the framework alone (`NodeLifecycle`) | memory | the framework | node `state`, `blockLost`; `node.state` | `node` record |

A **node diagnostic** is not a **graph-file diagnostic**. The second is today's problem in `ear.conf`'s text (a knobly
or `ear/...` code at a line and column: `Diagnostic`, `graph_file_state.hpp`, and `Diagnostic` in `protocol.ts:70-74`
[V]). They keep apart names everywhere: `NodeDiagnostic` (C++ and `protocol.ts`), node `diagnostics` beside
`GraphFile.diagnostics`, and an event's `EventSeverity { info, warn, error }` (spelled as `LogLevel`'s values) beside the
file's `Severity { error, warning }` (`graph_file_state.hpp:19` [V]).

### 3.2 The rule for choosing

Ask in this order; the first yes decides.

1. **Would the operator expect it after a restart of ear?** It is a **setting**. If it must move while audio plays,
   without a gap, is a number, and is used by the run's realtime code, it is a **parameter** (`.live`). A value used
   only off the realtime thread is a plain setting in this slice (G10).
2. **Does it have a value right now, which someone could read at any moment?** It is **status**. A value that is
   "none" most of the time is still status (`pairingCode` is `""` until a code is offered). A value that is not known
   yet is `null` (a choice starts unknown, 5.2).
3. **Did something happen that someone might act on, and is there nothing to read once it passed?** It is an
   **event**. If the thing also leaves a value behind, write both: `paired` (event) and `paired = true` (status).
4. **Is it a request from outside to do something now, which nothing should redo after a restart?** It is an
   **action**. If it should survive a restart, it is a setting.
5. **Is it about the node's machinery (queues, timing, restarts, rates, loops, claims), not its domain?** It is the
   framework's **node diagnostic**, or one of the framework's `ear.` status counters (4.6); a type cannot write either.
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
8); `gain`'s `clipping` is status and `clipped` is an event; "a run did not close in 10 s" is the diagnostic
`ear.close-overdue`; frames a feed came up short are the framework's status counter `ear.feedShortfall`; a library's
warning text is a log line.

### 3.3 Aggregates and owners

- **The node** (node-framework.md section 6) gains: a run number (`run`, from 1, one more per start), the status view
  (one value per declared field, and the framework's `ear.` counters its run has), transition counters, the node
  diagnostics kept per code and subject, `blockLost`, and, per live row, the live value and the gesture holding it, if
  any. All on the main loop, owned by `NodeHost`. Node diagnostics belong to the node, not the run: a restart does not
  clear them (flapping across restarts is exactly what they are for); status belongs to the run.
- **The gesture** is its own small aggregate, owned by a new main-loop service, `Gestures` (`ear_core`): an id (opaque,
  unique for the process's life, never reused), the session that owns it, its reach (the node and key pairs it moves)
  with the combined range and decimals, the committed value, the live value, the last sequence number, the time of the
  last tick, and whether its end or cancel has been read. It never outlives its session, its nodes, or a hand edit.
- **The reach index** is a value the keeper makes on its thread for each version of the working copy and the main loop
  holds for the running one: for each (node, live key) that `ear.conf` gives, the definition's place, the reach, and
  the committed value (7.2). It lets `gesture.begin` answer without the keeper.
- **The type schema** is a value made from the type spec at compile time and encoded once at boot: what the wire's
  `types.get` returns and what the editor renders.

## 4. The wire

### 4.1 Naming, versioning and compatibility

- **Names.** Requests are `<entity>.<verb>` (`node.set`, `node.invoke`, `gesture.begin`, `types.get`), as `node.add` and
  `graphFile.set` are. Notifications say what happened (`node.statusChanged`, `node.eventRaised`, `node.diagnostic`,
  `gesture.changed`), as `node.updated` and `graphFile.changed` do. JSON object keys are camelCase (`windowMs`,
  `pairingCode`); status field names and setting keys are camelCase; codes, event codes and action names are kebab-case
  (`pairing-failed`, `reset-clip`), the framework's under `ear.` (`ear.status-flapping`, and the status counters
  `ear.feedShortfall`). Durations end in `Ms`; instants are `t`, UTC to the microsecond, as on standard output.
- **One value-kind vocabulary** on the wire, for setting rows, status fields, event arguments and action arguments:
  `text`, `wholeNumber`, `decimal`, `flag`, `choice`, with `values` for a choice's values and for a text row's fixed
  set. (In C++ a whole number is `number`, as `setting::number` is today; `decimal` is a real number.) `flag` exists for
  status and arguments; a `flag` setting row is G11.
- **The protocol number.** `graph.get`'s result gains `"protocol": 1` from step 5.1. **An absent `protocol` is 0**:
  today's wire, with none of this page's methods. The number moves only on a breaking change: removing or renaming a
  method, notification, member or code, changing a member's type, or changing an order guarantee.
- **Features, for additive growth.** The graph also gains `"features": [...]`, a list of names; a client gates every
  method and every request member it sends on the feature that added it, never on a -32601 probe. This page's
  features, each added by the step that builds it (section 11): `types` (5.1), `status` (5.2: status, events, node
  diagnostics), `actions` (5.4: `node.invoke`), `node.set` (5.4), `live` (6.3: decimal, indexed and live rows,
  `applied: retune`), `gestures` (6.4). Later slices add theirs (slice 8's `recovery`); the list only grows within a
  protocol.
- **Members ear sends are additive**: results, notifications, records and snapshots may gain members at any time, and
  a client ignores members it does not know. **Members a client sends are not**: ear's decoders refuse an unknown key
  (`requireObject`, `src/control/graph_json.cpp:33-42` [V]), which keeps typo detection, so a request member exists
  only from the feature that adds it and a client checks the feature first. `errors.json` gains the case of an unknown
  request member (-32602, `data.pointer` naming it).
- **Every enum is open from protocol 1** (G7): `state`, a route's `status`, `phase`, `reason`, `severity`, a row's
  `kind`, `applied`, `defaultFrom`, codes of every sort. A client draws an unknown value neutrally (state `starting`'s
  style, a code as its text, an unknown event as its `msg`) and never fails on one. So slice 8's `recovering` is
  additive. `web/src/protocol.ts:13` keeps its closed union for typing but gains a fallback branch.
- **A client that sees a higher `protocol`** shows a banner and keeps working with what it understands.
- **Today's clients keep working.** Every request they send keeps its meaning but one: a `node.update` that changes only
  live rows no longer restarts the node (7.4). The editor already ignores unknown notifications
  (`web/src/graph.ts:107-108` [V]). `node.update` refused with the new `parameter-held` shows its message as any
  refusal does (`App.tsx`'s `describe`).
- **Inside a batch**, `gesture.change` and `gesture.cancel` are refused (`not-in-batch`): they skip the keeper, so they
  could not keep a batch's order.

### 4.2 Requests

All are JSON-RPC 2.0 requests with named params. Errors are -32000 with `data.code` (4.5); a malformed param is -32602
whose message names the JSON pointer, as today, and whose `data.pointer` now carries it too.

| Method | Feature | Params | Result | `data.code` refusals | Path |
|---|---|---|---|---|---|
| `types.get` | `types` | `{"ifNoneMatch"?: string}` | `{"protocol","hash","typeSchemas","frameworkStatus"}`, or `{"protocol","hash","notModified": true}` | none | control thread (fixed at boot) |
| `node.set` | `node.set` | `{"name", "settings": {key: value \| null}}` | `{"applied"}` | unknown-node, invalid-settings, parameter-held, shared-definition, file-invalid, stopping, and the desired graph's | keeper |
| `gesture.begin` | `gestures` | `{"name", "key"}` | `{"gesture", "value", "reach", "min", "max", "decimals"}` | unknown-node, not-live, shared-definition, parameter-held, file-invalid, file-refused, stopping | control thread, then the main loop (7.2) |
| `gesture.change` | `gestures` | `{"gesture", "seq", "value"}` | `{}` if sent with an id | unknown-gesture, invalid-settings, not-in-batch | control thread, then the main loop directly |
| `gesture.end` | `gestures` | `{"gesture", "value"?}` | `{"value"}`: what was committed | unknown-gesture, invalid-settings (the gesture stays open), stopping, and the desired graph's (the gesture is aborted) | keeper |
| `gesture.cancel` | `gestures` | `{"gesture"}` | `{}` if sent with an id | unknown-gesture, not-in-batch | control thread, then the main loop directly |
| `node.invoke` | `actions` | `{"name", "action", "args"?: {...}}` | `{}` | unknown-node, unknown-action, invalid-args, not-running, not-restartable, restarted, stopping, action-failed, action-timeout | keeper, then the run |

`node.update`'s result gains `applied` too (additive, with the `node.set` feature).

**`types.get`**

```json
{"jsonrpc":"2.0","id":7,"method":"types.get","params":{"ifNoneMatch":"fnv1a64:0000000000000000"}}
```
```json
{"jsonrpc":"2.0","id":7,"result":{
  "protocol":1,
  "hash":"fnv1a64:9c2e41f07a3b5d18",
  "frameworkStatus":[
    {"name":"ear.feedShortfall","kind":"wholeNumber","initial":0,"unit":"frames","display":"panel",
     "help":"frames the node's feed came up short this run"},
    {"name":"ear.handoffDropped","kind":"wholeNumber","initial":0,"unit":"frames","display":"panel",
     "help":"frames the node's handoff had no room for this run"}],
  "typeSchemas":[
    {"name":"gain",
     "settings":[
       {"key":"channels","kind":"wholeNumber","required":false,"default":2,"min":1,"max":64,"unit":"channels"},
       {"key":"gainDb","kind":"decimal","required":false,"default":0,"min":-90,"max":24,"unit":"dB",
        "decimals":1,"taper":"db","minIsSilence":true,"live":{"smoothingMs":20}},
       {"key":"trimDb","kind":"decimal","required":false,"default":0,"min":-24,"max":24,"unit":"dB",
        "decimals":1,"taper":"linear","live":{"smoothingMs":20},"indexed":{"by":"channels","from":1}}],
     "inputs":["in"],
     "outputs":["out"],
     "status":[
       {"name":"clipping","kind":"flag","initial":false,"display":"panel","help":"an output went above full scale",
        "flapsAbove":{"transitions":10,"windowMs":10000}}],
     "events":[
       {"code":"clipped","severity":"warn","sentence":"channel {channel} clipped at {peakDb} dB",
        "args":[{"name":"channel","kind":"wholeNumber","min":1,"max":64},{"name":"peakDb","kind":"decimal"}]}],
     "actions":[
       {"name":"restart","by":"ear","help":"stop the node and start it again, without editing ear.conf","args":[]},
       {"name":"reset-clip","by":"type","target":"code","help":"clear the clipping flag","args":[]}]},
    {"name":"rtp",
     "settings":[
       {"key":"address","kind":"text","required":true,
        "mustBe":"\"0.0.0.0\" or an IPv4 multicast group (224.0.0.0-239.255.255.255), as a dotted quad",
        "pattern":"^[0-9]{1,3}(\\.[0-9]{1,3}){3}$"},
       {"key":"port","kind":"wholeNumber","required":true,"min":1,"max":65535},
       {"key":"format","kind":"text","required":true,"mustBe":"\"L16\" or \"L24\"","values":["L16","L24"]},
       {"key":"channels","kind":"wholeNumber","required":true,"min":1,"max":64},
       {"key":"rate","kind":"wholeNumber","required":false,"min":8000,"max":192000,"unit":"Hz",
        "defaultFrom":"graphFrequency"},
       {"key":"latencyMs","kind":"wholeNumber","required":false,"default":100,"min":1,"max":1000,"unit":"milliseconds"}],
     "inputs":[],"outputs":["stream"],"status":[],"events":[],
     "actions":[{"name":"restart","by":"ear","help":"stop the node and start it again, without editing ear.conf","args":[]}]}
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
- `inputs` and `outputs` are the public banks' names, in declaration order. Members come from settings, so the schema
  does not list them; inside banks are never on the wire.
- A status field: `name`, `kind` (`flag`, `wholeNumber`, `decimal`, `choice`, `text`), `initial` (`null` when it starts
  unknown), `display` (`panel`, `box`, `callout`), `flapsAbove` (`{"transitions","windowMs"}`), and as they apply
  `min`, `max`, `unit`, `values` (a choice's), `maxBytes`, `help`. Its writer is not on the wire.
- `frameworkStatus`: the framework's own status fields (4.6), the same shape; a node has those its run's resources
  give it.
- An event: `code`, `severity` (`info`, `warn`, `error`), `sentence` (with `{arg}` places), `args` (each `name`, `kind`
  of `wholeNumber`, `decimal`, `flag`, `choice`, `text`, and `min`, `max`, `values` as they apply).
- An action: `name`, `by` (`ear` or `type`), `target` (`code` for a trigger, `run` for a call), `help`, `args` (setting
  rows, as above). Every type lists `restart` first.
- The hash is opaque: FNV-1a 64 over the exact bytes of the `typeSchemas` and `frameworkStatus` encoding made at boot,
  written `fnv1a64:<16 hex>`. Clients only compare it. It is not a security measure.
- The graph's `types` (a list of type names, `protocol.ts:63` [V]) stays as it is; the schemas are `typeSchemas`, the
  name node-primitives.md section 14 uses.

**`node.set`** sets the named keys and leaves the others (a JSON merge patch of the node's settings: `null` removes a
key, which then reads its default; removing a required key is `invalid-settings` with `data.key` and detail `missing`).
It is `node.update` with the node's current settings merged, made at the source like every edit. `applied` says what
the commit did to the node: `retune` (only live rows changed: no restart), `restart` (the run restarted behind the same
block), `replace` (its ports changed: removed and added, routes included), `unchanged` (nothing differed; nothing is
written).

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
row's default. A node with no run (failed, starting) can be held: ticks set the node's live value, which seeds its next
run's slots, and the end commits as usual.

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

**`node.invoke`** runs an action. `restart` is every node's. A trigger is answered once it is handed to the code (not
when the code took it); a call is answered with its handler's outcome.

```json
{"jsonrpc":"2.0","id":12,"method":"node.invoke","params":{"name":"desk","action":"restart"}}
{"jsonrpc":"2.0","id":12,"result":{}}
{"jsonrpc":"2.0","id":13,"method":"node.invoke","params":{"name":"amp","action":"reset-clip"}}
{"jsonrpc":"2.0","id":13,"result":{}}
{"jsonrpc":"2.0","id":14,"method":"node.invoke","params":{"name":"kitchen","action":"forget-pairing","args":{"server":"ma-7f3c"}}}
{"jsonrpc":"2.0","id":14,"result":{}}
```

### 4.3 The graph, and notifications

`graph.get` and `graph.subscribe` answer today's Graph with three members more at the top and five more per node:

```json
{"protocol":1,
 "features":["types","status","actions","node.set","live","gestures"],
 "typeSchemaHash":"fnv1a64:9c2e41f07a3b5d18",
 "graphFrequency":48000,"blockSize":256,"realtime":true,"scheduling":"SCHED_FIFO 88",
 "types":["card","file","gain","rtp","sendspin","white"],
 "nodes":[
   {"name":"amp","type":"gain","settings":{"gainDb":-6},"inputs":["in.1","in.2"],"outputs":["out.1","out.2"],
    "state":"running","run":1,"blockLost":false,
    "status":{"clipping":false},
    "gestures":[{"gesture":"g4","key":"gainDb","value":-4.25}],
    "diagnostics":[]},
   {"name":"kitchen","type":"sendspin","settings":{"server":"ws://192.168.1.10:8927/sendspin","name":"Kitchen"},
    "inputs":["to.left","to.right"],"outputs":["from.left","from.right"],"state":"running","run":3,"blockLost":false,
    "status":{"clientId":"5d0c...","connection":"connected","url":"ws://192.168.1.10:8927/sendspin","listenPort":0,
              "instance":"","serverName":"Music Assistant","paired":false,"pairingCode":"123-456","pairingHeld":false,
              "stream":"none","streamRate":0,"streamChannels":0,"streamBits":0,"sending":false,"sendingRate":0,
              "ear.feedShortfall":0,"ear.handoffDropped":0},
    "gestures":[],
    "diagnostics":[{"code":"ear.status-flapping","subject":"connection","active":true,"count":2,
                    "t":"2026-11-08T10:02:11.120400Z","transitions":14,"windowMs":10000,
                    "msg":"node kitchen connection flapping: 14 changes in 10s"}]}],
 "routes":[]}
```

- `features`: 4.1.
- `run`: the node's run number, from 1; 0 while it has had none (a node whose block is not ready yet).
- `blockLost`: the node's block is gone (I1); it cannot restart until it is removed and added, or ear restarts.
- `status`: every declared field, and the framework's `ear.` counters its run has, each at its initial value until the
  run writes it; reset to the initial values when a run closes (I6), and the reset is sent (below). Always present
  (`{}` for a type with none).
- `gestures`: the open gestures that move one of this node's keys, with their live value. The committed value is in
  `settings` (or the row's default). Always present.
- `diagnostics`: one entry per code and subject (4.6), each with `active`, `count` (how many times that code and
  subject was raised or became active since the node was added), `t`, `msg` and its fields. Always present.
- One main-loop turn makes the whole snapshot, so status, health, gestures and diagnostics agree with each other.

`node.added` gains `run` (0), `blockLost`, `status` (the initial values), `gestures` (`[]`) and `diagnostics` (`[]`), so
a client need not fetch the schema to show a new node. `node.state` gains `run`: `starting` carries the new run's
number. `node.updated` gains `applied` (`retune` or `restart`; a replacement is `node.removed` and `node.added`, as
today).

| Notification | Params | From | Rate |
|---|---|---|---|
| `node.statusChanged` | `{"name","run","fields":{name: value},"reset"?: true}`: the fields that changed since the last one; with `reset`, every field at its initial value | the status drain | at most 30 a second per node |
| `node.eventRaised` | `{"name","run","code","severity","t","args":{...},"msg"}` | the status drain | 32 at once, then 1 a second per node |
| `node.diagnostic` | `{"name","run"?,"code","subject","active","count","t","msg", the code's fields}` | the framework | per code (4.6) |
| `gesture.changed` | `{"gesture","phase", and by phase: "reach","value","reason","code","key","detail"}` | `Gestures` | `change`: at most 30 a second per gesture |

```json
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","run":3,"fields":{"connection":"connected","serverName":"Music Assistant","paired":false}}}
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","run":3,"fields":{"pairingCode":"123-456"}}}
{"jsonrpc":"2.0","method":"node.eventRaised","params":{"name":"kitchen","run":3,"code":"pairing-failed","severity":"warn","t":"2026-11-08T10:01:52.004211Z","args":{"reason":"pairing-code-mismatch"},"msg":"pairing failed: pairing-code-mismatch"}}
{"jsonrpc":"2.0","method":"node.statusChanged","params":{"name":"kitchen","run":3,"reset":true,"fields":{"clientId":"","connection":null,"url":"","listenPort":0,"instance":"","serverName":"","paired":false,"pairingCode":"","pairingHeld":false,"stream":null,"streamRate":0,"streamChannels":0,"streamBits":0,"sending":false,"sendingRate":0,"ear.feedShortfall":0,"ear.handoffDropped":0}}}
{"jsonrpc":"2.0","method":"node.diagnostic","params":{"name":"kitchen","run":3,"code":"ear.status-flapping","subject":"connection","active":true,"count":2,"t":"2026-11-08T10:02:11.120400Z","transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"begin","value":-6,"reach":[{"name":"amp","key":"gainDb"}]}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"change","value":-4.25}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"end","value":-3.5}}
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
- `phase: "aborted"` carries `reason`: `hand-edit` (a disk change), `file-saved` (`graphFile.set`), `cancelled`,
  `session-closed`, `idle` (no message about it for 30 s, 6.5), `refused` (its end was refused; the end's reply carries
  the code), `node-removed` (removed or replaced). It carries `value`, the committed value the slots returned to,
  except on `hand-edit` and `file-saved`, where the slots take the value the new file runs, told by the `node.updated`
  that follows (7.3).
- **A run's status reset.** When a run closes (I6: its teardown's step 5, or its leak past the close deadline), the
  node's status view goes back to every field's initial value, and the drain sends one `node.statusChanged` with
  `reset: true` and every field, carrying the closed run's number, after that run's last values. A client replaces the
  node's status with it. A `node.statusChanged` whose `run` is above the node's also replaces (a client that missed the
  reset loses nothing). The framework's counters reset with it.

**Order**, added to batch2.md section 6's rules:

1. Within a node, the drain sends a `node.statusChanged` with the fields written so far before any `node.eventRaised`
   that followed them in the same writer's order, so `connection = connected` reaches a client before `paired`.
2. A run's last status values reach a client before the `node.state` its loss causes (the loss crosses after them in
   the run's queue, 6.3), and its reset follows its last values.
3. A gesture's begin: `gesture.changed` `begin` to the other sessions, then the reply to the owner, in one main-loop
   turn. A gesture's end: `graphFile.changed`, then `node.updated` (the committed settings, `applied: "retune"`), then
   `gesture.changed` `end`, then the reply.
4. An abort by a hand edit or a save: `gesture.changed` `aborted` for each open gesture, then `graphFile.changed`, then
   that change's graph events; the slots settle last, in the same turn.
5. A restart: the old run's status reset, then `node.state` `starting` (with the new `run`), then the reply; `running`
   comes later.

### 4.4 What a client folds, and what it ignores

| Notification | Changes | Ignored when |
|---|---|---|
| `node.statusChanged` | with `reset`, or a `run` above the node's: the node's `status` replaced and its `run` set; otherwise merged | the node is unknown, or `run` is below the node's |
| `node.state` | `state`, `reason` and `run` | the node is unknown |
| `node.eventRaised` | nothing in the graph; the activity list | the node is unknown |
| `node.diagnostic` | the node's `diagnostics`: the entry of the same `code` and `subject` replaced, `count` as sent | the node is unknown |
| `gesture.changed` | `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value` wherever the id is; `end` and `aborted` remove it everywhere; `tick-refused` changes nothing (the owner's notice) | no node holds the id (except `begin`) |
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
| `not-running` | `node.invoke` of a type's action: the node has no run (failed, starting before `build` made its run object, stopping) | |
| `not-restartable` | `node.invoke` of `restart`: the node's block is lost (I1) | `reason` (`block-lost`) |
| `restarted` | `node.invoke`: the run ended (restart, settings change) before the action ran | |
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
{"jsonrpc":"2.0","id":21,"error":{"code":-32000,"message":"shared-definition amp2 trimDb.1 at 3:6: also amp3 latencyMs","data":{"code":"shared-definition","place":{"line":3,"column":6},"with":{"name":"amp3","key":"latencyMs"}}}}
```

`web/src/protocol.ts` gains `ControlErrorCode` beside `CommandErrorCode` and `FileErrorCode`; C++ gains
`ControlRefusal` in `ear_control` beside `FileRefusal` (`graph_file_state.hpp:59` [V]), with `controlRefusalCode` the one
home of these strings.

### 4.6 The framework's node diagnostics and status counters

Each diagnostic is kept per **code and subject** (the subject is a field, a loop or a claim; empty when the code has
none). `active` says whether the condition holds now: a condition code (flapping, a claim wait) becomes active and later
inactive with the same code and subject; an observation code (a drop, a slow handler) is sent with `active: false` and
only joins the list. The editor badges active entries and lists all, and needs no per-code knowledge to clear a badge.

| Code | Subject | Fields | When | Rate | Owner |
|---|---|---|---|---|---|
| `ear.close-overdue` | — | `detail` (what was still open, with loops) | a run did not close within the close deadline and was leaked (I5a; built [V: `node_host.hpp:45-52`]) | once per run | slice 1, wire here |
| `ear.loop-slow` | the loop's name | `ms` | a strand handler ran past 100 ms (node-primitives.md 6.4) | at most once per 10 s per loop | slice 3, wire here |
| `ear.loop-source-left` | the loop's name | `sources` | a run object called `closed` with sources still armed on that loop, cancelled at teardown step 4b (node-primitives.md 6.4) | once per run and loop | slice 3, wire here |
| `ear.claim-wait` | the claim (`tcp:8928`) | `holder`, `waitedMs` | active while a run waits for a claim a going run holds; inactive once granted or failed at 15 s (node-primitives.md 6.9) | on each change | slice 3 (node-primitives.md 6.9 names it and its text), wire here |
| `ear.close-late` | — | `detail` | a run leaked at the close deadline closed later; its claims are released (node-primitives.md 6.10) | once per run | slice 3, wire here |
| `ear.status-flapping` | the field | `transitions`, `windowMs` | active once a field changes more than its `flapsAbove` within the window; inactive once a whole window passes at or below it | on each change, and at most once per window while active (with the new count) | here |
| `ear.events-dropped` | — | `heldBack` | events held back by the per-node rate (node-framework.md 8.3; it replaces slice 3's `notes-dropped` note) | at most once a second per node | here |
| `ear.records-dropped` | `status`, `realtime` or `queue` | `count` | `status` records held back on stdout (6.5), or records a run's realtime ring or queue had no room for | at most once a second per node and subject | here |
| `ear.realtime-overrun` | — | `codeUs`, `cycleUs` | reserved for a code that ran past its cycle; not built here | — | later |

Slice 8 adds `ear.health-flapping`, `ear.recovery-attempt` and `ear.readiness-missed` in the same shape.

**The framework's status counters** are status fields the framework declares, named under `ear.` (a dot no type's
field name can hold, 5.2), listed once in `types.get`'s `frameworkStatus`: `ear.feedShortfall` (frames a read of the
run's feed came up short, node-primitives.md 6.5's `shortfall`) for a run that holds a feed, and `ear.handoffDropped`
(frames its handoffs had no room for) for a run that holds a handoff. The tick samples them at most once a second per
node and sends a change in `node.statusChanged`; they reset with the run's status. They are counts, not states, so they
are not `status` records on stdout and are not judged for flapping; the handoff's count stays on the node's `stopped
dropped=<n>` record as today.

### 4.7 Fixtures

Each new file keeps today's shape (`{"description","requests","messages","graph","file"?}`), read by `ear_control`'s
tests and the editor's reducer tests.

| File | Covers |
|---|---|
| `test/protocol/types.json` | `types.get` with no hash, a stale hash and the current one; the schema of every built-in type, `frameworkStatus`, a `defaultFrom` row |
| `test/protocol/status.json` | a snapshot with status (a choice at `null`); `node.statusChanged` merges; `node.eventRaised` after a flush, with `severity`; a run's reset (`reset: true`) and then a new run's first fields; a higher `run` without a reset replacing; a stale `run` ignored; `node.state` with `run` |
| `test/protocol/diagnostics.json` | two fields flapping at once (two entries), one settling (`active: false`) while the other stays active; `count` as sent; an observation code; an unknown code drawn generically |
| `test/protocol/gestures.json` | begin (no echo to the owner, `begin` to another session first, then the reply), changes (one stale, one keep-alive changing nothing), end with its order; a reach of two keys; a reach of 64 keys as one notification per tick; a reach refused `shared-definition` (a non-live row; a row of another unit); `parameter-held`; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings` leaving the gesture open; an abort by a hand edit (no value) with its order; an abort by `file-saved`; `unknown-gesture` after the abort; `not-in-batch` |
| `test/protocol/node-set.json` | merge, `null` removing a key, a missing required key (`data.key`), each `applied` value, `parameter-held` |
| `test/protocol/actions.json` | restart with its order (reset, `starting`, reply, `running`), a trigger, `not-restartable`, every refusal of 4.5 that an action can get |
| `test/protocol/unknown-values.json` | an unknown state, route status, status field, event code, diagnostic code, gesture phase, `applied` value, member and notification; the graph after them |
| `test/protocol/errors.json` | every new code of 4.5 with its `data`; an unknown request member (-32602 with `data.pointer`) |
| `test/protocol/snapshot.json`, `edits.json` | regenerated: `protocol`, `features`, `typeSchemaHash`, and each node's `run`, `blockLost`, `status`, `gestures`, `diagnostics` (additive); `node.update`'s `applied` |
| `test/protocol/output-records.json` | the records of section 9 |

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
- `SettingRow` gains `std::optional<std::uint32_t> liveMs`, `std::optional<std::string_view> indexedBy`, a third rule
  kind `DecimalRule{low, high, unit, hints}`, the text hints and `defaultFrom`. `SettingKind` gains `decimal`;
  `SettingDeclaration` gains `bool live` and `bool indexed` (`node_type.hpp:318-325` [V] today).
- The walk: a decimal is a JSON number in range; an integer is taken and kept as given. An indexed family's keys are
  `<key>.<i>`, 1 to the count; another member number is `"<key>.<i>: no such member (<count key> is <n>)"`, placed at
  step 3 of the walk's order (`settings_table.hpp:205-213` [V]) where the family's row sits; unknown keys at step 2
  skip any key whose part before its last `.` names a family. Each refusal carries its key (4.5).
- Reads: `s.get<row>()` and `s.get<trimRow>(i)` (member `i`, from 0, as the harness's `input(bank, 0)`, while keys count
  from 1, as ports' members) read plain rows. **On a live row `get` does not compile** ("a live row is read through
  io.param"): a value captured in `start`, `build` or the run object would go stale at the first retune (7.4).
  `s.initial<row>()` reads a live row's value as the run started, for the rare start that needs it; its name says it
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
struct Writer { enum class Kind { rt, loop, resource } kind; LoopName loop = {}; };
inline constexpr Writer rt{Writer::Kind::rt};               // the run's code, through Io
/// A loop of the spec, by its handle (node-primitives.md 6.4: `loop::dedicated<"sendspin">`, listed in `.loops`).
template <class Handle> consteval Writer loop(Handle);
inline constexpr Writer resource{Writer::Kind::resource};   // a resource's adapter (slice 8)
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
- `writer::loop(worker)` takes a loop handle of slice 3 (node-primitives.md 6.4 and its G14, written to its
  recommendation and pending the owner's answer); the handle carries the loop's name, checked where it is made. A
  field whose loop the spec does not list in `.loops` does not compile. `Report::set` of a loop field asserts
  `Loop::inLoop()` of that loop in debug builds. A library thread (sendspin-cpp's own) that has something to report
  posts to the loop that owns the field. There is no `writer::thread`: slice 3 gives a type no threads of its own
  (node-primitives.md section 7), and a library's threads write no status directly.
- Names: as bank names (1 or more of `[A-Za-z0-9_-]`), camelCase by convention, unique in the table; `spec_detail::Name`
  is today's `BankName` (`type_spec.hpp:47-53` [V]) under a general name. The framework's own fields (`ear.`) hold a dot,
  so they never meet a type's.

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

/// The typed control-plane face of one run, over slice 3's RunHandle (node-primitives.md section 5): status writes and
/// events by the spec's handles, from the loops its fields name. Copyable (it holds the handle by reference, and the
/// handle outlives every thread that can reach it); every copy is the same run's. Never blocks but on one short mutex;
/// a write after the run's close began is dropped (I6). A text value is UTF-8 checked, control characters replaced,
/// and cut at its bound (8.2's rule, the framework's: sendspin's `notes::printable` goes). `lost` and `log` stay the
/// handle's own (`report.handle().lost(...)`, `report.handle().warn(...)`).
template <const auto& Spec> class Report {
public:
	template <const auto& Field> void set(typename FieldOf<Field>::Value value);   // a writer::loop field
	template <const auto& Event> void raise(typename ArgsOf<Event>::Values... values);
	RunHandle& handle() const noexcept;   // the name of ready.handle() (node-primitives.md G17)
private:
	explicit Report(RunHandle& handle);   // made by the framework (ReadyOf<Spec>::report) and the harness only
};
/// Slice 3's ReadyOf<Spec> (node-primitives.md 6.1), which `build` takes, gains one member: the run's typed Report.
/// ReadyOf is spec-typed, so a Report of another spec cannot be made: a mismatch does not compile.
template <const auto& Spec> class ReadyOf {
public:
	// slice 3's members: format, node, handle, store, loop, feed, wake, kept, recording, attach
	Report<Spec> report();
};
}
```

- A choice argument off the realtime thread takes a `std::string_view`, checked at run time against its values (a value
  not among them throws `std::logic_error`: a type bug); from `process` it takes a consteval-made index only.
- `RunHandle` (slice 3) replaces `InsidesEvents` (`node_type.hpp:174-180` [V]); `Report` adds the typed writes to it,
  carried in the same per-run channel so a status write, an event and a `lost` keep one writer's order (6.3).
  `RunHandle::say` stays for `sendspin` until step 5.5 removes it.
- **`TypeSpec`, whole.** Its one home is node-primitives.md 6.12, which gives every member and the order
  (designated initialisers must follow it): slice 2's `.settings` and `.banks`, slice 3's `.stores` and `.loops`, then
  this page's `.status`, `.events`, `.actions` and `.logCodes`. This page adds `.status` and `.events` in step 5.2,
  `.logCodes` in 5.3 and `.actions` in 5.4. Each defaults to an empty list, so a spec that gives only `.settings` and
  `.banks` stays as it is today.

  That aggregate deduction with designated initialisers fills the defaulted template parameters on GCC 14 is [U]
  (node-primitives.md U1, the same question); slice 3's S3.5 compiles it first, and step 5.2 adds the later members to
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
- The first block of a run starts at the slot's value, without a ramp.
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
  record in the run's ring and counts a transition (6.4).
- `io.raise` and `io.log` put one fixed-size record in the ring, stamped with the cycle's start (`GraphCycle::startNs`,
  `node_type.hpp:98` [V]): no formatting, no allocation.
- `io.triggered<t>()` compares the trigger's counter with the last one this run's code saw.
- The framework resolves every handle to an index when the code is prepared (as `Io::resolve` does for banks,
  `type_spec.hpp:216-231` [V]), so each call is an array index on the realtime thread.
- These are the realtime calls' names: `io.log`, `io.raise`, `io.set`, over a ring the framework makes per run (6.1),
  never acquired in `start`. This page is their home; node-primitives.md section 14 cites them.

### 5.5 Actions

```cpp
namespace ear::action {
/// Into the run's code, wait-free: no arguments, answered {} once handed over; the code sees io.triggered<t>() true once
/// in its next block. Several invokes before a block are one.
template <spec_detail::Name N> consteval Trigger<N> trigger(std::string_view help);
/// Into the run object, posted to `Loop`, a loop handle of the spec (node-primitives.md 6.4 and G14: one listed in
/// `.loops`, else no compile): arguments checked by `args`, a SettingsTable named for the action; the handler's
/// std::expected<void, std::string> is the reply.
template <spec_detail::Name N, const auto& Args, const auto& Loop>
consteval Call<N, Args, Loop> call(std::string_view help);
}
template <auto... Actions> struct ActionTable {};
```

- `restart` is the framework's, listed first in every type's schema, and a type action of that name does not compile.
- A trigger needs no run object and no loop: `gain` uses one. A call runs on the loop it names, posted there with
  slice 3's `RunLoop::postFromOutside`; a call naming a loop the spec does not list does not compile. The handler is a
  member of the run object:
  `std::expected<void, std::string> act(Invoked<forgetPairing>, const CheckedOf<forgetArgs>&)`, found by its argument
  type and held to that signature, as `check` is (`type_spec.hpp:368-373` [V]). A queued call whose run ends first is
  destroyed unrun at teardown step 1, when `closeToOthers` closes the loop to outside work, and its reply object answers
  `restarted` (or `stopping`) from its destructor (node-primitives.md 6.4, 6.10).
- An action's arguments are a `SettingsTable`, so a refusal has the settings walk's text, order and key: one checker
  for settings, action arguments and parameters (node-framework.md 7.1).

### 5.6 How the framework carries them (core, main loop and realtime)

- **Per run** (`NodeHost`, made at start, freed at I5 step 5): the run handle's channel (today's `Channel`,
  `node_host.cpp:173-190` [V], slice 3's `RunHandle`, gaining the queue of 6.3), its off-realtime queue (6.3), its
  realtime ring (6.1), one `std::atomic<double>` slot per live row member, one `std::atomic<std::uint64_t>` per `rt`
  field and one `std::atomic<std::uint32_t>` transition counter, one `std::atomic<std::uint32_t>` per trigger, and each
  live member's ramp state (realtime-owned). The code is bound to them when it is prepared
  (`BankBoundCode::bindPorts`, `type_spec.hpp:271-276` [V], grows a `bindRun`).
- **Per node**: the run number (the run handle's `generation()`), the status view, the live value of each held key (seeded
  into every new run's slots, so a restart mid-gesture keeps the hand's value), the node diagnostics kept per code and
  subject, `blockLost`.
- **Erased for the rest of ear**: `NodeType::schema()` returns a plain `TypeSchema` (core, nlohmann only), encoded once
  by `ear_control`. `NodeConfig::liveValues()` (default: none) gives each live key's checked value, for retunes.
  `NodeType::checkLive(config, key, value)` runs the row's rule and then the behaviour's `check` with that value in
  place, returning `InvalidSettings` with its key.
- **New core interfaces**, each implemented outside core: `RealtimeBell` (`ring() noexcept`; an eventfd the main loop
  watches, in `ear_pipewire`), the listeners `NodeStatusListener` (`statusChanged`, `statusReset`, `eventRaised`),
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

inline constexpr auto channelsRow = setting::defaulted("channels", setting::number(1, 64, "channels"), 2);
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

struct Gain {
	static Members<spec> ports(const Checked& s) {
		return {in(members(s.get<channelsRow>())), out(members(s.get<channelsRow>()))};
	}
	static StartResult start(StartOf<spec>&, const Checked&) { return GainCode{}; }   // node-primitives.md 6.1
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

A type with a run object writes status and events from its loops through the `Report` its `build` takes from
`ready.report()`. Sendspin's worker after step 5.5 (`worker` is its spec's `loop::dedicated<"sendspin">`,
node-primitives.md 4.1):

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
// Sendspin::build(ReadyOf<spec>& ready, const CheckedOf<spec>&): SendspinRun keeps ready.report() beside
// ready.handle() and its loops (node-primitives.md 4.1).
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
| `report-of-other-spec`, `report-constructed` | a `Report` comes only from `ReadyOf<spec>` |
| `triggered-not-a-trigger`, `action-named-restart`, `duplicate-status-name`, `duplicate-event-code` | as named |
| `status-in-unnamed-namespace`, `event-in-unnamed-namespace` | `linkage::named`, as for rows and specs |

### 5.9 The harness

`TypeHarness<spec, Behaviour>` and `NodeHarness` (`src/core/include/ear/testing/`) gain, all on the test's thread:

| Call | Does |
|---|---|
| `h.live<row>(value)`, `h.live<row>(member, value)` | moves the slot, as a gesture tick (and runs `checkLive`, failing the test on a refusal) |
| `h.retune(settings)` | a settings change of live rows only: slots move, no restart (asserts the plan would retune) |
| `h.status<field>()`, `h->status()` | the live view (typed, or as JSON) after the last drain |
| `h->statusRecord()` | every change, in order: `{field, from, to, run}`, and each reset |
| `h->events()`, `h->logs()`, `h->diagnostics()` | in order, each with its formatted `msg` (diagnostics with subject and `active`) |
| `h.trigger<action>()`, `h.invoke<action>(args)` | a trigger; a call, on the harness's manual loops (node-primitives.md section 10) |
| `h->drain()` | runs the tick now (the harness drains after every `process` and every `build` by default) |
| `h->advance(ms)` | moves the harness's clock, node-primitives.md section 10's one fake clock: flapping windows, rate limits and trailing records, the gesture idle deadline |

The all-type tests gain: every type's schema encodes; every status field's initial value is of its kind or null;
every `values` entry parses with its row's parse function; every optional row without a `default` has a `defaultFrom`
or a `help`; every live row of every type ramps from its default to another value with no sample unwritten; `restart`
is first in every type's actions.

## 6. The realtime contract

### 6.1 Channels into and out of the realtime thread

| Channel | Direction | Mechanism | Bound | On overflow |
|---|---|---|---|---|
| Parameter slot | in | one `std::atomic<double>` per live member, written by the main loop, loaded once a block by the framework | 256 live members per node | — (a slot always holds one value) |
| Trigger | in | one `std::atomic<std::uint32_t>` counter per trigger | — | several invokes in one block are one |
| `rt` status field | out | one `std::atomic<std::uint64_t>` per field, sampled at the drain; a transition counter beside it | 64 `rt` fields per type | — |
| Realtime ring | out | per run, made by the framework, single producer (the code), single consumer (the drain), fixed 48-byte records: kind, code, field, up to four 8-byte values, the cycle's start (`startNs`) | 256 records per run; log records are refused once 192 are in use, so status and events keep the last quarter | dropped, counted per kind, reported as `ear.records-dropped` (subject `realtime`) |
| Bell | out | `RealtimeBell::ring()`, an eventfd write, gated by an atomic exchange on the window as the level bell's is (`port_blocks.cpp:429` [V]) | at most one per level window per block (30 a second) | — |

`std::atomic<double>` and `std::atomic<std::uint64_t>` are lock-free on x86-64 and arm64; a `static_assert` on
`is_always_lock_free` makes it a build failure elsewhere [U: arm64 builds are not made yet].

### 6.2 What the realtime thread does, and never does

It loads each live slot once a block, advances each ramp, stores `rt` fields, writes ring records, and rings the bell at
most once per window. It never allocates, locks, formats, or makes a syscall other than the bell's write
(node-framework.md 7.4). The block rings the bell after `process` returns when an `rt` field changed or the ring holds
records, so a node whose only output is realtime is drained, and a node at rest costs nothing (8.2's rule, kept).

### 6.3 Off the realtime thread: one queue per run

- `Report<spec>` writes and the run handle's `lost` go into the run's queue: a `std::vector` of records (status change,
  event, lost) under one mutex, bounded at 1024 (a `lost` is never dropped); a full queue drops the record and counts
  it (`ear.records-dropped`, subject `queue`). The handle's `log` goes straight to the log sink, unordered with these,
  as slice 3 defines it.
- The first record into an empty queue posts one wake to the main loop (an atomic flag gates it, so a burst costs one
  post).
- Each run writer's records stay in its order, so a status write before an event is drained before it, and a `lost`
  after the last status is drained after it: node-framework.md 8.1's order, with no per-field flush.
- Text is made safe at the write, on the writer's thread: UTF-8 checked, control characters replaced, cut at the
  field's `maxBytes` and marked.
- The mutex is held for a push or a swap only; the main loop may wait behind a preempted writer for that long, never
  for I/O. (8.2 proposed a mutex per text field; one per run is the same cost and gives the order above.)

### 6.4 The drain and the tick, on the main loop

1. A wake (off-realtime), the bell (realtime), a gesture tick taken (6.6), or a trailing record due (6.5) marks the
   node dirty and, if no tick is due, arms one on `Timers` (`timers.hpp`, the main loop's) for 33 ms after the last
   tick, or now if that has passed.
2. The tick drains every dirty run: its realtime ring first, then its queue (a fixed choice; records of different
   writers have no order between them, node-framework.md 8.1). For each record: a status change updates the view, is
   offered to `StatusLog` (paced per field, 6.5), counts a transition, and marks the field for the wire; an event first
   sends the node's marked fields as one `node.statusChanged`, then `node.eventRaised` and an `event` record, its `t`
   the record's cycle start mapped to UTC for a realtime event (the drain's clock for the offset), the write's time
   otherwise; a `lost` fails the node as today.
3. `rt` fields are then sampled: a value that differs from the view's is marked for the wire (its change records came
   through the ring, written where the code changed it). The framework's counters are sampled at most once a second.
4. One `node.statusChanged` per node with marked fields; one `gesture.changed` `change` per gesture moved since the
   last tick; flapping is judged (6.5); drops become `ear.events-dropped` or `ear.records-dropped`; trailing status
   records due are written (6.5).
5. A `lost` of a run whose close began is dropped; its status and events are still drained until it closed, so a
   goodbye is heard; then its reset is sent (4.3); nothing of a run is drained once it closed or was leaked (I6, as
   slice 1 built it for notes).

### 6.5 Rates and bounds

| What | Bound | Policy |
|---|---|---|
| The tick | 30 a second, all nodes together | armed only when something is dirty |
| `node.statusChanged` | 30 a second per node | the latest values |
| `status` records on stdout | per node and field: 32 at once, then 1 a second | when the pace held a field's changes back, a **trailing** record with the field's current value is written at the next tick the pace allows (at most 1 s after the last held change), carrying `heldBack`; so the last `status` record of each field always matches the view (G9) |
| Events (wire and stdout) | 32 at once, then 1 a second per node | the pacing slice 3 moves into core for `say`; held back counted in `ear.events-dropped` |
| Flapping | a field above its `flapsAbove` (default 10 transitions in 10 s) | counted at each write (off-realtime) and by the transition counter (realtime), so changes between samples count; settled once a whole window passes at or below it |
| Gesture ticks into the main loop | one pending per gesture | the control thread keeps the newest until the main loop took the last (6.6) |
| `gesture.changed` `change` | 30 a second per gesture, one per gesture whatever its reach | sent at the tick; never to the owner |
| Off-realtime queue | 1024 records per run | dropped, counted |
| Action reply | 10 s | `action-timeout`; the handler's late answer is dropped |
| Gesture idle | 30 s with no message about it from its owner (a tick, keep-alives included) | aborted, `idle`; the timer stops once the owner's end or cancel was read (6.6) |
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

**Coalescing.** The gateway keeps, per gesture, the newest pending tick; if none was pending it posts one task to the
main loop, which takes whichever tick is newest when it runs. So a client sending 1000 ticks a second costs the main
loop at most one task per turn, and the audio still follows the last value.

**On the main loop**, `Gestures::move` checks the gesture and its owner, drops a stale `seq`, resets the idle timer,
and stops there when the value equals the live value (a keep-alive). Otherwise it checks the value against every
reached row and the type's `check` (`NodeType::checkLive`, 5.6; a refusal is dropped and the owner gets
`tick-refused`, at most once a second), writes every reached slot through `NodeHost::setLive` (for a node with no run,
only the node's live value), marks the gesture dirty and arms the tick (6.4).

## 7. The keeper path

### 7.1 Who does what

| Step | Thread | Owner |
|---|---|---|
| Decode `gesture.*`, `node.set`, `node.invoke`; read ahead; mark a gesture *ending* | control | `ear_control` (`decodeCommand`, the session) |
| The reach index of each version of the working copy | keeper | `ear_config` (`Keeper`, `GraphFileModel::reachIndex`) |
| `gesture.end`, `node.set`, `node.invoke`: ordering, file checks, the candidate | keeper | `ear_config` (`Keeper`) |
| `gesture.begin`: file state, the reach index, rows, holds | main loop | the keeper's main-loop side (which holds the running version's index), `Gestures` |
| Open, move, close and abort gestures; write slots; tell listeners | main loop | `Gestures` (`ear_core`), `NodeHost` |
| Plan, retune or restart, commit | main loop | the keeper's main-loop side, `DesiredGraph`, `NodeHost` |
| `gesture.change`, `gesture.cancel` notifications, a session's close | control, then main loop directly | the gateway, `Gestures` |

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
  M->>M: plan: retune; graphFile.changed; commit: nodeRetuned; Gestures.close(g4)
  M-->>E: graphFile.changed, node.updated, gesture.changed end, reply {-3.5}
  K->>K: write ear.conf when free
```

- **The reach index** (keeper thread). With every offer it posts (an accepted edit's candidate, a disk read,
  `graphFile.set`), the keeper sends the reach index of that text: `GraphFileModel::reachIndex()`, new, gives for each
  node and live key `ear.conf` gives the definition's place, every node and key whose expanded value has that
  definition (knobly's `definition()`, `written.hpp:926` in the image [V]), and the committed value. It is small (one
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
- **Cancel, session close, idle, node removed** abort on the main loop: every reached slot returns to the committed
  value, `gesture.changed` `aborted` with the reason and that value.
- **What is written, and when.** Only an end (and `node.set`, `node.update`, `graphFile.set`) changes the working copy;
  `ear.conf` is written behind as today, when the keeper is free (`keeper.cpp:78-81` [V]); its fsyncs
  (`confined_folder.cpp:251`, `277` [V]) delay the next keeper step, never a begin or a tick. At most one commit per
  gesture.

### 7.3 Refusals and collisions

| Lands during a gesture | What happens |
|---|---|
| A hand edit (disk), whatever it changes | every open gesture closed as aborted (`hand-edit`, no value) on the main loop before the offer's plan (G3); then the file is offered as today; last in the turn each reached slot is set to its node's running value (the new file's, if the plan accepted it and changed the key; else the committed value) |
| A hand edit with errors, or one the graph refuses | aborted as above; the graph keeps what it ran; slots return to the running version's values |
| `graphFile.set` (the pane's Save) | as a hand edit, reason `file-saved` |
| `node.set` or `node.update` changing a held key | refused `parameter-held`; nothing changes |
| `node.set` or `node.update` changing other keys of a reached node | applied; a restart keeps the hand's live value (the node's live value seeds the new run's slots) |
| `node.remove` of a reached node, or its replacement (ports change) | applied; the gesture aborted (`node-removed`) |
| Another session's `gesture.begin` on a reached pair | refused `parameter-held` |
| The owning session closes (socket gone) | aborted (`session-closed`) (G4) |
| No message about the gesture from its owner for 30 s, and no end or cancel read | aborted (`idle`) (G4) |
| `SIGTERM` | `DesiredGraph::close()`: later begins and ends get `stopping`; open gestures are not committed and are not announced (clients are closed with 1001); the last write writes the last accepted version, which holds no live value |

The keeper's main-loop side learns the change's kind from the offer: a new member `OfferKind` (`disk`, `set`, `edit`,
`gestureEnd{id}`) beside today's members. Today's `Offer` (`main_loop_side.hpp:24-30` [V]) has `file`, `wanted`,
`placements`, `cause` (`edit` or `disk`) and `droppedEdits`; `cause` does not tell `graphFile.set` from a node edit, nor
a gesture's end from either. The offer also gains the version's reach index (7.2).

A gesture's reach changes only by a hand edit (`node.set` sets values, never which definition a key uses; `node.add`
appends literal values), and a hand edit aborts every gesture first, so a reach never changes under an open gesture.

### 7.4 Retune: a live change without a restart

- `DesiredGraph::plan` classifies an update (`desired_graph.cpp:158-160` [V]: same type, same ports) as a **retune** when
  every key whose value differs is live in the type's table (`SettingDeclaration::live`). Otherwise it is an update
  (a restart), as today. The plan's outcome per node (`retune`, `restart`, `replace`, `unchanged`) is the `applied` of
  4.2.
- `DesiredNodeListener` gains
  `virtual void nodeRetuned(const NodeSpec& node, std::shared_ptr<const NodeConfig> config, std::span<const std::string> keys)`,
  whose default calls `nodeUpdated`. So the gateway (which sends `node.updated` either way, now with `applied`) and
  every test listener are unchanged; `NodeHost` overrides it: it keeps the new config for later restarts and writes
  each key's slot from `config->liveValues()`, unless a gesture holds the key. Commit order is today's; a retune is told
  where an update would be.
- So a hand edit, `node.update` or `node.set` that changes only `gainDb` retunes: the audio ramps, no `node.state`
  follows, and standard output has a `param` record.
- A run object and outside resources are not told of a retune in this slice: live rows reach only the run's code (2.4,
  G10).

### 7.5 `node.set`

On the keeper thread: `unknown-node` if the working copy has no such node; the node's settings from the working copy's
read, merged with the patch (null removes), then today's `updateNode`. Everything after is an edit as today; the reply
carries the plan's `applied` for the node.

### 7.6 Actions and restart

- `node.invoke` reaches the keeper's thread in order with every other request (node-framework.md 8.4), which needs no
  file and posts a step to the main loop at once. There `NodeHost::invoke(node, action, args, reply)` checks the action
  and its arguments against the type's table.
- `restart`: as a settings change with the same config (I5, I7), the block and routes kept, `ear.conf` untouched; the
  reply is posted once the teardown began, after the old run's reset and the `node.state` `starting` it causes. A node
  whose block is lost cannot restart (`restartRequested` does nothing then, `node_lifecycle.hpp:29-31` [V]): it is
  refused `not-restartable` (`reason: block-lost`), and the snapshot's `blockLost` lets the editor disable the button
  first. Slice 8's recovery wait is skipped.
- A trigger: the counter is incremented and the reply posted at once; `not-running` when the node has no run.
- A call: posted to the loop it names with `RunLoop::postFromOutside`; the reply when the handler returns. When the run
  ends, teardown step 1 closes that loop to outside work (`closeToOthers`): a queued call gets `restarted` (or
  `stopping`), and a running one finishes and replies before the goodbye that step 4a posts behind it
  (node-primitives.md 6.4, 6.10).
- **The keeper never waits for a handler.** Its step completes when the action is dispatched. The batch's reply is held
  on the main loop until every action in it answered (or its 10 s deadline passed), then posted to the control thread
  behind every event before it, as today. A slow handler holds that client's later requests with an id (the FIFO of
  6.6), never its gesture ticks and never the keeper's queue.
- An `action` record on standard output says who asked for what (section 9).

## 8. The editor

### 8.1 Stores and reducers

| Module | Holds | Tested by |
|---|---|---|
| `protocol.ts` | the wire types of section 4; `ControlErrorCode`; `NodeDiagnostic`, `EventSeverity`; open enums typed as `Known \| (string & {})` | typecheck |
| `graph.ts` | the graph, now with each node's `run`, `blockLost`, `status`, `gestures`, `diagnostics`; handlers for the new notifications (4.4) | vitest, every fixture of 4.7 |
| `types.ts` (new) | the type schemas by hash; `types.get` with `ifNoneMatch` on every snapshot whose hash differs; cached in `localStorage` (wrapped in `try`, as storage may be absent) | vitest, `types.json` |
| `gesture.ts` (new, pure) | the session's own gestures as a state machine (below); what to send for each input; the control mapping (8.3) | vitest, a fake client |
| `form.ts` (new, pure) | the settings form's per-field drafts against server values (8.2) | vitest |
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
| any | connection lost | — | idle (ear aborts with `session-closed`) |

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
- Non-live rows are edited in a form and sent together with one `node.set` ("Apply"), marked "restarts the node"; the
  count row of a family is also marked "changes the ports". Clearing an optional field sends `null`. The JSON text area
  stays behind an "Edit as JSON" toggle, for keys a form cannot show.
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
  | `hand-edit` | "ear.conf was edited by hand while you held amp gainDb; it now says -10." [Set -3.5 again] |
  | `file-saved` | "ear.conf was saved from the text pane while you held amp gainDb; it now says -10." [Set -3.5 again] |
  | `idle` | "Your change to amp gainDb was dropped after 30 s without a word from this editor." |
  | `node-removed` | "amp was removed or replaced while you held gainDb." |
  | `refused` | the end's refusal message |
  | `session-closed` | none (the editor reconnects and shows the snapshot) |

  "Set ... again" sends `node.set` with the value the operator last chose. On `hand-edit` and `file-saved` the control
  takes the value from the `node.updated` that follows (or keeps the committed value when none follows).
- `parameter-held` on begin: the control shows the holder's value and disables itself until the gesture ends.

### 8.4 Status, events, diagnostics and actions

- **Status**: fields with `display: "box"` on the node box, after the state line ("connected · Music Assistant");
  every field in the panel as a list, a flag as yes or no, a choice as its value, a number with its unit, `null` as
  "unknown"; `callout` fields as a banner on the box and in the panel while not at their initial value, with a copy
  button for text. Box and callout fields show only while the node is `running` and the value is not `null`.
- **Events**: the node's activity list in the panel, which starts when this page connected (standard output keeps the
  history); `warn` and `error` events also as a notice, coalesced per node and code ("pairing failed ×3") and at most
  one new notice per node per 5 s.
- **Diagnostics**: a badge on the box for each active entry ("flapping: connection"), its tooltip giving the count and,
  for flapping, the field's `flapsAbove`; the panel lists every entry with its time, count and whether it is active.
  An unknown code is shown by its `msg`, badged while active.
- **Actions**: one button per action, `restart` first; a call with arguments opens a small form from its rows; disabled
  while the request is in flight, while the node is `stopping`, and (for `restart`) while `blockLost`, with the reason
  as its description; a refusal shows its message.
- **Unknown values** (a newer ear): an unknown state is drawn as `starting`'s neutral style with its name; an unknown
  status kind as text; an unknown event code as its `msg`.

### 8.5 Accessibility

- Faders and knobs are `role="slider"` with `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, `aria-valuetext` ("-3.5
  dB", "-inf dB") and a label from the key, keyboard per the WAI-ARIA slider pattern (8.3); held by another editor:
  `aria-disabled` with the reason as its description.
- A polite live region announces callout fields when they change ("kitchen pairing code 123-456"), aborted gestures, a
  node turning `failed`, and flapping starting; at most one announcement per node per 5 s. `error` events are announced
  assertively. A refusal is shown near its control with `role="alert"`.
- When the selected node is removed or replaced, focus moves to the panel's heading; after a refused action, focus
  returns to its button.
- A keyboard path joins the canvas and the panel: Enter on a focused node box moves focus into its panel, Escape in the
  panel returns it to the box.
- State, flapping and route status are never shown by colour alone: each has its word or a text badge.
- Focus is visible on every control; the panel's controls are in reading order; every action button has a text label.

## 9. Standard-output records

New kinds, each with `v`, `seq`, `t`, `kind`, its fields, `msg`, as `status_log.hpp:31-56` [V] frames every record today.
`v` versions a record's *shape*: every change here is additive, so it stays 1. The set of kinds is open (a reader skips
kinds it does not know, and a kind may stop appearing); removing `note` is such a change (below). Each record carries
`run` where it has one: the run handle's `generation()`, which slice 3 puts on `log` records first (node-primitives.md
section 5).

```json
{"v":1,"seq":120,"t":"2026-11-08T10:01:50.311002Z","kind":"status","node":"kitchen","run":3,"field":"connection","from":"dialling","to":"connected","msg":"node kitchen connection dialling -> connected"}
{"v":1,"seq":121,"t":"2026-11-08T10:01:50.311040Z","kind":"status","node":"kitchen","run":3,"field":"pairingCode","from":"","to":"123-456","msg":"node kitchen pairingCode \"\" -> \"123-456\""}
{"v":1,"seq":140,"t":"2026-11-08T10:02:12.000300Z","kind":"status","node":"kitchen","run":3,"field":"connection","from":"dialling","to":"connected","heldBack":9,"msg":"node kitchen connection now connected (9 changes held back)"}
{"v":1,"seq":141,"t":"2026-11-08T10:04:00.100000Z","kind":"status","node":"kitchen","run":3,"reset":true,"msg":"node kitchen run 3 status reset"}
{"v":1,"seq":122,"t":"2026-11-08T10:01:52.004211Z","kind":"event","node":"kitchen","run":3,"code":"pairing-failed","severity":"warn","args":{"reason":"pairing-code-mismatch"},"msg":"node kitchen pairing failed: pairing-code-mismatch"}
{"v":1,"seq":123,"t":"2026-11-08T10:02:11.120400Z","kind":"diagnostic","node":"kitchen","run":3,"code":"ear.status-flapping","subject":"connection","active":true,"count":2,"transitions":14,"windowMs":10000,"msg":"node kitchen connection flapping: 14 changes in 10s"}
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
{"v":1,"seq":126,"t":"2026-11-08T10:03:41.000300Z","kind":"param","node":"amp","key":"gainDb","from":-3.5,"cause":"aborted","gesture":"g4","reason":"hand-edit","msg":"node amp gainDb gesture g4 aborted (hand-edit) at -3.5"}
{"v":1,"seq":127,"t":"2026-11-08T10:05:12.770000Z","kind":"action","node":"desk","action":"restart","result":"ok","msg":"node desk restart invoked"}
{"v":1,"seq":128,"t":"2026-11-08T10:05:20.770000Z","kind":"action","node":"kitchen","action":"forget-pairing","result":"action-failed","detail":"no pairing with ma-0000","msg":"node kitchen forget-pairing refused: no pairing with ma-0000"}
```

- `status`: every change as written, paced per node and field (6.5). A record after held-back changes is the trailing
  record: its `to` is the field's value now and `heldBack` counts the changes skipped, so **the last `status` record of
  each field always matches the view**. A run's reset is one record with `reset: true` and no field; a `from` after it
  is the field's initial value. A text value in `msg` is quoted when it is empty or holds a space; `null` is written
  `unknown`. The framework's `ear.` counters have no `status` records (4.6).
- `event`: `msg` is `node <n> ` and the sentence with its arguments; `severity` is also the record's level for tools;
  `t` for a realtime event is its cycle's start (6.4).
- `diagnostic`: the kind exists (`ear.close-overdue` [V]); it gains `subject`, `active`, `count` and the codes of 4.6.
- `param`: one per reached node and key at each committed live change, whatever made it (`cause`: `gesture`, `set` for
  `node.set` and `node.update`, `disk` for a hand edit or `graphFile.set`) and at each abort (`cause`: `aborted`, with
  `reason`; `to` is the committed value it returned to, absent for `hand-edit` and `file-saved`, whose new value the
  following `param` or `graph-file` record says). A non-live change has no `param` record: its `graph-file` and `node`
  records say it.
- `action`: one per `node.invoke` answered, with `result` (`ok` or the refusal's code) and `detail` for a refusal.
- `note`: written only by `sendspin` until step 5.5; then no code writes it, and the kind is removed from the encoder.
  No record's shape changes, so `v` stays 1; but operators grep `says pairing-code` (the `ear-on-talos` skill), so step
  5.5's commit carries a `BREAKING CHANGE:` footer naming the replacement (10.1), and the release notes say it.
- `output-records.json` gains one case per new kind, per diagnostic code, a trailing `status` record and a reset;
  AGENTS.md's record table gains the same lines.

## 10. Migration

### 10.1 Sendspin's notes

Step 5.5 rewrites sendspin's reporting against 3.2's table in one change:

- `notes.hpp` and `notes::printable` go (slice 3 already moved note pacing into core: node-primitives.md section 5):
  the framework makes text safe and paces events.
- The worker and key store write through the `Report<spec>` that `build` takes from `ready.report()` (sendspin is on
  slice 3's loops and `build` by then, node-primitives.md S3.7), with every field `writer::loop(worker)`, instead of
  `RunHandle::say`.
- `RunHandle::say`, core's note pacing and `NodeNoteListener` are removed once nothing calls them; `StatusLog::nodeSays`
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
| The editor before this page | works: it ignores the new notifications and members. A `node.update` changing only a live row no longer restarts the node (better, and not observable as an error) |
| `scripts/rpc-call.mjs` | works unchanged; gains `--follow` (step 5.2) |
| `scripts/image-smoke.sh` | works; step 5.1 adds a `types.get` check |
| The system tests | `says` matches move to `status` and `event` records (step 5.5); new tests per step |
| Tools reading stdout | new kinds appear; `note` disappears at step 5.5 (a `BREAKING CHANGE:` release) |

### 10.3 What changes in frozen headers

Each named header's first line says it changes only through a design page and a re-cut. `web/src/protocol.ts:5-6`
[V] says more: "only through an erratum in that page's Errata section (section 11)", that page being batch2.md. So step
5.1 adds one erratum to batch2.md section 11 that names this page as the home of the wire's additions from protocol 1
on (as `knobly-config.md`'s "Wire additions" is for the graph file's methods), and `protocol.ts`'s header names both
pages. Each later step's wire change is then made here and re-cut.

| Header | Change | Step |
|---|---|---|
| `node_type.hpp` | `SettingKind::decimal`; `SettingDeclaration::live`, `indexed`; `NodeConfig::liveValues()`; `NodeType::schema()`, `checkLive()` | 5.1, 6.1, 6.3 |
| `run_handle.hpp` (slice 3's) | the per-run queue behind it; `say` removed | 5.2, 5.5 |
| `command_error.hpp` | `InvalidSettings::key` | 5.1 |
| `settings_table.hpp` | decimal, indexed and live rows; text hints, `defaultFrom`; `initial<row>()`, `get` refused on live rows | 5.1, 6.1, 6.2, 6.3 |
| `type_spec.hpp` | `TypeSpec`'s later members; `Io`'s `param`, `set`, `raise`, `log`, `triggered`; `Smoothed`; status, event, action and log tables; `Report`, `ReadyOf` | 5.2, 5.3, 5.4, 6.3 |
| `node_host.hpp` | status, event and gesture listeners; `setLive`, `invoke`; `RealtimeBell`; `blockLost` in the view | 5.2, 5.3, 5.4, 6.3 |
| `desired_graph.hpp` | `nodeRetuned` with its default; the plan's per-node outcome | 5.4, 6.3 |
| `commands.hpp`, `events.hpp`, `gateway.hpp`, `session.hpp`, `graph_file_state.hpp` | the new commands and events; the direct gesture crossing; read-ahead; session ids; `ControlRefusal` | 5.1 to 6.4 |
| `graph_view.hpp` | `NodeView`'s `run`, `blockLost`, `status`, `gestures`, `diagnostics`; `GraphView`'s `protocol`, `features`, `typeSchemaHash` | 5.1, 5.2, 6.4 |
| `web/src/protocol.ts` | section 4's types | each step |

## 11. The cut: buildable steps

Each step keeps every test green and changes no audio path unless it says so. They start once slice 3 is built
(node-primitives.md S3.1 to S3.7: the run handle, loops, sendspin on them) (G6). Slice 5's steps come first; slice 6's
need 5.1 (the schema) and 5.2 (the run's queue). Each step adds its feature to `features` (4.1).

**5.1 The schema on the wire.** `NodeType::schema()` from the settings table (today's rows; text hints and
`defaultFrom` added to `rtp`'s, `card`'s and `sendspin`'s rows); `types.get` answered on the control thread from an
encoding made at boot; `protocol: 1`, `features: ["types"]` and `typeSchemaHash` in the graph; `InvalidSettings::key`
and `data.key`, `data.detail`; `data.pointer` on -32602; open enums in `protocol.ts`; `types.ts`; the settings form
rendered from the schema, with per-field drafts and the JSON toggle; the batch2.md erratum (10.3). Tests: `types.json`,
`unknown-values.json` and `errors.json` in `ear_control_tests` and vitest; `form.ts`'s drafts; the all-type tests that
every schema encodes, every `values` entry parses and every optional row has a `default`, `defaultFrom` or `help`;
`image-smoke.sh` calls `types.get`; a system test that `graph.get`'s hash matches `types.get`'s.

**5.2 Status and events off the realtime thread.** `StatusTable`, `EventTable`, `LogCodes` in `TypeSpec` (with
slice 3's U1 test extended); `Report<spec>` and `ReadyOf`; the per-run queue, the drain and the tick on `Timers`;
`run` on the wire and on `node.state`; `node.statusChanged` with its reset, `node.eventRaised`, the `status` (paced
per field, with trailing records and the reset record) and `event` records; `status` in snapshots and `node.added`;
node diagnostics keyed by code and subject, with `active` and `count`; flapping, `ear.events-dropped`,
`ear.records-dropped`, and slice 3's `ear.loop-slow`, `ear.loop-source-left`, `ear.claim-wait`, `ear.close-late` and
`ear.close-overdue` on the wire; the
framework's counters sampled; the harness's captures; `rpc-call.mjs --follow`. Tests: `ear_core_tests` with a test type
(order of status before event, a `lost` after the last status, a write of an old run dropped, the reset after the last
values, the queue's bound, flapping and settling with the harness's clock, two fields flapping, the tick's 33 ms cap,
the trailing record within 1 s of the last held change and matching the view); `status.json`, `diagnostics.json`;
`output-records.json`'s new cases; the editor's status list, hidden box fields and badges (vitest on the reducer).

**5.3 The realtime ring.** The ring with its log budget, `rt` fields, `io.set`, `io.raise`, `io.log`, cycle stamps, the
gated bell (`ear_pipewire`, an eventfd on the main loop). Tests: `ear_core_tests` (records in order, the bound, log
records refused past 192 while events pass, transitions counted between samples, an event's `t` from its cycle);
`ear_pipewire_tests` (a test block's code sets a field and raises an event; both reach the main loop within a window;
an idle block rings nothing); a test that `process` allocates nothing while writing (the handoff's allocation test's
method).

**5.4 Actions, restart and `node.set`.** `ActionTable`, triggers, calls on a loop of the spec, `node.invoke` through the
keeper, `restart` and `not-restartable`, `blockLost` in the view, deferred batch replies with their deadline,
`ControlRefusal`, the `action` record; `node.set` and `applied` (`restart`, `replace`, `unchanged`) on `node.set`,
`node.update` and `node.updated`; the editor's action buttons; sendspin's `forget-pairing` as the first call. Tests:
`actions.json`, `node-set.json`; `ear_config_tests` (an invoke ordered between two edits in one batch; the keeper takes
the next batch while a handler is slow; `node.set`'s merge); `ear_core_tests` (restart of a running, a failed and a
lost-block node; a trigger seen once; a queued call answered `restarted`); a system test that `restart` of a `card`
node on the test card brings it back without a change to `ear.conf`.

**5.5 Sendspin's notes split.** 10.1. Tests: the sendspin system tests, rewritten to read records;
`ear_node_sendspin_tests` for each field and event; the standard-output guard still passes. The commit carries the
`BREAKING CHANGE:` footer (9).

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
`checkLive`, `nodeRetuned`, `NodeHost::setLive`, `applied: retune`, the `param` record; the `gain` type. Tests:
`ear_core_tests` (a retune plan; a plan with one non-live key is an update; slots seeded on restart); compile-fail
`get-live-row`, `live-whole-smoothed`; `ear_node_gain_tests` through the harness (5.4's worked vector frame for frame,
the restart mid-ramp, steady blocks, a ramp the code never reads still keeping time, a log-taper ramp, trims, clip
latch, trigger); a system test: white noise through `gain` into a `file` node, `node.set` of `gainDb` mid-recording,
the WAV's level changes over 20 ms with no gap and no `node.state` record.

**6.4 Gestures.** `Gestures`, `gesture.*`, the reach index (`GraphFileModel::reachIndex`) posted with each offer, begin
on the main loop, the mixed-reach refusal and the combined range, the session's read-ahead and *ending* mark, the direct
crossing with its pending tick, keep-alives, the idle deadline, every abort of 7.3, `OfferKind`, rounding at the end,
`gesture.changed` without own echoes, `tick-refused`, the harness's gestures. Tests: `gestures.json`; `ear_config_tests`
(the index of a `@set` used twice, a key not given, a snippet's line; an index withheld for a text with errors; an
end's candidate; a hand edit mid-gesture aborting before the plan and the slots settling after it); `ear_core_tests`
(stale `seq`, a keep-alive moving nothing, out-of-range tick, a tick the type's `check` refuses, the idle deadline
reset by keep-alives and stopped by a read end, session close, node removed, restart mid-gesture keeping the live
value, a begin on a failed node seeding its next run, a reach mixing a non-live row refused, two ranges intersected);
`ear_server_tests` (a slow request in flight on a socket while that socket's ticks still move the slot; an end queued
behind it never losing to the idle deadline; the FIFO's bound); a latency test: begin answered while the keeper is
writing, in less than one main-loop turn after the control thread hands it over; a system test: two clients, one holds
a gesture, the other's `node.set` is `parameter-held`, a hand edit aborts and the WAV goes straight to the file's new
level.

**6.5 The editor's controls.** Fader and knob with 8.3's mapping, keyboard and wheel gestures, held and aborted states,
notices with "Set again", accessibility of 8.5. Tests: `gesture.ts` against a fake client (every edge of 8.1's table:
release and Escape while beginning, a refused begin, buffered moves, keep-alive after 2 s held still, end on lost
capture, Escape, keyboard idle end, aborted while dragging, reconnect); the taper formulas and their inverses; Backspace
on a focused knob sends no `node.remove`; rendering by eye, as today.

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
  so a begin and its end agree on the reach; a hand edit replaces the index and aborts every gesture in one main-loop
  turn. A bug here breaks "the audio never stays on a value `ear.conf` does not hold".
- **The reach depends on knobly's `definition()`.** A file whose value comes through two variables (`@set $a $b;`) has
  one definition at the end of the chain; whether that is what the operator means by "the same value" is the same
  question `updateNode` answers today, so the two agree by construction.
- **Rounding at the end** (G5) means the committed value can differ from the last live value by half a step (0.05 dB for
  one decimal): inaudible, and announced.
- **Status records** grow standard output: a chatty field is held to 32 at once and 1 a second, plus one trailing
  record; a node with many flapping fields writes one record a second per field.
- **One mutex per run** off the realtime thread: the main loop can wait behind a preempted writer for a push.
- **Ticks run the type's `check`** on the main loop: a parse of the node's settings per taken tick (at most one per
  gesture per main-loop turn). Settings tables are small; if a type's `check` is slow, that type's ticks are slow.
- **Slices 5 and 6 wait for slice 3** (G6): the run handle and loops are its.
- **Triggers are lossy by design**: several invokes in one block are one. An action that must count goes to a run
  object.

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
| Gesture begin/end implied by the first and an idle timeout | OSC controllers, MIDI CC | no commit point the operator chose; the idle timeout remains only as a guard |
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
| A per-field mutex for text status (node-framework.md 8.2) | — | one queue per run costs the same and gives the writer's order without a flush rule |
| A base class with virtual `onParam`, `onAction` | JUCE's `AudioProcessor`, VST3's `IEditController` | nothing to override: slots, ramps, ring and handlers are the framework's; a type declares (node-framework.md R1) |

## 14. Gate questions

The owner's calls, grouped. Each has its options and a recommendation; the doc is written to the recommendations.

### Gestures

**G1. A gesture on a value that several nodes share.** `@set $trim 0;` feeds two keys, or two nodes' `gainDb`. Today
`node.update` of that value changes every user of it in one commit, by design (`knobly-config.md`, "File format").
(a) Refuse the begin as `shared-definition` whenever the reach holds more than the key asked for (node-framework.md
8.5's wording). (b) The reach is every user, provided every reached row is live and of one kind and unit: ticks move
them all within the intersection of their ranges, and the end commits once, rounded to the smallest `decimals`; any
other reach is refused `shared-definition`. (c) Begin anyway, move one node only, and let the end move the rest.
**Recommendation: (b).** It is what the commit will do, so the audio during the gesture is what the file will say, and
it gives ganged faders for free; the refusal keeps the cases where a tick could not do what the end will (a non-live
row would need a restart per tick; another unit has no common scale). (c) breaks "the audio never stays on a value
`ear.conf` does not hold" in reverse.

**G3. Which hand edits abort a gesture.** (a) Every disk change and every `graphFile.set`. (b) Only those that change a
reached key's value or its definition's place. **Recommendation: (a).** Simple to state, to show and to test; hand
edits mid-gesture are rare; (b) needs a diff of two knobly models on the keeper's thread. The cost of (a), a fader that
stops when someone adds a comment, is softened by the slots going straight to the new file's value and the notice's
"Set again" (7.3, 8.3).

**G4. A gesture whose session closes, or that goes idle.** (a) Abort: back to the committed value; idle means 30 s
with no message about the gesture from its owner, the editor sending a keep-alive every 2 s while held, and the timer
stopping once an end or cancel is read. (b) Commit the last live value. (c) No idle rule at all, only session close.
**Recommendation: (a).** A dropped connection's last value was not chosen by anyone; committing it would write
`ear.conf` without an operator's release. The idle rule guards against a live but buggy script that never ends a
gesture (which would hold the key against every other editor); with keep-alives it never fires on an operator holding
a fader still. (c) leaves that script's hold for as long as its socket lives.

**G5. The committed value's precision.** (a) Write exactly what the end carries. (b) Round to the reach's `decimals` at
the end (half away from zero on the shortest decimal text; zero written `0`), and announce the rounded value.
**Recommendation: (b).** `ear.conf` stays readable (`gainDb -3.5;`, not `-3.4378125;`) whoever sent the value; the
difference is at most half a step. Hand edits and `node.set` keep any precision: only gestures round, since only they
come from a pointer's travel.

**G12. A slow request and gesture ticks on one socket.** batch2.md reads one message per connection at a time, so a
held action reply (10 s) or a write-delayed request stops that browser's ticks. (a) The session reads ahead:
`gesture.change` and `gesture.cancel` notifications skip a FIFO that keeps every other message in order (6.6). (b)
Replies may come out of order (JSON-RPC allows it by id), and batch2.md's order rule names which stay ordered. (c) The
editor opens a second socket for gestures; "owning session" is then defined across sockets. **Recommendation: (a).** It
changes nothing for messages with an id, so every order rule and fixture holds; (b) changes what every client may
assume about replies; (c) doubles connections and makes ownership and session close harder to state.

**G13. Where `gesture.begin` is answered.** (a) On the main loop, from a reach index the keeper posts with each version
(7.2). (b) Through the keeper, as revision 1 had it. (c) Through the keeper, but the client picks the gesture id so its
ticks can follow the begin before the reply. **Recommendation: (a).** A drag starts moving the audio within a main-loop
turn even while the keeper writes the last drag's end; the keeper still judges every end. (b) is the laggy knob at the
start of each drag; (c) needs a collision rule for ids from many sessions and still holds the begin's refusal back.

### Wire

**G2. Method shape.** (a) node-framework.md 8.5's `node.setParam {name, key, value, gesture: phase}` and one
`node.paramChanged` per node and key. (b) `node.set` (a merge of any keys, live or not, answering `applied`) plus
`gesture.begin`, `change`, `end`, `cancel`, one `gesture.changed` per gesture, and `node.invoke` for actions instead of
`node.action`. **Recommendation: (b).** Each method has one path and one set of errors; scripts get a partial settings
update they lack today; the names follow `<entity>.<verb>`; a wide gang costs one message per tick.

**G7. The protocol number.** (a) An absent `protocol` is 0 (today's wire); this page is 1; additive growth is a
`features` list, and clients gate each method and request member on it; every enum is open from 1. (b) Today's wire is
protocol 1 and this page is additive within it (revision 1): an old ear and a new one cannot be told apart. (c) Bump the
number for each slice, enums closed. **Recommendation: (a).** A client can tell what an ear offers without probing;
the number stays for breaking changes; open enums let slice 8's `recovering` and every later code arrive without a
bump.

### Standard output

**G8. Standard output beyond node-framework.md 8.1.** 8.1 puts parameters on stdout "at commit only" and actions not at
all. (a) Add `param` records for commits and aborts, and `action` records. (b) Only `param` at commit. (c) Neither.
**Recommendation: (a).** A restart invoked from a browser is otherwise invisible in `kubectl logs` (its `node` records
have no cause), and an abort explains why a fader jumped back.

**G9. The status change record under flapping** (Q2's "View + change record", R10's flapping). (a) Pace per node and
field (32 at once, then 1 a second), and always write a trailing record with the field's current value and `heldBack`
within a second of the last held change, so the last record of each field is true. (b) Write every change, unpaced. (c)
Pace as (a), and while a field is flapping replace its records by one summary record a second (transitions, first and
last value, the values seen). **Recommendation: (a).** It keeps every change of a calm field, never ends `kubectl logs`
on a value the node left, and bounds a flapping field to about one line a second, with `ear.status-flapping` saying
how hard it flaps; (b) lets one bad connection flood the output queue and push states out (the reason logs are paced
today, `status_log.hpp:52-56` [V]); (c) is a second record kind for what (a) and the diagnostic already say.

### Scope and order

**G6. Build order.** (a) Slice 3 first (node-primitives.md S3.1 to S3.7), then slice 5, then slice 6: `Report` sits on
slice 3's `RunHandle`, calls and loop fields run on its loops, and sendspin's notes are split once it is on them. (b)
Slice 5 first, with an interim handle (today's `Channel` with typed writes) and an interim `RunObject::invoke(call)` on
the main loop that must hand the call to the object's own thread at once, as `RunObject::close` does today
(`node_type.hpp:242` [V]). **Recommendation: (a).** (b) builds two APIs slice 3 replaces, and splits sendspin's notes on
threads slice 3 then moves; slice 3 changes little an operator sees (node-primitives.md section 8's five visible
changes), so it costs the control plane little time.

**G10. Live rows read off the realtime thread.** A sound card's mixer volume or a volume sent to Music Assistant
(sendspin's "volume and mute", on the roadmap) must be applied off the realtime thread. (a) In this slice live rows
reach only the run's code (`io.param`); the shape of a hook is reserved, `ready.watch<row>(loop, fn)` (the newest value
posted to that loop, one pending per row, on every slot write), and built with the first type that needs it. (b) Build
the hook now. **Recommendation: (a).** No built-in type in this cut needs it, and a hook with no user cannot be tested
against a real need; the limit is stated where a type author reads (2.4, 3.2, 7.4).

**G11. A switch (flag) setting kind.** Mute is the most common live control on a console; there is no `flag` row.
(a) Defer it: a mute today is a whole-number row 0..1, which steps (it renders as steppers, not a fader); a `flag` row
comes with the first type that needs one. (b) Add it now: written in knobly as `on`/`off` (or `true`/`false`), a
toggle in the editor, committed by `node.set`, live as a step. **Recommendation: (a).** No built-in type in this cut
needs it, and how knobly spells a boolean in a setting's place is unchecked [U]; the wire's value-kind vocabulary
already has `flag`, so adding the row later is additive.

## 15. Open items

- [U] Aggregate deduction of `TypeSpec` with designated initialisers and defaulted template parameters on GCC 14 (5.3,
  node-primitives.md U1); slice 3's S3.5 compiles it first and step 5.2 extends the test; the fallback is a consteval
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
- This page is written to node-primitives.md's G14 (loops as spec handles), G15 (`build`) and G17 (`ready.handle()`,
  and so `Report::handle()`), each at its recommendation and pending the owner's answer. An answer against one changes
  5.2, 5.3, 5.5, 5.7 and 10.1 here with node-primitives.md.
- `writer::resource` is named here and built by slice 8.
- `ready.watch<row>(loop, fn)` for live rows off the realtime thread: reserved (G10).
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
