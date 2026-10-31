# Knobly config: the owner's words

Verbatim, dated. The one home for what the owner said about this batch.

## 2026-10-30, the request

> use the mcurcio/knobly project as the config language, and enable bi-directional config edits
> in real-time via the UI

## 2026-10-30, scope answers (picked from offered options)

These are picks among options Claude framed; each is a revisable choice, not a stated rule.

| Question | Option picked | What the option said |
|---|---|---|
| What should "bi-directional" cover? | Also a text pane | Editor edits are written into the knobly file at once (comments and layout kept), edits made to the file by hand are applied to the running graph and shown in the editor live, plus the editor shows the knobly file's text and lets you edit it in the browser |
| A setting whose value came from a variable, snippet or `@include` | Edit at the source | Change the variable or included file where it is defined, so every node using it changes too |
| Should the JSON graph file go away? | Knobly only | `ear.json` and its parser are deleted; ear reads `ear.conf`. No compatibility path |

## 2026-10-31, gate answers (design gate page, saved 04:18 UTC)

| Question | Option picked | Note (verbatim) |
|---|---|---|
| Q1 Which thread does the keeper's work? | Its own thread | "The flow should be closer to:<br>* open the file and get a graph object<br>* watch the graph object for changes (from direct writes)<br>* when app wants to make a change, manipulate the graph object<br>* background thread periodically flushes the graph object back to disk" |
| Q2 ear restarts while the file on disk has errors | Start with nothing running and show the errors | none |
| Q3 The editable text pane and an open port | Editable now (accepting that anyone who reaches the port can exhaust ear's memory) | none |

## 2026-10-31, follow-up on the Q1 note (picked from offered options)

| Question | Option picked | What the option said |
|---|---|---|
| "periodically flushes": which did you mean? | After each change | Each edit to the graph object wakes the background thread, which writes the file as soon as it is free. Edits never wait on the disk; no timer. Several quick edits may go out in one write |
| Unwritten editor edits collide with a hand edit: who wins? | The hand edit | ear reloads the file; the editor edit that had not been written is dropped and the editor says so |

## 2026-10-31, scope at sizing (picked from offered options)

| Question | Option picked | What the option said |
|---|---|---|
| Which scope to build (the full design sized at 7 slices, about 12-14 hours of agent run) | Lighter: one thread, one file | The keeper, the writer and the file watch share one background thread. `ear.conf` alone for now: `@set` and snippets work and are edited at the source; `@include` is refused with a clear message and comes in a later batch. Cost: an edit can wait a few milliseconds behind a write, and no includes yet |
| The engine's rate and block size when `ear.conf` cannot be read at boot | Flags with defaults | `--graph-frequency` and `--block-size` flags (default 48000/256) for this case only |
