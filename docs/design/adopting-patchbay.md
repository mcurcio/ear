# The graph editor on patchbay: what is still to build

Status: designed; of A1 only the package and its tokens are built. The graph editor (`web/`,
[docs/editor.md](../editor.md)) draws everything itself: its tokens in `web/src/theme.css`, its controls in
`web/src/components/`, its styles in `web/src/editor.css` (1677 lines).
`@mcurcio/patchbay` (npm, 0.2.0) is a component library extracted from this editor: its `tokens.css` is `theme.css`
renamed, and its built primitives and hooks are generic versions of the editor's leaves. This page designs moving the
editor onto it, one area per change, so that the editor keeps its behaviour and ear keeps everything that is not
drawing. Most of what the editor would take (the shell, the palette, toasts, the code editor, the node box, wires and
meters) is still only designed in patchbay; those steps wait on patchbay's releases.

| Step | What | Waits on |
|---|---|---|
| **A1** | The rename and the theme (section 3.2); the package and its tokens are in | nothing |
| **A2** | The built primitives and hooks, one area per change (section 3.3) | the confirmations in section 6 |
| **A3** | The node box, wires, meters and the canvas sugar (section 3.4) | patchbay's graph layer, released |
| **A4** | Feedback, the palette and keys, the code editor (section 3.5) | patchbay's composites and hooks, released |
| **A5** | The shell, and deleting this page (section 3.6) | patchbay's `AppShell`, released |

