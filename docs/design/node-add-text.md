# Adding a node in knobly

**Not built.** This page is deleted when it is built; what lasts moves into `control-port.md`,
`graph-file.md`, `editor.md` and the `control-port` skill.

## The owner's rule

"A person edits knobly; JSON is the machine API" ([control-port.md](../control-port.md#a-nodes-own-text)).
The node panel's box follows it; the "Add a node" form still takes a type, a name and JSON
settings. This page makes the form take knobly too, so no place where a person types in the editor
takes JSON. `node.add` stays as it is, for scripts.

## 1. What the form takes

One whole statement, `node <type> <name> { ... }`, comments it owns included, typed in the pane's
textarea style. Choosing a type writes a **template** into the form while the person has not typed
over the last one: `node <type> <name> {\n}`, the name the type's word and the first free number
(`file1`, `white2`) among the nodes the editor holds. The template is a string the editor builds;
`web/` still has no knobly grammar, and a type's settings are not on the wire yet (slice 5), so the
block starts empty.

## 2. The request: `node.addText {text, version}`

- **Appended as `node.add` appends:** the text, whitespace around it dropped and its lines written
  with the file's own line ending, is added after the last top-level statement through knobly's
  written layer (`add_statements`, the same call `node.add` makes), so comments and layout are
  kept, and nothing else in the file changes.
- **Checked in context:** knobly parses the text alone first (so it can never run into the rest of
  the file); then the whole candidate file is read by the file's reader, so `@set` and `@snippet`
  apply as they do by hand. The text must be exactly one `node` statement: otherwise
  `invalid-text` with `ear/not-one-node`. A name already taken, a type ear does not know or
  settings the type refuses come back as the graph's codes (`duplicate-node`,
  `unknown-node-type`, `invalid-settings`), as for any edit.
- **Diagnostics placed in the form's text,** as `node.setText`'s are: knobly's syntax errors from
  parsing the text alone, and the reader's from the candidate, mapped back into the text sent.
- **Versioning:** `version` is required and strict, as for `graphFile.set` and `node.setText`: the
  text was written against the file's `@set` and `@snippet` lines at that version. The editor sends
  the newest version it holds when Add is pressed, so `file-changed` only answers a change racing
  the request; the form keeps the text and says to press Add again. Checks: `stopping`,
  `file-changed`, `file-invalid`, then the text's.
- **Accepted:** `graphFile.changed` (cause `edit`), `node.added` and its states, then `{}`; written
  behind.

## 3. The feature

**It extends `node-text`.** `node-text` is not in any release yet: it came with the open pull
request this change joins, so no ear announces `node-text` without `node.addText`, and one feature
keeps one gate for the editor's two knobly boxes. The wire stays additive to every released ear,
and protocol 1 stays. *If `node-text` were released first,* this would be a new feature name
(`node-add-text`) instead.

## 4. Fixtures and tests

`node-text.json` grows: an add that succeeds (with a comment and `$`-free settings), a stale
version, a text with a syntax error (placed in the text), two statements, and a name already
taken; the system test replays it against the real ear. Model tests cover appending as `node.add`
does, `@set` in the text, CRLF, and the text parsed alone.
