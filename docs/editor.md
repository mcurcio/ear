# The graph editor

ear's control port (7777) serves a graph editor at `/`: a browser page that draws the running
graph and edits it through the same JSON-RPC calls a script uses
([control-port.md](control-port.md)). Its source is `web/` (React, React Flow, Vite, TypeScript,
Vitest); it shares no code with the C++, only the wire format and the fixtures in `test/protocol/`.

## What it shows and does

- **Nodes** are boxes with one row per port, inputs on the left and outputs on the right, each
  with its handle and a live level meter. Under the node's name and type is its state word and a
  line that says why it is silent or what it waits for ([below](#a-nodes-health)). Along its
  bottom edge, a badge per companion ([below](#companions)).
- **Routes** are wires coloured by status (`linked`, `pending`, `refused`) and drawn by their
  from port's level: louder is thicker and brighter.
- **Editing:** a drag from an output to an input adds a route (any other drag is refused before
  it reaches ear); the delete key removes the selected wire or node; a side panel adds nodes (a
  type and JSON settings), edits the selected node's settings, and offers Start, Stop, Restart,
  Disable and Enable when ear has the `lifecycle` feature. A "not realtime" badge shows when the
  audio thread is not realtime.
- **The text pane** shows `ear.conf` with its diagnostics (a click puts the caret there) and
  saves it with Save (`graphFile.set`, based on the version the draft started from). Banners say
  when the graph refused the file, a write failed, editor edits were dropped by a hand edit, or a
  setting applies only at restart.

## Its rules

- **ear is the one source of truth.** The editor draws only what ear announced: a node or wire
  appears on its notification, never on the request that asked for it, and an event naming a node
  or route it does not know changes nothing. It never parses knobly; ear judges every text.
- **Every enum is open.** A value or member this editor does not know is drawn by its word,
  neutrally, or ignored, never a failure ([control-port.md](control-port.md#protocol-and-features)).
  A newer protocol than the editor knows shows a banner.
- **A draft is never overwritten.** A new `ear.conf` arriving while the pane has unsaved changes
  makes the pane stale ("ear.conf changed; reload") until the person reloads; ear refuses a Save
  over a file that changed meanwhile (`file-changed`).
- **Node positions live in the browser.** Unplaced nodes are laid out with dagre (sources left,
  sinks right, following the routes); a place the user dragged a node to is kept in the browser
  only, never sent to ear.
- **The counters are kept, not yet drawn.** With the `counters` feature the graph store keeps the
  graph's counters and each node's whole, from the snapshot and `node.added`, and replaces those a
  `counters.changed` names ([control-port.md](control-port.md#counters)); an unknown counter is
  kept with them. Nothing on the canvas or panel shows them yet.
- **Companion lists are kept as sent.** With the `companions` feature the graph store keeps
  each node's `companions` as sent, from the snapshot and `node.added`, and a
  `node.companionsChanged` replaces the named node's whole list
  ([control-port.md](control-port.md#companions)); an unknown kind or member is kept as sent.
  How they are drawn: [below](#companions).
- **Levels stay outside React state.** Each `levels` notification replaces a store with
  listeners per port, so at 30 messages a second only the meters and wires whose port changed
  re-render. A closed connection makes every meter read silent until new levels arrive.
- **A lost socket reconnects** after a growing wait (1 s doubling to 30 s), failing the calls in
  flight, and the new socket subscribes again (graph, graph file, levels), so the editor starts
  over from a fresh snapshot.

## A node's health

`web/src/health.ts` is the one place that turns a node's health members
([control-port.md](control-port.md#node-health)) into the box's word, style, line and enabled
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
through the reducer into the badges drawn. The rules:

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
- **A mark only while it moves.** A badge carries the warning mark (an "!" in a circle) only while
  its `dropped` or `shortfall` rose in the last minute, on the browser's clock; the box redraws
  when the minute ends. A rise is a value above the one last held for that node and companion: the
  first value seen (a snapshot's, a new configuration's), a value that fell (a new configuration
  seen without its `[]`, as after a reconnect) and an entry seen again after its list dropped it
  are not. *Why:* the canvas stays quiet when all is well and points at trouble while it happens;
  the totals are always in the tooltip and panel. `CompanionMoves` keeps what was held and when it
  rose, outside the reducer (which holds only what ear sent), and is the graph store's first
  listener, so a box drawn for a new graph reads that graph's moves.
- **Failed and disabled draw no strip** whatever list is held (their configuration was torn down,
  and ear's `[]` follows their `node.state`); a stopped node keeps its strip, as it keeps its
  companions.
- **In text, not colour alone.** The dot and the mark are hidden from screen readers beside
  visually hidden "ready", "not ready" and the mark's words ("dropped frames in the last minute",
  "played short in the last minute"); the strip is a list labelled "companions".
- **Not part of editing.** A badge is no handle and no node: clicking it selects its node and
  dragging it moves the box, as any part of the box does; nothing in it sends a request.

## Developing it

`scripts/dev.sh make web` builds it into `web/dist` (`--web-root web/dist` serves that build);
`make web-test` runs the typecheck, Vitest and the build. `npm run dev` (Vite, hot reload)
forwards `/rpc` to an ear on `localhost:7777`; `scripts/dev.sh`'s header shows how to publish its
port. Node.js comes from `web/.nvmrc` and the packages from `web/package-lock.json`.

The tests cover the reducer against every `test/protocol` fixture, the graph file's state and the
pane's logic (drafts, stale files, Save, refusals), the reconnecting client with a fake socket,
the connection rules, levels and layout, `health.ts` and `companions.ts`. There is no browser test runner:
rendering is checked by eye. *Why:* a Playwright leg costs about 150 MB of browsers and a minute
or two a CI run, while the logic sits in the reducer, client and rules, which Vitest covers
against the same fixtures the server is tested with.

React Flow (`@xyflow/react`) was chosen over Rete.js (its own editor framework) and LiteGraph
(drawn on a canvas, which makes per-port meters custom work).
