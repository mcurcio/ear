# A node's own knobly text

**Not built.** This page is deleted when it is built; what lasts moves into `control-port.md`,
`graph-file.md`, `editor.md` and the `control-port` skill.

## The owner's words

> "Why is the editor box 'raw json' when knobly is the canonical raw language?"

Approved direction: the node panel's box shows and edits that node's own statement in knobly,
exactly as it is in `ear.conf` (units, `@use`, `$variables`, comments); ear checks the edited text
with the reader it uses for the file and replaces only that statement, the rest of the file left
byte for byte; diagnostics come back with line and column; `node.add` and `node.update` stay, as
the machine API. The rule: **a person edits knobly; JSON is the machine API.**

## 1. What the box shows

The node's whole statement, from its first owned token to its last, the comments knobly says it
owns included (a comment line right above it, a comment after it on its line): knobly's
`statement::place()` and `statement::text()`. So `node card desk { @use card_out MOTU; }` shows
as written, and so does `rate 44.1khz;` or `playback $ch;`: a `@use` line and a `$variable` are
text in the statement and stay text; the `@snippet` and `@set` lines they name are not in the box
and are never changed by it. *Why the whole statement and not the block body:* the type is part of
what a person reads and may change, and the box then holds one complete knobly statement that
reads alone; the name in it is the box's anchor (below).

**A node with its own text** is one whose statement is written at the top level of `ear.conf` and
read once (no parent statement, one copy). Every other node is **shared**: its statement sits in a
`@snippet` that top-level `@use` lines splice in, so one text makes several nodes or carries
`$params`. The box shows a shared node's statement read-only, with where it is written, and says
to edit it in the pane. Replacing it from one node's box would change every node it makes, which
is what `shared-definition` already refuses.

*What this changes from `node.update`:* `node.update` edits values where they are defined, so
`playback 4` on `desk` rewrites `@set $ch` and changes both cards, and removing a key a snippet
writes is `shared-definition`. Replacing a node's own text never touches another line: a person who
writes `playback 4;` in `desk`'s box in place of `@use card_out MOTU;` changes `desk` alone. The
only `shared-definition` left for the box is a shared node.

## 2. The request: `node.setText {name, text, version}`

- **What it may change:** anything in the statement: settings and their spellings, `@use` lines,
  `$variable` uses, comments, `disabled true;`, and the type (the graph replaces the node, as a
  hand edit of the type does). **A rename is refused:** routes name the node, so renaming its
  statement alone would leave every route naming a node that is gone; a rename is made in the pane,
  where its routes are renamed in the same text.
- **How it is checked:** the text, with whitespace before and after it dropped, replaces the
  statement's byte range in the working copy's text; that candidate is loaded and read whole by
  the file's own reader (`GraphFileModel::load`), so the file's `@set` and `@snippet` lines apply
  exactly as they would to a hand edit. The candidate must then hold, where the box's text went,
  exactly one top-level statement, a `node` with the box's name; otherwise it is `invalid-text`
  with an ear diagnostic: `ear/not-one-node` (zero statements, several, or not a `node`: "the text
  must be one node statement: node <type> <name> { ... }") or `ear/node-renamed` (at the name:
  "this text edits node desk: rename it in ear.conf, where its routes are renamed with it").
  Removing a node stays `node.remove`. A candidate that reads is judged by the desired graph like
  any edit (judge before keep), so a type change whose ports a route names gets the graph's code.
- **Invalid text:** `invalid-text` with `data.diagnostics` placed **in the box's text** (line and
  column from 1, UTF-16 columns, the box's first line is 1). A diagnostic the edit caused
  elsewhere (in the body of a snippet the box `@use`s) is placed at the box's 1:1 and its message
  starts with where it is in `ear.conf` as it stands ("ear.conf 7:5: ...").
- **Versioning:** `version` is required, as for `graphFile.set` (HTTP's `If-Match`): the box's
  text was cut from that version, so a stale one is `file-changed`, whatever changed. The server
  stays strict and simple; the editor keeps the person's draft working across unrelated changes
  (section 3). Checks in order: `stopping`, `file-changed`, `file-invalid` (the working copy has
  errors: fix it in the pane, as for every node edit), `unknown-node`, `shared-definition` (with
  the shared statement's place in `ear.conf`), then the text's own checks. The same text as now is
  `{}` and changes nothing.
- **Result and events:** `{}` once the graph runs it, after `graphFile.changed` (cause `edit`) and
  the node events it causes (`node.updated`, or a replace), as for every edit; written behind.

## 3. How the text reaches the editor

The editor already holds the whole text in the GraphFile. So the GraphFile gains one member,
`nodeTexts`, saying where each node's statement is in that text, never the text again:

```ts
type NodeText = { name: string; start: {line, column}; end: {line, column}; shared?: true };
```

`end` is one past the statement's last character. It is present whenever the file reads without
errors (`[]` with no nodes) and absent otherwise, since node edits are refused then. A shared
node's entry points at the statement in its snippet; several names may share one range. The editor
slices `text` with the pane's `offsetOf`. *Why not `node.getText`:* a request is a round trip per
selection and per file change, and it would send a slice of a text the client already holds.
*Why not on the Graph's node:* the box's text must come from the same version `node.setText`
sends, and the Graph does not carry the file's version.

**The editor's draft** is kept as the pane's is: a draft is never overwritten. When a new
GraphFile arrives and the node's own text in it is the text the draft started from, the draft moves
onto the new version silently (the change was elsewhere); when the node's text changed and the
draft is dirty, the box goes stale ("desk changed in ear.conf; reload"), and a `file-changed` reply
does the same.

## 4. The box as a knobly editor

The pane is a plain monospace `<textarea>` with its diagnostics listed under it (a click puts the
caret there) and no highlighter, so the box is the same: the same textarea style and the same
diagnostics list, with Save sending `node.setText`. No knobly grammar in `web/`; ear judges every
text. A highlighter, if one comes, comes to both at once.

## 5. Open set and compatibility

Protocol 1 stays. The feature **`node-text`** adds `nodeTexts` on the GraphFile, `node.setText`,
and the diagnostic codes `ear/not-one-node` and `ear/node-renamed`. A client sends `node.setText`
only when `graph.get`'s features hold `node-text`; without it the editor's box falls back to the
JSON settings and `node.update`. `node.add` and `node.update` are unchanged.

## Left for later

- The add-node form still takes a type and JSON settings; a knobly add (a statement appended) would
  follow the same rule and is not asked for yet.