[V] = verified in the file named (patchbay's files by their path in its repository); [U] = unverified, with the step
that checks it.

## 1. The owner's words that bind it

The choices still open are in section 6; each answer goes in this table with its date when it is given.

| # | The owner's words or decision | Date | What it means here |
|---|---|---|---|
| D1 | No GraphCanvas wrapper: patchbay ships node and edge types and adds sugar beside the app's own `<ReactFlow>` (`CanvasTools`, `useAddAt`, `useFollowFit`), so every React Flow prop and later feature stays reachable | 2026-11-25 | `Canvas.tsx` keeps rendering `<ReactFlow>` itself and takes patchbay's three pieces one by one (3.4); patchbay's pb-nx9.15 is settled |

## 2. What is built

- **The editor.** React 19.3, `@xyflow/react` 12.12, dagre, Vite 8, Vitest in its default environment (no DOM, no
  Storybook): logic tests, plus tests that read `theme.css` and `editor.css` as text to hold CSS equal to TS constants
  (`theme.test.ts`, `sizes.test.ts`, `layout.test.ts`). knobly comes from its GitHub Release tarball, pinned by
  `cmake/knobly.json` ([AGENTS.md](../../AGENTS.md), [editor.md](../editor.md#highlighting)).
- **patchbay 0.2.0.** Peer dependencies `react` and `react-dom` `^19`, and optionally `@xyflow/react` `^12` and
  `knobly` `^1.0.0` [V: `package.json`]; ear meets every range. Subpaths `./tokens.css`, `./base.css`, `./primitives`,
  `./hooks`; `./composites`, `./shell` and `./graph` resolve but export nothing yet [V: `src/*/index.ts`].
  Built: the `--pb-` tokens with a light set, a system-dark set and a chosen-dark set and React Flow's `--xy-*`
  variables mapped onto them; Button, IconButton, Spinner, Pill, Badge, WarnMark, KeyValue, SectionTitle, Field,
  TextInput, Select, Checkbox, Splitter, Toolbar, ToolGroup, Panel, Dialog, Sheet, ThemeSwitch; `useTheme` and
  `usePersistentState`. It releases by semantic-release; while it is 0.x a breaking change is a minor release.
- **The values match.** Every colour in `theme.css` is the same in `tokens.css`, in light and both dark sets; the names
  change (`--X` → `--pb-X`, `--knobly-Y` → `--pb-code-Y`). patchbay's system-dark rule is `:root:not([data-theme])`
  where ear's is `:root:not([data-theme="light"])`; they agree for every value `applyTheme` writes.
- **The editor loads it.** `web/package.json` pins `@mcurcio/patchbay` 0.2.0 exactly, and `npm ci` takes the
  tarball-installed knobly 1.0.0 as meeting its `^1.0.0` peer [V: `npm ls` dedupes it]. The order the styles load in,
  and patchbay's mapping of React Flow's `--xy-*` variables, are in [editor.md](../editor.md#tokens-and-themes).

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

The package and its tokens are in: patchbay 0.2.0 pinned exactly, `tokens.css` loaded before `theme.css`, and
React Flow's `--xy-*` variables mapped by patchbay ([editor.md](../editor.md#tokens-and-themes)). `base.css` is
optional and waits for A5. What A1 still has to do:

1. **The rename.** One mechanical change: every `var(--X)` in `editor.css` and the components becomes `var(--pb-X)`,
   `--knobly-*` becomes `--pb-code-*`, the font literals become `--pb-font-*` and `--pb-text-*`, and `theme.css` is
   deleted. A missed name fails silently, so the same change adds a test that every `var(--…)` in `web/src` names a
   token in patchbay's `tokens.css` or a variable ear declares (`--state`, `--inspector-width`, `--gutter-digits`,
   `--code-size`).
2. **The theme.** `useTheme({ key: "ear-editor/1/theme" })`, so the choice people kept is read where it is; patchbay's
   ThemeSwitch with `labels` carrying ear's tooltip words if they stay. `themeScript` inline in `index.html` would end
   the first-paint flash (section 6).

### 3.3 A2: the built primitives, one area per change

| Change | ear today | patchbay | Notes |
|---|---|---|---|
| Spinner and WarnMark | `Spinner.tsx`, `WarnMark.tsx`, raw `warn-mark` spans in `Counters.tsx` and `CompanionStrip.tsx` | Spinner, WarnMark (`label`) | |
| Splitters | `Splitter.tsx`, `sizes.ts` dragged, keyed, clamp | Splitter, `edge="start"` | Bounds stay in `sizes.ts` until A5 |
| Canvas tools | `CanvasTools.tsx`, a plain `role=toolbar` | ToolGroup and IconButton, inside React Flow's `<Panel>` | Roving tab stop (new); `fitPadding` still reserves 76 px for 40 px touch tools |
| Shortcut sheet | `ShortcutSheet.tsx` on `.palette-backdrop` | Dialog | Gains a Close button and a scrim press |
| Forms and busy buttons | AddNodeJson, AddNodeText's select, lifecycle bar, Delete, the two Saves | Field, TextInput, Select, Button `busy`, `tone="danger"` | Busy stays focusable; ear's "Saving…" words passed as children if kept |
| Pills, chips, facts | `StatePill.tsx`, `.chip tone-*`, EngineFactsSection's `<dl>` | Pill, Badge, KeyValue | Needs Pill and Badge `accent` and `muted`, a filled Badge |
| Narrow inspector | CSS-only sheet in `editor.css` | Sheet (right below 760 px, bottom below 600 px) | Workspace becomes `position: relative` |
| Kept sizes | `prefs.ts` width, drawer, minimap | `usePersistentState` with the same keys and checks | Places stay `prefs.ts`'s |

The health-to-tone map for Pill: `running` ok; `starting`, `stopping`, `unknown` warn; `failed` err; `stopped` quiet;
`disabled` off; `reconfiguring` accent; `pulse` while configuring. The node box's stripe keeps its `--state` class until
A3.

### 3.4 A3: the node box, wires, meters, the canvas

Each waits for the patchbay release carrying it.

- **The node box.** `EarNode.tsx` wraps NodeBox and PortRow: the stripe tone and Pill from `health.ts`, the companions
  in the footer, a meter as each port's trailing slot, the handle ids from `connect.ts` passed through. `layout.ts`
  takes the node width, header height and row height from NodeBox's exported geometry instead of its own constants
  (220, 68, 22), and `layout.test.ts` checks those exports instead of reading `editor.css`.
- **Wires.** `LevelEdge.tsx` wraps Wire; `wires.ts` computes its tone, emphasis, dashes, flow, label and title.
  `GhostEdge.tsx` wraps GhostWire with its refused tone; `requests.ts` keeps a refused request one more second.
  `connect.ts`'s `handleCue` becomes `useConnectHints` over `isValidConnection`. The per-port `useLevel` path stays,
  and a profile at about 30 level messages a second shows no more rendering than today [U: checked in that change].
- **Meters.** `Meter.tsx` becomes patchbay's Meter fed by `levels.subscribe` and `levels.get`, reset when the
  connection closes. Whether the dBFS scale, zones, hold, fall and clip lamp (`meters.ts`) move or stay ear's is open
  (section 6).
- **The canvas.** `Canvas.tsx` keeps its own `<ReactFlow>` (D1) and takes patchbay's sugar beside it: `CanvasTools`
  inside the flow replaces `CanvasTools.tsx`, `useAddAt` serves both quick-add paths, and `useFollowFit` replaces
  `fit.ts`. Every React Flow prop stays ear's to pass: delete stays a request (`onBeforeDelete` returns false), the
  minimap is coloured by state class, and toasts stay mounted inside the flow so Show can fit the view.

### 3.5 A4: feedback, the palette and keys, the code editor

- **Banners and status words.** `TextPane.tsx`'s banners become Banner; the drawer header's words become StatusWords,
  with `drawer.ts`'s tones.
- **Palette and keys.** `Palette.tsx` becomes CommandPalette (centred for ⌘K, anchored at the pointer for quick add);
  `palette.ts` keeps the catalogue and loses `rank` and `anchoredAt`. `useShortcuts.ts` becomes patchbay's, with ⌘K and
  ⌘S still acting while typing; `keys.ts` keeps the bindings and takes `keyLabel` and `isTextTarget` from patchbay.
- **Toasts.** `Toasts.tsx` and `notices.ts`'s queue become Toaster and `useToasts`, keyed by code and subject.
  `notices.ts` keeps the refusal wording, `showOf` and `placedRefusal`.
- **The code editor.** `KnoblyText.tsx`, `highlight.ts` and `gutter.ts` become CodeEditor and CodeText with
  `language="knobly"`. knobly stays pinned by `cmake/knobly.json`; a bump through `scripts/knobly-pin.mjs` must stay in
  patchbay's peer range.

### 3.6 A5: the shell

`App.tsx`, `TopBar.tsx`, `Inspector.tsx` and `Drawer.tsx` compose AppShell; ear's words fill its slots. The drawer's
body stays mounted while closed (draft, caret and scroll kept), as now. `hooks.ts`'s `useMediaQuery` becomes
`useMediaBreakpoint`; `sizes.ts` keeps only ear's defaults. The change that lands A5 deletes this page.

### 3.7 Tests that move

| Test | What happens to it |
|---|---|
| `theme.test.ts` | Token sets and contrast go (patchbay's `tokens.test.ts` holds them); "no colour in `editor.css` and components" stays, for `--pb-*`; the new var-resolution test joins it; the cycle cases go with `theme.ts` |
| `sizes.test.ts` | Drag and key cases go in A2; the `editor.css` clamp and breakpoint cases go to patchbay's AppShell tests in A5 |
| `layout.test.ts` | Placement stays; the CSS half checks NodeBox's exported geometry (A3) |
| `prefs.test.ts`, `keys.test.ts`, `palette.test.ts`, `notices.test.ts`, `meters.test.ts` | Split: ear's keys, bindings, catalogue and wording stay; the generic mechanics' cases go with them |
| `fit.test.ts`, `gutter.test.ts`, `highlight.test.tsx` | Go with their modules, except `highlight.test.tsx`'s last case (what the pane, the node box and the add form send), which stays |
| `wires.test.ts`, `connect.test.ts`, `health` and the other rule tests | Stay |

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
- Visible changes, each confirmed before its step: the canvas tools' roving tab stop, busy buttons staying focusable,
  the shortcut sheet's Close button and scrim, the theme script, and small shifts in pills and chips.
- Nothing on the wire changes: `protocol.ts`, `rpc.ts` and the fixtures in `test/protocol/` are untouched.

## 5. Decisions already taken

- Exact pins, one patchbay release per bump.
- No canvas wrapper (D1): ear renders `<ReactFlow>` and adds patchbay's pieces beside it.
- Order: A1 before anything; A2's changes in any order once A1 is in; A3, A4 and A5 each after the patchbay release
  that carries their part.
- ear's words, rules, stores and keys never move into patchbay.

## 6. Open

| Question | Options | Leaning | Blocks |
|---|---|---|---|
| Inline `themeScript` in `index.html`? | yes (no flash, new behaviour); no | yes | A1's theme change |
| Keep ear's tooltip words on the theme switch? | ear's via `labels`; patchbay's | ear's | A1's theme change |
| Busy buttons: stay focusable with words kept (patchbay), or disabled with "Saving…" (ear)? | patchbay's; ear's words as children | focusable, ear's words as children | A2 forms |
| The canvas tools' roving tab stop | accept; keep Tab to each tool | accept | A2 canvas tools |
| Pill and Badge `accent`, `muted`, filled Badge in patchbay, or neutral tones here? | ask patchbay; accept neutral | ask patchbay | A2 pills and chips |
| patchbay's six builder choices (its pb-nx9.29), WarnMark's `label` among them | confirm; reverse | confirm | A2 |
| Meter's scale: patchbay's props with ear's values as defaults, or fractions from `meters.ts`? | props; fractions | props | A3 meters |
| How much of the toast queue patchbay takes | all mechanics as options; the thin design | all | A4 toasts |
| Chords while typing and Tab in the palette | per-binding opt-in, Tab closes; strict | opt-in, Tab closes | A4 palette and keys |
