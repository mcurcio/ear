# Live rows: what slice 6 left

Status: designed in outline; not built. Slice 6 (change marks, decimal, choice and indexed rows, live rows, retunes,
the `gain` type, gestures and the editor's faders) is built and in the library docs
([writing-a-node-type.md](../writing-a-node-type.md#change-marks), [#live-rows](../writing-a-node-type.md#live-rows),
[architecture.md](../architecture.md#retunes), [#gestures](../architecture.md#gestures),
[control-port.md](../control-port.md#gestures), [editor.md](../editor.md#live-numbers-faders-and-knobs)). This page
keeps the three things it left, each decided by the owner on 2026-11-26 ("ill take the recommendations").

## The owner's words that bind it

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| M3 | "Ideally all adjustments are live. Node restarts should be justified." | 2026-11-25 | A row restarts only with a true reason; the two below are the built rows whose reason is "has no change in place yet" |
| O6 | The recommendation taken: "has no change in place yet" is a true restart reason for `file`'s `path` and `card`'s `card`; a small follow-on slice makes both in place | 2026-11-26 | Below, [`file` and `card` in place](#file-and-card-in-place) |
| O12 | The recommendation taken: values a node learns itself become live rows later, not in slice 6 | 2026-11-26 | Below, [values a node learns itself](#values-a-node-learns-itself) |

## `file` and `card` in place

`file`'s `path` and `card`'s `card` restart their node today, saying so ("file has no change in place yet", "card has
no change in place yet", [writing-a-node-type.md](../writing-a-node-type.md#change-marks)). Both can be in place with
the built machinery ([architecture.md](../architecture.md#changes-in-place)):

- **`file`'s `path`**: a change version that claims the new path (refused `held by node <other>` as a start is),
  opens the new file on its writer loop and posts the switch there at the commit point, so the old file is closed
  after its last block and the new one gets the next. The mark becomes `.inPlace()`; the system test records across
  a change and finds every frame in exactly one of the two files.
- **`card`'s `card`**: a change version that replaces the card's engine companion make-before-break, as a reshape
  with companions would ([architecture.md](../architecture.md#not-built-yet)); it waits on that, or on a replacement
  of one companion on an unchanged bank, which is the smaller half of the same work.

Low cost right after slice 6, and nothing else waits for it.

## Values a node learns itself

A `sendspin` server's volume and an AirPlay sender's volume are values the node hears from its peer, not from
`ear.conf`. As live rows they would let the editor's fader show and move them, but they need a second writer of a
node's live values: today the main loop is the one writer ([architecture.md](../architecture.md#live-values)), and a
gesture or a retune settles the slots to the committed config. The design to come must say which writer wins (the
hand, `ear.conf` or the peer), whether a learned value is written back into `ear.conf`, and how a peer's change is
told (`param` with a new cause). Nothing in slice 6 blocks it.

## The speaker types

`processing-nodes.md`'s speaker types (`delay`, `crossover`, `eq`, `limiter`, `compressor`, `gate`, `tone`, and
`mix` and `select`; on its own branch) need nothing more from slice 6: each row is live, in place or a reshape in place
from its first commit, choosing its mark by the built rule
([writing-a-node-type.md](../writing-a-node-type.md#change-marks)):

| Need there | Mark |
|---|---|
| `gainDb`, `levelDb.N`, `freqHz.N`, `q.N`, thresholds, ratios | live, ramped (decimal) |
| `mute`, `mute.N`, `invert.N` | live, ramped (boolean, 0 to 1) |
| `link`, attack, release, knee, hold | live, stepped (the envelope smooths it) |
| `select`'s `input`, `eq`'s `bands` | live, stepped (whole number); the code crossfades on `changed()` |
| `eq`'s `type.N`, `crossover`'s `slope` | live, stepped (choice); the code crossfades on `changed()` |
| `delay`'s `ms.N`, `limiter`'s `lookaheadMs` | in place: they change the declared latency |
| `channels`, `inputs`, `key` | reshape in place |

That branch's interim (each live row an in-place row whose code ramps itself) is not to be built: every change would
be a full change in place with its deadline and records, and the ramp's start value would live in a code a swap
replaces.
