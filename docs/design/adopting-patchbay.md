# The graph editor on patchbay: what is still to build

Status: designed; A1 is built (the package, the rename onto its tokens, the theme through `useTheme` and ThemeSwitch),
A2 (section 3.3 marks each change), A3 (section 3.4), and of A4 all but the code editor (section 3.5 marks each).
The graph editor (`web/`, [docs/editor.md](../editor.md)) draws with patchbay's `--pb-*` tokens, theme switch and
built primitives and hooks (its spinner and warning mark, splitters, form fields and buttons, pills,
badges and facts, narrow sheet and kept values) and composites and hooks (banners, status words, the command palette,
the shortcut sheet, the key bindings and the toasts) and its whole graph layer (node boxes, port rows, wires, ghost
wires, connect hints, meters, the canvas tools, quick add's `useAddAt` and the view's `useFollowFit`), but still draws
its code boxes and shell itself: `web/src/components/`, its styles in `web/src/editor.css`.
`@mcurcio/patchbay` (npm, 0.7.0) is a component library extracted from this editor: its `tokens.css` is the editor's
old `theme.css` renamed, and its built primitives and hooks are generic versions of the editor's leaves. This page designs moving the
editor onto it, one area per change, so that the editor keeps its behaviour and ear keeps everything that is not
drawing. What the editor has still to take (the code editor and the shell) waits on patchbay's releases.

| Step | What | Waits on |
|---|---|---|
| **A1** | The package, the rename and the theme (section 3.2): built | — |
| **A2** | The built primitives and hooks, one area per change (section 3.3): built | — |
| **A3** | The node box, wires, meters and the canvas sugar (section 3.4): built | — |
| **A4** | Feedback, the palette and keys, the code editor (section 3.5): in part | the code editor waits on patchbay's CodeEditor; the rest on nothing (0.5.0) |
| **A5** | The shell, and deleting this page (section 3.6) | patchbay's `AppShell`, released |

