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

Built (step 3): [`editor.md`](../editor.md#the-shell). What remains here: the palette button (3.6), `⌘E` and `⌘S`
(3.6), and `F`, `L` and `M` on the canvas's tools (3.6), the toasts in the canvas's bottom right in place of its one
error banner (3.5), `Saving…` in the drawer header (3.5), the drawer's line-number gutter (3.5), and keeping the
inspector's width, the drawer's height and open state and the minimap (3.8).

### 3.2 Tokens and the dark theme

Built (step 1): [`editor.md`](../editor.md#tokens-and-themes). What remains here: the switch moves to the top bar
(3.1), the choice is kept per browser (3.8), and the palette's `Theme:` commands (3.6).

### 3.3 The node box

Built (step 2): [`editor.md`](../editor.md#the-node-box).

### 3.4 Wires and meters

Built (step 2): [`editor.md`](../editor.md#wires) and [`editor.md`](../editor.md#meters).

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
2. **The node box, wires and meters** (3.3, 3.4) (built).
3. **The shell** (3.1) (built): top bar, inspector, drawer and its header chip, the narrow-window sheet.
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
| A moving dash on every linked wire costs too little to notice at 30 level messages a second, on a graph of 20 wires | owed: measured in the browser's performance panel on the system test's graph (step 2 built it without that check, as no browser runs here); if it costs, the flow is drawn only on the selected and hovered wires (`wires.ts`'s `flowing`) |
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
