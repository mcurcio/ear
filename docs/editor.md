# The graph editor

ear's control port (7777) serves a graph editor at `/`: a browser page that draws the running
graph and edits it through the same JSON-RPC calls a script uses
([control-port.md](control-port.md)). Its source is `web/` (React, React Flow, Vite, TypeScript,
Vitest); it shares no code with the C++, only the wire format and the fixtures in `test/protocol/`.

## What it shows and does

The page is a top bar, the canvas with an inspector on its right, and a full-width `ear.conf` drawer
along the bottom ([below](#the-shell)).

- **Nodes** are patchbay's 240 px node boxes ([below](#the-node-box)): a stripe down the left edge and a pill (a
  dot and the word) in the state's colour, the type under the name, the line that says why the node is silent or
  what it waits for ([below](#a-nodes-health)), and beside the pill a warning mark while the node lost cycles in the
  last minute ([below](#counters)). Then one row per port, the inputs' with their handles on the left and then the
  outputs' with theirs on the right, each with a live level meter ([below](#meters)), and in the footer a badge per
  companion ([below](#companions)).
- **Routes** are wires coloured by status (`linked`, `pending`, `refused`) and drawn by their
  from port's level: louder is thicker and brighter. A linked wire carrying signal shows a slow
  flow, a refused one says `refused` at its midpoint, and hovering a wire names its two ends
  ([below](#wires)).
- **Editing:** a drag from an output to an input adds a route (any other drag is refused before
  it reaches ear); while a wire is drawn, every handle it can be dropped on grows in the ok colour and the
  rest turn grey ([below](#the-node-box)); the delete key removes the selected wire or node; the inspector
  adds nodes from a statement in knobly (its Graph subject, [below](#adding-a-node)), edits the
  selected node's own text in knobly (its Node subject, [below](#the-node-panels-text)), both as
  JSON settings from an ear without `node-text`, and offers Start, Stop, Restart, Disable and
  Enable in the inspector when ear has the `lifecycle` feature. The top bar gives the graph's rate
  and block size ("48 kHz · 256 frames", from `graphFrequency` in hertz) and realtime, or a "not
  realtime" badge, and a dropouts badge while the host cut cycles in the last minute; the inspector
  holds the graph's counters and the selected node's ([below](#counters)). A request on its way shows while it is
  (a dashed ghost wire, a spinner on its button, `Saving…` and `Adding…`), and a refusal becomes a toast at the
  canvas's bottom right, or lands where it was asked ([below](#feedback)).
- **Keyboard and palette:** Cmd+K (Ctrl+K off macOS) opens a command palette for going to a node, adding, the
  lifecycle calls, removing, `ear.conf`'s actions, the view and the theme; `?` lists the shortcuts
  ([below](#the-palette-and-the-keyboard)).
- **Adding on the canvas:** a double-click on empty canvas, or an output's wire dropped there, opens a type list at
  the pointer, and the new node lands where it was asked for ([below](#adding-on-the-canvas)).
- **Kept in the browser:** node places, the inspector's width, the drawer's height and open state, the minimap and
  the theme survive a reload ([below](#what-the-browser-keeps)).
- **The ear.conf drawer** holds the pane: `ear.conf` in patchbay's code editor, highlighted, with line numbers and a
  mark on each line a diagnostic names ([below](#highlighting)), its diagnostics listed under it (a click
  puts the caret there) and Save (`graphFile.set`, based on the version
  the draft started from). Banners say
  when the graph refused the file, a write failed, editor edits were dropped by a hand edit, or a
  setting applies only at restart (the graph rate or block size, as `pendingRestart` names them).
- **The theme** is light, dark or the system's (the default), switched by the button in the top
  bar, which cycles system, light and dark ([below](#tokens-and-themes)).

## Its rules

- **ear is the one source of truth.** The editor draws only what ear announced: a node or wire
  appears on its notification, never on the request that asked for it, and an event naming a node
  or route it does not know changes nothing. It never parses knobly; ear judges every text. Its
  highlighting only colours the text ([below](#highlighting)). A request on its way is drawn as a request (a ghost
  wire, a spinner), never as what it asks for, and is gone on its reply ([below](#requests-in-flight)).
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
  sinks right, following the routes). A place the person dragged a node to, and where a quick add
  put it, is kept in the browser's `localStorage` per ear and never sent to ear
  ([below](#what-the-browser-keeps)). Tidy lays every node out again, in the browser only.
- **Shortcuts never take a text field's keys.** Inside an input, textarea or select every single
  key is the text's ([below](#shortcuts)).
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
  re-render. A meter (patchbay's `Meter`) reads the store itself and paints through refs, and keeps
  its peak hold outside React, so neither is React state ([below](#meters)). A closed connection makes every
  meter read silent and forgets every hold until new levels arrive.
- **A lost socket reconnects** after a growing wait (1 s doubling to 30 s), failing the calls in
  flight (the requests in flight are forgotten with it; the toasts are kept), and the new socket subscribes again (graph, graph file, levels), so the editor starts
  over from a fresh snapshot.

## What patchbay draws

The editor draws with `@mcurcio/patchbay` (npm), a component library extracted from this editor: its tokens and
themes, primitives, composites, shell, graph layer and UI hooks. *Why:* the owner's principle (2026-11-26): "build
the correct model on patchbay primitives. Ear and rasl UIs are not precious to me. They should be reworked when
patchbay has a matching concept." Where the editor differed from patchbay in look or behaviour, it took patchbay's
model; nothing of ear's look was kept for its own sake.

| patchbay's (drawn from props) | ear's |
|---|---|
| Colours, radii, shadows, durations, faces and type sizes ([below](#tokens-and-themes)) | The words: `shell.ts`, `drawer.ts`, `pane.ts`, `notices.ts`'s refusal wording, `health.ts`'s, `meters.ts`'s tooltip |
| The shell's layout, sheets, splitters and bounds; buttons, pills, badges, facts, fields, banners, status words, the palette, the shortcut sheet, the toasts, the code editor and its diagnostics' list | The rules: `health.ts`, `companions.ts`, `counters.ts`, `marks.ts`, `selection.ts`, `requests.ts`, `wires.ts` (as the adapter onto Wire's props), `connect.ts`'s `canConnect` |
| The node box, port rows, wires, ghost wires, connect hints, meters, the canvas tools, `useAddAt`, `useFollowFit` | The protocol, the session and its stores: `protocol.ts`, `rpc.ts`, `editor.ts`, `graph.ts`, `graphFile.ts`, `levels.ts` |
| Kept values and the theme (`usePersistentState`, `useTheme`), key labels and matching (`useShortcuts`), palette ranking, the toast queue's mechanics, knobly highlighting | Storage keys and checks (`prefs.ts`), the command catalogue (`palette.ts`), the key bindings (`keys.ts`), first placement (`layout.ts`, dagre), the subject the inspector shows, Escape's order among ear's own surfaces |

- **ear's own `<ReactFlow>`.** There is no canvas wrapper: `Canvas.tsx` renders React Flow's `<ReactFlow>` and adds
  patchbay's pieces beside it ([The canvas](#the-canvas)), so every React Flow prop stays ear's (owner, 2026-11-25).
- **Words.** Every patchbay component takes the words it supplies itself (visible text and accessible names)
  through one `labels` prop, a partial object with English defaults (owner, 2026-11-26), with no app-wide provider.
  ear sets three: the palette's placeholder, the meters' tooltip (`labels.title`) and AppShell's drawer words
  (`ear.conf`, "Show ear.conf", "Hide ear.conf", "Resize ear.conf"); every other word is patchbay's default.
- **The pin.** `web/package.json` pins `@mcurcio/patchbay` exactly, and every bump is its own `build(deps)` change
  (a patch release of ear). Its links here name the release pinned.
- **What ear still draws itself,** each because patchbay has no matching concept: the counters' tables
  (`Counters.tsx`, a plain table), the lifecycle bar's segmented row (patchbay's Buttons in ear's row), the route panel's `node:port` buttons, the JSON settings textareas for an ear without `node-text` (a schema
  form waits for slice 5's type schema; the owner deferred patchbay's), the inspector's several-selected list, the
  companions' list in the node panel, the minimap's state colours and the wordmark. `editor.css` gives these plain
  controls ear's base rules, which pass by every `pb-` element.
- **Tests.** Component behaviour is tested in patchbay (jsdom and Storybook); ear keeps logic tests, plus the tests
  that hold `editor.css` and `index.html` to patchbay's exports ([Developing it](#developing-it)).

## The shell

The layout is patchbay's `AppShell`
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#appshell)): the top bar, the canvas,
the inspector at its right and the drawer along the bottom, the narrow and phone layouts, the splitters and the
sizes' bounds, filling the screen. `App.tsx` fills its slots with ear's: the top bar's slots from `TopBar.tsx`'s
`topBar`, `Canvas` (with patchbay's `CanvasTools`) as its child, `Inspector.tsx`'s `InspectorTitle` as the
inspector's title and `Inspector` as its content, and the
drawer's header and pane from `Drawer.tsx`'s `drawerSlots`, with the canvas's state in `useCanvas` and the drawer's in
`useDrawer`, so the top bar and the drawer's header read one status. AppShell keeps no data: the inspector's width,
the drawer's height and whether each is open are ear's, kept per ear ([What the browser keeps](#what-the-browser-keeps)).
*Why patchbay's:* the owner's principle is to take patchbay's model where it has the concept (2026-11-26).

### The top bar

AppShell's bar, 44 px across the top, holding what is about the whole graph and never about a selection, from left
to right in its four slots:

- the brand: the wordmark (dropped below 600 px);
- the status: the connection pill, `live` (ok), `connecting…` (warn) or `lost: retrying in 4 s` (err), a word beside its dot,
  the seconds counted down to the client's next socket, as `rpc.ts` tells the editor (`onRetry`), in a status
  region so a change is read out;
- the start, taking the free width and cut short at its end: the engine chip, a muted badge: the rate and block size, then `realtime` or the "not realtime" badge, filled in
  the error tone so it stands out, its tooltip the scheduling;
- the dropouts badge, in the warn tone ([Counters](#counters));
- a `protocol N` badge in the warn tone while ear speaks a newer protocol than this editor's, its tooltip saying
  some of it is not shown;
- the end: the palette button, its key (`⌘K` or `Ctrl+K`) as its text
  ([below](#the-palette)), the theme switch and the `ear.conf` toggle with the drawer's status words as a
  badge in the first word's tone;
- below 760 px, an Inspector button ([below](#narrow-windows)).

`shell.ts` holds the words and their tones (`shell.test.ts`). The palette, `ear.conf` and Inspector buttons are
patchbay's `Button`, as tall as the theme switch beside them. The pill is patchbay's `Pill` and the chips its
`Badge` ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#state)), which colour the word
and the dot, or the text and the edge, in the tone.

### The canvas

It fills the rest, a quiet dot grid in the theme's colours. `components/Canvas.tsx` renders React Flow's own
`<ReactFlow>`, so every React Flow prop stays ear's to pass, and adds patchbay's canvas pieces beside it, each
handing its handlers to `<ReactFlow>` through `mergeFlowProps`
([patchbay's](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#canvastools)). *Why:* the owner chose no
canvas wrapper, patchbay's sugar beside the app's own `<ReactFlow>` (2026-11-25), and patchbay's model where it
has the concept (2026-11-26).

- **The tools.** patchbay's `CanvasTools`, bottom left in React Flow's `Panel`: zoom in, zoom out, fit the view, tidy
  the layout and the minimap, in patchbay's words and glyphs. The group is one Tab stop, the arrow keys, Home and End
  move between its tools, and the minimap tool is pressed while the minimap shows; 28 px tools, 40 px on touch. Zoom
  and the fit move over `--pb-motion-slow` (180 ms, at once under reduced motion). Fit, tidy and the minimap are also
  `F`, `L` and `M` ([below](#shortcuts)), which the tools' tooltips and `aria-keyshortcuts` carry, and the palette's
  `Fit view`, `Tidy layout` and `Show minimap`.
- **Tidy** lays every node out again with dagre, as on first placement, and they slide there over
  `--pb-motion-slow`; moves are the browser's, so nothing is sent (`layout.ts`'s `relayout`), and the new places are
  kept ([below](#what-the-browser-keeps)).
- **The minimap** is React Flow's `MiniMap` as `CanvasTools` draws it, off by default, framed by patchbay, its boxes in
  their state's token; `editor.css` lifts it from patchbay's bottom right to the top right, as the toasts are at the
  bottom right ([below](#toasts)).
- **The view follows the graph** (patchbay's `useFollowFit` in `App.tsx`): it fits once the boxes are measured, and
  again whenever the nodes on the canvas (its key, their names) or the canvas's size change (a node added, the drawer
  opened), until the person pans or zooms (a press that moves nothing, as a click to deselect, keeps it following), or
  jumps to a node (a palette `Go to` or a toast's Show, which stop it through the hook's `stop()`). Fit (the tool, `F`, the palette) fits and follows again. Every fit leaves a tenth of the canvas on three
  sides and 76 px at the bottom, so the tools, at their touch size, never cover a box. *Why:* a single fit at React
  Flow's first measurement fitted boxes not yet laid out and clipped the graph, and a view that follows the graph is
  what a person expects until they move it themselves.
- **Quick add.** A double-click on empty canvas opens quick add instead of zooming, as does an output's wire dropped
  there ([below](#adding-on-the-canvas)).

Toasts sit at its bottom right ([below](#toasts)), and a route request on its way is a ghost wire
([below](#requests-in-flight)).

### The inspector

AppShell's inspector, on the right, 320 px at first (`sizes.ts`), resized between 280 and 560 px by the splitter on
its left edge (dragged, or focused and moved by the arrow keys, Home and End; its keys, drag and 36 px touch hit area
are patchbay's). On a wide window it is always open. AppShell draws its one header row from the subject's title
(`InspectorTitle`: "Graph", the node's name with its type, "Route", or the count, `selection.ts`'s `subjectTitle`),
as patchbay's `Panel`, which pads and scrolls the body, inside the shell's region named "Inspector"; the body is
`Inspector.tsx`'s `Inspector`. *Why:* one header row in the docked panel and the narrow sheet alike, the subject's
title beside the sheet's close button rather than a row of its own under it (owner, 2026-11-26). It shows one
subject, chosen by the selection (`selection.ts`, `selection.test.ts`):

- nothing selected: **Graph**: the engine facts in full (patchbay's `KeyValue`, a wrong or newer fact in the err
  or warn tone), the graph's counters and the add-node form;
- one node: **Node**: its name and type, its state pill as the box draws it, its line, the
  lifecycle bar (one segmented row; a refusal shown under it as well as in a toast), its knobly box (or JSON settings), its counters, its companions and Delete;
- one route: **Route**: from and to as `node:port` buttons that select that node, its status in
  words, its level meter (a `KeyValue` too) and Remove;
- several: a count ("3 nodes, 2 routes") and Delete for them all.

A name or route the graph does not hold is ignored, so the inspector shows only what ear announced.
Delete asks `node.remove` for each node and `route.remove` only for routes between kept nodes, as
the delete key does. The add-node form lives only in the Graph subject, so selecting something
drops a half-typed statement; a new template is written when it shows again. Slice 5's settings
form and status list are designed to land in the Node subject, under the lifecycle bar and its refusal line
([control-plane.md](design/control-plane.md#8-the-editor)).

### The drawer

AppShell's drawer, full width along the bottom, holding the pane whole. Its header (AppShell's, 44 px) stays when
the drawer is closed: `ear.conf`, then in words what the file needs, most urgent first: `refused`, `write failed`,
`stale: reload`, `unsaved`, `n errors` or `applies at restart` (`drawer.ts`, `drawer.test.ts`),
with Save beside them, which says `Saving…` while its request is on its way. The words are patchbay's
`StatusWords` ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#news-and-state)): each in its
tone (`applies at restart` in the neutral one), a dot between two, the row one polite live region, kept to one line
and cut short before Save. The pane's banners above the text (the file changed under the draft, with Reload; ear
refused the file; a write failed; editor edits were dropped, with a close button; a setting applies at restart) are
patchbay's `Banner`, as are the node box's stale banner and the add form's: a stripe and a soft surface in the tone,
its words read through the hidden live regions inside it, an error's at once (an alert) and the others' when the
reader is free (a status), so a banner shown with its words is read as well as one whose words change. The shell's chevron button at the header's end ("Show ear.conf" or "Hide ear.conf"), and the top bar's `ear.conf`
toggle, open and close it. It opens by
itself only when ear refuses the file or a write fails, each newly (the same refusal sent again
does not reopen it), or when a Save is refused; never on a hand edit. *Why a drawer:* the owner
chose a full-width bottom drawer over a third column, so lines need not scroll sideways and the
canvas keeps its width for a left-to-right graph. It is resized by the shell's splitter on its top edge, its height (320 px at first,
`sizes.ts`) the whole drawer's, header included, drawn between 120 px and 70 % of the window as the window is: a
height kept from a taller window comes back there. The text fills the body ([below](#highlighting)). Closing it hides the body but AppShell keeps it mounted, with the draft, the
caret and the scroll. The inspector and the drawer never move each other's content.

### Narrow windows

Below 760 px AppShell draws the inspector as patchbay's `Sheet` over the canvas's right edge (its inline end), with one
header row: the subject's title, which names the sheet, beside its close button ([The inspector](#the-inspector)), at
the width kept for the docked inspector (within 280 to 560 px and 48 px short of the canvas) and not resized. `App.tsx` holds whether it is open: it opens
when something is selected or from the top bar's Inspector button, and closes on its close button, Escape (before the
selection: [Shortcuts](#shortcuts)) or a click on the canvas; it is not
modal, so the canvas stays live, and closing it with the focus inside gives the focus back. Escape with the focus
inside the sheet is the sheet's own and closes it, even from a text field (patchbay's model). 760 rather than a
laptop-sized width keeps it docked on a foldable phone unfolded (about 890 px wide) and on a tablet in portrait, where
the canvas is still 480 px wide. The drawer keeps its place. Below 600 px (a phone in portrait) the sheet rises from
the bottom instead, the full width and at most 62 % of the workspace, scrolling inside, so the graph stays in sight
above it; and the top bar drops the wordmark and the engine chip (the Graph subject holds the same facts),
patchbay's theme switch shows only its icon (its label still names the choice) and the `ear.conf` toggle drops its glyph,
so its buttons fit beside the connection pill. AppShell picks docked, right or bottom from patchbay's
`useMediaBreakpoint` (wide, narrow below 760 px, phone below 600 px:
[its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#usemediabreakpoint)), which `App.tsx`
reads too for the Inspector button and the wordmark, and `editor.css` hides the engine chip and the toggle's glyph
and grows the targets by the same queries (patchbay's `mediaQueries`); `sizes.test.ts` holds `editor.css`'s queries
to them and `sizes.ts`'s first sizes to AppShell's bounds. Crossing 760 px moves the inspector between the region and
the sheet, which draws its content afresh: a half-typed add-node statement is dropped, as when the selection
changes.

### Phones and touch

The editor is built to work on a phone, in particular a foldable one whose two screens are about 466 × 678 px
folded (portrait: the bottom sheet above) and 890 × 626 px unfolded (landscape: the inspector docked). Folding or
unfolding is a resize, so the layout follows it and the view fits itself again unless the person moved it
([The canvas](#the-canvas)). For a finger (`(hover: none) and (pointer: coarse)` in `editor.css`):

- **No zoom on focus.** Every text field, the code boxes included, is 16 px, as iOS Safari zooms the page when a
  smaller field takes the focus. The code size is one patchbay token, `--pb-code-size` (16 px under a coarse pointer), shared by patchbay's code
  editor's textarea, its highlighted copy and its gutter, so their glyphs still line up ([Highlighting](#highlighting)).
- **Targets a finger finds.** The top bar's buttons are at least 36 px tall (patchbay's Button and ThemeSwitch under a coarse pointer), the canvas tools and the lifecycle bar
  40 px. A port handle's hit area grows under a coarse pointer (patchbay's PortRow,
  [above](#the-node-box)), so a wire starts from a fingertip; React Flow's pan and pinch zoom work as they do.
- **No keyboard.** The palette button shows a search icon instead of its key. The palette, the inspector's buttons
  and the drawer's Save do everything a shortcut does.
- **The whole screen.** The page asks for `viewport-fit=cover`; AppShell fills `100dvh`, so Safari's toolbar never
  covers the drawer, and pads its top bar, sides and drawer by the safe-area insets, so a notch, the rounded corners
  and the home indicator never cover a control.

Hover tooltips (a meter's dBFS, a badge's drop count) do not exist on a touch screen; the inspector shows the
selected node's or route's facts in words instead. Checked in Chromium's touch emulation at both sizes, not yet on
the device ([Developing it](#developing-it)).

The sizes, the drawer's open state and the minimap are kept per ear ([What the browser keeps](#what-the-browser-keeps)).

## Feedback

The editor still draws only what ear announced ([above](#its-rules)); what this adds is that a request is visible
while on its way, and its refusal lands where it was asked. Every request the editor makes goes through the
session's `ask` (`web/src/editor.ts`), which keeps it in flight and makes a toast of any refusal its caller does not
show in place.

### Requests in flight

`web/src/requests.ts` (`requests.test.ts`) keeps each request this editor sent and ear has not answered yet, with
what asked for it: a route's two ends, a node's name and the button, or the drawer or form. A reply clears it, and so
does a closed connection, at once.

- **The ghost wire.** While a route request is on its way, a ghost wire joins the two handles: patchbay's
  `GhostWire` ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#ghostwire),
  `components/GhostEdge.tsx`), a thin dashed `--pb-quiet` line, dimmed, with no status colour, no meter and no
  tooltip, which takes no pointer and cannot be selected, focused or deleted (Delete never sends `route.remove` for
  it). It is hidden once `route.added` draws the route, and goes on the reply. A refused one is GhostWire's
  `refused` tone, `--pb-err` at full opacity, for 1 s (`requests.ts`, timed on the monotonic clock) before it goes. *Why:* the owner chose a ghost while a route request is in flight, drawn as a request and
  never as a route.
- **Buttons.** Every button that sends a request is patchbay's `Button` with `busy` while its own request is on its
  way: a spinner, `aria-busy`, and presses ignored, but it stays focusable, so the focus is never lost mid-request
  ([patchbay's](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#actions)). Save in the drawer and
  in the node box show `Saving…` beside the spinner, and Add in the form `Adding…` (their `busyLabel`, announced
  politely); a lifecycle button keeps its word under the spinner, so the segmented row's widths hold. A button is
  disabled only while idle: a lifecycle button as `health.ts` says, Save and Add while there is nothing to send.
  Delete and Remove are `tone="danger"`. *Why:* the owner chose patchbay's model, focusable while busy
  (2026-11-26), over ear's old disabled button that dropped the focus. The spinner stops turning under reduced motion
  and keeps its open ring.

### Toasts

`web/src/notices.ts` (`notices.test.ts`) is the session's store over patchbay's toast queue, so a refusal made
outside React (the session's `ask`, a `node.state`) is a toast, and `components/Toasts.tsx` draws it with patchbay's
`Toaster`, inline in a React Flow panel at the canvas's bottom right so Show can move the view
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#news-and-state); the queue:
[`useToasts`](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#usetoasts)). *Why patchbay's:* the
owner chose patchbay's toast model (2026-11-26); ear keeps the words, the key and Show.

- **What a toast says.** A refusal says what was asked and why, with the code in monospace
  (`route noise:noise.mono → rec:FX refused: unknown-port rec:FX`). A call the closed connection left unanswered says
  `had no answer: not-connected`, one toast for all of them. JSON settings that do not parse say `not sent:
  invalid-json` with the parse's words, a code of the editor's own that ear never sends. A refusal is an `err` toast,
  an unanswered call or a fallback a `warn` one.
- **The queue's rules, patchbay's defaults.** A toast lasts 8 s on the monotonic clock, and none expires while the
  toasts are hovered or focused; at most four show, older ones folded into "and n more". ear keys a toast by its code
  and its subject, so the same code for the same subject within 5 s counts up ("×3") instead of stacking.
- **The Toaster's.** Its Dismiss button (patchbay's close glyph, named "Dismiss"), or Escape on a focused toast,
  closes one, and the focus moves to its neighbour. Each is
  read once from a live region beside the stack (a refusal at once, anything else when the reader is free), not again
  when it is drawn again.
- **Show.** A toast for a node or route ear holds has Show, which selects it and pans to it, then dismisses the toast.
- **Kept.** A closed connection clears none, so a reconnect drops nothing the person has not seen.

A change in place that could not finish raises a warning toast of its own, about its node, from the `node.state` that
begins its fallback (`fpp could not apply port in place, so it restarted: fell-back deadline 10s passed`): the node
restarts with the new settings, which `ear.conf` holds, so the panel keeps the new value. Slice 5's node events are
designed to feed this queue
([control-plane.md](design/control-plane.md#84-status-events-diagnostics-and-actions)).

### Where a refusal lands

`file-changed` and `invalid-text` stay in the drawer, the node box and the add form as built, with no toast. A
refused route shows on its ghost and in a toast. A lifecycle refusal shows under the inspector's lifecycle bar and in
a toast. Everything else is a toast. A refused Save still opens the drawer.

## The palette and the keyboard

### The palette

`web/src/palette.ts` (`palette.test.ts`) builds the palette's commands from the graph, the selection, the features
and the shell's state, so a command ear cannot take is never offered, and hands them to patchbay's `CommandPalette`
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#commands-and-keys)), which draws, ranks
and places them; `components/useCommands.ts` turns each into its request or its change of view. Cmd+K (Ctrl+K off
macOS) or the top bar's button opens it, centred near the top of the window. Its commands:

| Command | Does |
|---|---|
| `Go to <node>` | selects it and pans to it, as a toast's Show does |
| `Add <type>` | opens the inspector's add-node form with that type's template ([below](#adding-a-node)): the knobly form with `node-text`, the JSON one without |
| `Start`, `Stop`, `Restart`, `Disable`, `Enable <node>` | the lifecycle call, only with the `lifecycle` feature and only as `health.ts` allows ([below](#a-nodes-health)), its why as the hint; a refusal is a toast and the inspector's spinner shows it on its way |
| `Remove <node>`, `Remove route <from> → <to>` | as the Delete key, for any route whatever its status |
| `Open ear.conf` or `Close ear.conf`, `Save ear.conf`, `Reload ear.conf` | the drawer's actions: Save only while the draft can be saved, Reload only while it is stale; none from an ear that sent no graph file |
| `Fit view`, `Tidy layout`, `Show minimap` or `Hide minimap` | as their tools ([above](#the-canvas)) |
| `Theme: system`, `Theme: light`, `Theme: dark` | the two choices that are not the current one |
| `Keyboard shortcuts` | the shortcut sheet ([below](#shortcuts)) |

With nothing typed, the selection's commands come first (each selected node's lifecycle calls and Remove, then each
selected route's Remove), then Go to, Add, `ear.conf`'s, the view's, and every other node's and route's, each under
its group's heading. An option shows its hint (a lifecycle command's why) as a quieter second line and, where it has
one, its key. Its one word of ear's own is the field's placeholder, "Type a command or a node's name" (for
quick add, "Add a node of type…"), given through the palette's `labels`; every other word is patchbay's default.

- **patchbay's palette.** Matching, ranking, the marked letters, the keys (the arrows, Page Up and Page Down, Home and
  End, Enter; Escape, Tab, a press outside or the focus leaving close it) and where it sits are `CommandPalette`'s:
  a query's letters in order in a label, word starts and runs scoring most, each group's matches best first and the
  groups by their best; the focus stays in its field and goes back where it was when it closes. *Why:* the owner chose
  patchbay's model for the palette (2026-11-26), Tab closing it as a combobox's list closes and each item having a
  hint line; its ranking and grouping replace ear's own, which listed at most 50 in one ranked list.
- **Nothing of its own.** Everything it does is a request ear judges, drawn when ear announces it
  ([above](#its-rules)), or a change of the browser's own view.

### Shortcuts

`web/src/keys.ts` (`keys.test.ts`) is the editor's table of bindings, in patchbay's key strings (`Mod+K`), and the
shortcut sheet's lines; patchbay's `useShortcuts` binds the table on the window and its `ShortcutSheet`, the dialog `?`
opens, lists the lines in three groups (Commands, View, Selection) with the keys drawn for the platform
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#useshortcuts)).

| Key | Does |
|---|---|
| `⌘K` (`Ctrl+K` off macOS) | opens or closes the palette |
| `⌘E` | shows or hides the `ear.conf` drawer |
| `⌘S` | saves the drawer's draft, only while it can be saved; the browser's Save page never opens |
| `F`, `L`, `M` | fit the view, tidy the layout, show or hide the minimap |
| `A` | the palette, with only its `Add` commands; the node is then laid out as any other |
| `?` | the shortcut sheet |
| Escape | closes the narrow inspector's sheet, else clears the selection; the palette and the shortcut sheet close on their own Escape |
| Delete, Backspace | React Flow's: removes the selection, asking ear ([above](#the-inspector)) |

Single keys and Escape act only when the focus is not in a text field (an input of a text kind, a textarea, a select,
contenteditable, or anything inside `.nokey`) and with no modifier but the Shift a character needs (`?`); a key must
match its modifiers exactly, so `⌘⇧K` is not `⌘K`. The three chords opt in to acting in a text field, since they type
nothing, so `⌘S` saves the drawer's draft while typing in it; it saves only that draft, even with the focus in the
node panel's box. While the palette is open only `⌘K` acts (closing it); while the shortcut sheet is open none do, as
its dialog holds the focus and Escape, its close button or a press on the scrim closes it. *Why that filter:* it is
patchbay's keyboard model (2026-11-26), and React Flow 12.12.0 skips input, select, textarea, contenteditable and
anything inside `.nokey` for Delete by the same rule (its `isInputDOMNode`, checked in the pinned package), so its
keys and the editor's coexist with the focus in the inspector's text. Slice 6's knobs are designed to carry `nodrag`,
`nowheel`, `nopan` and `nokey` ([control-plane.md](design/control-plane.md#83-gestures-from-pointer-and-keyboard)).

## Adding a node

`web/src/addText.ts` is the add-node form's logic (`addText.test.ts` replays `node-text.json`'s
refusals through it); `components/AddNodeText.tsx` draws it. With the `node-text` feature the form
takes a whole statement, `node <type> <name> { ... }`, in a code editor as the drawer's is, and Add sends
`node.addText` ([control-port.md](control-port.md#a-nodes-own-text)): ear appends it where
`node.add` would, reads it with the whole file (so `$ch` and `@use card_out MOTU;` work as by
hand), and places its diagnostics in the form's text. Its type list is patchbay's `Field` and `Select`; an ear
without the feature gets the JSON form instead (`AddNodePanel.tsx`): a name (`TextInput`), the type list and the
settings as JSON, each in a `Field`, which names its control and spaces the form. The rules:

- **A template per type.** Choosing a type writes `node <type> <name> {\n}`, the name the type's
  word and the first number no node holds (`file1`, `white2`); the block starts empty, since a
  type's settings are not on the wire. The template follows the chosen type, and the nodes as they
  change, only while the text is still the last template: what the person typed is never written
  over.
- **Add sends the newest version the editor holds.** `file-changed` then means ear.conf changed
  while the request was on its way; the form keeps the text and says to press Add again.
- **A node added gives a fresh template.** The node appears when ear announces it; the form's
  diagnostics are listed under it as the pane's, and a click puts the caret there.

## Adding on the canvas

- **Quick add.** A double-click on empty canvas (in place of React Flow's zoom), or a wire from an output dropped on
  empty canvas where no handle took it, opens the palette's `Add` commands at the pointer, kept inside the window.
  Both are patchbay's `useAddAt`
  ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#useaddat)), which finds the place on the
  flow; `connect.ts`'s `quickAddKind` keeps ear's rule that a wire from an input opens nothing. Choosing a type opens the inspector's add-node form with its template, as `Add <type>` does; ear still
  judges the text, and the node still appears only on `node.added`.
- **Placement hints.** The pointer's place is kept for the template's name (with no template, the JSON form, for
  the next node added). When `node.added` brings that name, `Places` puts it there (`layout.ts`'s `quickPlace`: the
  box centred on a double-click with its header at the pointer, its left edge at a wire's drop) instead of asking
  dagre, even over a kept place. A node arriving under another name (the person renamed it) drops the hint, as does
  selecting something (which drops the form, [above](#the-inspector)), and that node is laid out as before. A
  typed draft is never overwritten by the template ([above](#adding-a-node)).
- **No route is asked.** A wire dropped on empty canvas places the node at the drop, and the person draws the wire.
  *Why:* the owner chose that a wire dropped there asks for no route: it would be a second request the person did
  not see, and a type's first input is not always the one they mean.

Tidy is `L` and the palette's `Tidy layout` ([The canvas](#the-canvas)).

## The node panel's text

`web/src/nodeText.ts` is the box's logic (`nodeText.test.ts` replays `test/protocol/node-text.json`
through it); `components/NodeTextBox.tsx` draws it. The node panel is the inspector's Node subject
([above](#the-inspector)). With the `node-text` feature
([control-port.md](control-port.md#a-nodes-own-text)) the inspector's Node subject holds its
statement as `ear.conf` has it, units, `@use` lines, `$variables` and the comments it owns included
(`# the take` above `node file rec { path "take1.wav"; }`), in a code editor as the drawer's is, with Save
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
  statement read-only, on the code editor's read-only surface, its label saying "(read-only)", with where it is and that the pane
  edits it; while `ear.conf` has errors there are no `nodeTexts` and the box says to fix them in
  the pane. Neither offers Save. The text keeps its full contrast: it is never dimmed.

## Highlighting

The pane, the node panel's box and the add-node form are patchbay's `CodeEditor` with `language="knobly"`
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#code)): a native textarea holding the
text, the caret, the selection and every edit, a highlighted copy of what it shows drawn behind it, line numbers in a
gutter beside it, and a mark on each line a diagnostic names. How it draws, highlights, marks, indents and reads its
diagnostics is patchbay's, at its defaults: ear passes it the text, the edit handler, its diagnostics and, for the
node box and the add form, `rows`, and sets none of its words. *Why display only:* the owner decided that ear's C++
reader stays the one judge of an edit and the source of every diagnostic, so the browser's knobly never accepts,
refuses or diagnoses a text. What ear keeps:

- **The diagnostics are ear's.** Each box hands `CodeEditor` the diagnostics ear placed in its text (the pane's
  `shownDiagnostics`, the box's and the form's `errors`), as they come off the wire: knobly's and ear's codes, a
  severity it may not know (drawn with patchbay's neutral mark). They are marked in the gutter and under the text and
  read with the textarea, and are also listed under the box by patchbay's `DiagnosticList`
  ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#code)): a count ("No problems", "2
  problems") in a polite status line that is there from the start, then a row per diagnostic in CodeEditor's own words
  ("Line 3, column 5: error unknown-type: …") after its severity's mark, each a button that puts the caret at its
  place, as a press on a gutter mark does. *Why patchbay's:* the owner's principle (2026-11-26); it replaced ear's own
  plain list (`3:5 unknown-type …`), so what a gutter mark's tooltip says is written out in the same words where
  touch and the keyboard reach it (owner, 2026-11-27: no fact only in a tooltip). An `error` span in the highlighting is a token that does
  not lex (a stray character, an unterminated string), never a diagnostic.
- **What is sent is the text as typed.** A textarea holds `\n` only, so a CRLF file is shown, and edited, with
  `\n`; the drafts' stale and dirty logic and what Save and Add send are as above, and an unedited draft is never
  changed. `sent.test.ts` checks that the pane's Save, the box's Save and Add send the caller's text unchanged, CRLF
  and non-ASCII text included.
- **The drawer's box fills its body**, which the drawer's splitter sizes, so its own resize corner is off
  (`editor.css`'s `.text-pane-code`); it is named "ear.conf" for screen readers. The node box and the add form size
  by `rows` (3 to 12, the text's lines and one more) and keep the corner. A read-only node box is drawn on the code
  editor's read-only surface, never dimmed.

**The package.** `knobly/highlight` from knobly's first release, v1.0.0 (MIT): a scanner generated
from knobly's grammar, with no parser runtime. patchbay's `CodeEditor` imports it the first time a box is drawn, so
the build puts it in a chunk of its own (2.56 KB, 1.15 KB gzipped) and a box shows plain text until it has loaded.
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
so with one version on both sides it matches what ear's reader parses. patchbay takes knobly as an optional peer
(`^1.0.0` in 0.8.0); npm counts the tarball's 1.0.0 as meeting it (`npm ls knobly` shows one copy, deduped), and a
knobly bump through `scripts/knobly-pin.mjs` must stay inside that range.

## Tokens and themes

The editor draws with patchbay's tokens (`@mcurcio/patchbay`, pinned exactly in `web/package.json`):
every colour, radius, shadow, duration, face and type size in `editor.css` and the components is a
`--pb-*` custom property from its `tokens.css`, and ear defines none of its own. patchbay documents
the tokens, their light and dark values and the contrast rules on
[its tokens page](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/tokens.md#the-tokens); this
section says only how ear uses them.

**The order the styles load.** `main.tsx` loads React Flow's `style.css`, then
`@mcurcio/patchbay/tokens.css` and patchbay's optional `base.css` (the page's box sizing, body colours and type, and
the accent focus ring), then `editor.css`. patchbay's tokens point React Flow's own
variables (`--xy-*`) at them on `.react-flow`, after React Flow's defaults, so its canvas, dot grid
and minimap follow the theme without its `colorMode` prop (which would fight `data-theme`).
A patchbay component brings its own stylesheet (importing `@mcurcio/patchbay/primitives` loads
`primitives.css`), and `editor.css`'s rules for ear's own buttons pass by any element with a `pb-`
class (inside `:where()`, so they weigh no more), so patchbay's controls draw as patchbay draws them.

**What ear draws with which.**

- **Surfaces:** `--pb-surface` the canvas, `--pb-raised` boxes, panels and controls, `--pb-sunken` meter
  tracks, badges and read-only text; `--pb-line` and `--pb-line-strong` the borders and the dot grid.
- **Text:** `--pb-text`, `--pb-muted` (type labels, the health line, counter labels), `--pb-accent`
  (selection, focus, the reconfiguring pill) and `--pb-on-strong` (text on a filled badge: the "not realtime" badge,
  the warning mark).
- **States:** `--pb-ok` running, linked and ready; `--pb-warn` starting, stopping, an unknown
  state and the warning mark; `--pb-err` failed, refused and the diagnostics' errors; `--pb-quiet`
  stopped and a pending route; `--pb-off` disabled and a companion not ready. Asked-for silence is never `--pb-err`
  ([above](#a-nodes-health)).
- **Soft surfaces** under banners and the dropouts badge (`--pb-warn-soft`, `--pb-err-soft`), the
  meters' (`--pb-meter-low`, `--pb-meter-mid`, `--pb-meter-hot`, `--pb-meter-peak`), the shadows
  (`--pb-shadow`, `--pb-shadow-lifted`) and the code tokens (`--pb-code-*`, [above](#highlighting)).
- **Tones:** patchbay's `Pill`, `Badge` and `KeyValue` take a tone by name; the classes `tone-ok`, `tone-warn`,
  `tone-err` and `tone-muted` colour the drawer's status words from the state tokens.
- **Type:** `--pb-font-ui` and `--pb-text-md` (13 px) for the page, `--pb-text-sm` (12 px) for
  small text, `--pb-font-code` for code, names and numbers as sent, and `--pb-code-size` for the
  code boxes and the gutter ([Phones and touch](#phones-and-touch)). The other sizes stay
  literal, as patchbay has no token for them: 15 px (the wordmark, the
  palette's field, the shortcut sheet's title), 14 px (the inspector's title), 11 px (the node box's
  health line and port rows), 10 px (the type label, a companion's badge) and
  16 px (text fields under a coarse pointer).
- **Corners and motion:** 8 px on boxes, 6 px on controls and text boxes, pills fully round; 120 ms
  for hover and selection, 180 ms for what slides, `--pb-motion-pulse` (2 s, the configuring dot),
  and `--pb-motion-flow` (1 s, the wires' flow); patchbay's Spinner turns on `--pb-motion-spin` and stops itself. The
  first two are 0 under `prefers-reduced-motion: reduce`, which also stops the configuring pulse and hides the wires'
  flow (patchbay's Pill and Wire: an infinite animation at 0 s would churn rather than stop), and leaves the peak
  tick's fall, which is data, not decoration.

**ear's own variables.** Besides the tokens, `editor.css` reads a few variables that hold no value of
their own design, only a token picked per state: `--state` (a node's state class picks its tone, for its minimap
box).

**The theme.** It is `system` by default, following `prefers-color-scheme`. *Why:* the owner chose
that the editor follows the system until the person chooses. It is patchbay's: `App.tsx` holds the
choice with `useTheme` under ear's own key (`themeKey()` in `prefs.ts`, `ear-editor/1/theme`), which
writes `data-theme="light"` or `"dark"` on `<html>` (system removes it) and keeps the choice, and the
top bar draws patchbay's ThemeSwitch, with its words ([above](#the-top-bar)). The switch's cycle,
tooltip and label are patchbay's, documented with
[its hooks](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#usetheme). The palette has a
`Theme:` command for each other choice ([above](#the-palette)).

**No flash on the first paint.** `index.html` inlines patchbay's `themeScript(themeKey())` in its
`<head>`, before any stylesheet or module, so a kept light or dark choice is on `<html>` before the
page draws; `theme.test.ts` holds the inlined text equal to what patchbay generates for ear's key.
Without a kept choice, `index.html`'s `color-scheme` meta lets the first paint follow the system.
*Why:* the owner chose patchbay's script and its wording for the switch (2026-11-26).

**Tokens are a test.** `theme.test.ts` reads `editor.css`, every source in `web/src` but the tests,
and patchbay's `tokens.css`, and fails naming the file and the name when a `var()` names neither a
patchbay token nor one of ear's own variables (a missed rename would otherwise draw nothing,
silently), when `editor.css` declares any other variable, or when `editor.css` or a component holds
a colour of its own. Contrast is patchbay's to keep: its tokens test fails when a text, state, on-strong
or code token falls below 4.5:1 on a surface it is drawn on, in either theme, or when its two copies
of the dark set differ. Vitest blanks stylesheets by default, so `vite.config.ts` lets it read
`editor.css` and `tokens.css` as text.

Forced colours keep the system's colours, as [Highlighting](#highlighting) says.

## What the browser keeps

`web/src/prefs.ts` (`prefs.test.ts`) holds ear's keys and the check of every kept value, and keeps the node places;
patchbay keeps the rest under those keys: the inspector's width and the minimap (`App.tsx`) and the drawer
(`useDrawer.ts`) are its `usePersistentState`, the theme its `useTheme`
([its hooks](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#usepersistentstate)). All of it is
in `localStorage` as JSON, every read and write in `try` (so a private window, blocked storage or a full one keeps an
editor that remembers for the page's life), and every value is checked on read, a bad one ignored in favour of the
default; a value is written only when the person changes it. `main.tsx` makes one `Prefs` for the page.

- **Per ear**, keyed by the page's `location.host` (`ear-editor/1/<host>/…`): node places, the inspector's width
  (clamped to AppShell's bounds), the drawer's height (as set: AppShell draws it within the window as it is) and
  whether it is open, and the minimap.
- **Per browser** (`ear-editor/1/theme`): the theme, kept by patchbay's `useTheme` as JSON (as `prefs.ts` kept it
  before, so a choice made then still holds) and applied by `index.html`'s inline script before the first paint, so
  a kept dark choice never flashes light ([Tokens and themes](#tokens-and-themes)).
- **Places** are written when a drag ends (or a keyboard move), when new nodes are placed and on Tidy (which forgets
  the absent ones too), never on a state change. A name not in the graph keeps its place for 30 days from when it
  was last seen, timed on the wall clock (it outlives the page; every other editor clock is monotonic), so a node
  removed and added back finds it; at most 500 are kept, the most recently seen. They are never sent to ear
  ([above](#its-rules)). *Why:* the owner wanted node places kept across reloads in the browser, per ear, so a
  reload no longer scatters a hand-placed graph and places stay the browser's own.

Slice 5's type-schema cache is designed to use this module too ([control-plane.md](design/control-plane.md#81-stores)).

## The node box, wires and meters

### The node box

`components/EarNode.tsx` draws it with patchbay's `NodeBox`, one `PortRow` per port
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#nodebox)); ear gives it the words, the
tone and the handle ids, and the handles' rule is `web/src/connect.ts`'s `canConnect`. *Why patchbay's:* the owner's
principle is to take patchbay's model where it has the concept (2026-11-26), and its geometry is exported so layout
code sizes boxes by it (owner, 2026-11-26: NodeBox's geometry as exported constants, mirrored by tokens a brand
may not change).

- **The state is a stripe and a pill.** NodeBox's 4 px stripe down the box's left edge and a pill in its status slot
  (patchbay's `Pill`: a dot and the word, with a failure's code after it in monospace, `StatePill.tsx`) take the
  state's tone (`health.ts`'s `stateTone`): ok running, warn starting, stopping and an unknown state, err failed,
  quiet stopped, off disabled, and the accent reconfiguring. The word is always there, so the state is never colour
  alone. A failed box (`failed`) has an `--pb-err` border and its header on `--pb-err-soft`, its line in `--pb-err`. A
  disabled one (`disabled`) has a dashed border, a hatched stripe and muted words. `configuring` pulses the dot only,
  never the text, and not at all under reduced motion (the Pill's `pulse`).
- **The name and the type.** The name is the box's title, cut with an ellipsis and whole in its tooltip; the type is
  its subtitle, the type's word as sent, in `--pb-muted`. Nothing in `web/` names a node type, so there are no
  per-type icons or colours.
- **The line** under the header is `health.ts`'s ([below](#a-nodes-health)), and the box has none while there is
  nothing to say.
- **The ports are stacked:** the inputs' rows, their handles on the left edge, then the outputs', theirs on the right,
  each row the box's width with the port's name toward its handle and its meter ([below](#meters)) in PortRow's
  trailing slot. Side by side, a name and a meter (at least 48 px) do not fit in half the box.
- **Its size is patchbay's.** The box is NodeBox's default width, `NODE_WIDTH` (240 px); `layout.ts`'s `nodeSize` is
  patchbay's `nodeBoxSize` for its ports, its line (as `health.ts` would show it) and its footer (while it has a
  companion strip), which dagre places by, and quick add places by `NODE_HEADER`, `NODE_PAD` and `NODE_ROW`.
  `layout.test.ts` checks both against those exports. A line or strip that comes or goes later changes the box's
  height, not its place.
- **Handles** are PortRow's: a dot with a larger hit area, both patchbay's sizes (`HANDLE_SIZE`, `HANDLE_HIT`; the hit
  area larger again under a coarse pointer). Each handle's id is `connect.ts`'s `handleId` (`in:<port>`,
  `out:<port>`), passed through unchanged.
- **Hints while a wire is drawn** are patchbay's `useConnectHints`
  ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#useconnecthints)) over `connect.ts`'s
  `canConnect`, handed to every PortRow by `<ConnectHints>` around the canvas: the handle the drag started at is
  `origin` (ringed in `--pb-accent`), every handle on the other side a drop on would make a route `ok` (grown, in
  `--pb-ok`) and every other there `no` (in `--pb-off`, its row muted); the handles on the drag's own side show
  nothing. From an output every input accepts, from an input every output (the editor accepts a drag from either
  end). React Flow's own check on the drop is the same hook's, so a hint never promises what the drop refuses. The
  hints change at a drag's start and end only, so a pointer move redraws no box. *Why patchbay's:* the owner's
  principle (2026-11-26); before it, ear also marked the drag's own side `no`.
- **Selection** is NodeBox's 2 px `--pb-accent` outline outside the border.
- **A change of ports redraws the box in place.** `node.updated` with its new `inputs` and
  `outputs` ([control-port.md](control-port.md#changes-of-ports)) changes the node the editor holds,
  not a new one: the box keeps its place, NodeBox has React Flow measure its handles again, a surviving handle keeps
  its wires (the routes were never removed, so they never go `pending`), a new handle comes unwired, and the counters
  and marks go on.

### Wires

Each route is patchbay's `Wire` ([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#wire)):
`components/LevelEdge.tsx` gives it what `web/src/wires.ts`'s `wireData` (`wires.test.ts`) makes of the route and its
from port's level. *Why patchbay's:* the owner's principle is to take patchbay's model where it has the concept
(2026-11-26).

- **Colour by status.** Its tone is one of Wire's: `ok` for linked (`--pb-ok`), `quiet` for pending (`--pb-quiet`) and
  `refused` (`--pb-err`); a status this editor does not know is drawn as pending. A pending route is one ear holds and
  has not linked yet, a live state, so it is a dashed `Wire` in the quiet tone; patchbay's word "pending" is for a
  request on its way, which is a ghost wire ([Requests in flight](#requests-in-flight)). *Why:* the owner kept
  patchbay's grey for pending routes rather than an ear colour of its own (2026-11-26), and kept "pending" for a
  request sent and not yet answered (2026-11-27), so patchbay 0.10.0 dropped Wire's own `pending` tone. The Route
  subject's words for those three statuses take the same colours. Selected, the wire is `--pb-accent` whatever its
  status.
- **Size by level.** Its emphasis is the from port's rms on the meters' scale (`levels.ts`'s `meterFraction`, the
  -60 dBFS floor): 1.5 px to 3.5 px wide and from 45 % to full opacity. Every status but `linked` is dashed.
- **The flow.** A linked wire whose from port's rms is above the floor shows dots moving from the output to the
  input at a fixed speed (`--pb-motion-flow`). It means "signal", not how loud. There is none under reduced motion,
  and none on pending, refused or an unknown status.
- **The level without a rebuild.** A level message (about 30 a second) redraws each wire whose from port it moves,
  and only that: Wire keeps its path, its flow and its hit path, and takes the level as a CSS variable on its group,
  so a new level restyles the stroke and never rebuilds the path. Measured in headless Chromium on the `snapshot`
  fixture's graph at 30 level messages a second, against ear's own wire before it: no path's `d` changed either way,
  the style changes on the edges over 3 s fell from 635 to 270, the layouts over 5 s from 338 to 198, and script
  time from 93 ms to 81 ms.
- **The status in words.** A refused wire carries `refused` at its midpoint, and a status this editor does not know
  its own word, on Wire's label.
- **The tooltip**, on the wire's hit path, names the two ends (`noise:noise.mono → rec:record.right`) and, for a
  refused route, ear's detail.

### Meters

Each port's meter is patchbay's `Meter`
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/graph.md#meter)), fed by the level store:
`components/Meter.tsx` gives it the port's level and a reset, and `web/src/meters.ts` (`meters.test.ts`) its
tooltip's words. *Why patchbay's:* the owner chose that patchbay's Meter owns the dB scale (2026-11-26); ear
takes its defaults, which `meters.test.ts` pins to what this section says.

- **The scale:** an rms bar and a peak tick, dBFS down to -60, the same floor as the wires' (`levels.ts`).
- **Three zones.** The bar uncovers three fixed zones on that scale: `--pb-meter-low` below -18 dBFS,
  `--pb-meter-mid` from -18 to -6, `--pb-meter-hot` above.
- **Peak hold.** The tick holds its highest peak for 1.5 s, then falls at 20 dB/s, never below the
  live peak, and is hidden at the floor.
- **Clip lamp.** A peak at or over 0 dBFS (linear 1) lights a square at the meter's right end for 2 s, and the
  tooltip says "clipped".
- **Its words.** The tooltip, ear's through the meter's `labels.title`, says the peak and the rms in dBFS ("peak -6.0
  dBFS, rms -12.0 dBFS", each number written by patchbay's `formatMeterDb`, one decimal, as Meter's own tooltip
  writes it), or "silent" with nothing above the floor, refreshed at most four times a second while hovered. The meter is an image named for its
  port (`<node>:<port> level` in the inspector's route panel, `<port> level` in a node box); the numbers are never announced.
- **Outside React.** The meter subscribes to the level store and paints the bar, the tick, the lamp and the tooltip
  through refs, animating on animation frames only while the tick is above the live peak or the lamp is lit. A
  closed connection resets it: the bar empty, the hold and the lamp forgotten.
- **Times** are taken on the browser's monotonic clock, as [Recent marks](#recent-marks) does.

## A node's health

`web/src/health.ts` is the one place that turns a node's health members
([control-port.md](control-port.md#node-health)) into the box's pill word and tone (the stripe's too), the minimap's state class, line and
enabled buttons; its tests cover every phase. The rules it follows:

- **Silence the operator asked for is never drawn as a fault.** `stopped` ("stopped until Start or
  a restart of ear") and `disabled` ("disabled in ear.conf") each have their own quiet style, apart
  from `failed`'s. So has `reconfiguring`, a change in place, which is not a warning: its line
  names the keys it applies and those next ("reconfiguring port; latencyMs next"), and Start is
  off while it lasts. Stop, Restart and Disable stay on, each saying what it does to the change:
  "ends the change and stops", "ends the change and restarts", "ends the change and disables".
- **The box always says why a node is silent:** what it waits for (from `waitingFor`: "waiting for
  fpp.rtp", "waiting for the previous desk to stop", "waiting for the engine"), why it is
  stopping or configuring (from `cause`: "restarting: settings changed (card)", "restarting: ports
  changed (playback)", "configuring: trying again", "restarting: could not apply port in place"),
  or a failure's detail.
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

- **A badge per companion,** in list order, in the box's footer under the ports (NodeBox's `footer`), each
  patchbay's `Badge`: its kind as a short word (`card-playback` "card out", `card-capture` "card in", `rtp-receiver`
  "rtp", `sink`, `source`) and a dot, filled (●) in the ok tone when `ready`, hollow (○) in the off tone when not. A
  kind this editor does not know is drawn by its own text, in the same style. The footer is one row and cuts badges
  that do not fit. No list, or an empty one, draws no strip and no footer (`white`, `sendspin`, a node from an ear
  without the feature).
- **Totals in words.** The badge's tooltip and the node panel's "Companions" list give each one's
  name, word, bank, readiness and the total its counter carries since its configuration made it
  ("rec.writer (sink on writer): ready; 4096 frames dropped since it was made"; a source's
  shortfall "... frames played short since it was made"). An engine kind has no counter. A counter
  member the editor knows is shown whatever the kind; one it does not know is ignored. A companion
  a change in place replaces make-before-break gets its own badge, before its replacement's, and
  its words end ", retiring" ("fpp.rtp (rtp on from_rtp): ready, retiring").
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
  apart from the companion strip in the box's footer, whose badges mark their own counters
  (a sink's drops), so a mark in the header is the node's and one in the strip a helper's. Its
  tooltip gives the last minute's and the totals in words ("In the last minute: overran 1 time.
  Since it was added: missed 33 cycles, overran 3 times."). The graph's cuts never mark a node:
  they are the host's, on every output at once.
- **The dropouts badge** sits in the top bar after the engine chip while `cutCycles` rose in
  the last minute, with the last minute's count ("5 dropouts"), which falls as each rise leaves its
  minute; its tooltip says the host held up the audio thread and gives the totals ("44 dropouts
  (234 ms) in 2 stalls since ear started").
- **A mark lasts across a change of ports.** A node's counters never step back while it
  lives, and a change of its ports keeps the node (`node.updated` with its new ports), so its counts
  and its mark go on. The values held for a node are kept when it is removed (until the connection
  closes), so a new node under the name, which comes with zeros, is a fall and its counts start
  fresh. Companions' entries are still forgotten when their list drops them, as their counters
  count per configuration.
- **In text, not colour alone.** Each mark's glyph is hidden from screen readers beside its words
  ("in the last minute: overran 1 time", "3 more in the last minute"); the badge's count is its
  text. Nothing shows without the feature: an ear without it sends no counters.

## Recent marks

`web/src/marks.ts` is the one rule both marks follow, companions' badges and the counters'
(`marks.test.ts`): each reads its counters from the graph, and `GraphMoves` keeps their last values
and rises. The mark is patchbay's `WarnMark`, an "!" in a circle
([its docs](https://github.com/mcurcio/patchbay/blob/v0.10.0/docs/components.md#state)): named by its words where it
stands alone (a box's counter mark, a companion badge), and hidden from screen readers where its words are already the
text beside it (the dropouts badge, the panel's companion list and counter tables).

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
form (`addText.ts`), what the knobly boxes send (`sent.test.ts`), the reconnecting client with a fake socket,
the connection rules, levels and layout, `health.ts`, `companions.ts`, `counters.ts` and `marks.ts`, `wires.ts` (replaying the `snapshot` and `unknown-values` routes), `meters.ts` (a meter's words, and patchbay's scale pinned as stated here), the handles' rule (`connect.ts`'s `canConnect`), the box's sizes against patchbay's exported geometry (`layout.test.ts`), and the tokens and the theme's first paint (`theme.test.ts`: every `var()` names a token, and `index.html` inlines patchbay's theme script), the shell's words (`shell.ts`: the pill's countdown, the engine and protocol chips, the facts), the first sizes against AppShell's bounds and `editor.css`'s queries against patchbay's (`sizes.ts`), the inspector's subject and Delete's requests (`selection.ts`), the drawer's words and when it opens (`drawer.ts`), the JSON settings form's parse (`settings.ts`), the wire's status words (`wires.ts`), tidy (`layout.ts`), the client's retry wait and the editor's connection view (`rpc.test.ts`, `editor.test.ts`), the requests in flight and the ghosts (`requests.ts`), the toast store on patchbay's queue and a refusal's words (`notices.ts`, replaying `errors.json`), the session's `ask` on a fake socket (`editor.test.ts`), the palette's commands as patchbay's `CommandPalette` takes and ranks them, replaying the `lifecycle` and `unknown-values` fixtures (`palette.ts`), the key bindings as patchbay's `findShortcut` runs them (`keys.ts`), what the browser keeps, with throwing storage, bad values and the 30-day expiry (`prefs.ts`), placement hints and kept places (`layout.ts`), and which quick add a double-click or a dropped wire asks for (`connect.ts`'s `quickAddKind`). patchbay's modules import their own stylesheets, which Node cannot load, so `vite.config.ts` has Vitest inline the package (and blank its styles) for the tests that call its pure helpers. There is no browser test runner:
rendering is checked by eye. *Why:* a Playwright leg costs about 150 MB of browsers and a minute
or two a CI run, while the logic sits in the reducer, client and rules, which Vitest covers
against the same fixtures the server is tested with.

**The bundle.** The palette, the shortcuts, quick add and what the browser keeps grew the editor's script by 16.4 KB
(515.17 KB to 531.61 KB; 5.4 KB gzipped, 164.28 KB to 169.68 KB) and its styles by 2.0 KB (35.59 KB to 37.57 KB),
measured by the build before and after; the shell's design estimated under 15 KB (5 KB gzipped), so it is slightly
over. The whole shell, from tokens to this, grew the script from 486.58 KB to 531.61 KB (155.24 KB to 169.68 KB
gzipped) and the styles from 21.25 KB to 37.57 KB; the view's own fit, the segmented lifecycle bar and the theme
icon, the palette's and the narrow top bar's fixes, and then phones and touch, brought them to 533.15 KB (170.25 KB
gzipped) and 39.17 KB. No package was added for any of it. patchbay was the first: with the node box, wires,
meters and canvas tools on it, the build is 565.23 KB (181.55 KB gzipped) and 73.70 KB of styles, patchbay's own
stylesheets among them, and Vite warns that the script is over 500 KB. patchbay 0.8.0, whose stylesheets carry its
code editor and shell before the editor uses them, brought them to 565.36 KB (181.58 KB gzipped) and 78.17 KB. The
knobly boxes on its `CodeEditor` made them 569.24 KB (182.55 KB gzipped), with knobly's highlighter in its own
2.56 KB chunk, and 75.63 KB. The shell on its `AppShell`, with patchbay's `base.css`, made them 572.17 KB (183.34 KB
gzipped) and 77.12 KB. patchbay 0.9.0, with the inspector's one header row, made them 572.43 KB (183.43 KB gzipped)
and 77.94 KB. patchbay 0.10.0, whose stylesheets carry its new connecting, adding and diagnostics parts before the
editor uses them, made them 575.56 KB (184.36 KB gzipped) and 87.01 KB.

React Flow (`@xyflow/react`) was chosen over Rete.js (its own editor framework) and LiteGraph
(drawn on a canvas, which makes per-port meters custom work).
