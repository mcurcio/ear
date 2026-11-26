# Parameters: slice 6 of the node framework

Status: design, not built. Revised on 2026-11-26 against the code slices 3d (changes in place and reshapes, on
`master` since v4.3.0) and 5 (the control plane, on `claude/slice-5`) built, and against the owner's rule M3 from
`processing-nodes.md` (on `claude/admiring-hamilton-26crw1`), whose slice-6 extensions this page folds in as general
mechanisms. Slice 6 gives a node type decimal, choice and indexed rows, rows that change **live** (ramped or stepped
on the realtime thread, with no hook called and no restart), a reason on every row that still restarts, retunes in the
plan and in `NodeHost`, gestures from the control port, the `gain` type, and the editor's faders. Section 13 lists every
place this page departs from the first design (written before 3d and 5 were built) and why.

It cites the library docs for everything built and does not repeat them. The words below are this page's:

| Word | Meaning here |
|---|---|
| **row** | A `SettingsTable` row ([writing-a-node-type.md](../writing-a-node-type.md#settings-one-table-of-typed-rows)) |
| **live row** | A row marked `.live(...)`: a change of it moves a **slot** the code reads, never calling a hook |
| **slot** | One live member's latest value, a 64-bit word in the configuration's realtime channels |
| **retune** | The plan's outcome for a change whose every changed key is live |
| **gesture** | A drag of one live decimal row from the control port: begun, moved by ticks, ended (committed) or aborted |
| **reach** | Every node and key whose value is defined at the same place in `ear.conf` as the gesture's |

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
| R32 | "Keep last value" | 2026-11-08 | A gesture whose browser disconnects or goes silent ends as released, keeping the last value ear heard (8.5) |
| M3 | "Ideally all adjustments are live. Node restarts should be justified." | 2026-11-25 | Every row of a new type is live, in place or a reshape in place; a row that restarts states why, on the wire too (4.2, 4.3) |
| — | "It's all event driven" (recorded at `src/server/control_port.cpp`: no timers on the control thread) | — | Gestures end without a control-thread timer (8.5, open question O1) |

## 2. What is built, and what this slice adds to it

Built, and used here as it is:

- **Rows and the walk**: required, optional and defaulted rows of `text`, whole number, rate and boolean, `.inPlace()`,
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

What it adds, by layer:

| Layer | Adds |
|---|---|
| `ear_core` rows | `decimal` and `choice` rules; `defaulted` for every value kind but text and rate; indexed families; one change mark per row: `.live(ramp/step)`, `.inPlace()`, `.restarts(why)` |
| `ear_core` reads | `io.param<row>()` (a `Smoothed` or a `Stepped<V>`); `s.liveValue<row>()` for `check` and the change version; `forSegments` |
| `ear_core` realtime | Slots, ramp and step state and a write generation in `RealtimeChannels`; the advance before `process`; the snap at a drive |
| `ear_core` plan and host | `NodeOutcome::retune`, `NodeChange::retuned`; `NodeHost::setLive`; retunes in every phase; `Gestures` |
| `ear_config` | Decimal reading and writing; family lookups in `declaredKind`; the reach index; `OfferKind`; the related-save rule |
| `ear_control`, `ear_server` | Schema members; `applied: "retune"` and `retuned`; `gesture.*`; per-session addressing; read-ahead; TCP keepalive |
| `web` | Decimal, choice and family fields; live toggles and selects; faders and knobs; `gesture.ts` |
| Types | `gain` (new); a restart reason on every built restart row |

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
   `invert.1`, `invert.2` as small faders and switches. `desk`'s `card` field says "restarts the node: the sound card is
   reopened". The box shows `gainDb` as a knob.
2. **Turns the knob.** Pointer down sends `gesture.begin {"name":"amp","key":"gainDb"}`; the main loop answers from the
   running version's reach index without waiting for the keeper. Each animation frame sends `gesture.change` as a
   notification, read even while another request of this socket is held; the main loop writes `amp`'s slot and the
   realtime thread ramps to it over 20 ms. Other editors see the knob move, held, at most 30 times a second.
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
drops**: the kernel's keepalive ends the socket within about 30 s; the gesture ends as released (`session-closed`),
`-3.5` is written (R32).

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

1. **Gate on features**: `live` (retunes and `retuned`, `change: "live"`), `gestures`. Decimal, choice and indexed rows
   need no feature: they are schema vocabulary under `types`, and every enum is open
   ([control-port.md](../control-port.md#protocol-and-features)).
2. **Draw from the schema.** A row with `indexed` is a family: draw members 1 to the count row's current value, and keep
   any member the node's settings hold above it, marked inactive. A `restartReason` is shown beside "restarts".
3. **Gestures**: begin, ticks as notifications, end; no echo of your own `begin` or `change`; on `aborted` stop the
   control and tell the person; an `end` with a `reason` means ear ended it for you.

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
would go stale at the first retune), read `io.param` of a row that is not live, ramp a whole number, or leave a row
with no change mark on a node type, and it does not compile or does not pass the all-type tests (5.4, 5.6).

## 4. The domain model

### 4.1 Value kinds

One vocabulary for rows, status fields, event and action arguments. The wire's names are the built ones
([control-port.md](../control-port.md#type-schemas)).

| Kind | C++ rule | Reads as | Wire `kind` | `ear.conf` | New in slice 6 |
|---|---|---|---|---|---|
| text | `setting::text<Parse>(mustBe, hints)` | the parser's `T` | `text` | a word or a quoted string | — |
| whole number | `setting::number(low, high, unit)` | `std::uint32_t` | `wholeNumber` | an integer | — |
| rate | `setting::rate(low, high)` | `SampleRate` | `wholeNumber`, `unit` `Hz` | `48khz` | — |
| boolean | `setting::boolean()` | `bool` | `flag` | `true` / `false` | live |
| **decimal** | `setting::decimal(low, high, unit, hints)` | `double` | `decimal` | a number (`-3.5`, `0.25`, `1e3`) | yes |
| **choice** | `setting::choice<Enum>({{"peak", Enum::peak}, …})` | `Enum` | `text` with `values` | a word or a quoted string | yes |

A **choice** is a closed set of names read as an enumeration, so a code can step between them live; the wire keeps the
built `text` with `values`, so today's form draws it as its select. A free text (a path, a name, a URL) names a
resource and is never live. A choice and a text with `values` hints look alike on the wire; only a choice can be live.

### 4.2 Change modes

Every row of a node type has exactly one mark, and the schema's `change` says it.

| Mode | Mark | What a change of it does | Kinds | Wire `change` |
|---|---|---|---|---|
| **live, ramped** | `.live(setting::ramp(ms))` | The slot moves; the framework ramps the code's view over `ms` | decimal, boolean (0 to 1) | `live`, `live.transition` `ramp` |
| **live, stepped** | `.live(setting::step)` | The slot moves; the code sees the new value from the next block, with `changed()` and `previous()` to make its own crossfade | decimal, whole number, boolean, choice | `live`, `live.transition` `step` |
| **in place** | `.inPlace()` | The type's change version, as built ([architecture.md](../architecture.md#changes-in-place)) | every kind | `inPlace` |
| **reshape in place** | `.inPlace().shapesPorts()` | As built ([architecture.md](../architecture.md#reshapes)); a configuration with a companion still restarts | every kind | `inPlace`, `shapes` |
| **restart** | `.restarts("why")` | The node restarts, as built | every kind | `restart`, with `restartReason` |

A live row is never read by `ports()` (it cannot: 5.2), never shapes the ports and never reaches a loop's state: a value
applied off the realtime thread is an in-place row the change version posts to its loop, as built. A live row may be
the count row of a family that does not shape the ports (`eq`'s `bands`).

The modes cover `processing-nodes.md`'s needs as one mechanism each:

| Need there | Mode here |
|---|---|
| `gainDb`, `levelDb.N`, `freqHz.N`, `q.N`, thresholds, ratios | live, ramped (decimal) |
| `mute`, `mute.N`, `invert.N` (D7) | live, ramped (boolean, 0 to 1) |
| `link`, attack, release, knee, hold | live, stepped (the envelope smooths it) |
| `select`'s `input`, `eq`'s `bands` | live, stepped (whole number); the code crossfades on `changed()` |
| `eq`'s `type.N`, `crossover`'s `slope` | live, stepped (choice); the code crossfades on `changed()`: no longer in place "until text rows can be live" |
| `delay`'s `ms.N`, `limiter`'s `lookaheadMs` (D11) | in place: they change the declared latency, which only the change version declares (open question O4) |
| `channels`, `inputs`, `key` | reshape in place |

### 4.3 The rule for choosing a row's mode

The built rule chooses between a setting, status, an event and an action
([writing-a-node-type.md](../writing-a-node-type.md#status-and-events)). For a setting, ask in order; the first yes
decides:

1. **Does it change the ports?** Reshape in place, unless the configuration has a companion on a resized bank
   (then `.restarts("<why>")`, until reshapes with companions are built: [architecture.md](../architecture.md#not-built-yet)).
2. **Does only the realtime code use it, and can it move between two blocks without touching anything else?** Live:
   ramped when a jump would click and the code can use an in-between value, stepped otherwise.
3. **Can the type apply it while the node runs** (a new receiver, a replaced sink, a new code, a declared latency)?
   In place.
4. Otherwise `.restarts("<why>")`, the why naming what cannot change while running ("shairport-sync reads its name
   only at start"). The all-type tests fail a node type with a restart row and no reason (5.6).

### 4.4 Indexed families

A family is one row declaring members `<key>.1` to `<key>.<n>`, `n` the value of its **count row**, a required or
defaulted whole-number row above it in the table. Several families may share one count row (`mix`'s `levelDb.N` and
`mute.N` by `inputs`; `eq`'s four by `bands`). Two-dimensional families are not designed (`matrix` is not proposed).

- **Members up to the count row's maximum are accepted**, whatever the count is now. A member above the current count
  is kept, inactive: the code does not read it, the editor greys it, and it is active again when the count rises. So
  lowering `channels` or `bands` never refuses a file that holds `trimDb.3`, and a band turned off keeps its settings.
  A member above the maximum is `"trimDb.65: no such member (channels is at most 64)"`. Whether an inactive member
  should also be a graph-file warning is open question O3.
- An absent member reads the family's default.
- A family's key is a directive name with a `.` (`trimDb.1`), which knobly reads as one word and ear's `writableKey`
  accepts, so the first design's fallback spelling `trimDb-1` is dropped. Step 6.3's test writes and reads one through
  the C++ binding.
- The slots of a live family are made for the count row's maximum (section 6.1), so no count change, live or by reshape,
  allocates or resizes anything.

### 4.5 Aggregates and owners

- **The node** (`NodeHost`, main loop) gains, per held live key, the hand's value while a gesture holds it, which seeds
  any configuration made meanwhile.
- **The configuration** owns the slots and their ramp and step state, inside its realtime channels, made with it and
  freed at teardown step 5 ([architecture.md](../architecture.md#the-realtime-channels)).
- **The gesture** is owned by `Gestures` (`ear_core`, main loop): an opaque id never reused, the owning session's token,
  its reach with the combined range and decimals, the committed and live values, the last `seq`, the time of its
  owner's last message about it, and whether its end has been read.
- **The reach index** is a value the keeper makes on its thread for each version of the working copy; the main loop
  holds the running version's (8.3).

## 5. The C++ SPI

Everything extends `settings_table.hpp`, `type_spec.hpp`, `lifecycle.hpp` and `node_type.hpp`. Every new constant is
`inline constexpr` in the type's named namespace, as built.

### 5.1 Rules, rows and marks

```cpp
namespace ear::setting {
enum class Taper { linear, log, db };   // how a fader maps its travel (10.3); log also ramps on ln(v)
struct DecimalHints {
	std::uint8_t decimals = 2;            // display precision, and what a gesture's end rounds to
	Taper taper = Taper::linear;          // log: low > 0; db: low <= -60 and high >= 0 (else no compile)
	bool minIsSilence = false;            // a control shows `low` as -inf
};
/// A finite real in [low, high]: a JSON number (an integer is taken). Refused: "<key>: must be a number[ of <unit>]
/// from <low> to <high>". low above high, or a bound not finite, does not compile.
consteval Rule<double> decimal(double low, double high, std::string_view unit = {}, DecimalHints hints = {});
/// A closed set of names read as E. An empty set, a repeated name or a name that is not a writable word or string does
/// not compile. Refused: "<key>: must be one of <names>".
template <class E> consteval Rule<E> choice(std::initializer_list<std::pair<std::string_view, E>> names);

/// Every kind but text and rate takes a default; one outside the rule does not compile.
template <class V> consteval Row<V, true> defaulted(std::string_view key, Rule<V> rule, V byDefault);
/// A family <key>.1 .. <key>.<n>, n the checked value of `count`, a required or defaulted whole-number row above it.
template <class V, bool A>
consteval IndexedRow<V> indexed(std::string_view key, const Row<std::uint32_t, A>& count, Rule<V> rule, V byDefault);

struct Ramp { std::uint32_t ms; };     // 1 to 1000 ms; ramp(0) does not compile: write step
consteval Ramp ramp(std::uint32_t ms);
struct Step {};
inline constexpr Step step{};
enum class Change { restart, inPlace, live };
}
```

- **Marks**, on `Row` and `IndexedRow` alike: `.live(ramp(ms))`, `.live(step)`, `.inPlace()`, `.restarts(why)`; two
  marks, or an empty reason, do not compile. `.live` needs a row that always reads a value (required, defaulted or
  indexed) of a live kind (4.2); a ramp needs a decimal or a boolean.
- **`SettingRow`** (the erased row) changes: `bool inPlace` becomes `setting::Change change` with `restartReason`;
  `byDefault` becomes `std::optional<SettingValue>`, `SettingValue = std::variant<std::uint32_t, double, bool,
  ChoiceIndex>`; it gains `transition` (`step`, or a ramp's ms), and `indexedBy` (the count row's key) with `maxMembers`;
  `SettingRule` gains `DecimalRule` and `ChoiceRule`. `SettingKind` gains `decimal`; a choice is `text`.
- **`SettingsTable`** accepts `IndexedRow`s beside `Row`s (`setting::isRow` widened), checks that no plain row is named
  `<family>.<n>` and that a family's count row is above it, and counts live members (each family's maximum): over 256
  does not compile.

### 5.2 Reading settings

| Read | Where | Gives |
|---|---|---|
| `s.get<row>()`, `s.get<family>(i)` (member `i` from 0, keys from 1) | everywhere `CheckedOf` is handed | a row that is not live: as built; on a live row it does not compile ("a live row is read through io.param, or liveValue in check and the change version") |
| `s.liveValue<row>()`, `s.liveValue<family>(i)` | `check`, the change version, a test | a live row's value in these checked settings: the newest a retune adopted (7.2) |
| `s.count<family>()` | everywhere | the family's active members: its count row's value |
| `io.param<row>()`, `io.param<family>(i)` | `process` | the realtime view (5.3) |

`ports()` reads through `get`, so it cannot read a live row, and so a retune never changes the ports. A `configure` that
copies a `liveValue` into its code holds a value that goes stale at the first retune; only review catches that, and the
all-type tests' live sweep (5.6) catches a code that never follows its slot.

`check` runs at parse as built, and on the main loop for every gesture tick and every retune, with the tick's value in
place (`NodeType::checkLive`), so a rule across rows ("`lowHz` below `highHz`") judges what will play. The change
version receives `old` and `now` with their live values; it must not act on them (the slots already did).

### 5.3 What `process` gets

```cpp
template <const auto& Spec> class Io {
public:
	// built: operator[](bank), frames(), set, raise, log, triggered (docs/writing-a-node-type.md#reporting-from-the-code)
	template <const auto& Row> auto param() const noexcept;                   // Smoothed for a ramp, Stepped<V> for a step
	template <const auto& Row> auto param(std::size_t member) const noexcept; // a family's member, from 0
};
class Smoothed {   // one ramped member over this block, in the row's own unit; a boolean's weight 0..1
public:
	bool steady() const noexcept;                         // every frame is value()
	float value() const noexcept;                         // the block's last frame
	float operator[](std::uint32_t frame) const noexcept; // O(1), computed from the ramp, no buffer
	float target() const noexcept;                        // the slot's latest
};
template <class V> class Stepped {   // one stepped member: the value for the whole block
public:
	V value() const noexcept;
	bool changed() const noexcept;     // this block's value differs from the last block's
	V previous() const noexcept;       // the last block's value (value() when unchanged)
};
/// Calls fn(begin, end) over [0, frames) in pieces of at most `piece` frames, ending each where any ramp ends; one
/// call when every view is steady. For codes that recompute coefficients per piece (eq: 32 frames).
template <class Fn, class... Views>
void forSegments(std::uint32_t frames, std::uint32_t piece, Fn&& fn, const Views&... views) noexcept;
```

The views are values the framework computed before `process` (section 6.2); `io.param` only returns them. A handle resolves to
an index when the code is prepared, as banks do.

### 5.4 Compile-time refusals

Each a case in `src/core/tests/compile_fail/`, matched against GCC 14's wording: `decimal-low-above-high`,
`decimal-bound-not-finite`, `decimal-default-outside`, `log-taper-nonpositive`, `db-taper-range`, `choice-empty`,
`choice-repeated-name`, `choice-unwritable-name`, `default-text-row`, `indexed-count-below`,
`indexed-count-not-whole`, `indexed-count-optional`, `indexed-collides-with-row`, `live-text-row`, `live-rate-row`,
`live-optional-row`, `ramp-whole-number`, `ramp-choice`, `ramp-zero`, `two-marks`, `restarts-empty-reason`,
`live-members-over-256`, `get-live-row`, `live-value-not-live`, `param-not-live`, `param-row-not-in-spec`,
`param-family-without-member`, `param-member-of-plain-row`. That is 28 cases at about 1.3 s each.

`needsCode` and `hasChannels` (`type_spec.hpp`) count live rows, so a type with live rows always has channels and a
`configure` that answers a pass-through for it breaks the built rule that only a code can serve what the spec declares.

### 5.5 The erased side

- **One lookup for a key**: `AnySettingsTable::rowOf(key)` gives the row and, for a family member, its number; it
  replaces every exact match on `row->key`. Its users: the walk; `changesInPlace(key)`, which becomes
  `changeOf(key) -> setting::Change`; `NodeType::kindOf(key)`, which the graph file's `declaredKind` calls for reading
  and writing (so `trimDb.1 0.5;` is read as a decimal, not text); and `checkLive`.
- **The walk**: a decimal is a JSON number in range; a family's members walk in the family's place in the fixed order,
  member by member; an unknown key is skipped only when `rowOf` finds nothing; each refusal carries its full key.
- **The graph file**: a decimal is read with knobly's `number()` and written as the shortest round-trip fixed text
  (`std::to_chars`, `chars_format::fixed`): `-3.5`, `-6` for `-6.0`, `0` for a negative zero. knobly 1.0.0 reads
  `-3.5`, `1e3` and `0.25` as one number word, never gives a negative zero, and refuses `-.5` and `1.` with its own
  E4004 text (checked against the pinned grammar; step 6.2 pins it through the C++ binding).
- **`NodeType::schema()`** sends the rows as 9.2 shows; `NodeConfig::liveValues()` gives each live member's checked
  value, for seeding slots and for retunes.

### 5.6 The harness and the all-type tests

`TypeHarness` (built: [writing-a-node-type.md](../writing-a-node-type.md#its-own-tests)) gains:

| Call | Does |
|---|---|
| `h.change(settings)` (built) | now also answers `retune`, and the `retuned` keys of a mixed change |
| `h.live<row>(value)`, `h.live<family>(member, value)` | moves a slot as a gesture tick does: `checkLive`, then `setLive`; a refusal fails the test |

The all-type tests ([writing-a-node-type.md](../writing-a-node-type.md#what-every-type-must-also-pass)) gain: every
restart row of a node type has a reason; every live row moved to its minimum and its maximum leaves the ports unchanged
and the node unrestarted; every ramped row ramps from its default to its maximum with every sample written.

### 5.7 The `gain` type, whole

```cpp
// src/nodes/gain/gain_spec.hpp: slice 6's proof of decimal, indexed and live rows, Smoothed and a reshape in place.
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

inline float dbToFactor(float db) noexcept { return db <= -90.0F ? 0.0F : std::exp2(db * 0.16609640474F); }

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
				return dbToFactor(gain[i] + trim[i]) * (1.0F - mute[i]) * (1.0F - 2.0F * invert[i]);
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
configuration, each block's advance costs one atomic load when nothing moved (section 6.2).

## 6. The realtime contract

### 6.1 Slots and their state live in the channels

| Channel | Direction | Mechanism | Bound | On overflow |
|---|---|---|---|---|
| A slot (in) | main loop to code | one `std::atomic<std::uint64_t>` per live member (a decimal's bits, a whole number, a boolean 0 or 1, a choice's index), written by the main loop | 256 live members per spec, each family at its count row's maximum | — (the word holds the latest) |
| The write generation (in) | main loop to code | one `std::atomic<std::uint32_t>`, bumped (release) after each batch of slot writes | one | — |
| The snap (in) | `NodeHost` to code | one `std::atomic<bool>`, set before a drive (section 6.4) | one | — |
| Ramp and step state | the code's alone | per member: current, target, frames left, increment (ramps); previous and current (steps) | as the slots | — |

They are `RealtimeChannels` members beside the triggers' counters and their seen values, made with the configuration,
kept across every stop, start, change in place and swap, freed at teardown step 5
([architecture.md](../architecture.md#the-realtime-channels)). Storing a decimal as its 64 bits, as `rt` fields already
are, needs no `std::atomic<double>`: the built `is_always_lock_free` assertion covers it on every target. The rule "a
type with no `rt` field, event, log code or trigger has no channels" gains "or live row".

### 6.2 The advance, before `process`

`ErasedSpecCode::process` advances the views before it calls the type's `process`:

1. Exchange the snap; if it was set, every member's ramp and step state is set to its slot's value (section 6.4).
2. Load the generation (acquire). If it is the one last seen and no ramp is running, the views are as last block's
   (every step unchanged): done, one load.
3. Otherwise load every live member's slot (relaxed, after the acquire), start a ramp where the target moved, step
   where a step row moved, and remember the generation.
4. Build the views `io.param` returns.

It never allocates, locks, formats or makes a system call. A cycle longer than the block size does not run the code
([engine.md](../engine.md#the-quantum-guard)), so the ramp does not advance: intended, the block writes zeros anyway.

**Members of one reach** are written by the main loop in one batch before one generation bump, so they start their
ramps in the same block, except in the rare block whose load falls inside a batch: then the rest start one block later
(at most one block of ramp apart). A seqlock is not worth its retry loop on the realtime thread.

### 6.3 The ramp's rule

- **Constant time.** When a ramped member's target differs from its ramp's, a new ramp starts at the block's first
  frame from the current value over `N = max(1, round(ms × graphFrequency / 1000))` frames; a target that moves
  mid-ramp restarts from where it is, with a full `N`. The graph rate is fixed per process
  ([graph-file.md](../graph-file.md#the-graph-block)), so `N` is fixed per row.
- Frame `k` has `v0 + (t − v0) × (k + 1) / N` for `k < N`, then `t`; computed in double, given as float; `operator[]` is
  O(1) from (v0, t, offset, N).
- **Domain**: linear in the row's unit for `linear` and `db` tapers; on `ln(v)` for `log`; a boolean from 0 to 1. A
  `minIsSilence` row ramps linearly in dB from its minimum (a curved fade-in, as a console's); a mute ramps its weight.
- **Worked vector** (step 6.5's test): 48 000 Hz, block 256, 20 ms, `N = 960`; value `-6` steady, target `-3.5` stored
  before block 0. Frame 0 `-5.9973958`; frame 255 `-5.3333333`; frame 959 (block 3, frame 191) `-3.5`; frame 960 `-3.5`.
  Blocks 0 to 3 are not steady; block 4 is, `value() == -3.5`. A target of `-12` stored before block 2 starts block 2's
  ramp from frame 511's value (`-4.6666667`), reaching `-12` at frame 1471. The harness runs blocks of 1, 7 and 256
  frames through the same vector.

### 6.4 The snap at a drive

The first block after the configuration is driven (its first drive, or a start after a stop) starts every member at its
slot's value with no ramp and every step unchanged. `NodeHost` sets the snap before each `drive()`, never before a swap,
because the block cannot tell them apart (`CycleCause::drive` is set for both:
[engine.md](../engine.md#drive-and-fade)). So slots moved while the node was stopped are taken at the next start under
the drive's own fade-in ([architecture.md](../architecture.md#fades)); a swapped-in code goes on with every ramp where
the old one left it. This settles the first design's contradiction (a snap at start in one section, a ramp from the
stopped values in another) for the snap: the fade-in already hides the jump.

### 6.5 Across the lifecycle

| Moment | The slots |
|---|---|
| `configure` from empty | made with the channels, seeded from the config's live values, or from the hand's value of a held key |
| the code drives | the snap; then `start()` |
| a retune, in any phase | written at once; with no configuration, only the config moves (it seeds the next one) |
| a change in place, a reshape in place, a swap | kept; the new code is bound to the same channels and each ramp goes on |
| a stop, a start | kept; the start snaps |
| a restart, a failure, a removal | freed with the configuration at teardown step 5; the next configuration seeds its own |

No channel is allocated, freed or resized on the realtime thread or while the code that uses it can run, and one node's
channels are never another's.

## 7. Retunes: the plan and `NodeHost`

### 7.1 The plan's rule

`changeOf` ([architecture.md](../architecture.md#restarts-and-changes-of-settings)) splits the changed keys by
`rowOf(key)`'s change: the **live** keys go to a new `NodeChange::retuned`; the outcome is decided from the others as
built (`reconfigure`, `reshape`, `restart`, `stored`, `disable`, `enable`), and a change with only live keys, the ports
equal and `disabled` unchanged is the new `NodeOutcome::retune`. A change of only live keys whose ports differ cannot
happen (5.2); should it, the plan's port comparison makes it a reshape, and the all-type tests name the type.

So a mixed change (`{"channels": 4, "trimDb.2": null}`) is the reshape its non-live keys ask for, with `trimDb.2`
retuned beside it, and the change version never sees a change it has to apply to a live key.

### 7.2 `NodeHost`, by phase

`nodeChanged` with `retuned` keys writes those slots through `NodeHost::setLive` at once, in the same turn, whatever the
outcome and phase, then handles the outcome as built:

| Phase | The retuned keys | The outcome (non-live keys) |
|---|---|---|
| `running`, `stopped` | slots move; on `retune` the instance adopts the new config (the built `adopt`, which re-points its settings) | as built |
| `reconfiguring` (`changing`, before the commit point) | slots move; the config the change will adopt becomes the newest, so the commit cannot revert them | a `retune` never supersedes the change in flight; any other outcome as built |
| `committing` | slots move; the newest config is adopted after the commit completes | as built: waits for the invoke |
| not yet rested, `failed`, `recovering`, disabled | no configuration's slots, or the ones it has; the newest config seeds the next | `retune` restarts nothing and skips no recovery; any other as built |
| a restart in flight | the old configuration's slots move (heard through its fade-out); the new one seeds from the newest config | as built |

A fallback restarts "with the newest settings" as built, which hold the retuned values. A loop's state and companions
are not told of a retune.

A key a gesture holds is never retuned by an edit: the edit is refused `parameter-held` (8.6) before the plan.

### 7.3 On the wire and on standard output

`node.updated` and `node.set`'s answer carry `applied` (`retune` for a change of live keys only) and `retuned` (the live
keys moved, when any), dry runs included (9.3). No `node.state` follows a retune. Standard output has one `param`
record per retuned node and key (9.7).

## 8. Gestures: the keeper path

### 8.1 Who does what

| Step | Thread | Owner |
|---|---|---|
| Decode `gesture.*`; read ahead; mark a gesture ending; the session token | control | `ear_control` (`decodeCommand`, `ControlSession`), `ear_server` (the read loop) |
| The reach index of each version of the working copy | keeper | `ear_config` (`GraphFileModel::reachIndex`) |
| `gesture.end`: ordering, file checks, the candidate through `updateNode` | keeper | `ear_config` |
| `gesture.begin`, `gesture.change`, `gesture.cancel` | main loop, by a direct crossing | `GraphGateway::gesture` (new), `Gestures`, the keeper's main-loop side (file state, the running index) |
| Open, move, close, abort gestures; write slots; tell listeners | main loop | `Gestures` (`ear_core`), `NodeHost::setLive` |
| Whether a save or an edit is related to each open gesture | main loop | the keeper's main-loop side, `Gestures` |
| Delivery to all but the owner, or to the owner alone | control | `ControlHub` (new addressing) |

The control thread holds only the newest pending tick per gesture and an ending mark; `Gestures` on the main loop is the
one owner.

### 8.2 A gesture, step by step

```mermaid
sequenceDiagram
  participant E as Editor
  participant C as Control thread
  participant K as Keeper thread
  participant M as Main loop
  participant R as Realtime
  K->>M: with each offer: the version's reach index
  E->>C: gesture.begin {amp, gainDb}
  C->>M: gateway.gesture(Begin{session, amp, gainDb})
  M->>M: stopping, file state, node, row, reach, holds: g4 opened
  M-->>E: (others) gesture.changed begin; (owner) reply {g4, -6, reach, range}
  loop each animation frame
    E->>C: gesture.change {g4, seq, v} (no id, read ahead)
    C->>M: newest pending tick
    M->>R: slots = v, generation + 1
  end
  E->>C: gesture.end {g4, -3.5}
  C->>C: mark g4 ending
  C->>K: EndGesture{session, g4, -3.5}
  K->>K: candidate: gainDb -6 -> -3.5 at its definition
  K->>M: offer (gestureEnd g4)
  M->>M: plan: retune; commit; Gestures.close(g4)
  M-->>E: graphFile.changed, node.updated, gesture.changed end, reply {-3.5}
  K->>K: write ear.conf when free
```

- **Begin** (main loop, no keeper), in order: `stopping`; `file-invalid` or `file-refused` from the main-loop side's
  running version and recorded refusal (both built); `unknown-node`; `not-live` (the row is not a live ramped decimal:
  `data.change` says what it is, so a client sends `node.set` for a stepped one); the reach from the running index;
  `shared-definition` unless every reached row is a live ramped decimal of one unit with ranges that meet; a held pair
  is `parameter-held`, unless its gesture is stale (8.5). Then `Gestures` opens it, tells the others `begin` and
  answers. Nothing is written.
- **Ticks** (main loop): `Gestures::move` checks the owner, drops a stale `seq` or an equal value, checks the value
  against every reached row and the type's `check` (`checkLive`; a refusal is `tick-refused` to the owner, at most once
  a second), writes every reached slot through `setLive` (for a node with no configuration, its held value), bumps each
  node's generation once, and arms the tick for `gesture.changed` `change` (at most 30 a second, never to the owner).
- **End** (keeper): the candidate is the node's settings with `key` at the value rounded to the reach's `decimals`
  (half away from zero on the shortest decimal text; `-0` is `0`), through the built `updateNode`, which edits the value
  at its definition, so every reached node changes in one commit. On the main loop the candidate's changed pairs must be
  the reach (a guard), the gesture's own keys pass `parameter-held`, the plan commits (a `retune`), then `Gestures::close`
  writes each reached slot the committed value, tells `end`, and the reply carries it. An end refused for its value
  leaves the gesture open; any other refusal aborts it (`refused`). An end equal to the committed value writes nothing and
  closes it.
- **Cancel, removal** abort on the main loop: every reached slot returns to the committed value.

### 8.3 The reach index and defining lines

With every offer (an accepted edit's candidate, a disk read, `graphFile.set`) the keeper sends the version's
`ReachIndex`, made only when the text changed: for each node and live key the text gives, the definition (knobly's
`expanded_value::definition()`, or `written()` for an override, as `updateNode` already falls back), its **defining
lines** (a 64-bit hash of the text of the lines from the defining statement's first token to its `;`, with their
range), every pair whose expanded value has that definition, and the value. A text with errors has none.

knobly's `statement().place()` includes the comments a statement owns, the comment lines above it among them, so the
defining lines are taken from the statement's tokens, not its place; otherwise a comment added above `@set $trim 0;`
would be related. Through a `@snippet`, `definition()` gives the chain's end (`@set $trim 0`), not the snippet's line,
so a snippet's user and a direct user of `$trim` share one reach (both checked against the pinned knobly's JS build;
step 6.8 pins them through the C++ binding).

`node.set` on a shared definition already moves every user of it (built: `updateNode` edits the definition), which is
G1's reach; its `param` records name every user (9.7).

### 8.4 Reading ahead and the direct crossing

Today a connection reads its next message only once every reply to the last is queued
([control-port.md](../control-port.md#per-client-limits-and-stopping)); a tick would then wait behind a held
`node.invoke` (up to 10 s). So, in `ear_server`'s read loop and `ControlSession` together:

- **The transport reads every frame as it arrives.** A message with an id, a batch and every other notification go
  into the session's FIFO and run one at a time, in order, exactly as today; the FIFO holds 16, and at 16 the socket is
  not read until one is answered (one `async_read` outstanding, as Beast requires).
- **A `gesture.change` or `gesture.cancel` notification skips the FIFO**: checked for shape on the control thread and
  handed to `GraphGateway::gesture` at once. It has no reply, so no reply's order changes, and its id came from a begin's
  reply, so its begin already ran. A `gesture.end` or `gesture.cancel` read for an id marks it ending at once: later
  ticks for it are dropped there. In a batch both are refused `not-in-batch`.
- **Coalescing.** The gateway keeps the newest pending tick per gesture in a mailbox shared with the main loop (one
  mutex, held for a copy) and posts one task if none was pending: a thousand ticks a second cost at most one task per
  main-loop turn. The main loop's executor is FIFO, so a tick posted before an end was read lands before the end's
  offer.
- **Addressing.** Each session gets an opaque token from the hub when it attaches; it crosses to the main loop with
  every gesture command and comes back on gesture events. `ControlHub` still encodes each event once, and sends it to
  every graph-subscribed session but the owner (`begin`, `change`) or to the owner alone (`tick-refused`, and the
  replies).
- **A closed session**: its pending tick is handed on, its open gestures end as released (8.5), and its queued requests
  with an id still run in order, their replies dropped, as today.

### 8.5 Ending without a timer

The first design ended a silent gesture with a 30 s idle timer on the control thread and had the editor send keep-alive
ticks every 2 s. The control port has no timers (the owner's "It's all event driven"), so this page ends gestures by
events only (open question O1 asks the owner to confirm):

- **A dead peer closes its socket.** The server sets TCP keepalive on each accepted socket (idle 10 s, interval 5 s,
  3 probes) and `TCP_USER_TIMEOUT` 30 s, so the kernel ends a connection whose peer vanished within about 30 s, whether
  ear is sending to it or not. Its gestures then end as released, reason `session-closed` (R32). ear runs no timer: the
  kernel's keepalive is a socket option set once.
- **A silent but connected owner loses to contention.** `Gestures` keeps the main loop's time of each gesture's last
  message from its owner. When a `gesture.begin`, `node.set` or `node.update` from anyone else would be refused
  `parameter-held` and that gesture has been silent for 30 s, it is first ended as released (reason `stale`, R32), then
  the request goes ahead. A comparison at an event, not a timer.
- No keep-alive ticks: a hand that holds still sends nothing and keeps its gesture until someone else wants the key for
  30 s.
- **`SIGTERM`**: later begins and ends get `stopping`; open gestures are not committed (clients closed with 1001), as
  the first design had it (G16; open question O5).

### 8.6 Refusals, collisions and related saves

| Lands during a gesture | What happens |
|---|---|
| A hand edit or `graphFile.set` **related** to it | aborted (`hand-edit` or `file-saved`) on the main loop before the offer's plan; the file offered as built; last in the turn each reached slot is set to its node's running value, so the audio ramps once |
| Another client's `node.set` or `node.update` that changes its reach without touching a held key (giving another node `trimDb.1 $trim`) | aborted (`edited`), before the plan; the first design missed this path |
| A save or edit **not related** | nothing: no `gesture.changed`; its index entries replaced by the new version's |
| A hand edit with errors, or one the graph refuses | every open gesture aborted: no end could commit onto it |
| `node.set` or `node.update` changing a held key | refused `parameter-held` (unless the gesture is stale: 8.5) |
| `node.set` or `node.update` changing other keys of a reached node | applied, whatever its outcome: a change in place keeps the slots; a restart seeds the new configuration from the hand's value |
| A reshape removing or inactivating a reached member | applied; aborted (`key-removed`) before the commit's events |
| The reached node fails, restarts, stops, starts or is disabled | nothing: ticks move the hand's value, which seeds its next configuration |
| `node.remove`, or a replacement of a reached node | applied; aborted (`node-removed`) |
| The owning session closes | ended as released (`session-closed`) |
| A dry run | never judged: `Offer::dryRun` offers are skipped |

The keeper's main-loop side learns what kind of offer it holds from a new internal `OfferKind` (`disk`, `set`, `edit`,
`gestureEnd{id}`), beside the built `Offer`'s `cause` (`control::FileChangeCause`, which stays the wire's).

**When a change is related** (R31), judged per open gesture from the running index and the new version's, for any pair of
its reach: (1) its defining lines' text changed; (2) its value changed after expansion (a chain's base changed
elsewhere); (3) its reach changed (defined elsewhere now, or the pairs sharing its definition differ); (4) it is gone (the
node removed, renamed, replaced, the key undeclared or inactive); (5) the new text has errors or is refused. A key the
file did not give has no defining lines: the change is related when the new text gives the key or its node's `node`
line changes. One comparison per open gesture per offer.

## 9. The wire

Every change below goes in one commit with `docs/control-port.md`, `test/protocol/`, `src/control/` and
`web/src/protocol.ts` ([control-port.md](../control-port.md#changing-the-wire)).

### 9.1 Features

| Feature | Step | Adds |
|---|---|---|
| — (under `types`) | 6.1 to 6.4 | rows' `restartReason`, kind `decimal`, `decimals`, `taper`, `minIsSilence`, typed `default`s, `indexed` |
| `live` | 6.6 | `change: "live"` with `live`, `applied: "retune"`, `retuned` |
| `gestures` | 6.8 | `gesture.begin`, `change`, `end`, `cancel`; `gesture.changed`; the graph's per-node `gestures`; the new codes |

**An editor older than step 6.3** draws a family as one field it cannot fill (an Apply of it is `unknown key`). The
editor ships in the image with ear and is served by it, so only third-party clients can be older, and those gate on
`live` before drawing a live row; a family before 6.6 is a feature-free addition all the same, which this page accepts.

### 9.2 Type schemas

A row gains, as it applies:

| Member | What |
|---|---|
| `kind` | gains `decimal` (a JSON number) |
| `change` | gains `live`. Open, as built |
| `live` | `{"transition": "ramp", "ms": 20}` or `{"transition": "step"}` |
| `restartReason` | a restart row's why, one line |
| `default` | now of the row's kind: a decimal, a flag, or a choice's name |
| `min`, `max`, `unit`, `decimals`, `taper`, `minIsSilence` | a decimal's, as a decimal status field's are named ([control-port.md](../control-port.md#type-schemas)); `taper` is `linear`, `log` or `db` |
| `indexed` | `{"by": "<count key>", "from": 1, "max": <the count row's max>}`: the row is a family |

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
{"key":"card","kind":"text","required":true,"change":"restart","restartReason":"the sound card is opened only at configure","mustBe":"…"}
```

### 9.3 `node.set`, `node.update` and `node.updated`

`applied` gains `retune`; the answer and `node.updated` gain `retuned` (the live keys moved, family members by their
full key) when there are any. Both refuse `parameter-held` from feature `gestures` on.

```json
{"jsonrpc":"2.0","id":11,"method":"node.set","params":{"name":"amp","settings":{"gainDb":-12},"dryRun":true}}
{"jsonrpc":"2.0","id":11,"result":{"applied":"retune","retuned":["gainDb"]}}
{"jsonrpc":"2.0","method":"node.updated","params":{"name":"amp","type":"gain","settings":{"gainDb":-12},"applied":"retune","retuned":["gainDb"]}}
```

`test/protocol/unknown-values.json` already holds an `applied` of `retune` with an invented phase, as an unknown value a
client must survive; the fixture's description is rewritten to say `retune` is known from `live` on, and its unknown
value becomes another.

### 9.4 Gesture requests

| Method | Params | Result | Refusals (`data.code`) |
|---|---|---|---|
| `gesture.begin` | `{"name", "key"}` | `{"gesture", "value", "reach", "min", "max", "decimals"}` | `unknown-node`, `not-live`, `shared-definition`, `parameter-held`, `file-invalid`, `file-refused`, `stopping` |
| `gesture.change` | `{"gesture", "seq", "value"}` | `{}` when sent with an id | `unknown-gesture`, `invalid-settings`, `not-in-batch` |
| `gesture.end` | `{"gesture", "value"?}` | `{"value"}`, what was committed | `unknown-gesture`, `invalid-settings` (the gesture stays open), `stopping`, the desired graph's (it is aborted) |
| `gesture.cancel` | `{"gesture"}` | `{}` when sent with an id | `unknown-gesture`, `not-in-batch` |

```json
{"jsonrpc":"2.0","id":9,"method":"gesture.begin","params":{"name":"amp2","key":"trimDb.1"}}
{"jsonrpc":"2.0","id":9,"result":{"gesture":"g5","value":0,"reach":[{"name":"amp2","key":"trimDb.1"},{"name":"amp2","key":"trimDb.2"}],"min":-24,"max":24,"decimals":1}}
{"jsonrpc":"2.0","method":"gesture.change","params":{"gesture":"g5","seq":1,"value":1.5}}
{"jsonrpc":"2.0","id":10,"method":"gesture.end","params":{"gesture":"g5","value":1.5}}
{"jsonrpc":"2.0","id":10,"result":{"value":1.5}}
```

`seq` counts from 1 per gesture; a tick not above the last taken is dropped. A key the type declares and the file does
not give reaches only its node, at the row's default. A node with no configuration can be held.

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
- The owner gets `end`, `aborted` and `tick-refused`; the others `begin`, `change`, `end`, `aborted`.
- `end`'s `reason`, when ear ended it for its owner: `session-closed`, `stale`. `aborted`'s `reason`: `hand-edit`,
  `file-saved`, `edited`, `cancelled`, `refused`, `node-removed`, `key-removed`, with `value` (what the slots returned
  to) except after `hand-edit`, `file-saved` and `edited`, whose `node.updated` gives it. Open enums.
- **Folding**: `begin` adds `{gesture, key, value}` to each reached node's `gestures`; `change` sets `value`; `end` and
  `aborted` remove it; `tick-refused` changes nothing.
- **Order**, added to the built list ([control-port.md](../control-port.md#order)): a begin's `gesture.changed` before
  its reply, in one turn; an end's `graphFile.changed`, `node.updated` (`retune`), `gesture.changed` `end`, then the
  reply; an abort by a related change's `gesture.changed` `aborted` before that change's `graphFile.changed` and events,
  the slots settling last in the turn.

### 9.6 Error codes

All -32000 with `data.code`, each a case in `test/protocol/errors.json`.

| `data.code` | When | `data` also has |
|---|---|---|
| `invalid-settings` (built) | also a tick's or an end's value | `key` (a member's full key), `detail` |
| `parameter-held` (new) | `node.set`, `node.update`, `gesture.begin` would change a key an open gesture holds | `gesture`, `key` |
| `not-live` (new) | `gesture.begin` on a key that is not a live ramped decimal | `key`, `change` (`live` for a stepped row: send `node.set`) |
| `shared-definition` (extended) | `gesture.begin`: the reach holds a row that is not a live ramped decimal, another unit, or ranges that do not meet | `place` (built), `with` (new: the pairs) |
| `file-refused` (new) | `gesture.begin` while the graph refused the working copy's last hand edit | |
| `unknown-gesture` (new) | no open gesture of this session by that id | |
| `not-in-batch` (new) | `gesture.change` or `gesture.cancel` in a batch | |

### 9.7 Standard output: `param`

A new kind, with `v`, `seq`, `t`, `kind`, its fields and `msg` ([output-records.md](../output-records.md#the-envelope)):

```json
{"v":1,"seq":124,"t":"2026-11-08T10:03:00.500001Z","kind":"param","node":"amp","key":"gainDb","from":-6,"to":-3.5,"cause":"gesture","gesture":"g4","msg":"node amp gainDb -6 -> -3.5"}
{"v":1,"seq":125,"t":"2026-11-08T10:03:40.000300Z","kind":"param","node":"amp","key":"gainDb","from":-4.25,"to":-6,"cause":"aborted","gesture":"g4","reason":"cancelled","msg":"node amp gainDb gesture g4 aborted (cancelled): back to -6"}
```

One per reached node and retuned key at each committed live change (`cause`: `gesture`, `set` for `node.set` and
`node.update`, `disk` for a hand edit or `graphFile.set`; a mixed change's live keys too), and per abort (`cause:
aborted`, `reason`, `to` absent after a related change). An end ear made for its owner is `cause: gesture` with `reason`.

### 9.8 Fixtures

| File | Covers |
|---|---|
| `types.json` | `gain`'s schema whole; every built restart row's `restartReason` |
| `edits.json`, `node-set.json` | a retune, a mixed reshape with `retuned`, a dry run of each |
| `gestures.json` (new) | begin with no echo to the owner and `begin` to another first; ticks (one stale); end with its order; reaches of two keys and of 64; `shared-definition` with `with`; `parameter-held`; a stale holder ended by contention; a begin on a failed node; cancel; `tick-refused` to the owner only; an end refused `invalid-settings` left open; aborts by a related hand edit, `graphFile.set` and edit; an unrelated hand edit left open; `session-closed`; `unknown-gesture` after an abort; `not-in-batch` |
| `unknown-values.json`, `errors.json`, `snapshot.json`, `output-records.json` | this page's members, codes and records |

## 10. The editor

### 10.1 Stores

| Module | Gains | Tested by |
|---|---|---|
| `protocol.ts` | section 9's types; `ControlErrorCode`'s new codes | typecheck |
| `graph.ts` | each node's `gestures`; `retuned` on `node.updated`; `gesture.changed` | vitest, every fixture |
| `form.ts` | the `decimal` control and its `problemOf` check; families expanded in `fieldsOf` with inactive members; live rows committed at once; `asksFirst` passing `retune`, `confirmWords` for it; the "restarts: why" hint | vitest |
| `rpc.ts` | `notify()`: a message with no id | vitest |
| `gesture.ts` (new, pure) | the session's own gestures as a state machine (10.3), the taper mapping | vitest, a fake client |
| `keys.ts` | a focused `role="slider"` or anything in `.nokey` counts as a text target, so `f`, `l`, `m`, `a`, `?` and Escape do not fire the editor's shortcuts on a knob | vitest |

Nothing in `web/` names a node type. Without `gestures`, live decimals are number fields committed by `node.set`;
without `live`, every row is drawn as today.

### 10.2 How each row renders

| Row | Control | Commits |
|---|---|---|
| decimal, not live | a number field with `decimals` and the unit | Apply |
| live ramped decimal | a fader mapped by `taper` with a number field beside it | a gesture; the field by `node.set` on Enter |
| live boolean | a switch | `node.set` at once |
| live choice | a select | `node.set` at once |
| live whole number, live stepped decimal | steppers and a number field | `node.set` on Enter or a step |
| family | one control per member to the count row's value; members above it greyed ("kept; not used while channels is 2"); above 8 members, a compact strip behind a disclosure | as its kind |
| restart row | as today, its hint "restarts the node: <restartReason>" | Apply, which asks first |

The box shows a knob for each live ramped decimal that is not in a family when the type has one or two (`gain`:
`gainDb`). Live controls follow the server except one the operator holds; they are disabled while the graph file has
errors or was refused and while the connection is not open. A live change from the form does not touch the draft of
the other fields.

### 10.3 `gesture.ts`

| State | Input | Sends | Next |
|---|---|---|---|
| idle | press, or first key | `gesture.begin` | beginning (intent: hold) |
| beginning | move | nothing (keeps the newest) | beginning |
| beginning | release / Escape | nothing yet | beginning (intent: end / cancel) |
| beginning | reply, intent hold | the kept value as tick 1 | active |
| beginning | reply, intent end | `gesture.end` with the shown value | ending |
| beginning | reply, intent cancel | `gesture.cancel` | idle |
| beginning | refusal | — | idle; snaps back; a notice names the code (`not-live` with `change` `live`: the field's `node.set` instead) |
| active | move | `gesture.change` (`notify`), at most one per animation frame | active |
| active | release, `pointercancel` or `lostpointercapture` | `gesture.end` with the shown value | ending |
| active | Escape | `gesture.cancel` | idle |
| ending | reply | — | idle; shows the committed value |
| ending | refusal `invalid-settings` | `gesture.cancel` | idle; notice |
| any | `aborted` for its id | — | aborted until release, then idle; notice |
| any | `end` with a `reason` | — | idle; the committed value; notice |
| any | connection lost | — | frozen until release; then the snapshot's value and a notice |

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
[#phones-and-touch](../editor.md#phones-and-touch)). Faders and knobs are `role="slider"` with `aria-valuemin`,
`aria-valuemax`, `aria-valuenow`, `aria-valuetext` ("-3.5 dB", "-inf dB"); held by another editor, `aria-disabled` with
the reason; a polite live region announces aborts.

Notices for the owner's own gesture: `hand-edit` "ear.conf was edited by hand and changed amp gainDb while you held it;
it now says -10." [Set -3.5 again]; `file-saved` the same from the text pane; `edited` "Another editor changed what
shares amp2 trimDb.1 while you held it."; `node-removed`; `key-removed` "amp2 now has 2 channels, so trimDb.3, which you
held, is not used."; `refused` the end's refusal; `stale` "Another editor took amp gainDb after you held it still for
30 s; ear kept -3.5."; a socket lost mid-drag "Connection lost while you held amp gainDb; ear keeps the last value it
heard."

## 11. Order of work

Steps are named "step 6.n" and sections of this page "section 6.n". Each step builds, keeps every test green and changes the wire, the docs and the fixtures in the same commit where it
touches them. Steps 6.1 to 6.4 change no audio path.

| Step | What | Needs | Wire |
|---|---|---|---|
| 6.1 | Change marks and restart reasons | slice 5 | `restartReason` |
| 6.2 | Decimal rows, typed defaults | 6.1 | kind `decimal` |
| 6.3 | Indexed families | 6.2 | `indexed` |
| 6.4 | Choice rows | 6.1 | none (text with `values`) |
| 6.5 | Slots, ramps, steps, `io.param` | 6.2 | none |
| 6.6 | Retunes | 6.5 | feature `live` |
| 6.7 | The `gain` type | 6.3, 6.6 | none |
| 6.8 | Gestures | 6.6 | feature `gestures` |
| 6.9 | The editor's live controls | 6.6 (switches and selects), 6.8 (faders) | none |

**6.1 Change marks and restart reasons.** `setting::Change`, `.restarts(why)`, `SettingRow::change` and `restartReason`
replacing `inPlace`, the schema's `restartReason`, the all-type test that every restart row has one; each built
restart row given its why: `file` `path` ("the recording is opened only at configure"), `card` `card`, `playback` and
`capture`, `rtp` `channels` ("a companion on a resized bank cannot be replaced in place yet"), `airplay` `name` and
`airplay2`. Tests: `types.json`; compile-fail `two-marks`, `restarts-empty-reason`; the all-type test failing a test
type with no reason.

**6.2 Decimal rows and typed defaults.** `decimal`, `DecimalHints`, `defaulted<V>` for decimal and boolean,
`SettingValue`, `SettingKind::decimal`, the walk, knobly's `number()` reader and the fixed writer, `NodeType::kindOf`,
the schema's decimal members and typed `default`, the form's decimal field. Tests: range, an integer taken; compile-fail
`decimal-*`, `log-taper-nonpositive`, `db-taper-range`, `default-text-row`; read `-3.5`, `-6`, `0.25`, `1e3`, a `@set`
value; `-.5` and `1.` refused with knobly's E4004; write and read back; `-0.04` at one decimal written `0`; a
`node.update` from today's editor round-tripping `-3.5`.

**6.3 Indexed families.** `IndexedRow`, `rowOf` in the walk, `changeOf(key)`, `kindOf` and `checkLive`'s path,
`get<family>(i)`, `count<family>()`, inactive members, the schema's `indexed`, the form's member fields. Tests: walk
order with a family; members above the count accepted and inactive, above the maximum refused; unknown keys beside a
family; a member's change planned by its family's mark (a restart family restarts, an in-place one reconfigures); the
graph file reading `trimDb.1 0.5;` as a decimal and writing it back; compile-fail `indexed-*`.

**6.4 Choice rows.** `choice<E>`, `ChoiceRule`, its schema as text with `values`, its default by name. Tests: a name
refused with its list; compile-fail `choice-*`; the form's select.

**6.5 Slots, ramps and steps** (no retune yet: a change of a live row is still taken as an in-place change, or a restart
for a type with no change version). `.live(ramp/step)`, the slots, generation, snap and state in `RealtimeChannels`,
the advance in `ErasedSpecCode::process`, `Io::param`, `Smoothed`, `Stepped`, `forSegments`, `liveValue`, `get`
refused on live rows, `hasChannels` and `needsCode` counting live rows, `NodeHost::setLive`, seeding at configure, the
snap before each drive, `h.live`. Tests in `ear_core_tests` with a test type: section 6.3's vector frame for frame at blocks of
1, 7 and 256; a target moved mid-ramp; steady blocks cost one load (a counter in the test build); the snap at a first
drive and at a start after a stop, none at a swap; a swap keeping a ramp in flight; a log-taper ramp; a boolean ramp;
a step's `changed()` and `previous()`; no allocation in the advance (the built counting allocator); compile-fail
`live-*`, `ramp-*`, `get-live-row`, `live-value-not-live`, `param-*`, `live-members-over-256`.

**6.6 Retunes** (changes the audio path: a change of live keys ramps instead of reconfiguring). `NodeOutcome::retune`,
`NodeChange::retuned`, `changeOf`'s split, every `NodeOutcome` switch (`events.cpp`, `commands.cpp`,
`node_lifecycle.cpp`, `lifecycle_wire.hpp`, `events.hpp`, `protocol.ts`, `desired_graph_test.cpp`), `NodeHost` by 7.2's
table with `adopt`, `applied: "retune"`, `retuned`, feature `live`, the `param` record, `checkLive` on retunes, the
form's live switches and selects by `node.set`, `asksFirst` passing `retune`. Tests: a retune plan; a mixed reconfigure
and a mixed reshape with `retuned`; a retune during `changing` not superseding it and kept at its commit; a retune
during `committing`; a fallback restart keeping the retuned value; a retune of a failed and of a disabled node; slots
seeded on restart; `edits.json`, `node-set.json`, `unknown-values.json`'s rewrite.

**6.7 The `gain` type.** 5.7 whole. Tests (`ear_node_gain_tests`): unity, -6 dB (factor 0.5012) and mute (0) once
steady; mute's and invert's ramps stepping no more than 10 ms allows; trims per channel; the clip latch, event and
trigger; `channels` 2 to 4 and back in place with channels 1 and 2 never silent and never `stopping`; the all-type
tests. System test: white noise through `gain` into a `file`; `node.set` of `gainDb` mid-recording; the WAV's level
moving over 20 ms with no gap and no `node.state` record.

**6.8 Gestures.** `Gestures`, `GraphGateway::gesture`, the reach index with defining lines from tokens, begin on the
main loop, the mailbox, read-ahead in `ear_server` and `ControlSession` with the FIFO of 16, the session token and the
hub's addressing, TCP keepalive and `TCP_USER_TIMEOUT`, `stale` by contention, `OfferKind`, the related-change rule for
saves and edits, every abort of 8.6, rounding at the end, `tick-refused`, the docs' "one message in flight" and "no
timers" sentences rewritten. Tests: `gestures.json`; reach indexes of a `@set` used twice, a key not given, a
snippet's user, a comment added above a definition (unrelated); the related-change cases on hand-made index pairs;
`ear_server_tests` (a held `node.invoke` while ticks still move a slot; an end queued behind it; a closed socket ending
its gestures behind its earlier requests; the FIFO's bound; keepalive set on accepted sockets); begin answered while
the keeper writes; system tests: two clients, one holds, the other's `node.set` is `parameter-held`, then after 30 s
of silence takes it (`stale`); a hand edit of the held value aborts it and the WAV goes straight to the new level; a
hand edit elsewhere leaves it going; a client whose socket is reset mid-gesture leaves its last value in `ear.conf` and
the WAV.

**6.9 The editor's faders and knobs.** 10.2 and 10.3, `keys.ts`, `rpc.ts`'s `notify`, notices with "Set again",
accessibility. Tests: `gesture.ts` against a fake client (every edge of 10.3's table); the tapers and their inverses;
Backspace and `m` on a focused knob sending nothing; a hand check of touch capture on iPad Safari and Android Chrome
(`lostpointercapture` after a cancelled touch), the one check vitest cannot make.

**When `processing-nodes.md`'s types can start.** `ear_dsp` depends on nothing here and can start now. The speaker
types (`delay`, `crossover`, `eq`, `limiter`, `compressor`, `gate`, `tone`) start after 6.6, built with their live rows
live from the first commit: their in-place rows (`delay`'s `ms.N`, `limiter`'s `lookaheadMs`) and reshapes need only
6.3. `mix` and `select` start after 6.6 too, in parallel with 6.7 to 6.9; nothing of them waits for gestures, which
only add faders. This page recommends against `processing-nodes.md`'s interim (step 2: each live row an in-place row
whose code ramps itself): every change would be a full change in place with its deadline and records, the ramp's
start value would live in a code a swap replaces, and all of it is thrown away at 6.6, which is four small steps after
the row kinds it needs anyway.

## 12. Decisions taken

The designer's, unless marked the owner's; the owner can overturn any.

| # | Decision | Why |
|---|---|---|
| G1 (owner) | A gesture's reach is every pair sharing the definition, all live ramped decimals of one unit; the range their intersection | The owner's answer |
| G3 (owner, R31) | Only a related save aborts a gesture; an edit that changes a reach is related too | The owner's answer; the edit path was a hole |
| G4 (owner, R32) | A closed or silent session's gesture ends as released, keeping its last value | The owner's answer |
| G5 | A committed value is rounded to the reach's `decimals` | `ear.conf` stays readable |
| G13 | Begin is answered on the main loop from the reach index the keeper posts | The second drag moves audio within a turn |
| P6.1 | One change mark per row (`live`, `inPlace`, `restarts(why)`) replacing `bool inPlace` | M3 made the reason part of the row; one mark cannot contradict another |
| P6.2 | Restart reasons are enforced by the all-type tests, not at compile time | Action arguments are rows of the same kind and never change (open question O2) |
| P6.3 | A live row is one 64-bit slot whatever its kind; a boolean ramps as a weight 0 to 1 | One mechanism for D7's mute and invert, no special case; the bits are the built `rt` encoding |
| P6.4 | Two transitions, `ramp(ms)` and `step`; a crossfade is the code's, on `changed()` | Crossfades differ by type (equal-power for `select`, a filter pair for `eq`); the framework owns only the edge and the previous value, which survive swaps |
| P6.5 | `choice<E>` is the one live text kind; free text is never live | A closed set reads as an index on the realtime thread; a path or name is a resource |
| P6.6 | Slots are made for each family's count maximum; nothing resizes | A reshape in place binds the same channels; a live count (`bands`) needs no allocation |
| P6.7 | Family members up to the maximum are accepted, above the count inactive | Lowering a count never refuses a file; a band turned off keeps its settings |
| P6.8 | The plan decides the outcome from non-live keys and retunes the live ones beside it, whatever the outcome | Mixed changes were undesigned; the change version never sees a live key to apply |
| P6.9 | A retune never supersedes a change in flight; the commit adopts the newest config | A slot move must not restart a change, and the commit must not revert it |
| P6.10 | The snap at every drive, none at a swap | The drive fades in anyway; a swap must keep each ramp |
| P6.11 | Gestures only on live ramped decimals; booleans, choices and stepped rows commit by `node.set` | A toggle or a select is one click, not a drag; X5 (no faders on in-place rows) still holds |
| P6.12 | No control-thread timer: kernel keepalive for dead peers, contention after 30 s of silence for live ones, no keep-alive ticks | The owner's "It's all event driven" (open question O1) |
| P6.13 | Decimal, choice and indexed rows need no feature | They are schema vocabulary a client reads from the schema; the editor ships with ear |
| X6 | A value applied off the realtime thread is an in-place row the change version posts to its loop | Built machinery covers it |

## 13. Departures from the first design

| The first design | This page | Why |
|---|---|---|
| G11: no boolean setting kind; only whole numbers and decimals live | Booleans are built; they and choices can be live | M3 and D7 (mute, invert, link); the kind exists |
| "Value kinds `choice` for settings" as its own wire kind | A choice is `text` with `values` on the wire | Today's form draws it already; settings gain only `decimal` |
| `.live(smooth(ms))` implying `.inPlace()`; `bool inPlace` | One mark of three; `restarts(why)`; schema `restartReason`, `live.transition` | M3: a restart states why; the plan needs live keys apart from in-place ones |
| `defaulted` with a decimal default | `defaulted<V>` and `SettingValue` designed, as the built default holds only whole numbers | The code allows nothing else today |
| Indexed members refused above the count | Accepted to the maximum, inactive above the count | Lowering a count would refuse a file |
| A reshape resizes a family's slots, survivors keep ramps | Slots sized for the maximum, never resized | `RealtimeChannels` is fixed-size and bound unchanged across a swap |
| `std::atomic<double>` slots, an arm64 [U] | 64-bit words as `rt` fields are | Already lock-free and asserted; the [U] goes |
| Snap at start in 5.4, ramp from stopped values in 6.6 | Snap at every drive; `NodeHost` sets it | The block cannot tell a drive from a swap; the fade-in hides the jump |
| Retune only when every changed key is live; mixed changes unsaid | Outcome from non-live keys, live keys retuned beside it; `retuned` on the wire | A `{channels, gainDb}` change had no owner for `gainDb` |
| A retune "keeps the new config" | `adopt` re-points the instance; during `changing` the commit adopts the newest | The instance's settings point into one config |
| `gain` with no change version; `channels` restarts | `channels` reshapes in place with a change version giving a new code; `mute`, `invert.N` added | M3 and processing-nodes D9 |
| `processing-nodes.md`'s `eq` `type.N` in place "until text rows can be live" | Live stepped choice; the code crossfades | Choices can be live now |
| A 30 s idle timer on the control thread; 2 s keep-alive ticks | Kernel keepalive and stale-by-contention; no keep-alive | The control port's no-timers rule |
| Read-ahead owned by the session | The server's read loop and the session together | The transport reads the next frame only after `done` today |
| Own echoes left out by the gateway | Session tokens and hub addressing | Sessions have no identity and the hub sends one encoding to all |
| Related saves judged for disk and `graphFile.set` only | Also for edits from other clients that change a reach | Such an edit aborted the gesture as `refused` |
| Defining lines from the defining statement | From its tokens | knobly's statement place owns its leading comments |
| `NodeStatusListener`, diagnostics' `active` and `count`, `activity.ts` "new" | Built; not designed again | Slice 5 built them |
| `h.retune`, `h.outcome` | The built `h.change` extended; `h.live` | One rule, `changeOf`, already serves the harness |
| `trimDb-1` fallback spelling, [U] knobly | Dropped; `-3.5`, dotted names confirmed | Checked against the pinned knobly grammar |
| Gestures on live whole numbers via a fader | Gestures only on ramped decimals | A step row has no in-between value to drag through |

## 14. Open questions for the owner

| # | Question | Recommendation |
|---|---|---|
| O1 | The control port has no timers. Is ending a gesture by the kernel's TCP keepalive (a dead peer within about 30 s) and by contention after 30 s of silence acceptable in place of a 30 s idle timer? It means a silent but connected tab keeps its gesture until someone else wants the key | Yes: it keeps "all event driven", R32 still holds for every disconnect, and the one case it loses (a tab alive but frozen with its pointer down) is harmless until contended |
| O2 | Should a restart row's reason be a compile error rather than an all-type test? | The test: rows are shared with action arguments, which have no change; a separate node-row type would double the row API for one check |
| O3 | Should an inactive family member (`trimDb.3` while `channels` is 2) also be a graph-file warning? | No: the editor greys it and says why; a warning in `ear.conf` would nag about a value kept on purpose |
| O4 | Should rows that change the declared latency (`delay`'s `ms.N`, `limiter`'s `lookaheadMs`) become live, with latency a pure function of settings re-derived after every commit? | Not in slice 6: in place is already gapless with the code's tap crossfade, and declared latency moving on every tick would re-carry the plan each turn. Revisit if someone wants a delay fader |
| O5 | At `SIGTERM`, should open gestures be committed as released (R32's spirit) rather than dropped? | Keep them dropped: a stop is nobody's release, the keeper refuses edits once stopping, and the audio stops anyway |
| O6 | Under M3, should built types lose their restart rows: `file` `path` and `card` `card` in place (each a change version over built replacements), reshapes in place with companions for `card`'s `playback`/`capture` and `rtp`'s `channels`? | Yes, as its own small slice after 6, starting with `file` and `card` (low cost); reshapes with companions stay in [architecture.md](../architecture.md#not-built-yet) until a type needs them live. Slice 6 gives every such row its reason (6.1) |
| O7 | Should values a node itself learns (sendspin's server volume, the AirPlay sender's volume) become live rows with a second writer? | Reserve the shape; build it in a later slice. Slice 6's slots keep the main loop the one writer |

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
| A framework crossfade transition | Each type's crossfade differs; the edge and previous value are enough |
| An idle timer on the control thread | The owner's no-timers rule (O1) |
| Resizing slots on a reshape | The channels are bound unchanged across a swap; the maximum is small |
| A base class with virtual `onParam` (JUCE, VST3) | A type declares; it writes only what is its own (R2) |
| Gestures on in-place rows (X5) | A drag would chop the audio into holds |
| `processing-nodes.md`'s interim of self-ramping in-place rows | Costly, discarded at 6.6 (section 11) |

## 16. Risks

- **The direct crossing** and begin from the reach index bypass the keeper; a bug breaks "the audio never stays on a
  value `ear.conf` does not hold". Every abort ends by setting the slots to the running value, last in the turn.
- **Read-ahead** changes a rule the transport and two docs state; the ending mark exists so a tick never lands after its
  end was read.
- **The related-change rule** can over-reach; it errs towards aborting.
- **A behaviour change**: a `node.update` of only live rows no longer restarts or reconfigures; any client reading
  `node.state` after such an edit waits for nothing.
- **130 live members** for every `gain` configuration: one load per steady block; a moving gang loads 130 words in that
  block only.
- **Ticks run the type's `check`** on the main loop, at most once per gesture per turn.
- **Touch capture** stays unverified until 6.9's hand check.
- **More compile-time machinery**: 28 compile-fail cases at about 1.3 s each.
