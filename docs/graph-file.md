# The graph file, `ear.conf`

`ear.conf` is the one file that says which nodes exist, with what settings, and which routes join
their ports. It lives in the graph file folder (`--config-dir`, default `config`), it is written
in the knobly configuration language ([mcurcio/knobly](https://github.com/mcurcio/knobly)), and
ear keeps it live: edits made through the control port are applied and written back into it with
comments and layout kept, and hand edits are applied as soon as they are saved. It is not
committed to the repository. The code is `src/config/`.

## An example

```conf
graph {
    rate 48khz;
    block_size 256;
}
@set $ch 2;                      # comments survive every edit
@snippet card_out $id {          # written once, used by both cards
    card $id;
    playback $ch;
}
node white noise;
node file rec { path "take1.wav"; }
node card desk { @use card_out MOTU; }
node card porch { @use card_out USB2; }
route noise:noise.mono rec:record.left;
route noise:noise.mono desk:play.1;
```

## Directives

Only ear's three directives are allowed at the top level.

| Directive | Form | Rules |
|---|---|---|
| `graph` | `graph { rate <rate>; block_size <frames>; }` | Required, once; no value; its block holds exactly `rate` and `block_size`, each once |
| `node` | `node <type> <name>;` or `node <type> <name> { <key> <value>; ... }` | One type, one name, an optional block of settings, each key once |
| `route` | `route <node>:<port> <node>:<port>;` | From an output to an input; no block |

Anything else is `ear/unknown-directive`, a malformed directive is `ear/bad-directive`, and a
missing `graph` block, or a `graph` block without `rate` or `block_size`, is
`ear/missing-directive`.

**Node names** are unique, 1 to 64 characters of letters, digits, `-` and `_`. *Why no `.`:*
engine objects a node makes are named `<node>.<suffix>` (`desk.playback`, `fpp.rtp`), so they can
never collide with a node.

**Ports** are named `<bank>.<member>`, and a route names one by its whole name,
`<node>:<bank>.<member>` (`route kitchen:from.left desk:play.1;`). Each type's banks are listed
in [node-types.md](node-types.md#ports-at-a-glance). Older names (`MONO`, `FL`, `playback_1`) are
`unknown-port`; no compatibility is kept.

### The `graph` block

The graph's own settings, fixed at boot ([below](#settings-applied-only-at-restart)):

| Key | Rule |
|---|---|
| `rate` | The graph's rate ([rates](#rates)), 8 kHz to 768 kHz |
| `block_size` | Frames a cycle, a whole number from 1 to 4294967295 |

Variables and snippets work inside it as in a node's block (`rate $r;`, `@use frames 256;`). What
each problem says:

| Problem | Code | Message |
|---|---|---|
| No `graph` block | `ear/missing-directive` | `graph is missing: write graph { rate 48khz; block_size 256; }` |
| A second one | `ear/bad-directive` | `graph is given twice` |
| A value, or no block | `ear/bad-directive` | `graph takes no value and a block: graph { rate 48khz; block_size 256; }` |
| Another key | `ear/bad-directive` | `graph has no setting <key>: it takes rate and block_size` |
| A key twice | `ear/bad-directive` | `graph's <key> is given twice` |
| A key missing | `ear/missing-directive` | `graph is missing rate: write rate 48khz; inside graph { }`, or `block_size 256;` |
| A bad `block_size` | `ear/bad-directive` | `block_size takes a whole number from 1 to 4294967295` |

**Older files are refused.** A top-level `graph_frequency` or `block_size` is
`ear/unknown-directive` saying what to write instead, from the file's own values where they read
(else 48khz and 256): `graph_frequency is no longer a directive: graph settings moved: write graph
{ rate 44.1khz; block_size 512; }`. No compatibility is kept; with these lines there is no
missing-`graph` error besides.

**Upgrading a file** is a one-time edit: the two old lines become one `graph { rate <rate>;
block_size <frames>; }` block, every rate setting (`rtp`'s `rate`) gets its unit (`48000` becomes
`48khz`, `44100` becomes `44.1khz`), and a `--graph-frequency` flag becomes `--rate` with a unit.
Until the file is upgraded ear stays up but runs nothing: its errors are `graph-file error`
records and the pane's diagnostics, and the engine takes `--rate` and `--block-size`. A
`--graph-frequency` left in the arguments exits 1 at boot instead. Fix the file by hand or with
the pane's Save, then restart ear if its rate or block size differs from the flags'
([below](#settings-applied-only-at-restart)). On the owner's cluster, where the file lives on a
volume seeded only when missing, the `ear-on-talos` skill has the steps.

### Rates

A rate is never a bare number: it is one word, a number (`48`, `44.1`) and then `hz` or `khz` in
any case, with nothing between (`48khz`, `44.1kHz`, `48000hz`). It must come to a whole number of
hertz, worked out exactly. There is no `MHz`, sign, exponent or space before the unit, and a
quoted `"48khz"` is knobly's wrong-kind error, as a quoted number is. The graph's `rate`, the
`--rate` flag and every rate setting ([node-types.md](node-types.md#rtp)) read it the same way
(`ear/core/rate_text.hpp`). A problem is `ear/bad-rate`, at the value, the key first:

| Written | Message |
|---|---|
| `48000` | `rate: 48000 has no unit: write 48000hz or 48khz` |
| `48mhz` | `rate: 48mhz is not a rate: write a number, then hz or khz, such as 48khz, 44.1khz or 48000hz` |
| `44.1003khz` | `rate: 44.1003khz is not a whole number of hertz` |
| `4khz` (the graph's) | `rate: 4khz is outside 8khz to 768khz` |
| `48 khz` (the graph's) | `rate takes one value: write the unit with no space, such as 48khz` |

ear writes a rate as `<n>khz` when it is whole kilohertz, `<n>.<d>khz` when it is whole hundreds
of hertz (`44.1khz`, `88.2khz`), else `<n>hz` (`11025hz`). The 768 kHz cap is the graph's; a rate
setting's range is its type's.

### Knobly's features

- **Comments** (`# ...`) and layout survive every edit ear makes.
- **Variables:** `@set $name value;`, used as `$name`.
- **Snippets:** `@snippet name $param { ... }`, used inside a node's block as `@use name arg;`.
- **`@include` is refused** (`ear/include-unsupported`: "@include is not supported yet: write the
  whole graph in ear.conf"), so no other file is ever read.

### Settings and their kinds

Each setting is read and written as the kind its node type declares: **text** (a bare word or a
quoted string), a **whole number**, or a **rate** ([rates](#rates)). A rate setting is written
with its unit in `ear.conf` and is a whole number of hertz in the node's settings on the control
port (`rate 44.1khz;` is `"rate": 44100`); an edit that changes it writes ear's form, and one that
does not leaves its spelling as written. A key the type does not declare is invalid settings. The
keys, ranges and defaults of each type are in [node-types.md](node-types.md). A text setting, node
name or port holding a control character other than newline, tab and carriage return cannot be
written in `ear.conf` and is refused.

### `disabled true;`

`disabled` is the framework's key inside a node's braces, never a setting of its type:

```conf
node card porch { disabled true; card USB2; playback 2; }
```

keeps the node (its block, ports and routes) and configures nothing
([architecture.md](architecture.md#stopped-versus-disabled)). It is read as a boolean: `true` or
`false`, or a variable such as `$quiet`. A bare `disabled;` or more than one value is
`ear/bad-directive` ("disabled takes true or false: write disabled true;"), and a `node` line with
the word `disabled` on it (`node white noise disabled;`) is `ear/bad-directive` with the hint "to
disable a node, write disabled true; inside its braces".

## Diagnostics

A problem in the text is a diagnostic: a severity (`error` or `warning`), a code, a message, and
a line and column (from 1; the column counts UTF-16 code units, as a browser does). Knobly's own
codes look like `E2001`; ear's are:

| Code | When |
|---|---|
| `ear/missing-file` | `ear.conf` does not exist |
| `ear/unreadable-file` | `ear.conf` is a symlink, a directory, another kind of file, or larger than 1 MiB |
| `ear/include-unsupported` | An `@include` |
| `ear/unknown-directive` | A directive other than `graph`, `node` and `route`, an old `graph_frequency` or `block_size` among them |
| `ear/bad-directive` | A malformed directive or `graph` block, a setting given twice, a bad `disabled` line |
| `ear/missing-directive` | No `graph` block, or one without `rate` or `block_size` |
| `ear/bad-rate` | A rate not written as a number and `hz` or `khz`, not whole hertz, or (the graph's) outside 8 kHz to 768 kHz ([rates](#rates)) |
| `ear/too-large` | An edit's or a `graphFile.set`'s text over 1 MiB (answered `invalid-text`) |
| `ear/unwritable-edit` | An edit knobly could not write (answered `invalid-text`) |
| `ear/not-one-node` | A `node.setText` text that is not exactly one `node` statement ([below](#a-nodes-own-text)) |
| `ear/node-renamed` | A `node.setText` text naming another node, at the name |

**A file with any error does not apply.** The graph keeps running what it ran, the editor shows
why, and every `node.*` and `route.*` edit is refused `file-invalid` until the text is fixed by
hand or replaced with `graphFile.set` (the pane's Save). The control port's lifecycle requests
(`node.start`, `node.stop`, `node.restart`) still work, since they edit nothing.

**A file the graph refuses** (it reads, but names a port that does not exist, say) is shown with
its refusal (`refusal` in the file's state, with the line it points at), and the graph keeps
running what it ran. `running` then names the version the graph actually runs.

## Edits at the source

An edit through the control port (`node.add`, `node.update`, `node.remove`, `route.add`,
`route.remove`, `node.disable`, `node.enable`) changes the file **where the value is defined**:

- Setting `playback 4` on `desk` rewrites `@set $ch 4;`, so both cards change in one commit.
- Setting `card` on `desk` rewrites only `MOTU` on its `@use` line.
- `node.remove` removes the node's line and every `route` line that names it.
- Added nodes and routes are appended at the end. No edit writes the `graph` block: only
  `graphFile.set` or a hand edit changes it.
- `node.disable` sets the node's own `disabled` line to `true`, or writes `disabled true;` first in
  its braces (making `{ disabled true; }` on a node written without braces). A `disabled $quiet;`
  that other nodes share becomes `disabled true;` on that node alone, `$quiet` left as it was.
- `node.enable` removes the node's own `disabled` line, and braces left empty with it
  (`node white noise;` comes back).
- `node.update` never touches the `disabled` line, and a `disabled` key in given settings is
  `invalid-settings`; `node.add` takes `"disabled": true` beside its settings.

**`shared-definition`.** An edit that would have to change or remove a line that also makes other
nodes or routes is refused `shared-definition`, with that line's place. Removing `playback` from
`desk` above is refused, because that line sits in the snippet both cards use; so is a
`node.disable` whose `disabled` line sits in a shared snippet. Edit the text instead
(`graphFile.set`, or by hand), which changes every user in one judged commit.

### A node's own text

`node.setText` ([control-port.md](control-port.md#a-nodes-own-text)) is the edit a person makes:
it replaces one node's own statement, comments it owns included, with the text given, and leaves
every other byte as it was. It never edits where a value is defined: in the example above,
writing `node card desk { card MOTU; playback 4; }` for `desk` changes `desk` alone, and
`@set $ch` and the snippet stay as they are for `porch`. The text is checked as part of the whole
file, by the same reader, so a `$ch` or an `@use card_out USB2;` in it reads as it would by hand.
A node whose statement is written in a snippet (`@snippet two $n { node white $n; }` used at the
top level) has no text of its own: replacing it would change every node it makes, so it is
`shared-definition`, and the pane edits it.

**Judge before keep.** Every edit becomes a candidate text, is judged by the desired graph
against the whole wanted graph, and becomes the working copy only if accepted
([architecture.md](architecture.md#how-a-change-moves)). Nothing refused is kept or written, and an
edit cut short leaves the working copy whole.

## Hand edits win

ear watches the graph file folder (inotify: close-after-write, renames, creates, deletes).
Whenever `ear.conf` differs from what ear last read or wrote, ear reloads it, and any accepted
editor edits not yet written are dropped and counted (`droppedEdits` on the wire, `graph-file
dropped <n> edits` on standard output; the editor says so). A hand edit the graph refuses is shown
with its refusal, and the graph keeps running what it ran, including any dropped edit, so
`running` can name a version no file holds.

*Why:* there is one judge of the disk. The keeper alone decides whether the disk changed, by
comparing it with what it last read or wrote, whatever caused the difference.

## Writes

Every accepted edit is written back once the keeper has nothing waiting (**write-behind**), so
quick edits share one write and an edit can wait a few milliseconds behind a write. The reply to
an edit comes when the graph runs it, before the write; `graph-file written <version>` on standard
output says when it landed. An edit survives a restart of ear unless a hand edit lands first or ear
crashes before the write. A failed write sets `writeFailed` (cause `write`) and is retried after
the next accepted change; at stop, the last accepted version is written before ear exits.

## Settings applied only at restart

The `graph` block's `rate` and `block_size` are fixed at boot, and the engine runs at exactly
these. When the file cannot be read or does not give both, the engine takes `--rate` and
`--block-size`. A file whose two values differ from the engine's still runs, and says
`pendingRestart` (on the wire) or `graph-file pending-restart <keys>` (on standard output): they
apply at the next start. Nodes always see the engine's format.

## File safety

- `ear.conf` is never read or written through a symlink, and at most 1 MiB is read. While it is
  unreadable nothing is written.
- `graphFile.set` creates a missing file.
- ear writes by swapping a new file in (`renameat2(RENAME_EXCHANGE)`) and keeps it only if the
  file it replaced is what it last read or wrote, keeping the file's mode. Otherwise it puts the
  hand's text back and reloads.
- The folder must be writable by ear for edits to be saved. A read-only mount (a Kubernetes
  ConfigMap) cannot be edited, and a first boot without `ear.conf` runs nothing until the pane
  saves one.
- Every graph file change sends the whole text, comments included, to every subscribed client, so
  never put a secret in `ear.conf`.

## Traps

- **Editors that save in place** (truncate, then write) change the file before they finish. If ear
  reads it or writes in that moment, it sees an empty or partial file, counts that as a hand edit
  (dropping unwritten edits early) and may show errors until the editor's close event reloads the
  whole text. Editors that write a new file and rename it over `ear.conf` are seen once, whole.
- **In-place and `mmap` writers are not seen until they close the file**, and meanwhile ear's own
  write of unwritten edits can collide with them.
- **A raced reader can see ear's text for an instant**: a write swaps ear's text in before it
  compares what it replaced, and swaps the hand's text back if a hand save landed in between.
- **The watch event for ear's own write announces nothing**: the disk check compares content. A
  test waits for `graph-file written <version>`, never for a quiet period.
- **A message to the control port is at most 64 KiB**, so a text over about 64 KiB cannot be saved
  with `graphFile.set`, although ear reads up to 1 MiB from disk.

See [control-port.md](control-port.md#the-graph-file-over-the-port) for the `graphFile.*` calls and
[output-records.md](output-records.md#graph-file) for the `graph-file` records.
