# Rates with units, in a `graph { }` block

Not built. The owner's decisions: a rate in `ear.conf` is never a bare number but a number with
its unit (`44.1khz`, `48khz`, `48000hz`); the graph's settings move into a `graph { }` block whose
keys are `rate` ("rate is a good name since it will already be nested under the graph { ... }
scope") and `block_size` (a plain frame count); old files are refused with a hint and no
compatibility is kept; the flag becomes `--rate 48khz`; every other rate setting takes the same
form. A major release.

```conf
graph {
    rate 48khz;
    block_size 256;
}
```

## 1. The grammar

A rate is one knobly word: a number (digits, optionally `.` and more digits), then `hz` or `khz`
in any case, with nothing between. knobly lexes `44.1khz` as one DIGIT_WORD, a word value, read
with knobly's `word()` (a quoted `"48khz"` is knobly's wrong-kind error, as a quoted number is
for a whole-number setting). Its value times 1 or 1000 must be a whole number of hertz, computed
exactly in integers (never a `double`), above 0 and at most 4294967295.

- **No `MHz`, no space before the unit, no sign, no exponent.** No rate ear runs at needs
  `MHz`, and a space makes two values (`rate 48 khz;` is the existing "takes one value" error).
- **The ranges are the reader's**, not the grammar's: the graph's rate is 8 kHz to 768 kHz (the
  cap the owner was asked about); `rtp`'s `rate` keeps 8 kHz to 192 kHz.
- **One home.** `ear/core/rate_text.hpp` (`readRate`, `rateText`), used by the file's reader,
  the `--rate` flag and the settings table's rate rows. It takes a rate's number, so it joins
  `test/sample_rate_exits.cmake`'s list.
- **The written form** (`rateText`): `<n>khz` when the rate is whole kHz, `<n>.<d>khz` when it is
  whole hundreds of Hz (`44.1khz`, `88.2khz`), else `<n>hz` (`11025hz`, `22050hz`). Reading it
  back gives the same rate.

Refusals (code `ear/bad-rate`, new: one code for every rate the grammar or a range refuses):

| Written | Message |
|---|---|
| `48000` | `48000 has no unit: write 48000hz or 48khz` |
| `48 kHz` | (two values: the directive's own "takes one value") |
| `48mhz`, `fast` | `48mhz is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or 48000hz` |
| `44.1003khz` | `44.1003khz is not a whole number of hertz` |
| `4khz` (graph) | `rate: 4khz is outside 8khz to 768khz` |

## 2. The `graph { }` block

- A top-level directive with no value and a block; the directives become `graph`, `node` and
  `route`. Required, once, exactly the keys `rate` and `block_size`, each required, each once.
- Variables and snippets work inside it as in a node's block: the reader reads knobly's rendered
  tree, so `rate $r;` and an `@use` are read the same as written values.
- Errors:

| Case | Code | Message |
|---|---|---|
| No `graph` block | `ear/missing-directive` | `graph is missing: write graph { rate 48khz; block_size 256; }` |
| A second block | `ear/bad-directive` | `graph is given twice` |
| A value or no block | `ear/bad-directive` | `graph takes no value and a block: graph { rate 48khz; block_size 256; }` |
| An unknown key | `ear/bad-directive` | `graph has no setting <key>: it takes rate and block_size` |
| A key twice | `ear/bad-directive` | `graph's <key> is given twice` |
| A key missing | `ear/missing-directive` | `graph is missing rate: write rate 48khz; inside graph { }` (or `block_size 256;`) |
| A bad `block_size` | `ear/bad-directive` | `block_size takes a whole number from 1 to 4294967295` (unchanged) |

- **An old file.** A top-level `graph_frequency` or `block_size` is `ear/unknown-directive` with
  the hint, written with the file's own values where they read: `graph_frequency is no longer a
  directive: graph settings moved: write graph { rate 44.1khz; block_size 512; }` (48khz and 256
  where a value does not read). A file with an old line and no `graph` block gets no
  missing-directive too: the hint already says it.
- **The old flag.** `--graph-frequency` is `ear: --graph-frequency is gone: write --rate with a
  unit, such as --rate 48khz` and exit 1, before boost's generic "unrecognised option".

## 3. How ear writes it back

No control-port edit writes the `graph` block: `node.*` and `route.*` edits touch only their own
statements and append after the last statement, and `graphFile.set` replaces the whole text.
Only `rtp`'s `rate` is a rate an edit writes (6).

## 4. The wire

Unchanged. `graph.get`'s `graphFrequency` stays a number in Hz, `pendingRestart`'s keys stay
`graphFrequency` and `blockSize`. The editor shows the rate as `48 kHz` and the pane's
pending-restart banner names "graph rate" and "block size". `test/protocol/graph-file.json` and
its typed twin `src/control/tests/graph_file_fixture.hpp` hold ear.conf text: their texts move to
the new form, and the diagnostic lines they carry move down by two (the block is four lines where
the two directives were two). That is fixture content, not wire shape.

## 5. Everything holding ear.conf text

The config and control tests and fixtures, `test/protocol/graph-file.json`, the system tests'
`graphFileText` and their texts, `web/src/pane.test.ts`, `scripts/image-smoke.sh`, the
`sendspin` and `ear-on-talos` skills (and `pipewire-engine`'s words), `docs/README.md`,
`docs/graph-file.md`, `docs/node-types.md`, `docs/architecture.md`, `docs/engine.md`, and
`control-plane.md`'s example. The Dockerfile seeds no file and `config/` is not in the repository.

## 6. A rate setting's kind

A third `SettingKind`, `rate`, made by a new rule `setting::rate(low, high)` (two `SampleRate`s)
in a type's settings row, so `rtp`'s row reads `std::optional<SampleRate>` straight.

- **In the file:** read with knobly's `word()` and `readRate` (any rate the grammar gives; the
  row's range is the table's), so a bad unit is `ear/bad-rate` at the value. Written back by an
  edit with `rateText` (`44100` becomes `rate 44.1khz;`); an edit that does not change the rate
  leaves its spelling (`44100hz`) alone.
- **On the wire:** a whole number of Hz, as today (`"rate": 44100`). It keeps the wire unchanged,
  matches `graphFrequency`, and the editor already edits settings as raw JSON. A string is
  refused.
- **The table's refusal**, the same for the file and the wire: `rate: must be a rate from 8khz to
  192khz (on the control port, a whole number of Hz)`.
