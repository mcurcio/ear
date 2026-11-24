# The editor's shell: a slicker graph editor

Status: design, not built. It changes only `web/`: no request, notification, fixture or C++ changes, so protocol 1
and `test/protocol/` stay as they are. The editor as built, and the rules every part below keeps, are in
[`docs/editor.md`](../editor.md); this page cites them rather than repeat them. Slices 5 and 6
([`control-plane.md`](control-plane.md#8-the-editor)) add a settings form, knobs, faders and notices to the editor;
this page designs the frame they land in, so that section 8 of that page changes only where 4 below says.

[U] = unverified, with the check that settles it (section 6).

## 1. The owner's words that bind this page

| # | The owner's words | Date | What it means here |
|---|---|---|---|
| E1 | "design a slicker UI experience" | 2026-11-24 | This page: the editor's layout, look, feedback and keyboard, within every rule `editor.md` holds |
| E2 | "ideally", asked whether node places are kept across reloads in the browser's `localStorage`, per ear | 2026-11-24 | Places are kept (3.7, 3.8) |
| E3 | "ok", to not asking for a route when a wire dropped on empty canvas adds a node | 2026-11-24 | The node is placed at the drop and the person draws the wire (3.7) |
| E4 | "ok", to `ear.conf` in a bottom drawer rather than a third column | 2026-11-24 | The drawer (3.1) |
| E5 | "yes", to the ghost wire while a route request is in flight | 2026-11-24 | The ghost wire (3.5) |
| E6 | "yes", to the theme following the system by default | 2026-11-24 | `system` by default (3.2) |

Section 7 lists these as decisions; no question is open.

## 2. What is built, and what is wrong with it

As built ([`editor.md`](../editor.md#what-it-shows-and-does)): a full-window React Flow canvas with the stock
`Background` and `Controls`, and one 360 px side panel that stacks, top to bottom, the title, the connection and error
banners, the engine facts and the graph's counters, the add-node form, the selected node's panel and the whole
`ear.conf` pane (a 20-row textarea). Reading the code (`App.tsx`, `Panels.tsx`, `editor.css`) against what an operator
does with it:

| # | What an operator meets | Where it comes from |
|---|---|---|
| P1 | Selecting a node pushes `ear.conf` down the panel; reading the file and a node at once means scrolling between them | one column holds everything (`App.tsx`'s `aside`) |
| P2 | `ear.conf` is edited in a 360 px column with no wrapping, so most lines scroll sideways | the pane lives in the side panel |
| P3 | A refused request (a route ear refused, a lifecycle call) shows as one banner at the panel's top, often scrolled out of view, far from the wire or button that caused it | `App.tsx`'s single `error` string |
| P4 | Nothing says a request is on its way: a wire drawn shows nothing until ear announces it, a Start button stays as it was | the editor draws only what ear announced (correct), and has no in-flight state |
| P5 | Adding a node is only the panel's form; nothing on the canvas offers it, and a new node lands wherever dagre puts it | `AddNodeText.tsx` is the one way in |
| P6 | A node's state is a coloured word in 11 px text; at a glance across a graph of ten nodes, failed and running look alike | `.ear-node-state` |
| P7 | Colours are hex values scattered across `editor.css` and `LevelEdge.tsx`; there is no dark theme, which also holds back knobly's dark tokens ([`editor.md`](../editor.md#highlighting)) | no tokens |
| P8 | Node places are forgotten on reload; every visit starts from dagre's layout | `Places` lives for the page's life |
| P9 | Everything is a mouse trip: no shortcut but Delete, no way to jump to a node by name | React Flow's defaults |

## 3. The design

### 3.1 The layout

```text
┌──────────────────────────────────────────────────────────────────────────────────┐
│ EAR   ● live   48 kHz · 256 frames · realtime   ⚠ 5 dropouts      ⌘K  ◐  ▤ ear.conf │  top bar, 44 px
├───────────────────────────────────────────────────────────────┬──────────────────┤
│                                                               │ Inspector        │
│                         canvas                                │  (graph, node    │
│                                                               │   or route)      │
│  ┌──────────┐                                                 │                  │
│  │ − + ⤢ ⊞ ▦│  floating tools                       toasts ▸  │                  │
├──┴──────────┴─────────────────────────────────────────────────┴──────────────────┤
│ ear.conf ● unsaved · 2 errors                                        Save ⌘S   ▾ │  drawer header, 32 px
│  1 graph { rate 48khz; block_size 256; }                                         │
│  2 node white noise;                                                             │  drawer, resizable
└──────────────────────────────────────────────────────────────────────────────────┘
```

- **The top bar** holds what is about the whole graph and never about a selection: the wordmark; the connection pill
  (`live`, `connecting…`, `lost: retrying in 4 s`, each a word beside its dot); the engine facts as one chip
  (`rateLabel` and the block size, then `realtime` or a `not realtime` badge); the dropouts badge as built
  ([`editor.md`](../editor.md#counters)); the newer-protocol banner as a chip whose tooltip says what is not shown; and
  on the right the palette button (3.6), the theme switch (3.2) and the `ear.conf` toggle with the drawer's status chip.
- **The canvas** fills the rest. React Flow's `Controls` give way to one floating tool group, bottom left: zoom out,
  zoom in, fit (`F`), tidy (`L`: lay every node out again with dagre, 3.7) and the minimap toggle (`M`, React Flow's
  `MiniMap`, off by default). The background is a quiet dot grid in the theme's colours.
- **The inspector**, on the right (320 px, dragged between 280 and 560), shows one subject, chosen by the selection:
  - nothing selected: **Graph**: the engine facts in full, the graph's counters (`CounterTable`, as built) and the
    add-node form;
  - one node: **Node** (3.3's header, the lifecycle bar, its knobly box, counters, companions, Delete);
  - one route: **Route**: from and to as `node:port` buttons that select that node, its status in words, its level
    meter, Remove;
  - several: a count ("3 nodes, 2 routes") and Delete for them all.

  The inspector is where slice 5's settings form and status list go
  ([`control-plane.md`](control-plane.md#82-how-each-row-kind-renders)); nothing in this page decides their content.
- **The `ear.conf` drawer**, along the bottom, full width, holds today's pane whole ([`editor.md`](../editor.md#what-it-shows-and-does):
  its banners, `KnoblyText`, diagnostics and Save), with a line-number gutter (3.5). Its 32 px header stays when the
  drawer is closed and says, in words, what the file needs: `unsaved`, `stale: reload`, `refused`, `n errors`,
  `write failed`, `applies at restart`, or nothing. `⌘E` (`Ctrl+E` off macOS) opens and closes it; it opens by itself
  only on a refusal or a write failure, never on a hand edit. Dragged between 120 px and 70 % of the window.

The inspector and the drawer never move each other's content: P1 and P2 go. Below 900 px wide the inspector becomes a
sheet over the canvas's right edge, closed by Escape or a click on the canvas, and the drawer keeps its place.

### 3.2 Tokens and the dark theme

Built (step 1): [`editor.md`](../editor.md#tokens-and-themes). What remains here: the switch moves to the top bar
(3.1), the choice is kept per browser (3.8), and the palette's `Theme:` commands (3.6). `--meter-mid` and `--meter-hot`
exist and are drawn in step 2 (3.4); numbers' `tabular-nums` and monospace port names come with the node box (3.3).

### 3.3 The node box

```text
╭──────────────────────────────╮
│▌ desk                 card   │   name bold; type as a small label, right
│▌ ● running  !                │   the state pill: dot and word; the counters' mark after it
│▌ waiting for fpp.rtp         │   the health line, muted, one line, full text on hover
├──────────────────────────────┤
│◉ play.1  ▮▮▮▮▮▯▯▯▯ │          │   inputs left, outputs right, each a meter
│◉ play.2  ▮▮▯▯▯▯▯▯▯ │          │
├──────────────────────────────┤
│ card out ●   card in ○       │   the companion strip, as built
╰──────────────────────────────╯
```

- **The state is a stripe and a pill.** A 4 px stripe down the box's left edge and a pill (a dot and the word) take the
  state's token: `--ok` running, `--warn` starting and stopping, `--err` failed, `--quiet` stopped, `--off` disabled, and
  `--warn` for a state this editor does not know (as today, [`editor.md`](../editor.md#a-nodes-health)). The word is
  always there, so the state is never colour alone; at a glance across the canvas the stripe answers P6. A failed box
  also gets a 1 px `--err` border; a disabled one a dashed border and its stripe hatched, its text at full contrast.
  `configuring` pulses the pill's dot only, never the text.
- **The type label** is the type's own word, unstyled by type: nothing in `web/` names a node type
  ([`control-plane.md`](control-plane.md#81-stores)), so there are no per-type icons or colours.
- **Width 220 px** (from 200), so `waiting for the previous desk to stop` fits more often; `layout.ts`'s `nodeWidth`
  and `headerHeight` follow, and its tests.
- **Handles** grow to a 10 px dot with a 20 px hit area. While a wire is being drawn from an output, every input handle
  that `isValidConnection` allows is ringed in `--accent` and every other is dimmed; the handle under the pointer grows.
  Today a drag to an invalid place is already refused before ear; this shows where it can go before the drop.
- **Selection** is a 2 px `--accent` ring outside the border and a lifted shadow; hover lifts the shadow alone.

### 3.4 Wires and meters

- **Wires** keep their meaning ([`editor.md`](../editor.md#what-it-shows-and-does): colour by status, thickness and
  brightness by the from port's level) and take the tokens (`--ok`, `--warn`, `--err`). Three additions:
  - a linked wire whose level is above the floor shows a slow flow (a dash moving along it, its speed fixed, so it
    says "signal" and not how loud); none under reduced motion, and none on `pending` or `refused`, which stay dashed
    and still;
  - a `refused` wire carries a small `refused` label at its midpoint (React Flow's `EdgeLabelRenderer`), so the
    status is in words on the canvas, not colour alone;
  - a hovered wire thickens by 1 px and shows its two ends as `from → to` in a tooltip.
- **Meters** keep their scale (rms bar and peak tick, dBFS down to -60, `levels.ts`) and draw the rms bar in three
  zones: `--meter-low` below -18 dBFS, `--meter-mid` to -6, `--meter-hot` above. The peak tick holds its highest value
  for 1.5 s, then falls at 20 dB/s; the hold is the meter component's own state, outside React state like the levels
  ([`editor.md`](../editor.md#its-rules)), and resets when the connection closes. A peak at or over 0 dBFS lights the
  meter's right end for 2 s (a clip lamp), with "clipped" in its tooltip.

### 3.5 Feedback: in flight, toasts and where errors land

The editor still draws only what ear announced ([`editor.md`](../editor.md#its-rules)). What changes is that a request
is visible while it is on its way, and its refusal lands where it was asked.

- **In flight** (P4). A new pure module, `requests.ts`, keeps the requests this editor sent and not yet answered, each
  with what asked for it (a route's two ends, a node's name and the button). While one is in flight:
  - a wire drawn shows a ghost: a thin dashed `--muted` line between the two handles, plainly not a route (no status
    colour, no meter, not selectable), gone on the reply; the route is drawn when `route.added` arrives, as today;
  - a lifecycle button shows a spinner and is disabled; the others stay as `health.ts` says;
  - Save in the drawer and Add in the form say `Saving…` and `Adding…`.
- **Toasts** (P3) replace the panel's single error banner. A refusal becomes a toast in the canvas's bottom right,
  saying what was asked and why it was refused (`route rec:FX → desk:play.1 refused: unknown-port rec:FX`), with the code in monospace. A toast stays 8 s, or until dismissed while hovered or focused; at most
  four show, older ones folding into "and 3 more"; the same code for the same subject within 5 s counts up ("×3")
  instead of stacking. Toasts are a polite live region; a refusal is assertive. A toast for a node or route has a Show
  button that selects it and pans to it.
- **Where errors land.** A refusal that has a place keeps it: `invalid-text` and `file-changed` in the drawer and the
  node box as built; a refused route on the ghost, which turns `--err` for 1 s before it goes; a lifecycle refusal
  under the inspector's lifecycle bar as well as in a toast.
- **The drawer's gutter** gives each line its number and a dot on each line a diagnostic names (`--err`, `--warn`);
  hovering the dot shows its messages, a click puts the caret there, as the list under the text does. The gutter is a
  second `pre` beside the textarea, so `KnoblyText`'s rule that the textarea holds everything
  ([`editor.md`](../editor.md#highlighting)) is untouched.

`requests.ts` and `notices.ts` (the toast queue: add, coalesce, expire on the monotonic clock, as `marks.ts` does) are
pure and tested; the components only draw them. Slice 5's notices
([`control-plane.md`](control-plane.md#84-status-events-diagnostics-and-actions)) are fed into the same queue, so its
"coalesced per node and code" rule is this one.

### 3.6 Keyboard and the command palette

`⌘K` (`Ctrl+K` off macOS) opens a palette over the canvas: a search field over a list of commands, ranked by a
subsequence match on their words, as a WAI-ARIA combobox and listbox. Its commands come from a pure
`palette.ts` over the graph, the selection and the features, so a command ear cannot take (Start without `lifecycle`,
the knobly form without `node-text`) is never offered:

| Command | Does |
|---|---|
| `Go to <node>` | selects it and pans to it |
| `Add <type>` | opens the inspector's add-node form with that type's template ([`editor.md`](../editor.md#adding-a-node)) |
| `Start`, `Stop`, `Restart`, `Disable`, `Enable` `<node>` | the lifecycle call, offered as `health.ts` allows, its `why` as the hint |
| `Remove <node>`, `Remove route <from> → <to>` | as the Delete key |
| `Open ear.conf`, `Save ear.conf`, `Reload ear.conf` | the drawer's actions, offered when they apply |
| `Fit view`, `Tidy layout`, `Toggle minimap`, `Theme: system / light / dark` | as their buttons |

Shortcuts, each listed in a sheet that `?` opens: `⌘K` palette, `⌘E` drawer, `⌘S` save the drawer's draft (only
while it is dirty and not stale), `F` fit, `L` tidy, `M` minimap, `A` add (the palette, filtered to `Add`), Escape
(close the palette, the sheet, or clear the selection, in that order), and Delete and Backspace as built. Single keys
act only while the canvas has focus; inside any textarea or input every key is the text's.

### 3.7 Adding on the canvas, and places

- **Quick add** (P5). A double-click on empty canvas, or a wire from an output dropped on empty canvas, opens a small
  type list at the pointer (the palette's `Add` commands). Choosing a type opens the inspector's add-node form with its
  template, as `Add <type>` does; ear still judges the text, and the node still appears only on `node.added`. The
  pointer's place is kept as a **placement hint** for the template's name: when `node.added` brings that name,
  `Places` puts it there instead of asking dagre. A hint the person's edit made stale (they renamed the node) is
  dropped, and the node is laid out as today. A wire dropped on empty canvas asks for no route once the node arrives
  (E3): it would be a second request the person did not see, and a type's first input is not always the one they
  mean.
- **Tidy** lays out every node with dagre again (today's first placement, `layout.ts`) and animates them there over
  180 ms; moves are the browser's, so nothing is sent.
- **Places kept** (P8, E2): `Places` reads and writes the browser's `localStorage` under a
  key per ear (the page's `location.host`), each write in `try`, so a private window or blocked storage keeps today's
  behaviour. A name not in the graph keeps its place there for 30 days, so a node removed and added back finds it. They
  are still never sent to ear ([`editor.md`](../editor.md#its-rules)).

### 3.8 What the browser keeps

Per ear (`location.host`), in `localStorage`, every read and write in `try` and every value checked on read: node
places (E2), the inspector's width, the drawer's height and whether it is open, the minimap, and the theme (per
browser, not per ear). A new module, `prefs.ts`, owns the keys and their checks; nothing else touches storage.

## 4. What it changes in the built system

| Where | Change |
|---|---|
| `web/src/App.tsx` | the shell: top bar, canvas, inspector, drawer, toasts; selection-driven inspector subject; shortcuts |
| `web/src/components/` | new `TopBar`, `Inspector`, `Drawer`, `Toasts`, `Palette`, `ShortcutSheet`, `CanvasTools`, `QuickAdd`; `EarNode`, `LevelEdge`, `Meter`, `Panels`, `TextPane`, `KnoblyText` (the gutter) restyled and split as above |
| `web/src/theme.css` (new), `editor.css` | built (step 1): the tokens, both themes; `editor.css` uses tokens only |
| `web/src/requests.ts`, `notices.ts`, `palette.ts`, `prefs.ts` (new) | pure, each with its vitest |
| `web/src/layout.ts` | 220 px boxes; placement hints; `Places` over `prefs.ts` (E2) |
| `docs/editor.md` | rewritten where it describes the layout and look; new sections for feedback, the palette and what the browser keeps; the knobly tokens' dark set |
| `control-plane.md` section 8 | its panel becomes the inspector's Node subject, its notices this page's queue (3.5); nothing else |
| packages | none added: the palette, toasts, splitters and gutter are hand-written; `MiniMap` and `EdgeLabelRenderer` are in `@xyflow/react` already |
| the wire, C++, fixtures | nothing |

The bundle should grow by under 15 KB (5 KB gzipped) [U].

## 5. Building it

Each step is its own pull request, a `feat(web)` commit, and leaves the editor whole; `docs/editor.md` changes with
each.

1. **Tokens and themes** (3.2) (built): `theme.css`, the dark set, the knobly dark tokens, the contrast test, the theme switch.
   Nothing moves yet.
2. **The node box, wires and meters** (3.3, 3.4).
3. **The shell** (3.1): top bar, inspector, drawer and its header chip, the narrow-window sheet.
4. **Feedback** (3.5): `requests.ts`, `notices.ts`, the ghost wire, toasts, the gutter.
5. **Keyboard** (3.6) and **canvas adding** (3.7, without kept places).
6. **What the browser keeps** (3.8, E2).

Tests ([`editor.md`](../editor.md#developing-it): no browser runner, rendering checked by eye): `requests.test.ts` (in flight until reply or connection lost, a ghost per route request);
`notices.test.ts` (coalescing, the cap, expiry on a fake monotonic clock, a reconnect clears nothing the person has not
seen); `palette.test.ts` (commands per feature set and phase, replaying the `lifecycle` and `unknown-values` fixtures,
ranking); `prefs.test.ts` (storage that throws, values of the wrong shape, the 30-day expiry); `layout.test.ts`
(placement hints taken, a stale hint dropped).

## 6. Unverified, and how each is settled

| [U] | Settled by |
|---|---|
| A moving dash on every linked wire costs too little to notice at 30 level messages a second, on a graph of 20 wires | step 2, measured in the browser's performance panel on the system test's graph; if it costs, the flow is drawn only on the selected and hovered wires |
| The bundle grows by under 15 KB | step 5's build output, against today's |
| React Flow 12 lets the canvas's single-key shortcuts and its own Delete handling coexist with focus in the inspector's text | step 5, by eye; `control-plane.md`'s same [U] for knobs (its 8.3) is settled by the same check |

## 7. Decisions taken

| # | Decision | Owner's words | Why |
|---|---|---|---|
| D1 | Node places are kept across reloads in the browser's `localStorage`, per ear, never sent to ear | E2 | A reload no longer scatters a hand-placed graph, and places stay the browser's own |
| D2 | A wire dropped on empty canvas places the new node at the drop and asks for no route | E3 | It would be a second request the person did not see, and a type's first input is not always the one they mean |
| D3 | `ear.conf` lives in a full-width bottom drawer | E4 | Full width answers P2, and the canvas keeps its width for a left-to-right graph |
| D4 | A route request in flight shows a ghost wire | E5 | It is drawn as a request, never as a route (no status, no meter, not selectable), and is gone on the reply |
| D5 | The theme follows the system until the person chooses | E6 | |

No question is open.
