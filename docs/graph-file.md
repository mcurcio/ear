# The graph file, `ear.conf`

`ear.conf` is the one file that says which nodes exist, with what settings, and which routes join
their ports. It lives in the graph file folder (`--config-dir`, default `config`), it is written
in the knobly configuration language ([mcurcio/knobly](https://github.com/mcurcio/knobly)), and
ear keeps it live: edits made through the control port are applied and written back into it with
comments and layout kept, and hand edits are applied as soon as they are saved. It is not
committed to the repository. The code is `src/config/`.

## An example

```conf
graph_frequency 48000;
block_size 256;
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

Only ear's four directives are allowed at the top level.

| Directive | Form | Rules |
|---|---|---|
| `graph_frequency` | `graph_frequency <Hz>;` | Required, once; a whole number from 1 to 4294967295 |
| `block_size` | `block_size <samples>;` | Required, once; the same range |
| `node` | `node <type> <name>;` or `node <type> <name> { <key> <value>; ... }` | One type, one name, an optional block of settings, each key once |
| `route` | `route <node>:<port> <node>:<port>;` | From an output to an input; no block |

Anything else is `ear/unknown-directive`, a malformed directive is `ear/bad-directive`, and a
missing `graph_frequency` or `block_size` is `ear/missing-directive`.

**Node names** are unique, 1 to 64 characters of letters, digits, `-` and `_`. *Why no `.`:*
engine objects a node makes are named `<node>.<suffix>` (`desk.playback`, `fpp.rtp`), so they can
never collide with a node.

**Ports** are named `<bank>.<member>`, and a route names one by its whole name,
`<node>:<bank>.<member>` (`route kitchen:from.left desk:play.1;`). Each type's banks are listed
in [node-types.md](node-types.md#ports-at-a-glance). Older names (`MONO`, `FL`, `playback_1`) are
`unknown-port`; no compatibility is kept.

### Knobly's features

- **Comments** (`# ...`) and layout survive every edit ear makes.
- **Variables:** `@set $name value;`, used as `$name`.
- **Snippets:** `@snippet name $param { ... }`, used inside a node's block as `@use name arg;`.
- **`@include` is refused** (`ear/include-unsupported`: "@include is not supported yet: write the
  whole graph in ear.conf"), so no other file is ever read.

### Settings and their kinds

Each setting is read and written as the kind its node type declares: **text** (a bare word or a
quoted string) or a **whole number**. A key the type does not declare is invalid settings. The
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
| `ear/unknown-directive` | A directive other than the four |
| `ear/bad-directive` | A malformed directive, a setting given twice, a bad `disabled` line |
| `ear/missing-directive` | No `graph_frequency` or no `block_size` |
| `ear/too-large` | An edit's or a `graphFile.set`'s text over 1 MiB (answered `invalid-text`) |
| `ear/unwritable-edit` | An edit knobly could not write (answered `invalid-text`) |

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
- Added nodes and routes are appended at the end.
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

`graph_frequency` and `block_size` are fixed at boot, and the engine runs at exactly these. When
the file cannot be read or does not give both, the engine takes `--graph-frequency` and
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
