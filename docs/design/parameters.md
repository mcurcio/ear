# Parameters: slice 6 of the node framework

Status: design; steps 6.1 (change marks and restart reasons), 6.2 (decimal rows and typed defaults), 6.3 (indexed
families), 6.4 (choice rows), 6.5 (live rows: slots, ramps, steps, live values, `io.param`) and 6.6 (retunes, feature
`live`) the `gain` type with the all-type live sweeps (step 6.7) and gestures (step 6.8, feature `gestures`) are built and in the library docs ([writing-a-node-type.md](../writing-a-node-type.md#change-marks), [#decimal-rows](../writing-a-node-type.md#decimal-rows),
[#indexed-families](../writing-a-node-type.md#indexed-families), [#choice-rows](../writing-a-node-type.md#choice-rows),
[#live-rows](../writing-a-node-type.md#live-rows), [architecture.md](../architecture.md#the-realtime-channels),
[#live-values](../architecture.md#live-values), [#retunes](../architecture.md#retunes),
[control-port.md](../control-port.md#retunes), [output-records.md](../output-records.md#param),
[node-types.md](../node-types.md#gain), [writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)),
the rest is not. Revised on 2026-11-26 against the code slices 3d (changes in place and reshapes, on
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
- **Gestures** (step 6.8): `Gestures`, the reach index, read-ahead, feature `gestures`
  ([architecture.md](../architecture.md#gestures), [graph-file.md](../graph-file.md#reaches),
  [control-port.md](../control-port.md#gestures)).

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

`gain` ([node-types.md](../node-types.md#gain)) has `channels` (default 2, a reshape in place), live ramped `gainDb` and `mute`, and the live indexed
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
rule, `SettingRow::rampMs`, the count of live members and its bound of 256; a gesture's end rounding to `decimals`
([architecture.md](../architecture.md#gestures)).

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
all-type tests' live sweep ([writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)) catches a code that never follows its slot.

`check` runs at parse, with given live values in place (`NodeConfig::checkLive`), and for every gesture tick with
every open gesture's hand value in place ([architecture.md](../architecture.md#gestures)), all built. The change version
reads live values through `liveValue`; it must not act on them, because the slots already did.

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

The all-type live sweeps, with `gain` the first type they cover, are built
([writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)).

### 5.7 The `gain` type

Built ([node-types.md](../node-types.md#gain), `src/nodes/gain/gain_spec.hpp`). Since 130 live members (1 + 1 + 64 +
64) are made for every configuration, each block's advance costs one atomic load when nothing moved
([architecture.md](../architecture.md#the-realtime-channels)).

## 6. The realtime contract

Built ([architecture.md](../architecture.md#the-realtime-channels), [#live-values](../architecture.md#live-values),
[#retunes](../architecture.md#retunes), [writing-a-node-type.md](../writing-a-node-type.md#live-rows)).

## 7. Retunes: the plan and `NodeHost`

Built ([architecture.md](../architecture.md#retunes), [control-port.md](../control-port.md#retunes),
[output-records.md](../output-records.md#param)), with an edit of a held key refused `parameter-held`
([control-port.md](../control-port.md#gestures-and-the-graph-file)).

## 8. Gestures: the keeper path

Built (step 6.8): [architecture.md](../architecture.md#gestures) (`Gestures` on the main loop, its silence timer,
releases, the stop), [graph-file.md](../graph-file.md#reaches) (the reach index, defining lines from tokens, the
related-change rule) and [control-port.md](../control-port.md#gestures) (the requests, read-ahead and the FIFO of 16,
`gesture.changed` and its addressing, the graph's per-node `gestures`, every row of the related-change table, the codes,
the order).

## 9. The wire

Built: [control-port.md](../control-port.md#gestures), [#error-codes](../control-port.md#error-codes),
[#order](../control-port.md#order), [output-records.md](../output-records.md#param) and `test/protocol/gestures.json`.

## 10. The editor

Built (step 6.9): [editor.md](../editor.md#live-numbers-faders-and-knobs) (each row's control, the box's knobs,
[#faders-and-knobs](../editor.md#faders-and-knobs) the tapers, keys and accessibility,
[#gestures-in-the-editor](../editor.md#gestures-in-the-editor) `gesture.ts`'s states and notices), with the touch
hand check owed ([testing.md](../testing.md#what-ci-does-not-prove)).

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
| 6.7 | The `gain` type: **built** | 6.6 | `types.json` gains `gain` | node-types.md |
| 6.8 | Gestures: **built** | 6.6 | feature `gestures` | architecture.md#gestures, control-port.md#gestures, graph-file.md#reaches |
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
how; `node-set.json` holds its retune cases by `gain` nodes (step 6.7). The form's live
switches and selects, committed at once by `node.set` with the `alsoChanged` notice, are built
([editor.md](../editor.md#the-settings-form)).

**6.7 The `gain` type** is built, with the all-type live sweeps, its system test and `node-set.json`'s retunes:
[node-types.md](../node-types.md#gain), [writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)
and [testing.md](../testing.md#what-each-test-program-proves) say how, and `types.json` holds its schema.

**6.8 Gestures** is built: [architecture.md](../architecture.md#gestures),
[graph-file.md](../graph-file.md#reaches), [control-port.md](../control-port.md#gestures),
[output-records.md](../output-records.md#param) and [testing.md](../testing.md#what-each-test-program-proves) say how.

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