[V] = verified in the file named (patchbay's files by their path in its repository); [U] = unverified, with the step
that checks it.

## 1. The owner's words that bind it

**The principle** (owner, 2026-11-26): "build the correct model on patchbay primitives. Ear and rasl UIs are not
precious to me. They should be reworked when patchbay has a matching concept." Where the editor differs from patchbay
in look or behaviour, it takes patchbay's model; nothing of ear's look or behaviour is kept for its own sake.

Every decision is taken; a later one goes in this table with its date. D6, D7, D8 and
D15 are rasl's and change nothing here; they are listed so the numbers match patchbay's and rasl's records.

| # | The owner's words or decision | Date | What it means here |
|---|---|---|---|
| D1 | No GraphCanvas wrapper: patchbay ships node and edge types and adds sugar beside the app's own `<ReactFlow>` (`CanvasTools`, `useAddAt`, `useFollowFit` with `onFitted`), so every React Flow prop and later feature stays reachable | 2026-11-25 | `Canvas.tsx` keeps rendering `<ReactFlow>` itself and takes patchbay's pieces one by one (3.4); patchbay's pb-nx9.15 is settled |
| D2 | patchbay's six builder choices confirmed: WarnMark's `label`, Button `type="button"` by default, a busy button stays focusable, Toolbar wraps, Dialog and Sheet bodies are tab stops, Badge outlined | 2026-11-26 | A2 takes the primitives as they are |
| D3 | Pill and Badge `accent` and `muted` tones; Badge's change tones and `filled` variant (built in 0.4.0) | 2026-11-26 | The state pill, chips and "not realtime" badge map onto them (3.3) |
| D4 | Busy buttons stay focusable with a spinner and a `busyLabel` (built in 0.5.0); a tool group keeps one Tab stop | 2026-11-26 | The Saves and lifecycle buttons stop disabling while busy; the canvas tools get a roving tab stop |
| D5 | ear inlines patchbay's `themeScript` in `index.html` and takes patchbay's ThemeSwitch wording | 2026-11-26 | No first-paint flash for a kept choice; no `labels` override (A1's theme step) |
| D6 | rasl takes patchbay's base look: no rasl brand stylesheet | 2026-11-26 | rasl's; nothing here |
| D7 | rasl takes patchbay's Dialog, Pill, resizable inspector and Sheet behaviour | 2026-11-26 | rasl's; ear takes the same components in A2 |
| D8 | Delete `@rasl/ui` | 2026-11-26 | rasl's; nothing here |
| D9 | patchbay's keyboard model: a chord acts in a text field only when its binding opts in; Tab closes the palette; palette items have a hint line | 2026-11-26 | ⌘K and ⌘S opt in; A4's palette and keys |
| D10 | patchbay's Meter owns the dB scale (built in 0.5.0) | 2026-11-26 | `meters.ts`'s scale, zones, hold and fall go with Meter in A3 |
| D11 | patchbay's toast model (built in 0.5.0) | 2026-11-26 | `notices.ts`'s store runs on the pure queue `useToasts` holds and Toaster draws it (A4); ear keeps the wording |
| D12 | NodeBox's geometry as exported constants, mirrored by tokens a brand may not change | 2026-11-26 | `layout.ts` sizes boxes by `nodeBoxSize` and places quick add by the constants (built in A3) |
| D13 | SchemaForm deferred | 2026-11-26 | The JSON settings form stays ear's |
| D14 | Every component takes its own words (the visible text and accessible names it supplies itself) through one `labels` prop, a partial object with English defaults; the per-word props (`closeLabel`, `dismissLabel`, ...) are gone (built in 0.7.0). No app-wide provider for now | 2026-11-26 | ear sets two: the palette's placeholder (`labels.placeholder`) and the meters' tooltip (`labels.title`); every other word is patchbay's default |
| D15 | rasl gets patchbay's theme switch, as ear does | 2026-11-26 | rasl's; nothing here |

## 2. What is built

- **The editor.** React 19.3, `@xyflow/react` 12.12, dagre, Vite 8, Vitest in its default environment (no DOM, no
  Storybook): logic tests, plus tests that read `editor.css` (and patchbay's `tokens.css`) as text to hold CSS equal to
  TS constants and every `var()` to a token (`theme.test.ts`, `sizes.test.ts`, `layout.test.ts`). knobly comes from its GitHub Release tarball, pinned by
  `cmake/knobly.json` ([AGENTS.md](../../AGENTS.md), [editor.md](../editor.md#highlighting)).
- **patchbay 0.7.0.** Peer dependencies `react` and `react-dom` `^19`, and optionally `@xyflow/react` `^12.1.0` and
  `knobly` `^1.0.0` [V: `package.json`]; ear meets every range. Subpaths `./tokens.css`, `./base.css`, `./primitives`,
  `./hooks`, `./composites`, `./graph`, `./testing` (the brand contrast check); `./shell` resolves but exports nothing
  yet [V: `dist/*.js`'s exports]. Every component takes its own words through one `labels` prop (D14).
  Built: the `--pb-` tokens with a light set, a system-dark set and a chosen-dark set and React Flow's `--xy-*`
  variables mapped onto them, the type tokens (`--pb-font-*`, `--pb-text-*`, `--pb-code-size`); Button, IconButton,
  Spinner, Pill, Badge, WarnMark, KeyValue, SectionTitle, Field, TextInput, Select, Checkbox, RadioGroup, Splitter,
  Toolbar, ToolGroup, Panel, Dialog, Sheet, ThemeSwitch; Banner, CommandPalette, DiffView, ShortcutSheet, StatusWords,
  Toaster; Meter, NodeBox, PortRow, Wire, GhostWire, `useConnectHints`, CanvasTools, `useAddAt`, `useFollowFit`;
  `useTheme` and `themeScript`, `usePersistentState`, `useShortcuts`, `useToasts`,
  `useMediaBreakpoint`, `useLiveValue`. It releases by semantic-release; while it is 0.x a breaking change is a minor release.
- **The editor draws with it.** `web/package.json` pins `@mcurcio/patchbay` 0.7.0 exactly, and `npm ci` takes the
  tarball-installed knobly 1.0.0 as meeting its `^1.0.0` peer [V: `npm ls` dedupes it]. `theme.css` is gone: every
  colour in it had the same value in `tokens.css`, in light and both dark sets [V: compared before the rename], so
  the rename (`--X` → `--pb-X`, `--knobly-Y` → `--pb-code-Y`, `--code-size` → `--pb-code-size`, the faces and the 13
  and 12 px sizes → `--pb-font-*` and `--pb-text-*`) changed no value. How the editor uses the tokens, its own
  variables and the test that every `var()` resolves are in [editor.md](../editor.md#tokens-and-themes).

## 3. The design

### 3.1 What moves and what stays

patchbay takes the drawing and the generic mechanics; ear keeps what knows about ear.

| Moves to patchbay (drawn from props) | Stays ear's |
|---|---|
| Colours, radii, shadows, durations, fonts (`theme.css`) | The words: `shell.ts`, `drawer.ts`, `pane.ts`, `notices.ts`'s refusal wording, `health.ts`'s words |
| Spinner, WarnMark, ThemeSwitch, Splitter, the canvas tools' frame, the narrow sheet, form controls, busy buttons | The rules: `health.ts`, `companions.ts`, `counters.ts`, `marks.ts`, `selection.ts`, `requests.ts`, `wires.ts` (as the adapter onto Wire's props) |
| The theme choice and kept values (`useTheme`, `usePersistentState`) | Storage keys and checks (`prefs.ts`: `ear-editor/1/…`, per host, places pruning) |
| The node box, port rows, wires, ghost wires, connect hints, meters, the canvas tools, `useAddAt` and `useFollowFit` (once built) | The protocol, the session and stores: `protocol.ts`, `rpc.ts`, `editor.ts`, `graph.ts`, `graphFile.ts`, `levels.ts` |
| The toast queue's mechanics, palette ranking and anchoring, key labels, knobly highlighting and the gutter (once built) | The command catalogue (`palette.ts`), the key bindings (`keys.ts`), first placement (`layout.ts`, dagre) |
| The shell's grid, sheets and drawer (once built) | Which subject the inspector shows; Escape's order among ear's own surfaces; the `<ReactFlow>` element and its props (D1) |

### 3.2 A1: the package, the tokens, the theme

1. **The package and its tokens** (built). patchbay pinned exactly, `tokens.css` loaded first, React Flow's `--xy-*`
   variables mapped by patchbay ([editor.md](../editor.md#tokens-and-themes)). `base.css` is optional and waits for
   A5.
2. **The rename** (built). Every `var(--X)` in `editor.css` and the components is `var(--pb-X)`, and `theme.css` is
   deleted; `theme.test.ts` fails on any `var()` that names neither a patchbay token nor one of ear's own variables
   ([editor.md](../editor.md#tokens-and-themes)).
3. **The theme** (built). `useTheme({ key: themeKey() })` (`ear-editor/1/theme`), so the choice people kept is read
   where it is; patchbay's ThemeSwitch with its own words, and `themeScript` inline in `index.html`, ending the
   first-paint flash (D5). `theme.ts` and ear's `ThemeSwitch.tsx` are gone; ear's generic button rules in
   `editor.css` pass patchbay's controls by (any `pb-` class), so each draws from its own stylesheet
   ([editor.md](../editor.md#tokens-and-themes)).

### 3.3 A2: the built primitives, one area per change

| Change | ear today | patchbay | Notes | Status |
|---|---|---|---|---|
| Spinner and WarnMark | `Spinner.tsx`, `WarnMark.tsx`, raw `warn-mark` spans in `Counters.tsx` and `CompanionStrip.tsx` | Spinner, WarnMark (`label`) | | built |
| Splitters | `Splitter.tsx`, `sizes.ts` dragged, keyed, clamp | Splitter, `edge="start"` | Bounds stay in `sizes.ts` until A5. `edge` is logical since 0.7.0 (owner, 2026-11-26: start and end follow the text direction); the editor sets no `dir` and is left-to-right, so `start` is the inspector's left and the drawer's top | built |
| Canvas tools | `CanvasTools.tsx`, a plain `role=toolbar` | ToolGroup and IconButton, inside React Flow's `<Panel>` | Roving tab stop (new); `fitPadding` still reserves 76 px for 40 px touch tools | built |
| Shortcut sheet | `ShortcutSheet.tsx` on `.palette-backdrop` | Dialog, through patchbay's ShortcutSheet | Gains a Close button and a scrim press; built with A4's palette and keys, which replaced this step | built |
| Top bar and drawer buttons | plain `<button>`s for the palette, `ear.conf` and Inspector toggles, shorter than ThemeSwitch, and the drawer's toggle beside a small Button | Button (the drawer's toggle `size="sm"`, as its Save) | The same height as the theme switch, and 36 px under a coarse pointer from patchbay's own rule | built |
| Forms and busy buttons | AddNodeJson, AddNodeText's select, lifecycle bar, Delete, the two Saves | Field, TextInput, Select, Button `busy`, `tone="danger"` | Busy stays focusable, its words in `busyLabel` (D4); the lifecycle row keeps its segmented CSS, its words under the spinner | built |
| Pills, chips, facts | `StatePill.tsx`, `.chip tone-*`, EngineFactsSection's `<dl>` | Pill, Badge, KeyValue | Pill and Badge `accent` and `muted`, a filled Badge (D3); the counter tables stay ear's markup | built |
| Narrow inspector | CSS-only sheet in `editor.css` | Sheet (right below 760 px, bottom below 600 px) | The workspace was already `position: relative`; the sheet is titled "Inspector" and keeps the docked width | built |
| Kept sizes | `prefs.ts` width, drawer, minimap | `usePersistentState` with the same keys and checks | Places stay `prefs.ts`'s, read and written with patchbay's `readKept` and `writeKept` | built |

The health-to-tone map for Pill is built (`health.ts`'s `stateTone`, [editor.md](../editor.md#the-node-box)). It draws the node
box's stripe too since A3; the `--state` classes are left only for the minimap.

### 3.4 A3: the node box, wires, meters, the canvas

patchbay's graph layer carrying each is released (0.6.0).

- **The node box** (built). `EarNode.tsx` wraps NodeBox and PortRow: the stripe tone and Pill from `health.ts`, the
  companions as Badges in the footer, `PortMeter` in each port's trailing slot, the handle ids from `connect.ts` passed
  through, and PortRow's hints from `useConnectHints` (built with the wires). The ports are
  stacked, as a name and a meter do not fit side by side in 240 px. `layout.ts` sizes boxes by `nodeBoxSize` and
  places quick add by `NODE_HEADER`, `NODE_PAD` and `NODE_ROW` instead of its own constants (220, 68, 22), and
  `layout.test.ts` checks those exports instead of reading `editor.css` ([editor.md](../editor.md#the-node-box)).
- **Wires** (built). `LevelEdge.tsx` wraps Wire; `wires.ts`'s `wireData` computes its tone (`ok`, `warn`, `err`, as
  the statuses were coloured), emphasis, dashes, flow, label and title. `GhostEdge.tsx` wraps GhostWire with its
  refused tone; `requests.ts` keeps a refused request one more second. `connect.ts`'s `handleCue` is gone:
  `useConnectHints` over `canConnect` (ear's rule) gives the hints and React Flow's drop check, merged with the canvas's
  other handlers by `mergeFlowProps`. The per-port `useLevel` path stays; at 30 level messages a second no path is
  rebuilt, and the edges restyle less than before [V: measured in headless Chromium, [editor.md](../editor.md#wires)].
- **Meters** (built). `Meter.tsx`'s `PortMeter` is patchbay's Meter fed by `levels.subscribe` and `levels.get`, reset
  when the connection closes. The dBFS scale, zones, hold, fall and clip lamp moved with it at Meter's defaults:
  patchbay's Meter owns the scale (D10). `meters.ts` keeps only what Meter does not hold, ear's tooltip words
  ([editor.md](../editor.md#meters)); `PortMeter` is each port's trailing slot (built with the node box).
- **The canvas** (built). `Canvas.tsx` keeps its own `<ReactFlow>` (D1) and takes patchbay's sugar beside it:
  `CanvasTools` inside the flow replaces `CanvasTools.tsx` (patchbay's words, glyphs and minimap; `editor.css` lifts
  the minimap to the top right, where it was, since patchbay puts it at the bottom right where the toasts are);
  `useAddAt` serves both quick-add paths, `connect.ts`'s `quickAddKind` keeping ear's rule that only an output's wire
  opens it (`droppedOnPane` is gone); and `useFollowFit` in `App.tsx` replaces `fit.ts`, its key the names of the nodes
  on the canvas and its fit options ear's padding. A jump to a node (Go to, a toast's Show) stops following through
  the hook's own `onMoveStart` with an event, as patchbay has no call to stop it. ear needs no `onFitted`: nothing
  waits on the layout. Every React Flow prop stays ear's to pass: delete stays a request (`onBeforeDelete` returns
  false), the minimap is coloured by state class, and toasts stay mounted inside the flow so Show can fit the view.
  The three hooks' handlers reach `<ReactFlow>` through `mergeFlowProps` ([editor.md](../editor.md#the-canvas)).

### 3.5 A4: feedback, the palette and keys, the code editor

- **Banners and status words** (built). `TextPane.tsx`'s banners, the node box's and the add form's are Banner; the
  drawer header's words are StatusWords, with `drawer.ts`'s tones (its `muted` is drawn as StatusWords' neutral tone)
  ([editor.md](../editor.md#the-drawer)).
- **Palette and keys** (built). `Palette.tsx` is CommandPalette (centred for ⌘K, anchored at the pointer for quick
  add); `palette.ts` keeps the catalogue, hands it over with `paletteItems`, and lost `rank` and `anchoredAt`.
  `useShortcuts.ts` is patchbay's `useShortcuts`, with ⌘K, ⌘E and ⌘S opting into text fields (D9); `keys.ts` keeps the
  bindings as key strings and the sheet's lines, and takes `keyLabel`, `detectPlatform` and the text-field rule from
  patchbay. `ShortcutSheet.tsx` is patchbay's ShortcutSheet; while it is open no binding acts, as its Dialog holds the
  focus ([editor.md](../editor.md#the-palette-and-the-keyboard)).
- **Toasts** (built). `Toasts.tsx` is Toaster, inline in a React Flow panel; `notices.ts`'s `Notices` is the session's
  store over the pure queue `useToasts` holds (`addToast`, `holdToasts`, `expireToasts`, `visibleToasts`), keyed by
  code and subject, with patchbay's defaults (D11). It is not `useToasts` itself, since the session raises toasts outside
  React (its `ask`, a `node.state`) and its tests read them without React. `notices.ts` keeps the refusal wording,
  `showOf` and `placedRefusal` ([editor.md](../editor.md#toasts)).
- **The code editor.** `KnoblyText.tsx`, `highlight.ts` and `gutter.ts` become CodeEditor and CodeText with
  `language="knobly"`. knobly stays pinned by `cmake/knobly.json`; a bump through `scripts/knobly-pin.mjs` must stay in
  patchbay's peer range.

### 3.6 A5: the shell

`App.tsx`, `TopBar.tsx`, `Inspector.tsx` and `Drawer.tsx` compose AppShell; ear's words fill its slots. The drawer's
body stays mounted while closed (draft, caret and scroll kept), as now. `hooks.ts`'s `useMediaQuery` is already
`useMediaBreakpoint` (built, with A4: `App.tsx` reads its width class, and `editor.css`'s phone and touch queries are
its `mediaQueries`); `sizes.ts` keeps only ear's defaults. The change that lands A5 deletes this page.

### 3.7 Tests that move

| Test | What happens to it |
|---|---|
| `theme.test.ts` | Done in A1: token sets, contrast and the cycle cases went to patchbay (its `tokens.test.ts` and `useTheme`'s tests); it keeps "no colour in `editor.css` and components", and holds every `var()` to a token and `index.html`'s theme script to patchbay's |
| `sizes.test.ts` | Drag and key cases went in A2 (built: patchbay's Splitter tests them); the breakpoint case reads patchbay's `useMediaBreakpoint` widths and `mediaQueries` (built); the `editor.css` clamp and breakpoint cases go to patchbay's AppShell tests in A5 |
| `layout.test.ts` | Placement stays; the CSS half checks NodeBox's exported geometry instead (built in A3) |
| `prefs.test.ts` | Split in A2 (built): it reads the simple values as `usePersistentState` does, with ear's keys and checks; the hook is tested in patchbay |
| `keys.test.ts`, `palette.test.ts` | Split in A4 (built): ear's bindings, run through patchbay's `findShortcut`, and its catalogue, handed to `rankCommands`, stay; the parsing, ranking and anchoring cases went to patchbay |
| `notices.test.ts` | Kept in A4 (built): its queue cases run on patchbay's queue through ear's store and pin patchbay's defaults; the wording cases stay |
| `meters.test.ts` | Split in A3 (built): the tooltip's words stay, and a case pins Meter's defaults to editor.md; the hold, fall, lamp and reset cases went with `PeakHold` (patchbay's Meter tests them) |
| `fit.test.ts` | Gone with `fit.ts` in A3 (built): patchbay's `useFollowFit` tests the following, the stop and Fit |
| `gutter.test.ts`, `highlight.test.tsx` | Go with their modules, except `highlight.test.tsx`'s last case (what the pane, the node box and the add form send), which stays |
| `wires.test.ts`, `connect.test.ts` | Stay (built in A3): `wires.test.ts` checks `wireData`, its width and opacity cases gone with ear's own stroke (Wire draws the emphasis); `connect.test.ts` checks `canConnect` against the drop rule, the origin, ok and no hints gone to patchbay's `useConnectHints` tests, and `quickAddKind`, `droppedOnPane`'s pane and drop cases gone to `useAddAt`'s |
| `health` and the other rule tests | Stay |

Component behaviour is tested in patchbay (jsdom and Storybook); ear keeps logic tests.

## 4. What it changes in the built system

- `web/package.json` and its lock gain `@mcurcio/patchbay`, pinned exactly. Every bump is a `build(deps)` change and
  a patch release of ear.
- `docs/editor.md`: each step rewrites the section it touches to cite patchbay's docs instead of restating them —
  [tokens and themes](../editor.md#tokens-and-themes) in A1; [recent marks](../editor.md#recent-marks),
  [the shell](../editor.md#the-shell) and [what the browser keeps](../editor.md#what-the-browser-keeps) in A2;
  [the node box, wires and meters](../editor.md#the-node-box-wires-and-meters) in A3;
  [feedback](../editor.md#feedback), [the palette and the keyboard](../editor.md#the-palette-and-the-keyboard),
  [highlighting](../editor.md#highlighting) and [the drawer's gutter](../editor.md#the-drawers-gutter) in A4; the shell
  again in A5. `make docs-check` holds the anchors comments cite.
- Visible changes, each confirmed (section 1): the canvas tools' roving tab stop, busy buttons staying focusable,
  the shortcut sheet's Close button and scrim, the theme script, the top bar's and drawer's buttons matching patchbay's,
  and small shifts in pills and chips; in A3 and A4
  the node box's look (240 px wide, the pill beside the name, the type under it, the ports stacked, the companions
  as Badges, PortRow's handle hints), the wires' look (Wire's 1.5 to 3.5 px by level, the accent when selected, no
  hover thickening, the round flow dots, the status word on a neutral label, and no hint on the drag's own side), the
  ghost wire in `--pb-quiet`, the canvas tools' words, glyphs and order (zoom in first) and their animated zoom and
  fit, a minimap that no longer pans or zooms the view, the banners' stripe and close button, the palette's grouped list with each hint on a second line and patchbay's
  ranking (D9), no binding acting while the shortcut sheet is open, a chord matching its modifiers exactly, Show
  dismissing its toast (D11), and the meters' lamp as a square beside the bar and their accessible names (D10).
- Nothing on the wire changes: `protocol.ts`, `rpc.ts` and the fixtures in `test/protocol/` are untouched.

## 5. Decisions already taken

- Exact pins, one patchbay release per bump.
- No canvas wrapper (D1): ear renders `<ReactFlow>` and adds patchbay's pieces beside it.
- Order: A1 before anything; A2's changes in any order once A1 is in; A3, A4 and A5 each after the patchbay release
  that carries their part.
- ear's words, rules, stores and keys never move into patchbay.

## 6. Open

Nothing: every decision is taken (section 1).
