# Parameters: slice 6 of the node framework

Status: design; steps 6.1 (change marks and restart reasons), 6.2 (decimal rows and typed defaults), 6.3 (indexed
families), 6.4 (choice rows), 6.5 (live rows: slots, ramps, steps, live values, `io.param`) and 6.6 (retunes, feature
`live`) are built and in the library docs ([writing-a-node-type.md](../writing-a-node-type.md#change-marks), [#decimal-rows](../writing-a-node-type.md#decimal-rows),
[#indexed-families](../writing-a-node-type.md#indexed-families), [#choice-rows](../writing-a-node-type.md#choice-rows),
[#live-rows](../writing-a-node-type.md#live-rows), [architecture.md](../architecture.md#the-realtime-channels),
[#live-values](../architecture.md#live-values), [#retunes](../architecture.md#retunes),
[control-port.md](../control-port.md#retunes), [output-records.md](../output-records.md#param)), the rest is not. Revised on 2026-11-26 against the code slices 3d (changes in place and reshapes, on
`master` since v4.3.0) and 5 (the control plane, on `claude/slice-5`) built, and against the owner's rule M3 from
`processing-nodes.md` (on `claude/admiring-hamilton-26crw1`), whose slice-6 extensions this page folds in as general
mechanisms. Slice 6 gives a node type decimal, choice and indexed rows, rows that change **live** (ramped or stepped
on the realtime thread, with no hook called and no restart), a reason on every row that still restarts, retunes in the
plan and in `NodeHost`, gestures from the control port, the `gain` type, and the editor's faders. Section 13 lists every
place this page departs from the first design (written before 3d and 5 were built) and why. It was revised again after
three reviews (realtime, API, plan) on the same day.

It cites the library docs for everything built and does not repeat them. The words below are this page's:

| Word | Meaning here |
|---|---|
| **row** | A `SettingsTable` row ([writing-a-node-type.md](../writing-a-node-type.md#settings-one-table-of-typed-rows)) |
| **live row** | A row marked `.live(...)`: a change of it moves a **slot** the code reads, never calling a hook |
| **slot** | One live member's latest value, a 64-bit word in the configuration's realtime channels |
| **live values** | `NodeHost`'s per-node vector of every live member's current value, the main loop's mirror of the slots (4.5) |
| **retune** | The plan's outcome for a change whose every changed key is live |
| **gesture** | A drag of one live decimal row from the control port: begun, moved by ticks, ended (committed) or aborted |
| **reach** | Every node and key whose value is defined at the same place in `ear.conf` as the gesture's |
| **release** | A gesture ended by ear for its owner (its session closed, or it fell silent), committing its last accepted value |

## 1. The owner's words that bind this slice

The R and G numbers are the register the node framework's design used; M3 is `processing-nodes.md`'s.

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| R2 | "consider how to abstract configuration and control plane events so that nodes can build on a shared base primitive layer" | 2026-11-07 | Parameters are a framework subsystem: a type declares rows and reads them; it writes no slot, queue, thread or wire code |
| R14 | "The node audio threads should be concerned with reading inputs and writing outputs. The framework should provide the necessary primitives …" | 2026-11-08 | `process` reads a parameter through `Io`, from wait-free slots the framework keeps |
| R17 | "I want the node to control whether settings can be changed in place, with the framework supporting it. …" | 2026-11-08 | Each row says how a change to it is taken: live, in place, or a restart (4.2) |
| Q6 | "Commit at gesture end" | 2026-11-07 | Ticks move the audio; only a gesture's end writes `ear.conf` |
| P2 | "I want a complete proposal or a design doc before implementation. The goal is high UX and DX" | 2026-11-08 | Journeys first; every declaration, request and record shown whole |
| G1 | A fader for a value several settings share: "(b)", as recommended | 2026-11-08 | A gesture moves its whole reach, all live decimals of one unit, within the range they all allow (8.3) |
| R31 | "Only a related save" | 2026-11-08 | A save stops a gesture only when it changes the dragged value, its defining lines or its reach (8.6) |
| R32 | "Keep last value" | 2026-11-08 | A gesture whose browser disconnects or goes silent ends as released, keeping the last value ear heard and accepted (8.5) |
| M3 | "Ideally all adjustments are live. Node restarts should be justified." | 2026-11-25 | Every row of a new type is live, in place or a reshape in place; a row that restarts states why, on the wire too (4.2, 4.3) |
| O1 to O12 | "ill take the recommendations" | 2026-11-26 | The owner took this page's recommendation on each of its twelve questions; section 12 records each as a decision (O1 to O12) |
| — | "It's all event driven" (recorded at `src/server/control_port.cpp`: "No timers: a connection stays until its peer or stop() ends it") | — | The control thread gets no timer and no socket option changes; a silent gesture is ended by a main-loop timer, where timers are built (8.5, decision O1) |

Ids from other pages that this page uses, in their words:

| # | From | Words |
|---|---|---|
| G11 | the first design | No boolean setting kind; only whole numbers and decimals can be live |
| G16 | the first design | At `SIGTERM` open gestures are dropped, not committed |
| X5 | the first design | No faders on in-place rows: a drag would chop the audio into holds |
| D7 | `processing-nodes.md` | A live boolean ramps from 0 to 1 over its smoothing, and the code multiplies by it (mute, polarity) |
| D9 | `processing-nodes.md` | No row restarts its node: each is live, in place, or a reshape in place, and a row that must restart says why |
| D11 | `processing-nodes.md` | A change of delay (`delay`'s `ms.N`, `limiter`'s `lookaheadMs`) is in place, its new latency declared at the commit point |

## 2. What is built, and what this slice adds to it

Built, and used here as it is:

- **Rows and the walk**: required, optional and defaulted rows of `text`, whole number, rate, boolean, decimal and
  choice ([#decimal-rows](../writing-a-node-type.md#decimal-rows), [#choice-rows](../writing-a-node-type.md#choice-rows),
  a default of its rule's kind), one change
  mark per row (`.inPlace()` or `.restarts(why)`, [#change-marks](../writing-a-node-type.md#change-marks)), indexed
  families with their member grammar and the one key lookup `rowOf` ([#indexed-families](../writing-a-node-type.md#indexed-families)),
  `.shapesPorts()`, `.defaultFrom()`, `.help()`, the refusal's `key` and `detail`
  ([writing-a-node-type.md](../writing-a-node-type.md#settings-one-table-of-typed-rows)). The boolean kind (`flag` on
  the wire) exists, so a live boolean needs only the live mark.
- **The plan's outcomes** and `changeOf`, the one rule the plan and the type harness share
  ([architecture.md](../architecture.md#restarts-and-changes-of-settings)); **changes in place** and their machine
  ([#changes-in-place](../architecture.md#changes-in-place)); **reshapes**, in place when every changed key is in place,
  the type has a change version and the configuration no companion ([#reshapes](../architecture.md#reshapes)).
- **The realtime channels**, bound when the code is prepared and kept across a swap, with the triggers' inward counters
  ([#the-realtime-channels](../architecture.md#the-realtime-channels)); `Io`'s `set`, `raise`, `log` and `triggered`
  ([writing-a-node-type.md](../writing-a-node-type.md#reporting-from-the-code)).
- **The wire**: `types.get` and its rows ([control-port.md](../control-port.md#type-schemas)), `node.set` with merge
  patches, `dryRun` and `applied` ([#nodeset-and-dry-runs](../control-port.md#nodeset-and-dry-runs)), status and events
  ([#status](../control-port.md#status)), the per-client limits ([#per-client-limits-and-stopping](../control-port.md#per-client-limits-and-stopping)).
- **The graph file keeper**'s edits at a value's definition (`updateNode`,
  [graph-file.md](../graph-file.md#edits-at-the-source)) and hand edits winning ([#hand-edits-win](../graph-file.md#hand-edits-win)).
- **The editor's settings form**, its draft, dry run and Apply asking first ([editor.md](../editor.md#the-settings-form)).
- **Live rows** (step 6.5): `.live(ramp/step)`, slots, the generation word, the advance and the snap at a drive,
  `io.param` with `Smoothed`, `Stepped` and `forSegments`, `io.count`, `liveValue` and `liveCount`, `NodeHost`'s live
  values and `setLive`, `h.live` ([writing-a-node-type.md](../writing-a-node-type.md#live-rows),
  [architecture.md](../architecture.md#the-realtime-channels), [#live-values](../architecture.md#live-values)).
- **Retunes** (step 6.6): the plan's split of live keys into `NodeChange::retuned`, `NodeOutcome::retune`, a disabled
  node's `stored` and a failed node not retried, `NodeHost` by phase, configurations seeded from the live values,
  `NodeConfig::checkLive` and `h.live` running it, feature `live` (`change: "live"` with `live`, `applied: "retune"`,
  `retuned`, `alsoChanged`) and the `param` record ([architecture.md](../architecture.md#retunes),
  [control-port.md](../control-port.md#retunes), [output-records.md](../output-records.md#param)).

What it adds, by layer:

| Layer | Adds |
|---|---|
| `ear_core` rows | the third change mark, `.live(ramp/step)`, on plain rows and families |
| `ear_core` reads | `io.param<row>()` (a `Smoothed` or a `Stepped<V>`), `io.count<family>()`; `s.liveValue<row>()` and `s.liveCount<family>()` for `check` and the change version; `forSegments` |
| `ear_core` realtime | Slots, ramp and step state and one generation word (writes and snaps) in `RealtimeChannels`; the advance before `process`; the snap at a drive |
| `ear_core` plan and host | `NodeOutcome::retune`, `NodeChange::retuned`; `NodeHost`'s live values and `setLive`; retunes in every phase; `Gestures` with its silence timer |
| `ear_config` | The reach index; `OfferKind`; the related-change rule; the main-loop side's working-copy state |
| `ear_control`, `ear_server` | Schema members; `applied: "retune"` and `retuned`; `gesture.*` and `GestureRequests`; per-session addressing; read-ahead |
| `web` | The compact strip for a family above 8 members; live toggles and selects; faders and knobs; `gesture.ts` |
| Types | `gain` (new) |
| Fixtures | `test/protocol/rows.json` (built with decimal rows): test types' schemas, for the row vocabulary no built type has before `gain`, extended by each step |

## 3. Journeys

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
route noise:noise.mono amp:in.1;
route amp:out.1 desk:play.1;
```

`gain` (5.7) has `channels` (default 2, a reshape in place), live ramped `gainDb` and `mute`, and the live indexed
families `trimDb.<n>` and `invert.<n>` by `channels`.

### 3.1 The operator, in the editor

1. **Selects `amp`.** The form draws from the schema: `channels` as a number field marked "changes the ports, in
   place"; `gainDb` as a fader from -inf to +24 dB showing `-6.0 dB`; `mute` as a switch; `trimDb.1`, `trimDb.2`,
   `invert.1`, `invert.2` as small faders and switches. `desk`'s `card` field says "restarts the node: card has no change
   in place yet" (4.3). The box shows `gainDb` as a knob.
2. **Turns the knob.** Pointer down sends `gesture.begin {"name":"amp","key":"gainDb"}`; the main loop answers from the
   running version's reach index without waiting for the keeper, even while another request of this socket is held.
   Each animation frame sends `gesture.change` as a notification, read at once too; the main loop writes `amp`'s slot
   and the realtime thread ramps to it over 20 ms. Other editors see the knob move, held, at most 30 times a second.
   While the pointer is down and still, the editor repeats its last tick every 10 s, so ear does not take the hand as
   gone (8.5).
3. **Releases at `-3.5`.** `gesture.end` makes the keeper edit `gainDb -6;` to `gainDb -3.5;` at its source; the plan
   says `retune`; every client gets `graphFile.changed`, `node.updated` (`"applied":"retune"`, `"retuned":["gainDb"]`)
   and `gesture.changed` `end`, then the owner its reply. No `node.state` follows; standard output has a `graph-file`
   record and a `param` record.
4. **Clicks `mute`.** A live boolean commits at once by `node.set {"name":"amp","settings":{"mute":true}}`: `retune`,
   the factor ramps to 0 over 10 ms, no gesture and no Apply.
5. **Sets `channels` to 4** and applies: the dry run says `reshape` in place; Apply asks nothing; channels 1 and 2 play
   on with no gap ([architecture.md](../architecture.md#reshapes)), `trimDb.3`, `trimDb.4`, `invert.3`, `invert.4`
   appear at their defaults.
6. **Sets `channels` back to 2.** `trimDb.3` stays in `ear.conf` if it was written and is shown greyed, "kept; not
   used while channels is 2"; nothing is refused (4.4).

**A hand edit elsewhere** while the knob is held (a comment above `node card desk`) is not related: nothing is sent,
and the release edits the new text. **A hand edit of the held value** (`gainDb -10;`) aborts the gesture
(`hand-edit`) and the audio ramps once, straight to `-10`; the editor offers "Set -3.5 again". **The laptop's Wi-Fi
drops** mid-drag: the editor reconnects and at once sends `gesture.end` for the id it remembers, with the value it
shows; the id is the credential, so the new session may end it, and `-3.5` is written. Had the laptop not come back,
the gesture would have fallen silent and ear would have ended it as released 30 s after its last message (`stale`),
writing the last tick it accepted (R32).

### 3.2 The script author

```bash
U=ws://127.0.0.1:7777/rpc
node scripts/rpc-call.mjs $U node.set '{"name":"amp","settings":{"gainDb":-12}}'
{"applied":"retune","retuned":["gainDb"]}
node scripts/rpc-call.mjs $U node.set '{"name":"amp2","settings":{"trimDb.2":null,"channels":4}}'
{"applied":"reshape","inPlace":true,"ports":{"added":["in.3","in.4","out.3","out.4"],"removed":[]},"retuned":["trimDb.2"],"routesDropped":[]}
node scripts/rpc-call.mjs $U types.get | jq '.typeSchemas[] | select(.name=="card") | .settings[] | {key, change, restartReason}'
```

A refusal carries its key, as built: `invalid-settings amp: gainDb: must be a number of dB from -90 to 24`, with
`data.key` `gainDb`. A script never needs a gesture: `node.set` moves a live row, and a fader is for hands.

### 3.3 The client implementer

1. **Gate on features**: `live` (retunes and `retuned`, `change: "live"`), `gestures`. Kinds `decimal` and `choice` and
   the `indexed` member need no feature: they are schema vocabulary under `types`, and every enum is open
   ([control-port.md](../control-port.md#protocol-and-features)); the owner confirmed this (decision O9).
2. **Draw from the schema.** A row with `indexed` is a family: its members' keys are `<key>.<n>`, `n` from 1 in decimal
   with no sign and no leading zero (4.4), the one rule for mapping a `settings` key, a `retuned` entry or an
   `invalid-settings` `data.key` to the family's field. Draw members 1 to the count row's current value, and keep any
   member the node's settings hold above it, marked inactive. A `restartReason` is shown beside "restarts". A `choice`
   is a closed set: draw a select and send only its `values`.
3. **Gestures**: begin, ticks as notifications, end; no echo of your own `begin` or `change`; on `aborted` stop the
   control and tell the person; an `end` with a `reason` means ear ended it for you. Keep the gesture id: after a
   reconnect, `gesture.end` or `gesture.cancel` with it from the new session ends the old session's gesture. While a
   hand holds still, repeat the last tick at least every 10 s.

### 3.4 The node type author

```cpp
inline constexpr auto gainRow =
	setting::defaulted("gainDb", setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db,
	                                                                  .minIsSilence = true}), 0.0)
		.live(setting::ramp(20));
inline constexpr auto muteRow = setting::defaulted("mute", setting::boolean(), false).live(setting::ramp(10));
// in process:
const Smoothed gain = io.param<gainRow>();   // dB, ramped by the framework
const Smoothed mute = io.param<muteRow>();   // 0..1, ramped
```

Nothing else: no slot, no queue, no hook, no wire or editor code. Read a live row with `s.get<gainRow>()` (where it
would go stale at the first retune), read `io.param` of a row that is not live, ramp a whole number, leave a row of a
node type's settings with no change mark, or give a restart no reason, and it does not compile (5.4).

## 4. The domain model

### 4.1 Value kinds

One vocabulary for rows, status fields, event and action arguments: the wire's kind names are the built ones
([control-port.md](../control-port.md#type-schemas)), a setting's `choice` included.

| Kind | C++ rule | Reads as | Wire `kind` | `ear.conf` | New in slice 6 |
|---|---|---|---|---|---|
| text | `setting::text<Parse>(mustBe, hints)` | the parser's `T` | `text` | a word or a quoted string | — |
| whole number | `setting::number(low, high, unit)` | `std::uint32_t` | `wholeNumber` | an integer | — |
| rate | `setting::rate(low, high)` | `SampleRate` | `wholeNumber`, `unit` `Hz` | `48khz` | — |
| boolean | `setting::boolean()` | `bool` | `flag` | `true` / `false` | live |
| decimal | `setting::decimal(low, high, unit, hints)` | `double` | `decimal` | a number (`-3.5`, `0.25`, `1e3`) | built ([#decimal-rows](../writing-a-node-type.md#decimal-rows)); live |
| choice | `setting::choice<Enum>({{"peak", Enum::peak}, …})` | `Enum` | `choice` with `values` | a word or a quoted string | built ([#choice-rows](../writing-a-node-type.md#choice-rows)); live |

A choice (built) is a closed set of names read as an enumeration, so a code can step between them live: a choice's
index on the realtime thread is its name's place in its list (the built `ChoiceIndex`), and `Stepped<E>` maps it back.
A free text (a path, a name, a URL) names a resource and is never live.

### 4.2 Change modes

Every row of a node type has exactly one mark, and the schema's `change` says it.

| Mode | Mark | What a change of it does | Kinds | Wire `change` |
|---|---|---|---|---|
| **live, ramped** | `.live(setting::ramp(ms))` | The slot moves; the framework ramps the code's view over `ms` | decimal, boolean (0 to 1) | `live`, `live.transition` `ramp` |
| **live, stepped** | `.live(setting::step)` | The slot moves; the code sees the new value from the next block, with `changed()`, `before()` and `framesSince()` to make its own crossfade | decimal, whole number, boolean, choice | `live`, `live.transition` `step` |
| **in place** | `.inPlace()` | The type's change version, as built ([architecture.md](../architecture.md#changes-in-place)) | every kind | `inPlace` |
| **reshape in place** | `.inPlace().shapesPorts()` | As built ([architecture.md](../architecture.md#reshapes)); a configuration with a companion still restarts | every kind | `inPlace`, `shapes` |
| **restart** | `.restarts("why")` | As built ([writing-a-node-type.md](../writing-a-node-type.md#change-marks)) | every kind | `restart`, with `restartReason` |

A live row is never read by `ports()` (it cannot: 5.2), never shapes the ports and never reaches a loop's state: a value
applied off the realtime thread is an in-place row the change version posts to its loop, as built. A live row may be
the count row of a family that does not shape the ports (`eq`'s `bands`); its count is then read like any live value,
never through `count` (5.2).

The modes cover `processing-nodes.md`'s needs as one mechanism each:

| Need there | Mode here |
|---|---|
| `gainDb`, `levelDb.N`, `freqHz.N`, `q.N`, thresholds, ratios | live, ramped (decimal) |
| `mute`, `mute.N`, `invert.N` (D7) | live, ramped (boolean, 0 to 1) |
| `link`, attack, release, knee, hold | live, stepped (the envelope smooths it) |
| `select`'s `input`, `eq`'s `bands` | live, stepped (whole number); the code crossfades on `changed()` |
| `eq`'s `type.N`, `crossover`'s `slope` | live, stepped (choice); the code crossfades on `changed()`: no longer in place "until text rows can be live" |
| `delay`'s `ms.N`, `limiter`'s `lookaheadMs` (D11) | in place: they change the declared latency, which only the change version declares (decision O4) |
| `channels`, `inputs`, `key` | reshape in place |

### 4.3 The rule for choosing a row's mode

The rule for in place and restart is built ([writing-a-node-type.md](../writing-a-node-type.md#change-marks)). Slice 6
inserts one question as its second, after "does it change the ports?":

- **Does only the realtime code use it, and can it move between two blocks without touching anything else?** Live:
  ramped when a jump would click and the code can use an in-between value, stepped otherwise.

The built rows' reasons stay as built: `file`'s `path` and `card`'s `card` say "has no change in place yet", which the
owner accepted under M3 with a small follow-on slice to make both in place (decision O6).

### 4.4 Indexed families

Built ([writing-a-node-type.md](../writing-a-node-type.md#indexed-families)): the family row, its count row, the member
grammar, members up to the maximum taken and those above the count kept inactive, the refusal above the maximum. What
live rows add to them:

- **Inactive is only the code's view.** Every live member up to the maximum, active or not, is seeded at configure,
  written on a retune and kept ramping by the advance, so a member that becomes active (a reshape raising `channels`,
  a live `bands` stepped up) plays its file value from its first sample.
- The slots of a live family are made for the count row's maximum ([architecture.md](../architecture.md#the-realtime-channels)), so no count change, live or by reshape,
  allocates or resizes anything.

### 4.5 Aggregates and owners

- **The node** (`NodeHost`, main loop) gains its **live values**: one value per live member up to each family's
  maximum, the main loop's mirror of the slots. It is seeded from the node's settings when the node is added, written by
  every retune and gesture tick (a held key holds the hand's value), and is the one source for `liveValue`,
  `checkLive`, seeding a new configuration's slots and the `param` record's `from`. It is not part of a `NodeConfig`, so
  a retune never adopts or replaces a config, and a change that ends uncommitted cannot leave it behind the slots. Each
  config the plan makes from the desired graph already holds the committed live values, since they came from
  `ear.conf`.
- **The configuration** owns the slots and their ramp and step state, inside its realtime channels, made with it and
  freed at teardown step 5 ([architecture.md](../architecture.md#the-realtime-channels)).
- **The gesture** is owned by `Gestures` (`ear_core`, main loop): an id of 128 random bits in hex, never reused and
  not guessable (examples here shorten it), the owning session's token, its reach with the combined range and
  decimals, the committed value, the last accepted value (what the slots hold), the last `seq`, its silence timer, and
  whether its end has been read.
- **The reach index** is a value the keeper makes on its thread for each version of the working copy; the main loop
  holds the running version's (8.3).

## 5. The C++ SPI

Everything extends `settings_table.hpp`, `type_spec.hpp`, `lifecycle.hpp` and `node_type.hpp`. Every new constant is
`inline constexpr` in the type's named namespace, as built.

### 5.1 Rules, rows and marks

Built ([writing-a-node-type.md](../writing-a-node-type.md#live-rows)): `.live(ramp(ms))` and `.live(step)`, the one-mark
rule, `SettingRow::rampMs`, the count of live members and its bound of 256. Still to come: a gesture's end rounds to
`decimals` (step 6.8), and the all-type tests hold a `minIsSilence` row's low to exact zeros (step 6.7).

### 5.2 Reading settings

| Read | Where | Gives |
|---|---|---|
| `s.get<row>()`, `s.get<family>(i)` (built: member `i` from 0, keys from 1) | everywhere `CheckedOf` is handed | a row that is not live: as built; on a live row it does not compile ("a live row is read through io.param, or liveValue in check and the change version") |
| `s.liveValue<row>()`, `s.liveValue<family>(i)` | `check`, the change version, a test | a live row's value as the node's live values hold it now (4.5), not as the config was parsed |
| `s.count<family>()` (built) | everywhere | the family's active members, when its count row is not live; on a live count row it does not compile (`count-live-row`), as `get` does not |
| `s.liveCount<family>()` | where `liveValue` is | the active members of a family whose count row is live |
| `io.param<row>()`, `io.param<family>(i)` | `process` | the realtime view (5.3) |
| `io.count<family>()` | `process` | the active members this block, whether the count row is live (its stepped view) or not |

`ports()` reads through `get`, so it cannot read a live row, and so a retune never changes the ports. A `configure` that
copies a `liveValue` into its code holds a value that goes stale at the first retune; only review catches that, and the
all-type tests' live sweep (5.6) catches a code that never follows its slot.

`check` runs at parse as built, and with given live values in place through the built `NodeConfig::checkLive`
([architecture.md](../architecture.md#retunes)). Step 6.8 runs it on the main loop for every gesture tick, and the
parse of every edit, on the node's settings with its live values in place: every open gesture's hand value of that
node, and the tick's own value. So a rule across rows ("`lowHz` below `highHz`") judges what will play, even with two gestures
open on the two rows from two clients: the second to cross is refused (`tick-refused`) and the pair never plays. The
change version reads live values through `liveValue`; it must not act on them, because the slots already did.

### 5.3 What `process` gets, and 5.4 compile-time refusals

Built: [writing-a-node-type.md](../writing-a-node-type.md#live-rows) and `src/core/tests/compile_fail/live_bad.cpp`.

### 5.5 The erased side

- **One lookup for a key**: `AnySettingsTable::rowOf(key)` is built, and with it the walk, `NodeType::changeOf(key)`
  and `NodeType::kindOf(key)` ([writing-a-node-type.md](../writing-a-node-type.md#indexed-families)); a gesture's
  `checkLive` (6.8) finds a key's slot through `NodeType::liveIndexOf`.
- **`NodeType::schema()`** sends the rows as 9.2 shows; `NodeConfig::liveValues()` gives each live member's parsed
  value, every member up to the maximum, which seeds a node's live values when it is added and is compared by the plan
  to find retuned keys; after that the live values, not the config, are what slots and `liveValue` read (4.5).

### 5.6 The harness and the all-type tests

`TypeHarness`'s `h.change` answering `retune` and `h.live` running the type's check are built
([writing-a-node-type.md](../writing-a-node-type.md#its-own-tests)).

The all-type tests ([writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)) gain, in step
6.7 with `gain` as the first type they cover: every live row moved to its minimum and its maximum leaves the ports
unchanged and the node unrestarted; every ramped row ramps from its default to its maximum with every sample written;
every `minIsSilence` row at its minimum outputs exact zeros, whatever the other live rows hold. A sweep that finds no
registered type with a live row fails, as a guard that checks nothing does ([testing.md](../testing.md#the-rules)).
That restart rows carry a reason is a compile-time rule (5.1), not a test.

### 5.7 The `gain` type, whole

```cpp
// src/nodes/gain/gain_spec.hpp: a gain per channel, every adjustment live but the channel count (a reshape in place).
namespace ear::nodes::gain {
inline constexpr auto channelsRow =
	setting::defaulted("channels", setting::number(1, 64, "channels"), 2u).shapesPorts().inPlace();
inline constexpr auto gainRow =
	setting::defaulted("gainDb", setting::decimal(-90.0, 24.0, "dB", {.decimals = 1, .taper = setting::Taper::db,
	                                                                  .minIsSilence = true}), 0.0)
		.live(setting::ramp(20));
inline constexpr auto muteRow = setting::defaulted("mute", setting::boolean(), false).live(setting::ramp(10));
inline constexpr auto trimRow =
	setting::indexed("trimDb", channelsRow, setting::decimal(-24.0, 24.0, "dB", {.decimals = 1}), 0.0)
		.live(setting::ramp(20));
inline constexpr auto invertRow =
	setting::indexed("invert", channelsRow, setting::boolean(), false).live(setting::ramp(10));
inline constexpr auto in = bank::input<"in">;
inline constexpr auto out = bank::output<"out">;
inline constexpr auto clipping = status::flag<"clipping">(writer::rt).help("an output went above full scale");
inline constexpr auto clipped = event::of<"clipped">(EventSeverity::warn, "channel {channel} clipped at {peakDb} dB",
                                                     arg::number<"channel">(1, 64), arg::decimal<"peakDb">());
inline constexpr auto resetClip = action::trigger<"reset-clip">("clear the clipping flag");
inline constexpr TypeSpec spec{.settings = SettingsTable<channelsRow, gainRow, muteRow, trimRow, invertRow>{"gain"},
                               .banks = Banks<in, out>{},
                               .status = StatusTable<clipping>{},
                               .events = EventTable<clipped>{},
                               .actions = ActionTable<resetClip>{}};
using Checked = CheckedOf<spec>;

inline float dbToFactor(float db) noexcept { return std::exp2(db * 0.16609640474F); }
// gainDb's minimum is silence (minIsSilence), whatever the trim adds.
inline float gainToFactor(float db) noexcept { return db <= -90.0F ? 0.0F : dbToFactor(db); }

struct GainCode {
	bool latched = false;
	void process(Io<spec> io, const GraphCycle&) noexcept {
		if (io.triggered<resetClip>()) latched = false;
		const Smoothed gain = io.param<gainRow>();
		const Smoothed mute = io.param<muteRow>();
		const auto ins = io[in];
		const auto outs = io[out];
		for (std::size_t c = 0; c < outs.size(); ++c) {
			const Smoothed trim = io.param<trimRow>(c);
			const Smoothed invert = io.param<invertRow>(c);
			const auto factorAt = [&](std::uint32_t i) {
				return gainToFactor(gain[i]) * dbToFactor(trim[i]) * (1.0F - mute[i]) * (1.0F - 2.0F * invert[i]);
			};
			if (gain.steady() && trim.steady() && mute.steady() && invert.steady()) {
				const float factor = factorAt(0);
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * factor;
			} else {
				for (std::uint32_t i = 0; i < io.frames(); ++i) outs[c][i] = ins[c][i] * factorAt(i);
			}
			float peak = 0.0F;
			for (const float sample : outs[c]) peak = std::max(peak, std::abs(sample));
			if (peak > 1.0F && !latched) {
				latched = true;
				io.raise<clipped>(static_cast<std::int64_t>(c + 1), 20.0 * std::log10(static_cast<double>(peak)));
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
	Configured configure(CreateOf<spec>& cx, const Checked&) { return cx.applied(GainCode{}); }
	// Called only for a change of `channels` (its one in-place row): a code for the new ports. The latch starts clear.
	Changed configure(ChangeOf<spec>& cx, const Checked&, const Checked&) { return cx.applied(GainCode{}); }
};
}
```

`gain.cpp` is `makeNodeType<gain::spec, gain::Gain>()`; `CMakeLists.txt` is `ear_node_type(SOURCES gain.cpp TESTS
tests/gain_test.cpp)`; `validSettings` gains `{"gain", {{{"channels", 2}, {"gainDb", -6.0}, {"trimDb.1", 0.5},
{"invert.2", true}}}}`. Nothing else names the type. Since 130 live members (1 + 1 + 64 + 64) are made for every
configuration, each block's advance costs one atomic load when nothing moved ([architecture.md](../architecture.md#the-realtime-channels)). Every row carries its mark;
`gain` has no restart row.

## 6. The realtime contract

Built ([architecture.md](../architecture.md#the-realtime-channels), [#live-values](../architecture.md#live-values),
[#retunes](../architecture.md#retunes), [writing-a-node-type.md](../writing-a-node-type.md#live-rows)).

## 7. Retunes: the plan and `NodeHost`

Built ([architecture.md](../architecture.md#retunes), [control-port.md](../control-port.md#retunes),
[output-records.md](../output-records.md#param)). What step 6.8 adds: a key a gesture holds is never retuned by an
edit (the edit is refused `parameter-held`, 8.6, before the plan), and the parse's `check` of an edit runs with every
open gesture's hand value in place (5.2).

## 8. Gestures: the keeper path

### 8.1 Who does what

| Step | Thread | Owner |
|---|---|---|
| Decode `gesture.*`; read ahead; mark a gesture ending; the session token | control | `ear_control` (`decodeCommand`, `ControlSession`), `ear_server` (the read loop) |
| The reach index of each version of the working copy | keeper | `ear_config` (`GraphFileModel::reachIndex`) |
| `gesture.end` and releases: ordering, file checks, the candidate through `updateNode` | keeper | `ear_config` |
| `gesture.begin`, `gesture.change`, `gesture.cancel` | main loop, by a direct crossing | `GraphGateway::gesture` (new) in `ear_control`, which calls `Gestures` and `GestureRequests` |
| The file's state and the running reach index for a begin | main loop | `GestureRequests` (new, declared in `src/control/include/ear/control/requests.hpp` beside `GraphRequests`, implemented by the keeper's `MainLoopSide`) |
| Open, move, close, abort and release gestures; the silence timers; write live values and slots; tell listeners | main loop | `Gestures` (`ear_core`, given the reach and the session token as plain values), `NodeHost::setLive` |
| Whether a save or an edit is related to each open gesture | main loop | the keeper's `MainLoopSide`, `Gestures` |
| Delivery to all but the owner, or to the owner alone | control | `ControlHub` (new addressing) |

The control thread holds only the newest pending tick per gesture and an ending mark; `Gestures` on the main loop is the
one owner. `MainLoopSide` gains the newest working-copy version it was announced or offered and whether that version has
errors or was refused, which today only the keeper thread knows (`working_.read().hasErrors()`); `announce` keeps it
before it tells the listeners.

### 8.2 A gesture, step by step

```mermaid
sequenceDiagram
  participant E as Editor
  participant C as Control thread
  participant K as Keeper thread
  participant M as Main loop
  participant R as Realtime
  K->>M: with each offer and announce: the version's reach index and file state
  E->>C: gesture.begin {amp, gainDb}
  C->>M: gateway.gesture(Begin{session, amp, gainDb}) (skips the FIFO)
  M->>M: stopping, file state, node, row, reach, holds: g4 opened, timer armed
  M-->>E: (others) gesture.changed begin; (owner) reply {g4, -6, reach, range}
  loop each animation frame
    E->>C: gesture.change {g4, seq, v} (no id, read ahead)
    C->>M: newest pending tick
    M->>M: checkLive; live values = v; timer reset
    M->>R: slots = v, generation + 1
  end
  E->>C: gesture.end {g4, -3.5}
  C->>C: mark g4 ending
  C->>K: EndGesture{session, g4, -3.5} (in the session's FIFO)
  K->>K: candidate: gainDb -6 -> -3.5 at its definition
  K->>M: offer (gestureEnd g4)
  M->>M: plan: retune; commit; Gestures.close(g4)
  M-->>E: graphFile.changed, node.updated, gesture.changed end, reply {-3.5}
  K->>K: write ear.conf when free
```

- **Begin** (main loop, no keeper, skipping the session's FIFO: 8.4), in order: `stopping`; `file-invalid` when the
  newest working copy `MainLoopSide` knows has errors, `file-refused` when the graph refused it; `unknown-node`;
  `not-live` (the row is not a live ramped decimal: `data.change` says what it is, so a client sends `node.set` for a
  stepped one); the reach from the running index; `reach-mismatch` unless every reached row is a live ramped decimal
  of one unit with ranges that meet; a held pair is `parameter-held`. Then `Gestures` opens it, arms its silence timer
  (8.5), tells the others `begin` and answers. Nothing is written.
- **Ticks** (main loop): `Gestures::move` checks the gesture is open and not ending, drops a stale `seq`, resets the
  silence timer, drops an equal value, checks the value against every reached row and the type's `check` (`checkLive`
  with every open gesture's hand value in place, 5.2; a refusal is `tick-refused` to the owner, at most once a second,
  and the last accepted value stays), writes every reached node's live values and slots through `setLive`, bumps each
  node's generation once, and arms the tick for `gesture.changed` `change` (at most 30 a second, on the built status
  tick, never to the owner).
- **End** (keeper): the candidate is the node's settings with `key` at the value rounded to the reach's `decimals`
  (half away from zero on the shortest decimal text; `-0` is `0`), through the built `updateNode`, which edits the value
  at its definition, so every reached node changes in one commit. On the main loop the candidate's changed pairs must be
  the reach (a guard), the gesture's own keys pass `parameter-held`, the plan commits (a `retune`), then
  `Gestures::close` writes each reached node's live values and slots the committed value, tells `end`, and the reply
  carries it. An end equal to the committed value writes no `ear.conf` and makes no plan, but `close` still writes the
  committed value to every reached slot, since the slots hold the last tick. **Any refusal of an end aborts the
  gesture** (`refused`): the slots return to the committed value and the client begins again. An end with no `value`
  ends at the last accepted value.
- **Release** (8.5) is an end that ear starts on the main loop for its owner: `Gestures` builds it from the last
  accepted value, which is what the slots hold, and sends it to the keeper as an end. If the keeper or the plan refuses
  it, the gesture is aborted (`refused`) and the slots return to the committed value; a release is never left open.
- **Cancel, removal** abort on the main loop: every reached slot returns to the committed value.

### 8.3 The reach index and defining lines

With every offer (an accepted edit's candidate, a disk read, `graphFile.set`) and every announce the keeper sends the
version's `ReachIndex`, made only when the text changed: for each node and live key the text gives, the definition
(knobly's `expanded_value::definition()`, or `written()` for an override, as `updateNode` already falls back), its
**defining lines** (a 64-bit hash of the text of the lines from the defining statement's first token to its `;`, with
their range), every pair whose expanded value has that definition, and the value. A text with errors has none.

knobly's `statement().place()` includes the comments a statement owns, the comment lines above it among them, so the
defining lines are taken from the statement's tokens, not its place; otherwise a comment added above `@set $trim 0;`
would be related. Through a `@snippet`, `definition()` gives the chain's end (`@set $trim 0`), not the snippet's line,
so a snippet's user and a direct user of `$trim` share one reach (both checked against the pinned knobly's JS build;
step 6.8 pins them through the C++ binding).

`node.set` on a shared definition already moves every user of it (built: `updateNode` edits the definition), which is
G1's reach; its `param` records name every user (9.7), and from feature `live` its answer names the other nodes it
changed (`alsoChanged`, 9.3), so the editor can say so after a click on a live switch (10.2, decision O8).

### 8.4 Reading ahead and the direct crossing

Today a connection reads its next message only once every reply to the last is queued
([control-port.md](../control-port.md#per-client-limits-and-stopping)); a tick would then wait behind a held
`node.invoke` (up to 10 s). So, in `ear_server`'s read loop and `ControlSession` together:

- **The transport reads every frame as it arrives.** A message with an id, a batch and every other notification go
  into the session's FIFO and run one at a time, in order, exactly as today; the FIFO holds 16, and at 16 the socket is
  not read until one is answered (one `async_read` outstanding, as Beast requires).
- **Three messages skip the FIFO**, checked for shape on the control thread and handed to `GraphGateway::gesture` at
  once: a `gesture.change` (always a notification: one sent with an id is refused at once, -32600 with `data.code`
  `notification-only`, so a tick never silently loses read-ahead); a `gesture.cancel` sent as a notification (sent with
  an id it is queued in order and answered `{}`); and a `gesture.begin`, whose reply carries its own id and may
  overtake the replies of requests queued before it, which JSON-RPC allows and [#order](../control-port.md#order) will
  say. A begin reads only main-loop state, so nothing it answers depends on a queued request. A `gesture.end` stays in
  the FIFO, ordered with the session's edits, which it is one of.
- **The ending mark.** A `gesture.end` or `gesture.cancel` read for an id marks it ending on the control thread at
  once: later ticks for it are dropped there. Since any refusal of an end aborts the gesture (8.2), a gesture marked
  ending never takes a tick again.
- **Batches.** All four gesture methods in a batch are refused `not-in-batch`: a batch runs whole as keeper steps
  ([control-port.md](../control-port.md#the-envelope)), and a begin and a tick are not keeper steps.
- **Coalescing.** The gateway keeps the newest pending tick per gesture in a mailbox shared with the main loop (one
  mutex, held for a copy) and posts one task if none was pending: a thousand ticks a second cost at most one task per
  main-loop turn. The main loop's executor is FIFO, so a tick posted before an end was read lands before the end's
  offer.
- **Addressing.** Each session gets an opaque token from the hub when it attaches; it crosses to the main loop with
  every gesture command and comes back on gesture events. `ControlHub` still encodes each event once, and sends it to
  every graph-subscribed session but the owner (`begin`, `change`) or to the owner alone (`tick-refused`, `end` and
  `aborted` of its own gestures, and the replies). Owner-addressed events reach the owner whatever its subscriptions,
  so a script that never called `graph.subscribe` still learns that its gesture was aborted or released.
- **Any session may end or cancel a gesture by its id.** The id is 128 random bits that only the begin's reply carried,
  so holding it is the credential: an editor that reconnects after a network drop ends the gesture its old, half-open
  session holds by sending `gesture.end` with the id it remembers. Ticks stay the owner's alone; the end's `gesture.changed`
  goes to both sessions.
- **A closed session**: its pending tick is handed on; its queued requests with an id still run in order, their replies
  dropped, as today; only then are its open gestures released (`session-closed`, 8.5). So a `gesture.end` queued
  behind a held `node.invoke` when the socket closed commits its own value, not the last tick.

### 8.5 Ending a gesture whose owner is gone

The first design ended a silent gesture with a 30 s idle timer on the control thread. The control port has no timers
("a connection stays until its peer or stop() ends it"), and this page keeps it so: no control-thread timer and no
change to the sockets. The main loop, though, has timers (`Timers::after`, [`ear/core/timers.hpp`](../../src/core/include/ear/core/timers.hpp))
and already runs the status tick, and the gesture lives there:

- **A closed socket** releases its session's gestures (reason `session-closed`) once its queued requests have run (8.4).
- **A silent owner.** `Gestures` arms one `Timers::after(staleAfter)` per open gesture, 30 s by default, re-armed by
  every message of its owner about it (a tick, even one equal to the last, or a refused one). When it fires the gesture
  is released (reason `stale`): its last accepted value is committed (R32). This covers a peer that vanished without a
  close, a frozen or backgrounded tab, and a peer behind a proxy that ends the TCP connection (the editor's
  `npm run dev` proxy, an ingress, `kubectl port-forward`), which no socket option could see through. A key held by a
  silent owner is refused `parameter-held` until then, with the holder's silence in `data.silentMs`, so a client can
  show "held, idle for 12 s". There is no contention rule: a request never ends someone else's gesture.
- **Keep-alive ticks.** While a hand holds a control still, the editor repeats its last tick every 10 s; an equal value
  writes nothing but re-arms the timer.
- `staleAfter` is a main-loop setting given to `Gestures` with the main loop's injectable clock, so the core and
  gateway tests move the clock rather than wait, and the system test passes a short value by a test-only flag.
- **`SIGTERM`**: later begins and ends get `stopping`; open gestures are not committed (clients closed with 1001), as
  the first design had it (G16; decision O5).

The owner chose the main-loop timer over the event-only alternative (decision O1).

### 8.6 Refusals, collisions and related changes

| Lands during a gesture | What happens |
|---|---|
| A hand edit or `graphFile.set` **related** to it | aborted (`hand-edit` or `file-saved`) on the main loop before the offer's plan; the file offered as built; last in the turn each reached slot is set to its node's running value, so the audio ramps once |
| Another client's edit that changes its reach without touching a held key (`node.set` or `node.update` giving another node `trimDb.1 $trim`; `node.addText` adding a node that uses `$trim`; `node.setText` of a reached node that leaves the held key's value) | aborted (`edited`), before the plan |
| `node.set`, `node.update` or `node.setText` changing a held key's value | refused `parameter-held` |
| `node.set` or `node.update` changing other keys of a reached node | applied, whatever its outcome: a change in place keeps the slots; a restart seeds the new configuration from the live values |
| Any committed change (a reshape or a retune of a live count row) that makes a reached member inactive | applied; aborted (`key-removed`) before the commit's events |
| `node.disable`, `node.enable`, or the reached node failing, restarting, stopping or starting | nothing: ticks move the live values, which seed its next configuration |
| `node.remove`, or a replacement of a reached node | applied; aborted (`node-removed`) |
| A save or edit **not related** (route edits always are not) | nothing: no `gesture.changed`; its index entries replaced by the new version's |
| A hand edit with errors, or one the graph refuses | every open gesture aborted: no end could commit onto it |
| The owning session closes, or falls silent | released (`session-closed`, `stale`: 8.5) |
| A dry run | never judged: `Offer::dryRun` offers are skipped |

The keeper's main-loop side learns what kind of offer it holds from a new internal `OfferKind` (`disk`, `fileSet`,
`edit`, `gestureEnd{id}`), beside the built `Offer`'s `cause` (`control::FileChangeCause`, which stays the wire's).

**When a change is related** (R31), judged per open gesture from the running index and the new version's, for any pair of
its reach: (1) its defining lines' text changed; (2) its value changed after expansion (a chain's base changed
elsewhere); (3) its reach changed (defined elsewhere now, or the pairs sharing its definition differ); (4) it is gone (the
node removed, renamed, replaced, the key undeclared or inactive); (5) the new text has errors or is refused. A key the
file did not give has no defining lines: the change is related when the new text gives the key or its node's `node`
line changes. One comparison per open gesture per offer. Which of the table's rows applies is decided by the offer's
`OfferKind` and, for an edit, whether it changes a held key's value (refused) or only the reach (aborted).

## 9. The wire

Every change below goes in one commit with `docs/control-port.md`, `test/protocol/`, `src/control/` and
`web/src/protocol.ts` ([control-port.md](../control-port.md#changing-the-wire)).

### 9.1 Features

| Feature | Step | Adds |
|---|---|---|
| — (under `types`) | 6.2 to 6.4 (built) | kinds `decimal` and `choice`, `decimals`, `taper` and `minIsSilence`, typed `default`s, `indexed`: all built ([control-port.md](../control-port.md#type-schemas)) |
| `live` | 6.6 (built) | `change: "live"` with `live`, `applied: "retune"`, `retuned`, `alsoChanged`: built ([control-port.md](../control-port.md#retunes)) |
| `gestures` | 6.8 | `gesture.begin`, `change`, `end`, `cancel`; `gesture.changed`; the graph's per-node `gestures`; the new codes |

Slice 5 put its new schema members under features (`status`, `actions`); this page adds the row vocabulary under none,
since a client reads it from the schema it asked for and every enum is open, so an old client draws an unknown kind as
JSON. The owner accepted that (decision O9).

### 9.2 Type schemas

A row gains, as it applies:

| Member | What |
|---|---|
| `kind` | `decimal` and `choice` are built ([control-port.md](../control-port.md#type-schemas)) |
| `change`, `live` | built ([control-port.md](../control-port.md#type-schemas)) |
| `indexed` | built ([control-port.md](../control-port.md#type-schemas)); a `retuned` entry maps to its family's field by the same rule |

```json
{"name":"gain","inputs":["in"],"outputs":["out"],"reconfigureMs":10000,"settings":[
  {"key":"channels","kind":"wholeNumber","required":false,"default":2,"min":1,"max":64,"unit":"channels","change":"inPlace","shapes":true},
  {"key":"gainDb","kind":"decimal","required":false,"default":0,"min":-90,"max":24,"unit":"dB","decimals":1,"taper":"db","minIsSilence":true,"change":"live","live":{"transition":"ramp","ms":20}},
  {"key":"mute","kind":"flag","required":false,"default":false,"change":"live","live":{"transition":"ramp","ms":10}},
  {"key":"trimDb","kind":"decimal","required":false,"default":0,"min":-24,"max":24,"unit":"dB","decimals":1,"taper":"linear","change":"live","live":{"transition":"ramp","ms":20},"indexed":{"by":"channels","from":1,"max":64}},
  {"key":"invert","kind":"flag","required":false,"default":false,"change":"live","live":{"transition":"ramp","ms":10},"indexed":{"by":"channels","from":1,"max":64}}],
 "status":[{"name":"clipping","kind":"flag","initial":false,"display":"panel","help":"an output went above full scale","flapsAbove":{"transitions":10,"windowMs":10000}}],
 "events":[{"code":"clipped","severity":"warn","sentence":"channel {channel} clipped at {peakDb} dB","args":[{"name":"channel","kind":"wholeNumber","min":1,"max":64},{"name":"peakDb","kind":"decimal"}]}],
 "actions":[{"name":"reset-clip","by":"type","target":"code","help":"clear the clipping flag","args":[]}]}
{"key":"card","kind":"text","required":true,"change":"restart","restartReason":"card has no change in place yet","mustBe":"…"}
{"key":"type.1","kind":"choice","values":["peak","lowShelf","highShelf"],"required":false,"default":"peak","change":"live","live":{"transition":"step"},"indexed":{"by":"bands","from":1,"max":16}}
```

### 9.3 `node.set`, `node.update` and `node.updated`

Built ([control-port.md](../control-port.md#retunes)). Step 6.8 adds: both requests refuse `parameter-held` from
feature `gestures` on.

### 9.4 Gesture requests

| Method | Params | Result | Refusals (`data.code`) |
|---|---|---|---|
| `gesture.begin` | `{"name", "key"}`; skips the FIFO | `{"gesture", "value", "reach", "min", "max", "decimals"}` | `unknown-node`, `not-live`, `reach-mismatch`, `parameter-held`, `file-invalid`, `file-refused`, `stopping`, `not-in-batch` |
| `gesture.change` | `{"gesture", "seq", "value"}`; a notification only | none | sent with an id: -32600, `notification-only`; in a batch `not-in-batch`; a refused value is `tick-refused` (9.5) |
| `gesture.end` | `{"gesture", "value"?}` (no value: the last accepted); any session holding the id | `{"value"}`, what was committed | `unknown-gesture`, `stopping`, `not-in-batch`; `invalid-settings` or the desired graph's codes, after which the gesture is aborted (`refused`) |
| `gesture.cancel` | `{"gesture"}`; any session holding the id; as a notification it skips the FIFO | `{}` when sent with an id | `unknown-gesture`, `not-in-batch` |

```json
{"jsonrpc":"2.0","id":9,"method":"gesture.begin","params":{"name":"amp2","key":"trimDb.1"}}
{"jsonrpc":"2.0","id":9,"result":{"gesture":"g5","value":0,"reach":[{"name":"amp2","key":"trimDb.1"},{"name":"amp2","key":"trimDb.2"}],"min":-24,"max":24,"decimals":1}}
{"jsonrpc":"2.0","method":"gesture.change","params":{"gesture":"g5","seq":1,"value":1.5}}
{"jsonrpc":"2.0","id":10,"method":"gesture.end","params":{"gesture":"g5","value":1.5}}
{"jsonrpc":"2.0","id":10,"result":{"value":1.5}}
```

`seq` counts from 1 per gesture; a tick not above the last taken is dropped. A key the type declares and the file does
not give reaches only its node, at the row's default. A node with no configuration can be held. Ids in these examples
are shortened; a real one is 32 hex digits.

### 9.5 `gesture.changed`, the graph and order

`graph.get` and `graph.subscribe` gain per node `gestures` (the open gestures moving its keys, with their live value;
always present, `node.added` too).

```json
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"begin","value":-6,"reach":[{"name":"amp","key":"gainDb"}]}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"change","value":-4.25}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"end","value":-3.5,"reason":"session-closed"}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"aborted","reason":"cancelled","value":-6}}
{"jsonrpc":"2.0","method":"gesture.changed","params":{"gesture":"g4","phase":"tick-refused","code":"invalid-settings","key":"gainDb","detail":"must be a number of dB from -90 to 24"}}
```

- One notification per gesture, not per reached key; `begin` carries the reach.
- The owner gets `end`, `aborted` and `tick-refused`, whatever its subscriptions; the others (graph-subscribed) `begin`,
  `change`, `end`, `aborted`. A session that ended or cancelled another session's gesture by its id gets its reply and
  the `end` or `aborted` too.
- `end`'s `reason`, when ear ended it for its owner: `session-closed`, `stale`. `aborted`'s `reason`: `hand-edit`,
  `file-saved`, `edited`, `cancelled`, `refused`, `node-removed`, `key-removed`, with `value` (what the slots returned
  to) except after `hand-edit`, `file-saved` and `edited`, whose `node.updated` gives it. Open enums.
- **Folding**: `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value`; `end` and
  `aborted` remove it; `tick-refused` changes nothing.
- **Order**, added to the built list ([control-port.md](../control-port.md#order)): a begin's reply may overtake the
  replies of requests queued before it on the same socket (8.4); a begin's `gesture.changed` before its reply, in one
  turn; an end's `graphFile.changed`, `node.updated` (`retune`), `gesture.changed` `end`, then the
  reply; an abort by a related change's `gesture.changed` `aborted` before that change's `graphFile.changed` and events,
  the slots settling last in the turn.

### 9.6 Error codes

All -32000 with `data.code`, each a case in `test/protocol/errors.json`.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-settings` (built) | also an end's value | `key` (a member's full key), `detail` |
| `parameter-held` (new) | `node.set`, `node.update`, `node.setText`, `gesture.begin` would change a key an open gesture holds | `gesture`, `key`, `silentMs` (since its owner's last message) |
| `not-live` (new) | `gesture.begin` on a key that is not a live ramped decimal | `key`, `change` (`live` for a stepped row: send `node.set`) |
| `reach-mismatch` (new) | `gesture.begin`: the reach holds a row that is not a live ramped decimal, another unit, or ranges that do not meet | `line`, `column` (the definition), `with` (each pair and why: `kind`, `unit` or `range`) |
| `file-refused` (new) | `gesture.begin` while the graph refused the working copy's last hand edit | |
| `unknown-gesture` (new) | no open gesture by that id | |
| `not-in-batch` (new) | any `gesture.*` in a batch | |
| `notification-only` (new) | `gesture.change` sent with an id (-32600, not -32000) | |

`shared-definition` keeps its built meaning, for edits only. `parameter-held` comes with `gestures`; `not-live`,
`reach-mismatch`, `file-refused` and `unknown-gesture` are answered only to `gesture.*` requests, so a client without
`gestures` never sees them.

### 9.7 Standard output: `param`

A new kind, with `v`, `seq`, `t`, `kind`, its fields and `msg` ([output-records.md](../output-records.md#the-envelope)):

```json
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
```

The record and its causes `edit` and `disk` are built ([output-records.md](../output-records.md#param)). Step 6.8
adds the cause `gesture` for a gesture's end, with `gesture`; one record per abort (`cause: aborted`, `reason`, `to`
absent after a related change, `from` the live value the hand left); a release is `cause: gesture` with `reason`. A
committed record's `from` is the value the settings held before, which is the node's live value whenever no gesture
moves it.

### 9.8 Fixtures

| File | Covers |
|---|---|
| `rows.json` (built with its decimal rows and typed defaults: [control-port.md](../control-port.md#fixtures)) | schemas of test types (not the real ear's), held by `src/control/tests/type_schemas_test.cpp` encoding test specs and read by vitest's form tests: a family and choices (built), live ramped and stepped rows (6.6); each step extends it, so the row vocabulary has a fixture before any built type uses it |
| `types.json` | `gain`'s schema whole (6.7); every built row's `change` and `restartReason` are built |
| `edits.json`, `node-set.json` | `retuned: []` on every `node.updated` and `node.set` answer, `alsoChanged: []` on every `node.set` answer (built); with `gain` (6.7): a retune, a mixed reshape with `retuned`, a dry run of each; a disabled node's `stored` with `retuned`; `alsoChanged` on a shared definition |
| `gestures.json` (new) | begin with no echo to the owner and `begin` to another first; ticks (one stale); end with its order; reaches of two keys and of 64; `reach-mismatch` with `with`; `parameter-held` with `silentMs`; a silent holder released `stale`; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings`, aborted `refused`; an end from another session by id (the reconnect); an unsubscribed owner getting `aborted`; aborts by a related hand edit, `graphFile.set`, `node.set`, `node.addText` and `node.setText`; `node.setText` of a held key refused; a live count retune inactivating a held member (`key-removed`); an unrelated hand edit and a route edit left open; `session-closed` after a queued end; `unknown-gesture` after an abort; `not-in-batch` for each method; `notification-only` |
| `unknown-values.json`, `errors.json`, `snapshot.json`, `output-records.json` | this page's members, codes and records |

## 10. The editor

### 10.1 Stores

| Module | Gains | Tested by |
|---|---|---|
| `protocol.ts` | section 9's types; `ControlErrorCode`'s new codes | typecheck |
| `graph.ts` | each node's `gestures`; `retuned` on `node.updated`; `gesture.changed` | vitest, every fixture |
| `form.ts` | live rows committed at once (`asksFirst` passing `retune` and the "live" hints are built, as are families with inactive members, choices as selects and the "restarts: why" hint) | vitest, from `rows.json` until `gain` is in `types.json` |
| `rpc.ts` | `notify()`: a message with no id | vitest |
| `gesture.ts` (new, pure) | one state machine per control (10.3), the taper mapping, the ids it holds across a reconnect | vitest, a fake client |
| `keys.ts` | a focused fader's or knob's range input, or anything in `.nokey`, counts as a text target for the editor's shortcuts (`f`, `l`, `m`, `a`, `?`, Escape), though not for the input's own keys; today a range input is deliberately not a text target, and only the controls of 10.3 change that | vitest |

Nothing in `web/` names a node type. Without `gestures`, live decimals are number fields committed by `node.set`;
without `live`, every row is drawn as today.

### 10.2 How each row renders

| Row | Control | Commits |
|---|---|---|
| decimal, not live | built ([editor.md](../editor.md#the-settings-form)) | Apply |
| live ramped decimal | a fader mapped by `taper` with a number field beside it | a gesture; the field by `node.set` on Enter |
| live boolean | a switch | `node.set` at once; when its answer's `alsoChanged` is not empty, a notice "also changed amp2, amp3" |
| live choice | a select | as a live boolean |
| live whole number, live stepped decimal | steppers and a number field | `node.set` on Enter or a step |
| family | one control per member to the count row's value, those above it greyed (built: [editor.md](../editor.md#the-settings-form)); above 8 members, a compact strip behind a disclosure | as its kind |
| restart row | as today, its hint "restarts the node: <restartReason>" | Apply, which asks first |

The box shows a knob for each live ramped decimal that is not in a family when the type has one or two (`gain`:
`gainDb`). Live controls follow the server except one the operator holds; they are disabled while the graph file has
errors or was refused and while the connection is not open. A live change from the form does not touch the draft of
the other fields.

### 10.3 `gesture.ts`

| State | Input | Sends | Next |
|---|---|---|---|
| idle | press, or first key | `gesture.begin` | beginning (intent: hold) |
| beginning | 200 ms with no reply | — | beginning, shown pending |
| beginning | move | nothing (keeps the newest) | beginning |
| beginning | release / Escape | nothing yet | beginning (intent: end / cancel) |
| beginning | reply, intent hold | the kept value as tick 1 | active |
| beginning | reply, intent end | `gesture.end` with the shown value | ending |
| beginning | reply, intent cancel | `gesture.cancel` | idle |
| beginning | refusal | — | idle; snaps back; a notice names the code (`not-live` with `change` `live`: the field's `node.set` instead) |
| active | move | `gesture.change` (`notify`), at most one per animation frame | active |
| active | 10 s with no move | the last tick again | active |
| active | press or key on the same control (pointer during a keyboard gesture, or the reverse) | nothing: the same gesture goes on | active |
| active | release, `pointercancel`, `lostpointercapture`, window `blur` or `visibilitychange` to hidden | `gesture.end` with the shown value | ending |
| active | Escape | `gesture.cancel` | idle |
| ending | reply | — | idle; shows the committed value |
| ending | refusal | — | idle; ear aborted it (`refused`); the committed value; notice |
| any | `aborted` for its id | — | aborted until release, then idle; notice |
| any | `end` with a `reason` | — | idle; the committed value; notice |
| any | connection lost | — | frozen, the id and shown value kept |
| frozen | reconnected | `gesture.end` with the kept id and shown value, from the new session | ending |

The pointer and keyboard mapping is the first design's, unchanged:

| Taper | Value at travel `t` (0..1) |
|---|---|
| `linear` | `min + t × (max − min)` |
| `log` | `min × (max / min)^t` |
| `db` | piecewise linear through (0, min), (0.10, -60), (0.25, -40), (0.45, -20), (0.60, -10), (0.75, 0), (1, max) |

A `dB` unit does not imply the `db` taper: a row outside its bounds (`trimDb` from -24 to +24) maps linearly. Fader
drag follows the pointer; knob drag is vertical, 200 px the whole travel (Shift 2000 px); double-click or Alt-click
begins and ends at the default; arrow keys begin on the first press, tick 1% (Shift 0.1%) and end 400 ms after the last
key or on blur; Page Up/Down 10%, Home/End the ends; the wheel only on a focused control. Every control on a box carries
React Flow's `nodrag`, `nowheel`, `nopan` and `nokey` and `touch-action: none` ([editor.md](../editor.md#shortcuts),
[#phones-and-touch](../editor.md#phones-and-touch)). The form's faders are horizontal and carry `touch-action: pan-y`,
so a vertical swipe that starts on one scrolls the drawer and a horizontal one drags. Each fader and knob is drawn over a
visually hidden native `<input type="range">`, which takes focus and the keys and carries `aria-valuetext` ("-3.5 dB",
"-inf dB"), so a touch screen reader (VoiceOver, TalkBack) can adjust it by swipe, which it cannot do for a custom
`role="slider"`; an assistive change is a gesture begun, ticked and ended as the arrow keys' are. Held by another
editor, the input is disabled with the reason (and the holder's silence from `silentMs`); a polite live region
announces aborts.

Notices for the owner's own gesture: `hand-edit` "ear.conf was edited by hand and changed amp gainDb while you held it;
it now says -10." [Set -3.5 again]; `file-saved` the same from the text pane; `edited` "Another editor changed what
shares amp2 trimDb.1 while you held it."; `node-removed`; `key-removed` "amp2 now has 2 channels, so trimDb.3, which you
held, is not used."; `refused` the end's refusal; `stale` "ear heard nothing about amp gainDb for 30 s and kept -3.5.";
a socket lost mid-drag "Connection lost while you held amp gainDb; ear keeps the last value it heard." until the
reconnect's end answers.

## 11. Order of work

Steps are named "step 6.n" and sections of this page "section 6.n". Each step builds, keeps every test green and changes
the wire, the library docs it names and the fixtures in the same commit. Steps 6.1 to 6.4 change no audio path. Until
step 6.7 no built type has a decimal, choice, indexed or live row, so each of steps 6.2 to 6.6 extends
`test/protocol/rows.json` (9.8), schemas of test types that `src/control/tests/type_schemas_test.cpp` encodes from test
specs and vitest's form tests read; `types.json` stays what the real ear sends.

| Step | What | Needs | Wire | Library docs it rewrites |
|---|---|---|---|---|
| 6.1 | Change marks and restart reasons: **built** | slice 5 | `restartReason`; every built row's `change` | writing-a-node-type.md#change-marks, control-port.md#type-schemas |
| 6.2 | Decimal rows, typed defaults: **built** | 6.1 | kind `decimal`; `rows.json` | writing-a-node-type.md#decimal-rows, graph-file.md, editor.md#the-settings-form |
| 6.3 | Indexed families: **built** | 6.2 | `indexed` | the same |
| 6.4 | Choice rows: **built** | 6.2 | kind `choice` for settings | the same |
| 6.5 | Slots, ramps, steps, live values, `io.param`: **built** | 6.2, 6.3, 6.4 | none | writing-a-node-type.md#live-rows, architecture.md#the-realtime-channels |
| 6.6 | Retunes: **built** | 6.5 | feature `live` | architecture.md#restarts-and-changes-of-settings and #failures, control-port.md, output-records.md |
| 6.7 | The `gain` type | 6.6 | `types.json` gains `gain` | node-types.md |
| 6.8 | Gestures | 6.6 | feature `gestures` | control-port.md (#order, #per-client-limits-and-stopping: "one message in flight"), graph-file.md |
| 6.9 | The editor's live controls | 6.6 (switches and selects), 6.8 (faders) | none | editor.md; deletes this page and its row in [README.md](README.md) |

**6.1 Change marks and restart reasons** is built: [writing-a-node-type.md](../writing-a-node-type.md#change-marks)
and [control-port.md](../control-port.md#type-schemas) say how.

**6.2 Decimal rows and typed defaults** is built: [writing-a-node-type.md](../writing-a-node-type.md#decimal-rows),
[graph-file.md](../graph-file.md#settings-and-their-kinds), [control-port.md](../control-port.md#type-schemas) and
[editor.md](../editor.md#the-settings-form) say how.

**6.3 Indexed families** is built: [writing-a-node-type.md](../writing-a-node-type.md#indexed-families),
[graph-file.md](../graph-file.md#settings-and-their-kinds), [control-port.md](../control-port.md#type-schemas) and
[editor.md](../editor.md#the-settings-form) say how. The compact strip for a family above 8 members (10.2) is left to
step 6.9, with the other controls.

**6.4 Choice rows** is built: [writing-a-node-type.md](../writing-a-node-type.md#choice-rows),
[graph-file.md](../graph-file.md#settings-and-their-kinds), [control-port.md](../control-port.md#type-schemas) and
[editor.md](../editor.md#the-settings-form) say how.

**6.5 Slots, ramps, steps and live values** is built: [writing-a-node-type.md](../writing-a-node-type.md#live-rows) and
[architecture.md](../architecture.md#the-realtime-channels) say how.

**6.6 Retunes** is built: [architecture.md](../architecture.md#retunes), [control-port.md](../control-port.md#retunes),
[output-records.md](../output-records.md#param) and [writing-a-node-type.md](../writing-a-node-type.md#live-rows) say
how. Left to step 6.7, since no built type has a live row before `gain`: `edits.json` and `node-set.json` cases of a
retune by a real type (a retune, a mixed reshape with `retuned`, a dry run of each, a disabled node's `stored` with
`retuned`, `alsoChanged` on a shared definition; the keeper's tests hold these on a test type today). Left to the
editor: the form's live switches and selects committed at once by `node.set`, with the `alsoChanged` notice (10.2).

**6.7 The `gain` type.** 5.7 whole, and the all-type sweeps of 5.6 with `gain` as their first type. Tests
(`ear_node_gain_tests`): unity, -6 dB (factor 0.5012) and mute (0) once steady; `gainDb -90` with `trimDb.1 +24` exact
zeros; mute's and invert's ramps stepping no more than 10 ms allows; trims per channel; the clip latch, event and
trigger; `channels` 2 to 4 and back in place with channels 1 and 2 never silent and never `stopping`; with
`trimDb.3 -6`, channels 2 to 4 gives channel 3 exactly -6 dB from its first sample; the all-type tests. System test:
white noise through `gain` into a `file`; `node.set` of `gainDb` mid-recording; the WAV's level moving over 20 ms with
no gap and no `node.state` record.

**6.8 Gestures.** `Gestures` with its silence timer on the main loop's injectable clock, `GraphGateway::gesture`,
`GestureRequests` and `MainLoopSide`'s working-copy state, the reach index with defining lines from tokens, begin on
the main loop, the mailbox, read-ahead in `ear_server` and `ControlSession` with the FIFO of 16 and begin skipping it,
the session token and the hub's addressing, ending and cancelling by id from any session, releases built on the main
loop, `OfferKind`, the related-change rule for saves and edits, every row of 8.6, rounding at the end, `tick-refused`,
the docs' "one message in flight" sentence rewritten (the "no timers" one stays true). Tests: `gestures.json`; reach
indexes of a `@set` used twice, a key not given, a snippet's user, a comment added above a definition (unrelated); the
related-change cases on hand-made index pairs; `Gestures` in `ear_core_tests` with a moved clock: a silent gesture
released at `staleAfter`, re-armed by an equal tick; a release whose last tick was refused commits the last accepted
value; a release refused aborts to the committed value; two gestures on `lowHz` and `highHz` of a test type never
crossing; an end at `-6.04` (one decimal) of a gesture begun at `-6` leaving `ear.conf` unchanged and the slot's
target `-6`; `ear_server_tests`: a held `node.invoke` while ticks still move a slot and a begin is answered; an end
queued behind it, then the socket closed: the end's own value committed; the FIFO's bound; `gesture.change` with an
id refused `notification-only`; a hand edit with errors then a begin refused `file-invalid` on the main loop; system
tests (with `staleAfter` set short by a test-only flag): two clients, one holds, the other's `node.set` is
`parameter-held` with `silentMs`; the holder falls silent and is released `stale`, after which the other's `node.set`
goes; a hand edit of the held value aborts it and the WAV goes straight to the new level; a hand edit elsewhere
leaves it going; a client whose socket is reset mid-gesture, with its last tick refused, leaves its last accepted
value in `ear.conf` and the WAV, and no gesture open.

**6.9 The editor's faders and knobs.** 10.2 and 10.3, `keys.ts`, `rpc.ts`'s `notify`, notices with "Set again", the
reconnect's end, accessibility. Tests: `gesture.ts` against a fake client (every edge of 10.3's table, blur and
`visibilitychange`, a pointer press during a keyboard gesture continuing it, the reconnect ending by id); the tapers
and their inverses; Backspace and `m` on a focused knob sending nothing; the hidden range input moved by its own keys
as a gesture. Touch pointer capture on iPad Safari and Android Chrome (`lostpointercapture` after a cancelled touch)
is the one thing vitest cannot check: it joins the hand checks
([testing.md](../testing.md#what-ci-does-not-prove)), owed once at this step (decision O10). The step deletes this page and updates
[README.md](README.md).

**When `processing-nodes.md`'s types can start.** `ear_dsp` depends on nothing here and can start now. The speaker
types (`delay`, `crossover`, `eq`, `limiter`, `compressor`, `gate`, `tone`) start after step 6.6, which brings with it
everything they need: decimals (6.2), families (6.3), choices (6.4, `eq`'s `type.N` and `crossover`'s `slope`), slots
and steps (6.5) and retunes (6.6). They are built with their live rows live from the first commit. `mix` and `select`
start after 6.6 too, in parallel with 6.7 to 6.9; nothing of them waits for gestures, which only add faders. This page
recommends against `processing-nodes.md`'s interim (step 2: each live row an in-place row whose code ramps itself):
every change would be a full change in place with its deadline and records, the ramp's start value would live in a
code a swap replaces, and all of it is thrown away at 6.6.

## 12. Decisions taken

The designer's, unless marked the owner's; the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| G1 (owner) | A gesture's reach is every pair sharing the definition, all live ramped decimals of one unit; the range their intersection | The owner's answer |
| G3 (owner, R31) | Only a related change aborts a gesture; an edit from another client that changes a reach is related too | The owner's answer; the edit path was a hole |
| G4 (owner, R32) | A closed or silent session's gesture ends as released, keeping its last accepted value | The owner's answer; a refused tick was never heard as a value |
| G5 | A committed value is rounded to the reach's `decimals` | `ear.conf` stays readable |
| G13 | Begin is answered on the main loop from the reach index the keeper posts, and skips the session's FIFO | The second drag moves audio within a turn, even behind a held action |
| P6.1 | One change mark per row (`live`, `inPlace`, `restarts(why)`) replacing `bool inPlace`, required by `TypeSpec` at compile time and refused on action arguments (built for `inPlace` and `restarts`: [writing-a-node-type.md](../writing-a-node-type.md#change-marks)) | M3 made the reason part of the row; one mark cannot contradict another; a spec already asserts over its parts |
| P6.2 | Live values are a per-node vector in `NodeHost`, not part of a config | A retune then never adopts or replaces a config, so a change in flight keeps its target and an uncommitted end cannot revert the slots |
| P6.3 | A live row is one 64-bit slot whatever its kind; a boolean ramps as a weight 0 to 1 | One mechanism for D7's mute and invert, no special case; the bits are the built `rt` encoding |
| P6.4 | Two transitions, `ramp(ms)` and `step`; a crossfade is the code's, from `before`, `value` and `framesSince`, which the channels keep | Crossfades differ by type (equal-power for `select`, a filter pair for `eq`); kept in the channels they survive a swap |
| P6.5 | `choice<E>` is the one live text kind, kind `choice` on the wire; free text is never live | A closed set reads as an index on the realtime thread and must be told from hints on the wire |
| P6.6 | Slots are made for each family's count maximum, every member seeded and ramped, active or not; nothing resizes | A reshape in place binds the same channels; a member that becomes active is already right |
| P6.7 | Family members up to the maximum are accepted, above the count inactive; one spelling per member | Lowering a count never refuses a file; one member cannot appear twice |
| P6.8 | The plan decides the outcome from non-live keys and retunes the live ones beside it, whatever the outcome; a disabled node stays `stored`; a failed node is not retried by a live change | Mixed changes were undesigned; the built words keep their meaning; a fader drag must not start a failed node |
| P6.9 | A retune never supersedes a change in flight | A slot move must not restart a change |
| P6.10 | The snap at every drive, none at a swap, as an epoch in the generation word | The drive fades in anyway; a swap must keep each ramp; the steady block stays one load |
| P6.11 | Gestures only on live ramped decimals; booleans, choices and stepped rows commit by `node.set` | A toggle or a select is one click, not a drag; X5 still holds |
| P6.12 | A silent gesture is released by a main-loop timer per gesture; the control thread and the sockets are unchanged; keep-alive ticks every 10 s | Main-loop timers are built; a socket option would close slow readers and cannot see through a proxy (decision O1) |
| P6.13 | The row vocabulary needs no feature | A client reads it from the schema; the editor ships with ear (decision O9) |
| P6.14 | Any refusal of an end aborts the gesture; a release is built on the main loop from the last accepted value and is never left open | An open gesture after a refused end had no rule for its ticks, and a release has no owner to retry |
| P6.15 | A gesture id is unguessable, and holding it lets any session end or cancel it | A reconnected editor can end its own orphan at once |
| X6 | A value applied off the realtime thread is an in-place row the change version posts to its loop | Built machinery covers it |
| O1 (owner) | A gesture whose owner fell silent is released `stale` by a 30 s main-loop timer per gesture | It keeps the control thread without timers, covers proxies and frozen tabs, never closes a slow reader, and tests with a moved clock; TCP keepalive would change every connection's lifetime and miss peers behind a proxy |
| O2 (owner) | A release whose last tick was refused commits the last accepted value | It is what the audio plays, and R32 meant the value the person last heard |
| O3 (owner) | An inactive family member is not a graph-file warning | The editor greys it and says why; a warning would nag about a value kept on purpose |
| O4 (owner) | Rows that change the declared latency (`delay`'s `ms.N`, `limiter`'s `lookaheadMs`) stay in place, not live, in slice 6 | Gapless with the code's tap crossfade; latency moving on every tick would re-carry the plan each turn |
| O5 (owner) | At `SIGTERM` open gestures are dropped, not committed (G16) | A stop is nobody's release, and the keeper refuses edits once stopping |
| O6 (owner) | "has no change in place yet" is a true restart reason for `file`'s `path` and `card`'s `card`; a small follow-on slice makes them in place; reshapes with companions stay [not built](../architecture.md#not-built-yet) | Slice 6 is already large; `file` and `card` are low cost right after it |
| O7 (owner) | A live-only change leaves a disabled node `stored` and does not retry a failed node | `stored` keeps its built word; a live row cannot be why a node failed, and a drag must not start one |
| O8 (owner) | A click on a live switch or select whose value is a shared definition commits at once, then a notice names the other nodes from `alsoChanged` | One click stays one click, the notice tells the truth, and a second click undoes it |
| O9 (owner) | Kinds `decimal` and `choice` and the `indexed` member come under no feature | They are read from a schema the client asked for, enums are open, and the editor ships with ear |
| O10 (owner) | Touch pointer capture is a hand check owed once at step 6.9 ([testing.md](../testing.md#what-ci-does-not-prove)), not automated | Emulated touch would not show the iPad and Android behaviour it is about |
| O11 (owner) | Two open gestures may move rows a cross-row `check` binds (`lowHz`, `highHz`), each tick checked with the other's hand value | Two hands on an EQ is normal; the check keeps the pair valid |
| O12 (owner) | Values a node learns itself (sendspin's server volume, the AirPlay sender's volume) become live rows later, not in slice 6 | Slice 6's slots keep the main loop the one writer, and nothing here blocks a second |

## 13. Departures from the first design

| The first design | This page | Why |
|---|---|---|
| G11: no boolean setting kind; only whole numbers and decimals live | Booleans are built; they and choices can be live | M3 and D7 (mute, invert, link); the kind exists |
| `choice` a wire kind for settings | Kept: kind `choice` with `values`, as status fields have it | One spelling of one concept, and a closed set told from text hints |
| `.live(smooth(ms))` implying `.inPlace()`; `bool inPlace` | One required mark of three; `restarts(why)`; schema `restartReason`, `live.transition` | M3: a restart states why; the plan needs live keys apart from in-place ones |
| `defaulted` with a decimal default | `defaulted<V>` and `SettingValue`, the default not deduced | The built default holds only whole numbers, and built call sites pass `int` literals |
| Indexed members refused above the count | Accepted to the maximum, inactive above the count, seeded and ramped all the same | Lowering a count would refuse a file; a member raised to active must play its value |
| A reshape resizes a family's slots, survivors keep ramps | Slots sized for the maximum, never resized | `RealtimeChannels` is fixed-size and bound unchanged across a swap |
| `std::atomic<double>` slots, an arm64 unknown | 64-bit words as `rt` fields are | Already lock-free and asserted |
| Snap at start in one section, ramp from stopped values in another | Snap at every drive, an epoch `NodeHost` bumps | The block cannot tell a drive from a swap; the fade-in hides the jump |
| Retune only when every changed key is live; mixed changes unsaid | Outcome from non-live keys, live keys retuned beside it; `retuned` always on the wire | A `{channels, gainDb}` change had no owner for `gainDb` |
| A retune "keeps the new config" | Live values beside the config; no adopt on a retune | Adopting would replace a change's target and an uncommitted end would revert the slots |
| `previous()`, one block back | `before()` and `framesSince()`, kept in the channels | A swap during a crossfade would cut it |
| `gain` with no change version; `channels` restarts | `channels` reshapes in place with a change version giving a new code; `mute`, `invert.N` added | M3 and D9 |
| `processing-nodes.md`'s `eq` `type.N` in place "until text rows can be live" | Live stepped choice; the code crossfades | Choices can be live now |
| A 30 s idle timer on the control thread; 2 s keep-alive ticks | A 30 s timer per gesture on the main loop; 10 s keep-alive ticks | The control thread keeps its no-timers rule; the main loop has timers |
| Begin and end with ids queued like any request | Begin skips the FIFO; ticks are notifications only; end stays queued | A knob grabbed after a slow action must not wait for it |
| Read-ahead owned by the session | The server's read loop and the session together | The transport reads the next frame only after `done` today |
| Own echoes left out by the gateway | Session tokens and hub addressing | Sessions have no identity and the hub sends one encoding to all |
| Related saves judged for disk and `graphFile.set` only | Also for every edit from other clients that changes a reach | Such an edit aborted the gesture as `refused` |
| Defining lines from the defining statement | From its tokens | knobly's statement place owns its leading comments |
| A gesture refusal under `shared-definition` | Its own code, `reach-mismatch` | The built code means an edit's line makes other nodes |
| `NodeStatusListener`, diagnostics' `active` and `count`, `activity.ts` "new" | Built; not designed again | Slice 5 built them |
| `h.retune`, `h.outcome` | The built `h.change` extended; `h.live` | One rule, `changeOf`, already serves the harness |
| `trimDb-1` fallback spelling, knobly unknown | Dropped; `-3.5`, dotted names confirmed; a member grammar | Checked against the pinned knobly grammar |
| Gestures on live whole numbers via a fader | Gestures only on ramped decimals | A step row has no in-between value to drag through |

## 14. Open questions for the owner

None. The owner took every recommendation on 2026-11-26 ("ill take the recommendations"); section 12 records them as
decisions O1 to O12.

## 15. Rejected alternatives

| Alternative | Why not |
|---|---|
| Normalised 0..1 values on the wire (VST3) | `ear.conf` holds the row's unit, and a script writes it; the taper maps travel |
| Writing `ear.conf` on every tick | The owner chose "commit at gesture end" (Q6) |
| A parameter store beside `ear.conf` | Two sources of truth; hand edits could not win |
| Ticks or begins through the keeper | It may be writing; a laggy knob |
| Client-chosen gesture ids | Ids across sessions need a collision rule; a begin on the main loop is fast enough |
| One notification per reached key | A 64-trim gang is 1920 messages a second per client |
| Sample-accurate parameter events (CLAP, VST3) | A ramp per block is enough for faders; timestamped changes stay reserved |
| Constant-slope ramps | The length would depend on the distance; constant time keeps every move its `ms` |
| A seqlock per node for a gang | A retry loop on the realtime thread for at most one block of ramp |
| A framework crossfade transition | Each type's crossfade differs; the edge, `before` and `framesSince` are enough |
| An idle timer on the control thread | The control port's no-timers rule; the main loop has timers |
| TCP keepalive and `TCP_USER_TIMEOUT` on every socket | It changes every connection's lifetime, can reset a slow but live reader that the queue rule should close with 1008, and sees only a proxy, never the browser behind it |
| Stale by contention (a request ends a silent holder's gesture) | It needs a keeper round trip before the contender, and a cancel of the contender's gesture then returns to a value `ear.conf` no longer holds |
| Live values adopted into the config on each retune | It replaces a change's target mid-change and lets an uncommitted end revert the slots |
| A restart reason checked by the all-type tests | `TypeSpec` can assert it at compile time without touching action arguments |
| Resizing slots on a reshape | The channels are bound unchanged across a swap; the maximum is small |
| A base class with virtual `onParam` (JUCE, VST3) | A type declares; it writes only what is its own (R2) |
| Gestures on in-place rows (X5) | A drag would chop the audio into holds |
| `processing-nodes.md`'s interim of self-ramping in-place rows | Costly, discarded at 6.6 (section 11) |

## 16. Risks

- **The direct crossing** and begin from the reach index bypass the keeper; a bug breaks "the audio never stays on a
  value `ear.conf` does not hold". Every abort and every close ends by writing the slots to the committed value, last in
  the turn, and a release is never left open.
- **Read-ahead and begin out of order** change rules the transport and two docs state; the ending mark exists so a tick
  never lands after its end was read.
- **The related-change rule** can over-reach; it errs towards aborting.
- **Behaviour changes**: a `node.update` of only live rows no longer restarts or reconfigures, so a client reading
  `node.state` after such an edit waits for nothing; a live-only change no longer retries a failed node.
- **130 live members** for every `gain` configuration: one load per steady block; a moving gang loads 130 words in that
  block only.
- **Ticks run the type's `check`** on the main loop, at most once per gesture per turn.
- **Touch capture** stays unverified until 6.9's hand check (decision O10).
- **More compile-time machinery**: 14 more compile-fail cases at about 1.3 s each.
