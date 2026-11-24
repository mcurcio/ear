# The graph editor

ear's control port (7777) serves a graph editor at `/`: a browser page that draws the running
graph and edits it through the same JSON-RPC calls a script uses
([control-port.md](control-port.md)). Its source is `web/` (React, React Flow, Vite, TypeScript,
Vitest); it shares no code with the C++, only the wire format and the fixtures in `test/protocol/`.

## What it shows and does

The page is a top bar, the canvas with an inspector on its right, and a full-width `ear.conf` drawer
along the bottom ([below](#the-shell)).

- **Nodes** are 220 px boxes ([below](#the-node-box)): a stripe down the left edge and a pill (a
  dot and the word) in the state's colour, the type as a small label beside the name, the line
  that says why the node is silent or what it waits for ([below](#a-nodes-health)), and beside the
  pill a warning mark while the node lost cycles in the last minute ([below](#counters)). Then one
  row per port, inputs on the left and outputs on the right, each with its handle and a live level
  meter ([below](#meters)), and along the bottom edge a badge per companion ([below](#companions)).
- **Routes** are wires coloured by status (`linked`, `pending`, `refused`) and drawn by their
  from port's level: louder is thicker and brighter. A linked wire carrying signal shows a slow
  flow, a refused one says `refused` at its midpoint, and hovering a wire names its two ends
  ([below](#wires)).
- **Editing:** a drag from an output to an input adds a route (any other drag is refused before
  it reaches ear); while a wire is drawn, every handle it can be dropped on is ringed and the rest
  dimmed ([below](#the-node-box)); the delete key removes the selected wire or node; the inspector
  adds nodes from a statement in knobly (its Graph subject, [below](#adding-a-node)), edits the
  selected node's own text in knobly (its Node subject, [below](#the-node-panels-text)), both as
  JSON settings from an ear without `node-text`, and offers Start, Stop, Restart, Disable and
  Enable in the inspector when ear has the `lifecycle` feature. The top bar gives the graph's rate
  and block size ("48 kHz · 256 frames", from `graphFrequency` in hertz) and realtime, or a "not
  realtime" badge, and a dropouts badge while the host cut cycles in the last minute; the inspector
  holds the graph's counters and the selected node's ([below](#counters)).
- **The ear.conf drawer** holds the pane: `ear.conf`, highlighted ([below](#highlighting)), with
  its diagnostics (a click puts the caret there) and Save (`graphFile.set`, based on the version
  the draft started from). Banners say
  when the graph refused the file, a write failed, editor edits were dropped by a hand edit, or a
  setting applies only at restart (the graph rate or block size, as `pendingRestart` names them).
- **The theme** is light, dark or the system's (the default), switched by the button in the top
  bar, which cycles system, light and dark ([below](#tokens-and-themes)).

## Its rules

- **ear is the one source of truth.** The editor draws only what ear announced: a node or wire
  appears on its notification, never on the request that asked for it, and an event naming a node
  or route it does not know changes nothing. It never parses knobly; ear judges every text. Its
  highlighting only colours the text ([below](#highlighting)).
- **Every enum is open.** A value or member this editor does not know is drawn by its word,
  neutrally, or ignored, never a failure ([control-port.md](control-port.md#protocol-and-features)).
  A newer protocol than the editor knows shows a chip in the top bar ([below](#the-top-bar)).
- **A person edits knobly; JSON is the machine API.** What a person types about a node is its
  statement as `ear.conf` has it, in the add-node form, the node panel's box or the drawer's pane;
  JSON settings are for scripts (`node.add`, `node.update`), and the inspector shows them only to an ear
  without the `node-text` feature.
- **A draft is never overwritten.** A new `ear.conf` arriving while the drawer's pane has unsaved
  changes makes the pane stale ("ear.conf changed; reload") until the person reloads; ear refuses a Save
  over a file that changed meanwhile (`file-changed`). The node panel's box does the same for its
  node's text.
- **Node positions live in the browser.** Unplaced nodes are laid out with dagre (sources left,
  sinks right, following the routes); a place the user dragged a node to is kept in the browser
  only, never sent to ear. Tidy lays every node out again, in the browser only.
- **The counters are kept as sent.** With the `counters` feature the graph store keeps the
  graph's counters and each node's whole, from the snapshot and `node.added`, and replaces those a
  `counters.changed` names ([control-port.md](control-port.md#counters)); an unknown counter is
  kept with them. How they are drawn: [below](#counters).
- **Companion lists are kept as sent.** With the `companions` feature the graph store keeps
  each node's `companions` as sent, from the snapshot and `node.added`, and a
  `node.companionsChanged` replaces the named node's whole list
  ([control-port.md](control-port.md#companions)); an unknown kind or member is kept as sent.
  How they are drawn: [below](#companions).
- **Levels stay outside React state.** Each `levels` notification replaces a store with
  listeners per port, so at 30 messages a second only the meters and wires whose port changed
  re-render. A meter reads the store itself and paints through refs, and keeps its peak hold in
  its own object, so neither is React state ([below](#meters)). A closed connection makes every
  meter read silent and forgets every hold until new levels arrive.
- **A lost socket reconnects** after a growing wait (1 s doubling to 30 s), failing the calls in
  flight, and the new socket subscribes again (graph, graph file, levels), so the editor starts
  over from a fresh snapshot.

## The shell

`App.tsx` composes it: `TopBar`, `Canvas` (with `CanvasTools`), `Inspector` and `Drawer`, with the
canvas's state in `useCanvas` and the drawer's in `useDrawer`, so the top bar and the drawer's
header read one status.

### The top bar

44 px across the top, holding what is about the whole graph and never about a selection, from left
to right:

- the wordmark (hidden below 600 px);
- the connection pill: `live`, `connecting…` or `lost: retrying in 4 s`, a word beside its dot,
  the seconds counted down to the client's next socket, as `rpc.ts` tells the editor (`onRetry`);
- the engine chip: the rate and block size, then `realtime` or the "not realtime" badge, its
  tooltip the scheduling;
- the dropouts badge ([Counters](#counters));
- a `protocol N` chip while ear speaks a newer protocol than this editor's, its tooltip saying
  some of it is not shown;
- on the right, the theme switch and the `ear.conf` toggle with the drawer's status words as a
  chip;
- below 900 px, an Inspector button ([below](#narrow-windows)).

`shell.ts` holds the words (`shell.test.ts`).

### The canvas

It fills the rest, a quiet dot grid in the theme's colours. React Flow's `Controls` give way to one
floating tool group, bottom left: zoom out, zoom in, fit, tidy and the minimap toggle. Tidy lays
every node out again with dagre, as on first placement, and they slide there over
`--motion-slow`; moves are the browser's, so nothing is sent (`layout.ts`'s `relayout`). The
minimap is React Flow's `MiniMap`, off by default, its boxes in their state's token. A refused
request shows one banner at the canvas's bottom right until dismissed.

### The inspector

On the right, 320 px, dragged by its left edge (or, focused, its separator with the arrow keys,
Home and End) between 280 and 560 px. It shows one subject, chosen by the selection
(`selection.ts`, `selection.test.ts`):

- nothing selected: **Graph**: the engine facts in full, the graph's counters and the add-node
  form;
- one node: **Node**: its name and type, its state pill as the box draws it, its line, the
  lifecycle bar, its knobly box (or JSON settings), its counters, its companions and Delete;
- one route: **Route**: from and to as `node:port` buttons that select that node, its status in
  words, its level meter and Remove;
- several: a count ("3 nodes, 2 routes") and Delete for them all.

A name or route the graph does not hold is ignored, so the inspector shows only what ear announced.
Delete asks `node.remove` for each node and `route.remove` only for routes between kept nodes, as
the delete key does. The add-node form lives only in the Graph subject, so selecting something
drops a half-typed statement; a new template is written when it shows again. Slice 5's settings
form and status list are designed to land in the Node subject, under the lifecycle bar
([control-plane.md](design/control-plane.md#8-the-editor)).

### The drawer

Full width along the bottom, holding the pane whole. Its 32 px header stays when the drawer is
closed and says in words what the file needs, most urgent first: `refused`, `write failed`,
`stale: reload`, `unsaved`, `n errors` or `applies at restart` (`drawer.ts`, `drawer.test.ts`),
with Save beside them. The header's toggle, and the top bar's, opens and closes it. It opens by
itself only when ear refuses the file or a write fails, each newly (the same refusal sent again
does not reopen it), or when a Save is refused; never on a hand edit. *Why a drawer:* the owner
chose a full-width bottom drawer over a third column, so lines need not scroll sideways and the
canvas keeps its width for a left-to-right graph. Its body is dragged by its top edge between
120 px and 70 % of the window. Closing it hides the body but keeps it, with the draft, the caret
and the scroll. The inspector and the drawer never move each other's content.

### Narrow windows

Below 900 px the inspector is a sheet over the canvas's right edge. It opens when something is
selected or from the top bar's Inspector button, and closes on Escape or a click on the canvas.
The drawer keeps its place. `sizes.ts` holds the bounds and the breakpoint, and `sizes.test.ts`
holds `editor.css` to them.

The sizes, the drawer's open state and the minimap last as long as the page.

## Adding a node

`web/src/addText.ts` is the add-node form's logic (`addText.test.ts` replays `node-text.json`'s
refusals through it); `components/AddNodeText.tsx` draws it. With the `node-text` feature the form
takes a whole statement, `node <type> <name> { ... }`, in the drawer's textarea style, and Add sends
`node.addText` ([control-port.md](control-port.md#a-nodes-own-text)): ear appends it where
`node.add` would, reads it with the whole file (so `$ch` and `@use card_out MOTU;` work as by
hand), and places its diagnostics in the form's text. The rules:

- **A template per type.** Choosing a type writes `node <type> <name> {\n}`, the name the type's
  word and the first number no node holds (`file1`, `white2`); the block starts empty, since a
  type's settings are not on the wire. The template follows the chosen type, and the nodes as they
  change, only while the text is still the last template: what the person typed is never written
  over.
- **Add sends the newest version the editor holds.** `file-changed` then means ear.conf changed
  while the request was on its way; the form keeps the text and says to press Add again.
- **A node added gives a fresh template.** The node appears when ear announces it; the form's
  diagnostics are listed under it as the pane's, and a click puts the caret there.

## The node panel's text

`web/src/nodeText.ts` is the box's logic (`nodeText.test.ts` replays `test/protocol/node-text.json`
through it); `components/NodeTextBox.tsx` draws it. The node panel is the inspector's Node subject
([above](#the-inspector)). With the `node-text` feature
([control-port.md](control-port.md#a-nodes-own-text)) the inspector's Node subject holds its
statement as `ear.conf` has it, units, `@use` lines, `$variables` and the comments it owns included
(`# the take` above `node file rec { path "take1.wav"; }`), in the drawer's textarea style, with Save
and its diagnostics under it. *Why:* asked why the box was raw JSON when knobly is the canonical raw
language, the owner chose the node's own knobly statement, checked by ear's reader and replacing
only that statement. The rules:

- **Cut from the file the editor holds.** The text is the GraphFile's `text` between the node's
  `nodeTexts` places; nothing is fetched and the browser parses no knobly. It is highlighted as the
  pane is ([below](#highlighting)). Its `\r\n` become `\n`, as a textarea holds them, so an unchanged box on a
  CRLF file is not dirty; ear writes the file's own ending into what Save sends.
- **Save is `node.setText`** with the draft and the version it was cut from. ear answers once the
  graph runs it; the box then shows ear's text from the new GraphFile. `invalid-text`'s diagnostics
  are placed in the draft, listed under it like the pane's, and a click puts the caret there.
- **A draft lasts across changes elsewhere.** When a new GraphFile holds the node's text exactly as
  the draft started from (a route drawn, another node's edit, a hand edit elsewhere), the draft
  moves onto the new version untouched, so Save does not meet `file-changed` for a change that is
  not its node's. When the node's own text changed under a dirty draft, or ear answers
  `file-changed`, the box goes stale ("rec changed in ear.conf; reload") and keeps the draft until
  the person reloads.
- **Read-only when it cannot be replaced.** A node written in a snippet (`shared`) shows that
  statement on the sunken surface, its label saying "(read-only)", with where it is and that the pane
  edits it; while `ear.conf` has errors there are no `nodeTexts` and the box says to fix them in
  the pane. Neither offers Save. The text keeps its full contrast: it is never dimmed.

## Highlighting

The pane, the node panel's box and the add-node form draw their knobly highlighted, in one component,
`components/KnoblyText.tsx`, over the pure `web/src/highlight.ts`. *Why display only:* the owner
decided that ear's C++ reader stays the one judge of an edit and the source of every diagnostic, so
the browser's knobly never accepts, refuses or diagnoses a text. The rules:

- **The textarea holds everything.** Each box is still a plain textarea: the text, the caret, the
  selection, every edit, and so the diagnostics' caret placement, the CRLF handling, the drafts'
  stale and dirty logic and what Save and Add send are as above. Its glyphs are transparent; a
  highlighted copy of what it shows is drawn behind it (a `pre`, hidden from screen readers) with
  the same font, size, line height, padding, border and tabs and no wrapping, and follows its
  scroll. No editor library is added. Under forced colours (Windows high contrast) the copy is
  hidden and the textarea draws its text in the system's colours.
- **The copy is what the textarea shows.** A textarea holds `\n` only, so the copy is the value
  with each `\r\n` and lone `\r` as `\n`; the value itself is never changed. `highlight.test.tsx`
  checks that the runs drawn join to exactly that text and that the textarea, the pane's Save, the
  box's Save and Add get the caller's text unchanged, CRLF and non-ASCII text included.
- **knobly's classes, the editor's palette.** Each token is drawn by knobly's highlight class
  (`directive`, `keyword`, `word`, `variable`, `number`, `string`, `comment`, `punctuation`,
  `space`, `error`), each a token (`--knobly-directive`, ...) in `theme.css`, light and dark
  ([below](#tokens-and-themes)), each shade at 4.5:1 or better on the box's surface and on the
  read-only one in both themes, which `theme.test.ts` checks. A class this
  editor does not know is drawn plainly. An `error` span is a token that does not lex (a stray
  character, an unterminated string), never a diagnostic: those are ear's, listed under the box.

**The package.** `knobly/highlight` from knobly's first release, v1.0.0 (MIT): a scanner generated
from knobly's grammar, with no parser runtime; with `KnoblyText` it adds about 3.4 KB to the
editor's script (1.5 KB gzipped) and 2 KB to its styles.
`web/package.json` names the release's npm tarball by URL,
`https://github.com/mcurcio/knobly/releases/download/v1.0.0/knobly-1.0.0.tgz`, and
`web/package-lock.json` records its SHA-512, so `npm ci` installs exactly that file, from npm's
cache offline. *Why a URL:* knobly is published only on its GitHub releases and on GitHub Packages,
and GitHub Packages asks for a token even to read a public package, which `npm ci` in CI and in the
dev image has none of. The tarball's `package.json` depends on the ANTLR runtime (`antlr4`
4.13.2, from npm), so `npm ci` installs it, but `knobly/highlight` never imports it and the bundle
holds none of it.

That URL and SHA-512 are copies of knobly's one pin, `cmake/knobly.json`, which ear's reader is
built from too: one version for both, checked and moved as
[AGENTS.md](../AGENTS.md#build-run-and-test) says. The highlighter tokenises by knobly's grammar,
so with one version on both sides it matches what ear's reader parses.

## Tokens and themes

`web/src/theme.css` holds every colour, radius, shadow and duration the editor draws with, as
custom properties on `:root`, and is loaded before `editor.css`. `editor.css`, the components (the
wires' status colours are `editor.css`'s wire classes) and the knobly tokens use only these, and React Flow's
own variables (`--xy-*`) are pointed at them, so its canvas, dot grid and minimap follow the theme
without its `colorMode` prop (which would fight `data-theme`). The values live in `theme.css`
alone; the groups:

- **Surfaces:** `--surface` the canvas, `--raised` boxes, panels and controls, `--sunken` meter
  tracks, badges and read-only text; `--line` and `--line-strong` the borders and the dot grid.
- **Text:** `--text`, `--muted` (type labels, the health line, counter labels), `--accent`
  (selection, focus) and `--on-strong` (text on a filled chip: the "not realtime" badge, the
  warning mark).
- **States:** `--ok` running, linked and ready; `--warn` starting, stopping, pending, an unknown
  state and the warning mark; `--err` failed, refused and the diagnostics' errors; `--quiet`
  stopped; `--off` disabled and a companion not ready. Asked-for silence is never `--err`
  ([above](#a-nodes-health)).
- **Soft surfaces** under banners and the dropouts badge (`--warn-soft`, `--err-soft`), the
  meters' (`--meter-low`, `--meter-mid`, `--meter-hot`, `--meter-peak`), the shadows (`--shadow`,
  `--shadow-lifted`) and the knobly tokens ([above](#highlighting)).
- **Tones:** the classes `tone-ok`, `tone-warn`, `tone-err` and `tone-muted` colour the
  connection pill's dot, the top bar's chips and the drawer's status words from the state tokens.
- **Corners and motion:** 8 px on boxes, 6 px on controls and text boxes, pills fully round; 120 ms
  for hover and selection, 180 ms for what slides, `--motion-pulse` (2 s, the configuring dot) and
  `--motion-flow` (1 s, the wires' flow). The first two are 0 under
  `prefers-reduced-motion: reduce`, which also stops the configuring pulse and the wires' flow
  (`editor.css`: an infinite animation at 0 s would churn rather than stop), and leaves the peak
  tick's fall, which is data, not decoration.

**The theme.** It is `system` by default, following `prefers-color-scheme`. *Why:* the owner chose
that the editor follows the system until the person chooses. The switch cycles system, light and
dark and writes `data-theme="light"` or `"dark"` on `<html>`; system removes the attribute.
`web/src/theme.ts` is its logic, and `index.html`'s `color-scheme` meta lets the first paint follow
the system before the styles arrive. The switch sits in the top bar ([above](#the-top-bar)); the
choice lasts only as long as the page: a reload returns to the system's.

**Contrast is a test.** `theme.test.ts` reads `theme.css` and fails naming the theme and the pair
when any text, state, on-strong or knobly token falls below 4.5:1 on a surface it is drawn on, in
either theme. It also fails when the two copies of the dark set (the system's and the chosen one)
differ, when a colour lacks its dark value, or when `editor.css` or a component holds a colour of
its own. A failing pair changes the value, not the rule. The dark set is written twice rather than
with `light-dark()`, because the build's CSS minifier may rewrite that function. Vitest blanks
stylesheets by default, so `vite.config.ts` lets it read these two as text.

Forced colours keep the system's colours, as [Highlighting](#highlighting) says.

## The node box, wires and meters

### The node box

`components/EarNode.tsx` draws it; the handles' rule is `web/src/connect.ts`'s `handleCue`.

- **The state is a stripe and a pill.** A 4 px stripe down the box's left edge and a pill (a dot
  and the word, with a failure's code in monospace) take the state's token: `--ok` running,
  `--warn` starting, stopping and an unknown state, `--err` failed, `--quiet` stopped, `--off`
  disabled. The word is always there, so the state is never colour alone. A failed box also has an
  `--err` border. A disabled one has a dashed border and a hatched stripe, and its text stays at
  full contrast. `configuring` pulses the dot only, never the text, and not at all under reduced
  motion.
- **The type label** is the type's word as sent, in `--muted`. Nothing in `web/` names a node type,
  so there are no per-type icons or colours.
- **220 px wide.** `layout.ts`'s `nodeWidth` and `portHeight` are the box's CSS width and port-row
  height, which `layout.test.ts` holds equal. `headerHeight` is dagre's estimate of the three
  header rows. Port names are monospace and numbers use tabular figures.
- **Handles** are a 10 px dot with a 20 px hit area. While a wire is drawn, `handleCue` rings in
  `--accent` every handle a drop on would make a route and dims every other: from an output, every
  input; from an input, every output (the editor accepts a drag from either end). It is
  `isValidConnection`'s own rule, so the ring never promises what the drop refuses. The ringed
  handle under the pointer grows.
- **Selection** is a 2 px `--accent` ring outside the border with the lifted shadow. Hover lifts
  the shadow alone.

### Wires

`web/src/wires.ts` holds the rules (`wires.test.ts`); `components/LevelEdge.tsx` draws them.

- **Colour by status, size by level.** Colour comes from the status class over `--ok`, `--warn`
  and `--err`; an unknown status is drawn as pending. Width is 1.5 px plus 5 px at full scale, and
  1.5 px more when selected. Opacity is 0.45 plus 0.55 at full scale. Every status but `linked` is
  dashed and still.
- **The flow.** A linked wire whose from port's rms is above the -60 dBFS floor shows a dash moving
  from the output to the input at a fixed speed (`--motion-flow`). It means "signal", not how
  loud. There is none under reduced motion, and none on pending, refused or an unknown status.
- **The status in words.** A refused wire carries `refused` at its midpoint (React Flow's
  `EdgeLabelRenderer`). A status this editor does not know carries its own word, in `--muted`.
- **Hover** thickens the wire by 1 px. Its tooltip names the two ends
  (`noise:noise.mono → rec:record.right`) and, for a refused route, ear's detail.

### Meters

`web/src/meters.ts` holds the rules (`meters.test.ts`); `components/Meter.tsx` paints them.

- **The scale is unchanged:** an rms bar and a peak tick, dBFS down to -60 (`levels.ts`).
- **Three zones.** The bar uncovers three fixed zones on that scale: `--meter-low` below -18 dBFS,
  `--meter-mid` from -18 to -6, `--meter-hot` above.
- **Peak hold.** The tick holds its highest peak for 1.5 s, then falls at 20 dB/s, never below the
  live peak and never below the floor.
- **Clip lamp.** A peak at or over 0 dBFS (linear 1) lights the meter's right end for 2 s, and the
  tooltip says "clipped".
- **Outside React.** The meter subscribes to the level store, keeps its `PeakHold` in a ref, and
  paints the bar, the tick, the lamp and the tooltip through refs. It animates on animation frames
  only while the tick is above the live peak or the lamp is lit. A closed connection resets the
  hold.
- **Times** are taken on the browser's monotonic clock, as [Recent marks](#recent-marks) does.

## A node's health

`web/src/health.ts` is the one place that turns a node's health members
([control-port.md](control-port.md#node-health)) into the box's pill word, stripe style, line and enabled
buttons; its tests cover every phase. The rules it follows:

- **Silence the operator asked for is never drawn as a fault.** `stopped` ("stopped until Start or
  a restart of ear") and `disabled` ("disabled in ear.conf") each have their own quiet style, apart
  from `failed`'s.
- **The box always says why a node is silent:** what it waits for (from `waitingFor`: "waiting for
  fpp.rtp", "waiting for the previous desk to stop", "waiting for the engine"), why it is
  stopping or configuring (from `cause`: "restarting: settings changed (card)", "configuring:
  trying again"), or a failure's detail.
- **Fast phases do not flicker:** a starting phase's line shows only once the editor has seen the
  phase for 250 ms, measured on the browser's clock, never against the server's `since`.
- **Wires stay drawn in every phase,** since the graph keeps them linked; a wire from a silent
  node reads zeros, which its meters show. One node's phase changes nothing on another's box.
- **Actions follow the phase:** nothing while `stopping`; Start and Restart are refused when the
  block is lost (`blockLost`: restart ear); Start, Stop and Restart are refused while disabled
  ("enable it first"); a failed node offers Start ("try again"). A phase this editor does not know
  is drawn by its word and offers only what cannot harm.

## Companions

`web/src/companions.ts` is the one place that turns a node's companion list
([control-port.md](control-port.md#companions)) into the box's strip and the node panel's lines;
`companions.test.ts` covers its rules and replays the `companions` and `unknown-values` fixtures
through the reducer into the badges drawn. *Why badges:* asked what the editor shows of a node's
hidden helpers, the owner chose small badges in the node's box, one per helper, its kind and a dot
for ready, its drop count on hover; never boxes or handles of their own. The rules:

- **A badge per companion,** in list order, along the box's bottom edge under the ports: its kind
  as a short word (`card-playback` "card out", `card-capture` "card in", `rtp-receiver` "rtp",
  `sink`, `source`) and a dot, filled (●) when `ready`, hollow (○) with dimmed text when not. A
  kind this editor does not know is drawn by its own text, in the same neutral style. No list, or
  an empty one, draws no strip (`white`, `sendspin`, a node from an ear without the feature).
- **Totals in words.** The badge's tooltip and the node panel's "Companions" list give each one's
  name, word, bank, readiness and the total its counter carries since its configuration made it
  ("rec.writer (sink on writer): ready; 4096 frames dropped since it was made"; a source's
  shortfall "... frames played short since it was made"). An engine kind has no counter. A counter
  member the editor knows is shown whatever the kind; one it does not know is ignored.
- **A mark only while it moves.** A badge carries the warning mark only while its `dropped` or
  `shortfall` rose in the last minute, by the rule of [recent marks](#recent-marks), held per node
  and companion: a value that fell is a new configuration seen without its `[]`, and an entry seen
  again after its list dropped it is a first sighting. A mark while a total is above zero would
  stay on a recording that dropped once until its next restart, since the counters count per
  configuration. **This mark is a reading of the owner's rules, not an answer of theirs:** for the
  badges the owner asked for the drop count on hover and no mark; for the diagnostics counters the
  owner answered "panel plus recent" (the totals always in the panel, a mark on the canvas only
  while something happened in the last minute), and that rule is applied here to the companions'
  counters. The owner may overturn it (back to no mark), a change to `companionBadges`' `warning`
  and this section. `CompanionMoves` keeps what was held and when it rose.
- **Failed and disabled draw no strip** whatever list is held (their configuration was torn down,
  and ear's `[]` follows their `node.state`); a stopped node keeps its strip, as it keeps its
  companions.
- **In text, not colour alone.** The dot and the mark are hidden from screen readers beside
  visually hidden "ready", "not ready" and the mark's words ("dropped frames in the last minute",
  "played short in the last minute"); the strip is a list labelled "companions".
- **Not part of editing.** A badge is no handle and no node: clicking it selects its node and
  dragging it moves the box, as any part of the box does; nothing in it sends a request.

## Counters

`web/src/counters.ts` is the one place that turns the diagnostics counters
([control-port.md](control-port.md#counters)) into the inspector's totals, a node box's mark and the
graph's dropouts badge; `counters.test.ts` covers its rules and replays the `counters-changed` and
`unknown-values` fixtures through the reducer into what is drawn. *Why this way:* asked whether the
editor shows dropouts and a node's missed cycles unasked, the owner answered "panel plus recent":
the totals always in the inspector, and the canvas's marks and badge only while something
happened in the last minute. The rules:

- **The inspector always holds the totals.** In its Graph subject, under the engine facts, "Since
  ear started": `cycles`, "dropouts" (`cutCycles`), `stalls` and "time lost" (`lostMs`, in ms). In
  its Node subject, "Counters since it was added": "missed cycles", "overruns", "repositions",
  "driver changes".
  Every counter is shown, zeros included, each label's tooltip saying what it counts. A counter
  this editor does not know follows, in the order sent, by its own name (in monospace) and number,
  its tooltip "a counter this editor does not know". A row whose counter rose in the last minute
  carries the mark and, beside it, how much in words ("3 more in the last minute").
- **Only losses mark.** The loss counters are a node's four and the graph's `cutCycles` and
  `stalls`. `cycles` moves every cycle and `lostMs` follows `cutCycles`, so neither marks; an
  unknown counter may count anything (a total like `cycles`), so it is shown and never marked.
- **A node's mark sits after its state word** in the box's header, while one of its own loss
  counters rose in the last minute: what happened to that node beyond the graph's losses. It is
  apart from the companion strip at the box's bottom edge, whose badges mark their own counters
  (a sink's drops), so a mark in the header is the node's and one in the strip a helper's. Its
  tooltip gives the last minute's and the totals in words ("In the last minute: overran 1 time.
  Since it was added: missed 33 cycles, overran 3 times."). The graph's cuts never mark a node:
  they are the host's, on every output at once.
- **The dropouts badge** sits in the top bar after the engine chip while `cutCycles` rose in
  the last minute, with the last minute's count ("5 dropouts"), which falls as each rise leaves its
  minute; its tooltip says the host held up the audio thread and gives the totals ("44 dropouts
  (234 ms) in 2 stalls since ear started").
- **A mark lasts across a port-caused replace.** A node's counters never step back while it
  lives, so the values held for a node are kept when it is removed (until the connection closes).
  A port-caused replace's `node.added` carries at least what was held, so its carried counts are
  no rise and a mark the older node showed goes on; a new node under the name comes with zeros, a
  fall, so its counts start fresh. Companions' entries are still forgotten when their list drops
  them, as their counters count per configuration.
- **In text, not colour alone.** Each mark's glyph is hidden from screen readers beside its words
  ("in the last minute: overran 1 time", "3 more in the last minute"); the badge's count is its
  text. Nothing shows without the feature: an ear without it sends no counters.

## Recent marks

`web/src/marks.ts` is the one rule both marks follow, companions' badges and the counters'
(`marks.test.ts`): each reads its counters from the graph, and `GraphMoves` keeps their last values
and rises. The mark is an "!" in a circle (`WarnMark`).

- **A rise is a value above the one last held** for the same counter. The first value seen (a
  snapshot's, a new node's), a value that fell and a counter seen again after the graph dropped it
  are not; a reading may keep a dropped group's values instead (the node counters do,
  [above](#counters)), and then its return is judged against them. A closed connection forgets every value held, so the reconnect's snapshot is a first
  sighting: what was lost while the editor was away is in the totals, never marked as the last
  minute's.
- **The minute** is timed on the browser's monotonic clock (`performance.now()`, so a step of the
  wall clock never stretches or cuts it), from the message that brought the rise; a count of the
  last minute is the sum of the rises still in it. A box or the inspector redraws when a shown mark's
  minute ends or a count falls.
- **Noted before anything draws:** the moves are the graph store's first listeners, outside the
  reducer (which holds only what ear sent), so a box drawn for a new graph reads that graph's
  moves. *Why:* the canvas stays quiet when all is well and points at trouble while it happens; the
  totals are always in the tooltip and panel.

## Developing it

`scripts/dev.sh make web` builds it into `web/dist` (`--web-root web/dist` serves that build);
`make web-test` runs the typecheck, Vitest and the build. `npm run dev` (Vite, hot reload)
forwards `/rpc` to an ear on `localhost:7777`; `scripts/dev.sh`'s header shows how to publish its
port. Node.js comes from `web/.nvmrc` and the packages from `web/package-lock.json`.

The tests cover the reducer against every `test/protocol` fixture, the graph file's state and the
pane's logic (drafts, stale files, Save, refusals), the node panel's knobly box (`nodeText.ts`), the add-node
form (`addText.ts`), the highlighting (`highlight.ts`, and that it never changes the text sent), the reconnecting client with a fake socket,
the connection rules, levels and layout, `health.ts`, `companions.ts`, `counters.ts` and `marks.ts`, `wires.ts` (replaying the `snapshot` and `unknown-values` routes), `meters.ts` (the hold, the fall, the lamp and the reset on a fake clock), the handles' cue (`connect.ts`), the box's sizes against `editor.css` (`layout.test.ts`), and the theme (`theme.ts`, and the contrast of every pair in both themes in `theme.test.ts`), the shell's words (`shell.ts`: the pill's countdown, the engine and protocol chips, the facts), the sizes and the breakpoint against `editor.css` (`sizes.ts`), the inspector's subject and Delete's requests (`selection.ts`), the drawer's words and when it opens (`drawer.ts`), the JSON settings form's parse (`settings.ts`), the wire's status words (`wires.ts`), tidy (`layout.ts`), and the client's retry wait and the editor's connection view (`rpc.test.ts`, `editor.test.ts`). There is no browser test runner:
rendering is checked by eye. *Why:* a Playwright leg costs about 150 MB of browsers and a minute
or two a CI run, while the logic sits in the reducer, client and rules, which Vitest covers
against the same fixtures the server is tested with.

React Flow (`@xyflow/react`) was chosen over Rete.js (its own editor framework) and LiteGraph
(drawn on a canvas, which makes per-port meters custom work).
