# Processing nodes: level, dynamics, tone and speaker management

Status: design, not built; the owner answered its questions on 2026-11-25 (section 9), the last of them that every
adjustment is live and a restart must be justified, which this page now follows. It sets out the
catalogue of processing node types (transforms, in [the timing model](../architecture.md#the-timing-model)'s words) a
graph needs besides its sources and sinks, the shared DSP code under them, and the order they can be built in against
slices 5 and 6 (slice 3d, changes in place and reshapes, is built). The `gain` type and the live-parameter machinery it rides on are slice 6's
([`control-plane.md`](control-plane.md), sections 5.1, 5.4 and 5.7). This page extends that design and does not
repeat it.

## 1. The owner's words

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| M1 | "add basic mixer nodes, like volume, mute, etc. what are the basic nodes that would be useful when building a graph? compressor? boost? gate?" | 2026-11-24 | A catalogue, a recommended first set and its order (sections 3 and 8); volume and mute first |
| M2 | Asked whether `gain` should be built restart-only now or wait for slice 6: "I can wait til slice 6" | 2026-11-24 | `gain`, `mix` and `select` come with slice 6 (Q1) |
| M3 | Asked whether `eq`'s frequency, gain and Q should be live: "Ideally all adjustments are live. Node restarts should be justified." | 2026-11-25 | Every row of every type changes without a restart, live or in place; a row that restarts says why (section 3, decision D9). None does |

## 2. What a graph can already do, and what it cannot

Routes cover more than they seem to. Before adding a type, check that a route can't already do the job:

| Need | Already done by | Why no type |
|---|---|---|
| Sum several sources at unity | Several routes into one input port: PipeWire mixes them ([control-port.md](../control-port.md#levels): "the sum when several routes mix into it") | A `sum` type would copy what the port's mix already does |
| One source to several places (split, duplicate, mono to both speakers) | Several routes from one output port | Fan-out is free |
| Swap or remap channels | Routes naming other members | Routes are the patch bay |
| Silence | An unlinked input reads zeros ([writing-a-node-type.md](../writing-a-node-type.md#the-code)) | Nothing to make |
| Meter a point | Every public port is metered ([control-port.md](../control-port.md#levels)) | Every port already has a meter |
| Click-free start and stop | The framework's 5 ms fades ([architecture.md](../architecture.md#fades)) | The framework already does it |

What routes cannot do is change a signal. That covers level (volume, mute, trim, polarity), choosing between sources,
dynamics (limit, compress, gate, duck), tone (EQ) and the speaker work the owner's MOTU 24Ao rig needs (crossover to a
sub, per-speaker delay, a protecting limiter). Those are the types below.

## 3. The catalogue

Tiers are ordered by how much a router needs them. Every row changes without a restart (M3, decision D9), in one of
three ways:

- **Live**: a slice 6 parameter ([control-plane.md](control-plane.md), 5.1). It moves while the audio plays, ramped by
  the framework, with no hook called.
- **In place**: a `.inPlace()` row ([architecture.md](../architecture.md#changes-in-place)). The type's change version
  swaps in a new code at one cycle; the code carries the old one's state and crosses from old to new itself, so nothing
  is held silent. Used where a change must declare a new latency or resize what the code reads, which a live retune
  cannot (it calls no hook).
- **Reshape in place**: a row that changes the ports (`channels`, `inputs`, `key`), marked `.inPlace()`
  ([architecture.md](../architecture.md#reshapes)): the block keeps its surviving ports and their links, and the
  channels that stay keep playing. A configuration with a companion would restart instead, and none of these types has
  one.

A **restart** (a 5 ms fade out, a gap while it reconfigures and a 5 ms fade in:
[architecture.md](../architecture.md#restarts-and-changes-of-settings)) is used only where a row says why. No row
here does.

| Tier | Type | Ports | What it is for |
|---|---|---|---|
| 1 Level | `gain` | `in.1..N` → `out.1..N` | Volume, mute, per-channel trim, polarity. Slice 6's type, extended (4.1) |
| 1 Level | `mix` | `in1.1..C` … `in8.1..C` → `out.1..C` | A console: up to 8 C-channel inputs, each with its own level and mute, plus a master level |
| 1 Level | `select` | `in1.*` … `in8.*` → `out.*` | A source selector that crossfades: radio, AirPlay or Sendspin into one zone |
| 2 Dynamics | `limiter` | `in.*` → `out.*` | A brickwall ceiling with lookahead: protects speakers and keeps a summed bus out of clipping |
| 2 Dynamics | `compressor` | `in.*` (+ `key.*`) → `out.*` | Evens out level. With its `key` input it ducks: music drops under an announcement |
| 2 Dynamics | `gate` | `in.*` (+ `key.*`) → `out.*` | Closes on near-silence: hiss and hum between tracks, noise from an idle RTP stream |
| 3 Tone and speakers | `eq` | `in.*` → `out.*` | Parametric EQ, 1 to 8 bands. "Boost" in the tonal sense, a bass or treble shelf, is a band |
| 3 Tone and speakers | `crossover` | `in.*` → `low.*`, `high.*` | A Linkwitz-Riley split for a sub and its satellites. Three-way is two of them chained |
| 3 Tone and speakers | `delay` | `in.*` → `out.*` | Per-channel delay in ms: lines up speakers at different distances, or sound with a screen |
| 4 Utility | `tone` | → `out.*` | A sine at a set frequency and level, for checking speakers and levels. The worked example promoted ([writing-a-node-type.md](../writing-a-node-type.md#worked-example-tone)) |

**Not proposed, and why:**

- **`mute`** is a row of `gain` (and of each `mix` input), not a type. A type that does nothing but multiply by 0 or 1
  would be a `gain` with one row.
- **"Boost"** is covered twice: `gain` and `mix` go up to +24 dB for level, and `eq`'s shelves and peaks give tonal
  boost. Loudness-style "bass boost" is an `eq` preset, and presets are for the editor or a `@set` in `ear.conf`.
- **`matrix`** (a gain at every crosspoint) needs a two-dimensional indexed row, which slice 6 doesn't have. Routes
  already handle remapping and fan-in sums, so its one remaining job, a downmix with set coefficients, can be routes
  into a `mix`. Deferred until someone needs it.
- **`expander`** and **`deesser`** are a `gate` with a ratio and a `compressor` with a filtered key. They wait for a
  real user.
- **Reverb, chorus and other creative effects** are for producing audio, not routing it.
- **`pink`** noise for room measurement would sit beside `white`, but it belongs with the measurement tooling, not
  this slice.

## 4. The types

Rows below use slice 6's vocabulary (`setting::decimal`, `setting::indexed`, `.live`, `.shapesPorts()`;
[control-plane.md](control-plane.md), 5.1). Every type with channels has a `channels` row (1 to 64, default 2,
`.shapesPorts()`), and its banks take that many members. Ranges are inclusive. Every live row smooths over 20 ms unless
the row says otherwise.

### 4.1 `gain`

Slice 6's type ([control-plane.md](control-plane.md), 5.7: `channels`, live `gainDb` from -90 to +24 dB with -90 as
silence, live indexed `trimDb.N` from -24 to +24, and the clipping status, event and trigger), with `channels` a
reshape in place rather than a restart, plus:

| Row | Kind | Default | Change |
|---|---|---|---|
| `mute` | boolean | `false` | live, 10 ms: ramps the factor to 0 and back, so muting never clicks |
| `invert.N` | indexed boolean, by `channels` | `false` | live, 10 ms: ramps the channel's sign from +1 to -1 through 0, so a polarity flip never clicks |

A live boolean is a change to slice 6.1, which allows only whole numbers and decimals to be live. Its `Smoothed` gives
a value from 0 to 1 that ramps as a linear row's does, and the code multiplies by it (decision D7).

```conf
node gain zone1 { gainDb -12; mute false; }
route radio:stream.1 zone1:in.1;
route radio:stream.2 zone1:in.2;
route zone1:out.1 motu:play.1;
route zone1:out.2 motu:play.2;
```

### 4.2 `mix`

| Row | Kind | Default | Change |
|---|---|---|---|
| `inputs` | whole number, 1 to 8 | 2 | reshape in place, `.shapesPorts()`: the inputs that stay keep playing, a new one's level ramps up from silence |
| `channels` | whole number, 1 to 64 | 2 | reshape in place, `.shapesPorts()` |
| `levelDb.N` | indexed decimal by `inputs`, -90 to +12 dB (-90 is silence), db taper | 0 | live |
| `mute.N` | indexed boolean by `inputs` | `false` | live, 10 ms |
| `masterDb` | decimal, -90 to +12 dB, db taper | 0 | live |

The banks are `in1` to `in8` and `out`. Bank `inK` has `channels` members when K is at most `inputs`, and none
otherwise. Banks are fixed at compile time and members come from settings, and a bank with no members is already legal
(`card`'s `playback 0`), so the bank list stays fixed (decision D3). The code declares a path of no delay from each
input bank to `out` ([writing-a-node-type.md](../writing-a-node-type.md#latency): "a mixer declares each of its inputs'
paths into one output"), so downstream latency reaches every input.

*Why `mix` when fan-in sums:* fan-in sums at unity, with no level, no mute and nothing for the editor to draw as a
fader per source. `mix` is the console strip. Fan-in stays the answer for a fixed unity sum.

### 4.3 `select`

| Row | Kind | Default | Change |
|---|---|---|---|
| `inputs` | whole number, 2 to 8 | 2 | reshape in place, `.shapesPorts()`; removing the selected input fades to none |
| `channels` | whole number, 1 to 64 | 2 | reshape in place, `.shapesPorts()` |
| `input` | whole number, 0 to `inputs` (0 is none) | 1 | live, steps |
| `fadeMs` | whole number, 0 to 2000 | 50 | live, steps: the next crossfade takes it; one in flight finishes at its own |

The banks are as `mix`'s. When `input` steps, the code runs an equal-power crossfade over `fadeMs` from the old input to
the new one, and the code owns the crossfade's state. A slot that steps again mid-fade starts a new fade from the
current mix. *Why not `mix` with mutes:* one control means exactly one source at a time, and the crossfade is
equal-power, not two linear ramps that dip 3 dB in the middle.

### 4.4 `limiter`

| Row | Kind | Default | Change |
|---|---|---|---|
| `ceilingDb` | decimal, -30 to 0 dBFS | -1 | live |
| `releaseMs` | decimal, 1 to 2000 | 100 | live, steps |
| `lookaheadMs` | decimal, 0 to 20 | 5 | in place: it changes the declared latency, which only a change version can declare (decision D11) |
| `link` | boolean | `true` | live, steps: the gain envelope already smooths the switch |

It detects sample peaks. With `link`, it detects across every channel and applies one gain to all of them, so a stereo
image never shifts. Gain falls to its target within the lookahead, so no sample of the output goes above the ceiling,
and recovers with a one-pole release. The lookahead is declared as the node's latency on `in` to `out` (decision D6).
The delay line is sized for 20 ms when the node is configured, so a change of lookahead only moves the read tap, with
the crossfade `delay` uses (decision D11).
Its gain reduction becomes an `rt` status field `reductionDb` once slice 5 is built.

### 4.5 `compressor`

| Row | Kind | Default | Change |
|---|---|---|---|
| `thresholdDb` | decimal, -60 to 0 | -20 | live |
| `ratio` | decimal, 1 to 20 | 4 | live |
| `kneeDb` | decimal, 0 to 24 | 6 | live, steps |
| `attackMs` | decimal, 0.1 to 200 | 10 | live, steps |
| `releaseMs` | decimal, 1 to 5000 | 150 | live, steps |
| `makeupDb` | decimal, -24 to +24 | 0 | live |
| `link` | boolean | `true` | live, steps |
| `key` | boolean | `false` | reshape in place, `.shapesPorts()`: adds the `key` bank, `channels` members; the detector crossfades from `in` to `key` over 20 ms |

It is a feed-forward compressor: the detector is a peak follower with separate attack and release, the gain computer
has a soft knee, and the work is done in dB. With `key`, the detector reads the `key` bank instead of `in`. That is a
duck:

```conf
node compressor duck { key true; thresholdDb -40; ratio 10; attackMs 5; releaseMs 400; }
route music:out.1 duck:in.1;
route music:out.2 duck:in.2;
route doorbell:out.1 duck:key.1;      # the announcement drives the duck
route doorbell:out.1 zone:in2.1;      # and is mixed in over the ducked music
route duck:out.1 zone:in1.1;
route duck:out.2 zone:in1.2;
```

The code declares a path of no delay from `key` to `out` as well, so the key's source is told the latency downstream.

### 4.6 `gate`

| Row | Kind | Default | Change |
|---|---|---|---|
| `thresholdDb` | decimal, -90 to 0 | -60 | live |
| `hysteresisDb` | decimal, 0 to 20 | 3 | live, steps: it closes at `thresholdDb - hysteresisDb` |
| `rangeDb` | decimal, -90 to 0 (-90 is silence) | -90 | live |
| `attackMs`, `holdMs`, `releaseMs` | decimal, 0.1 to 50, 0 to 2000 and 1 to 5000 | 1, 50, 200 | live, steps |
| `link`, `key` | as `compressor`'s | `true`, `false` | as `compressor`'s |

It uses the same detector as `compressor`. Its gain moves between 0 dB and `rangeDb` with the attack and release, and
the hold keeps it open for a while after the level falls.

### 4.7 `eq`

| Row | Kind | Default | Change |
|---|---|---|---|
| `bands` | whole number, 1 to 8 | 1 | live, steps: a band added or removed crossfades over 20 ms between its output and its input, so it fades in or out of the chain |
| `type.N` | indexed text by `bands`: `peak`, `lowshelf`, `highshelf`, `lowpass`, `highpass`, `notch`, `bandpass`, `allpass` | `peak` | live, steps: the band runs the old and new filter for 20 ms and crossfades between them |
| `freqHz.N` | indexed decimal, 10 to 24000, log taper | 1000 | live |
| `gainDb.N` | indexed decimal, -24 to +24 | 0 | live |
| `q.N` | indexed decimal, 0.1 to 24, log taper | 0.707 | live |

Each band is a state-variable filter in its topology-preserving (TPT) form, one per channel, with the RBJ Audio EQ
Cookbook's responses (decision D10). Every code is built with all 8 bands, so `bands` only enables them. While a
frequency, gain or Q ramps, the band's coefficients are recomputed every 32 frames from the ramped values (and once at
each block's end), which a TPT filter takes without a burst, as a direct-form biquad does not. A text row is not a
slice 6 live kind, so `type.N` and `bands` are `.inPlace()` until text rows can be live: the change version swaps in a
code carrying the old one's filter state, and the code makes the crossfade. A frequency at or above half the graph rate
is clamped just below it, and the band says so in a status field (slice 5), rather than failing the node: a settings
change must never stop the audio for a value the code can bound.

### 4.8 `crossover`

| Row | Kind | Default | Change |
|---|---|---|---|
| `freqHz` | decimal, 20 to 20000, log taper | 80 | live: both outputs' filters move together, so `low` plus `high` stays flat while it ramps |
| `slope` | text: `lr2`, `lr4`, `lr8` (12, 24 and 48 dB per octave) | `lr4` | in place: a code that runs the old and new slope for 20 ms and crossfades both outputs |

The banks are `in`, `low` and `high`, each with `channels` members. Linkwitz-Riley: `low` plus `high` sums to an
allpass, flat in magnitude. A sub and its satellites:

```conf
node crossover xo { channels 2; freqHz 80; }
node mix sub { inputs 1; channels 1; }       # L and R lows fan into one input: the mono sub
route xo:low.1 sub:in1.1;
route xo:low.2 sub:in1.1;
route sub:out.1 motu:play.3;
route xo:high.1 motu:play.1;
route xo:high.2 motu:play.2;
```

### 4.9 `delay`

| Row | Kind | Default | Change |
|---|---|---|---|
| `ms.N` | indexed decimal by `channels`, 0 to 1000 | 0 | in place (decision D11) |

It delays each channel by the nearest whole frame. It declares `Latency{min, max}` on `in` to `out`, the smallest and
largest of its channels' delays (decision D6). A change of delay changes that latency, which only a change version can
declare, so `ms.N` is `.inPlace()` rather than live. The delay line is sized for the row's maximum when the node is
configured and kept by the instance, not the code, so the change version swaps in a code that reads the same line from a
new tap and crossfades from the old tap over 20 ms: no gap and no click (decision D11).

### 4.10 `tone`

Rows: `freqHz` (decimal, 10 to 24000, default 1000, live with a log ramp), `levelDb` (decimal, -90 to 0, default
-20, live) and `channels` (reshape in place). It is the worked example's sine, made a type, with the same phase rule.

## 5. Shared DSP: the `ear_dsp` package

No type may link another ([architecture.md](../architecture.md#packages)), so the code the dynamics and filter types
share goes in a package below them, as `ear_mdns` sits below `sendspin` and `airplay` (decision D4):

| Target | Owns | Never |
|---|---|---|
| `ear_dsp` (`src/dsp`) | Realtime-safe building blocks, all `noexcept` and allocation-free after construction: dB and factor conversion; `Svf` (a TPT state-variable filter with the RBJ responses, safe to retune per sub-block; cascades; Linkwitz-Riley as cascaded Butterworth); `TapCrossfade` (two read taps on a `DelayLine`, crossfaded); `PeakFollower` (attack and release coefficients from milliseconds at a `SampleRate`); `GainComputer` (threshold, ratio, soft knee); `DelayLine` (fixed size, given at construction); `EqualPowerFade` | A thread, a clock, PipeWire, or anything of ear's but `ear_core`'s `SampleRate` and `FrameClock` |

The rules that apply to it:

- **Any block size.** `block_size` can be anything from 1 to 2^32 - 1 ([graph-file.md](../graph-file.md#the-graph-block)),
  so no block may assume a power of two or a minimum length. State carries across blocks sample for sample.
- **Denormals are already flushed** on the data thread ([engine.md](../engine.md#denormals)), so a decaying filter or
  envelope needs no guard of its own.
- **Allocation happens in `configure`**: delay lines and per-channel state are members of the code, sized from settings
  before it drives ([writing-a-node-type.md](../writing-a-node-type.md#the-code)).
- **Write every output sample**, as every type does. The harness's NaN fill catches a band or channel that was skipped.

## 6. Tests

They follow [testing.md](../testing.md)'s rules: event-gated, nothing skipped, audio checked in the tests. Each type is
tested through `TypeHarness`. `ear_dsp` has its own unit tests.

| What | Proved by |
|---|---|
| `gain`: unity, -6 dB, mute | A full-scale sine comes out at exactly 1.0, 0.5012 and 0 factor once steady. Mute's ramp has no step larger than the 10 ms ramp's |
| `mix`: per-input level, master, an input of no members | Two DC inputs come out as the weighted sum. `inputs 3` has banks `in4` to `in8` with no members |
| `select`: crossfade | Two DC inputs at 1 and -1: the sum of squares of the two weights stays 1 within 1e-6 across the fade |
| `limiter`: ceiling | White noise at +12 dB never goes above the ceiling on any sample, and the declared latency equals the lookahead's frames |
| `compressor`: static curve | Steady sines at levels across the knee come out on the computed curve within 0.1 dB. The `key` bank drives it while `in` is constant |
| `gate`: open, hold, close, hysteresis | A burst opens it, the hold keeps it open, silence closes it. A level between the two thresholds doesn't chatter |
| `eq`: each band type | The magnitude at f0 and at two other frequencies matches the analytic response of the cookbook's transfer function within 0.01 dB |
| `eq`: retuned while playing | A sine through a band swept from 20 Hz to 20 kHz over 1 s, and its Q from 0.1 to 24, never steps by more than the steady response allows and never exceeds the larger of the two steady outputs by more than 0.1 dB; a band added, removed and its type changed each crossfade in 20 ms with no step |
| `crossover`: flat sum | `low` plus `high` for white noise has a flat magnitude within 0.01 dB. Each output is down 6 dB at the crossover frequency for an LR filter |
| `delay`: exact frames | An impulse comes out at exactly `round(ms × rate / 1000)` frames on each channel, and the declared latency is `{min, max}` |
| `delay` and `limiter`: changed in place | A sine through a change of `ms.1` (and of `lookaheadMs`) stays continuous: no sample steps further than the 20 ms crossfade allows, the node goes `reconfiguring` and back to `running`, never `stopping`, and the declared latency moves at the commit point |
| Reshapes in place | `mix` from 2 inputs to 3 and back, and `gain` from 2 channels to 4 and back: the inputs and channels that stay play on with no gap, never `stopping` |
| All types | The all-type tests (`test/nodes/`) through `validSettings`: every output written, settings refusals in order |

## 7. What it changes in the built system

When built: `docs/node-types.md` gains a section per type and a row in its ports table; `docs/architecture.md`'s
packages table gains `ear_dsp`; `test/nodes/all_types.hpp` gains each type's valid settings. The editor needs nothing:
it draws any type, and from slice 5 on it renders the schema's live rows as faders
([control-plane.md](control-plane.md), 8.2). A type folder registers itself
([writing-a-node-type.md](../writing-a-node-type.md#layout-and-registration)), so nothing else names it.

The extensions this page makes to the slice 6 design: live booleans (D7), `gain`'s `mute` and `invert.N` rows, and
`gain`'s `channels` as a reshape in place.

## 8. Order of work

Every type above needs decimal settings: thresholds, ratios, Q, Hz and dB. Most need indexed rows for their bands,
inputs and channels. Decimals and indexed rows are slice 6.1's row kinds, which are independent of live parameters.
Volume, mute and selection only make sense live, which needs all of slice 6 and therefore slices 3d and 5. Proposed
order:

1. **Row kinds, not yet live.** Pull forward slice 6.1's `setting::decimal` and `setting::indexed`, without `.live`:
   the settings walk, `SettingKind::decimal`, the graph file's reading and writing of decimals, `types.get`'s kinds,
   and the compile-fail cases. This touches the wire (a new setting kind), so `docs/control-port.md`, `test/protocol/`
   and `web/src/protocol.ts` change with it ([control-port.md](../control-port.md#changing-the-wire)).
2. **`ear_dsp` and the speaker types:** `delay`, `crossover`, `eq`, `limiter`, `compressor`, `gate` and `tone`, with
   no row restarting (M3). Until slice 6 there are no live rows, so every row this page marks live is `.inPlace()` for
   now: its change version swaps in a code carrying the old one's state, and the code ramps from the old values to the
   new over the row's smoothing itself, with no hold. The rows marked in place, and the reshapes, are built as they
   will stay.
3. **With slice 6:** `gain` (as designed, plus D7), `mix` and `select`, and step 2's live rows become `.live`, the
   framework's ramp taking over from the code's. That change is additive: a row that becomes live breaks no graph file
   and no client.

## 9. Decisions taken, and open questions

**Decisions this page takes (the designer's, open to the owner's veto):**

| # | Decision | Why |
|---|---|---|
| D1 | No `sum`, `split`, `matrix` or `meter` type | Routes and port meters already do these jobs (section 2) |
| D2 | Mute is a row of `gain` and `mix`, not a type. "Boost" is `gain`'s range and `eq`'s shelf | One type per job. Each would be a `gain` with one row |
| D3 | `mix` and `select` declare a fixed `in1` to `in8`, and the unused ones have no members | A spec's banks are fixed at compile time. Banks with no members are already legal. One bank per input keeps routes readable (`zone:in2.1`) |
| D4 | Shared DSP lives in a new `ear_dsp` package below the node types | No type links another. `ear_mdns` already sets the precedent |
| D5 | Dynamics are linked across channels by default | A stereo image must not shift when one side peaks |
| D6 | A limiter's lookahead and a delay's delay are declared latency on `in` to `out` | Both really hold audio, and sinks and sources downstream stamp and buffer by declared latency ([engine.md](../engine.md#latency-per-bank-pair)) |
| D7 | A live boolean ramps from 0 to 1 over its smoothing, and the code multiplies by it | Mute and polarity must be click-free toggles, and a toggle is a boolean in the editor, not a fader |
| D8 | A sidechain is a `key` input bank that a boolean row adds | A key needs its own routes, and a bank is what routes name |
| D9 | No row restarts its node: each is live, in place, or a reshape in place, and a row that must restart says why | M3. A restart stops the audio for a change the code can make while it plays |
| D10 | Filters (`eq`, `crossover`) are TPT state-variable filters with the cookbook's responses, retuned every 32 frames while a parameter ramps | A direct-form biquad whose coefficients move can burst; a TPT filter stays stable and smooth under modulation, with the same steady response |
| D11 | A change of delay (`delay`'s `ms.N`, `limiter`'s `lookaheadMs`) is in place: a line sized for the maximum, kept by the instance, read from a new tap that crossfades from the old over 20 ms, the new latency declared at the commit point | Declaring latency needs a change version, which a live retune does not call; the crossfade avoids the click a jumping tap makes |

**The owner's answers (2026-11-24 and 2026-11-25):**

| # | Question | Answer |
|---|---|---|
| Q1 | Should `gain` be built now restart-only, or wait for slice 6? | Wait for slice 6 (M2) |
| Q2 | Is 8 the right cap on `mix` and `select` inputs? | 8, as recommended. More inputs means a second `mix` feeding the first |
| Q3 | Should `limiter` detect true peaks (4× oversampled, ITU-R BS.1770) rather than sample peaks? | No preference; the designer's choice stands: sample peaks in v1. True peaks matter for lossy re-encoding (`sendspin` out), and wait for a user |
| Q4 | Should `eq`'s `freqHz`, `gainDb` and `q` be live? | All of them, and every other adjustment too: restarts must be justified (M3, D9, D10) |
| Q5 | Should `crossover` be three-way natively? | No, as recommended. Chaining two is exact for Linkwitz-Riley once the low band gets a matching allpass, which is an `eq` band (`allpass`) |
