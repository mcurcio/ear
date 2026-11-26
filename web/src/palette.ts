// The command palette's logic, pure (docs/editor.md#the-palette): its commands from the graph, the selection, the
// features and the shell's state, so a command ear cannot take is never offered; a subsequence ranking of their
// labels; and where a popover anchored at the pointer sits. A command only names a request or a change of the
// browser's own view: ear judges every request and draws every result (docs/editor.md#its-rules).
import { themeChoices, type ThemeChoice } from "@mcurcio/patchbay/hooks";
import { edgeId } from "./connect";
import { describeHealth, type HealthView } from "./health";
import type { ShortcutAction } from "./keys";
import { lifecycleFeature, type Graph, type GraphNode, type RouteEnds } from "./protocol";
import { lifecycleLabels, type LifecycleMethod } from "./requests";
import type { Selection } from "./selection";
import { wireEnds } from "./wires";

export type CommandAction =
  | { kind: "go"; node: string }
  | { kind: "add"; type: string }
  | { kind: "lifecycle"; method: LifecycleMethod; node: string }
  | { kind: "remove-node"; node: string }
  | { kind: "remove-route"; ends: RouteEnds }
  | { kind: "drawer"; what: "open" | "close" | "save" | "reload" }
  | { kind: "fit" }
  | { kind: "tidy" }
  | { kind: "minimap" }
  | { kind: "theme"; choice: ThemeChoice }
  | { kind: "shortcuts" };

export type CommandGroup = "Selected" | "Go to" | "Add" | "ear.conf" | "View" | "Node" | "Route";

export interface Command {
  /** Stable: "go:desk", "add:file", "node.start:desk", "remove:desk", "remove-route:<edgeId>", "view:fit". */
  id: string;
  /** What is searched and shown. */
  label: string;
  group: CommandGroup;
  /** health.ts's why for a lifecycle command. */
  hint?: string;
  /** keys.ts's shortcut, so the option shows its key. */
  shortcut?: ShortcutAction;
  action: CommandAction;
}

export interface PaletteContext {
  graph: Graph;
  selection: Selection;
  /** null: ear sent no graph file. */
  drawer: { open: boolean; canSave: boolean; stale: boolean } | null;
  minimap: boolean;
  theme: ThemeChoice;
}

/** "all" for Cmd/Ctrl+K; "add" for quick add and A, only the Add commands. */
export type PaletteScope = "all" | "add";

/** The lifecycle methods in the bar's order, each with its action in health.ts. */
const lifecycleOrder: { method: LifecycleMethod; action: keyof HealthView["actions"] }[] = [
  { method: "node.start", action: "start" },
  { method: "node.stop", action: "stop" },
  { method: "node.restart", action: "restart" },
  { method: "node.disable", action: "disable" },
  { method: "node.enable", action: "enable" },
];

function lifecycleCommands(node: GraphNode, group: CommandGroup): Command[] {
  const health = describeHealth(node);
  return lifecycleOrder
    .filter(({ action }) => health.actions[action].enabled)
    .map(({ method, action }) => ({
      id: `${method}:${node.name}`,
      label: `${lifecycleLabels[method]} ${node.name}`,
      group,
      hint: health.actions[action].why,
      action: { kind: "lifecycle", method, node: node.name },
    }));
}

function removeNode(node: GraphNode, group: CommandGroup): Command {
  return {
    id: `remove:${node.name}`,
    label: `Remove ${node.name}`,
    group,
    action: { kind: "remove-node", node: node.name },
  };
}

function removeRoute(route: RouteEnds, group: CommandGroup): Command {
  const ends = { from: route.from, to: route.to };
  return {
    id: `remove-route:${edgeId(ends)}`,
    label: `Remove route ${wireEnds(ends)}`,
    group,
    action: { kind: "remove-route", ends },
  };
}

/**
 * The palette's commands, in the order shown with nothing typed: the selection's first (a name or wire id the graph
 * does not hold is ignored), then Go to, Add, ear.conf's actions, the view, every other node's lifecycle and removes.
 */
export function commandsOf(context: PaletteContext, scope: PaletteScope = "all"): Command[] {
  const { graph, selection, drawer } = context;
  const adds: Command[] = graph.types.map((type) => ({
    id: `add:${type}`,
    label: `Add ${type}`,
    group: "Add",
    action: { kind: "add", type },
  }));
  if (scope === "add") return adds;

  const lifecycle = (graph.features ?? []).includes(lifecycleFeature);
  const names = new Set(selection.nodes);
  const ids = new Set(selection.routes);
  const selectedNodes = graph.nodes.filter((node) => names.has(node.name));
  const otherNodes = graph.nodes.filter((node) => !names.has(node.name));
  const selectedRoutes = graph.routes.filter((route) => ids.has(edgeId(route)));
  const otherRoutes = graph.routes.filter((route) => !ids.has(edgeId(route)));

  const selected: Command[] = [
    ...selectedNodes.flatMap((node) => [
      ...(lifecycle ? lifecycleCommands(node, "Selected") : []),
      removeNode(node, "Selected"),
    ]),
    ...selectedRoutes.map((route) => removeRoute(route, "Selected")),
  ];

  const goTo: Command[] = graph.nodes.map((node) => ({
    id: `go:${node.name}`,
    label: `Go to ${node.name}`,
    group: "Go to",
    action: { kind: "go", node: node.name },
  }));

  const file: Command[] = [];
  if (drawer !== null) {
    if (drawer.open) file.push(fileCommand("close", "Close ear.conf", "drawer"));
    else file.push(fileCommand("open", "Open ear.conf", "drawer"));
    // canSave already excludes a stale draft and a Save on its way.
    if (drawer.canSave) file.push(fileCommand("save", "Save ear.conf", "save"));
    if (drawer.stale) file.push(fileCommand("reload", "Reload ear.conf"));
  }

  const view: Command[] = [
    { id: "view:fit", label: "Fit view", group: "View", shortcut: "fit", action: { kind: "fit" } },
    { id: "view:tidy", label: "Tidy layout", group: "View", shortcut: "tidy", action: { kind: "tidy" } },
    {
      id: "view:minimap",
      label: context.minimap ? "Hide minimap" : "Show minimap",
      group: "View",
      shortcut: "minimap",
      action: { kind: "minimap" },
    },
    ...themeChoices
      .filter((choice) => choice !== context.theme)
      .map(
        (choice): Command => ({
          id: `theme:${choice}`,
          label: `Theme: ${choice}`,
          group: "View",
          action: { kind: "theme", choice },
        }),
      ),
    {
      id: "shortcuts",
      label: "Keyboard shortcuts",
      group: "View",
      shortcut: "shortcuts",
      action: { kind: "shortcuts" },
    },
  ];

  const others: Command[] = [
    ...(lifecycle ? otherNodes.flatMap((node) => lifecycleCommands(node, "Node")) : []),
    ...otherNodes.map((node) => removeNode(node, "Node")),
    ...otherRoutes.map((route) => removeRoute(route, "Route")),
  ];

  return [...selected, ...goTo, ...adds, ...file, ...view, ...others];
}

function fileCommand(what: "open" | "close" | "save" | "reload", label: string, shortcut?: ShortcutAction): Command {
  const command: Command = { id: `drawer:${what}`, label, group: "ear.conf", action: { kind: "drawer", what } };
  if (shortcut !== undefined) command.shortcut = shortcut;
  return command;
}

// ----- Ranking -----

export interface Match {
  score: number;
  /** The label's indices matched, ascending. */
  marks: number[];
}

const wordStartScore = 8;
const runScore = 5;
const otherScore = 1;
const maxGapCost = 3;
const separators = new Set([" ", ":", ".", "-", "_", "→", "(", ">", "/"]);

function isWordStart(label: string, index: number): boolean {
  if (index === 0) return true;
  const before = label[index - 1]!;
  if (separators.has(before)) return true;
  const here = label[index]!;
  // A lower-to-upper case change ("graphFile"): before is a lowercase letter, here an uppercase one.
  return before !== before.toUpperCase() && here !== here.toLowerCase();
}

/**
 * How well `query` matches `label`: every query character (lowercased, whitespace removed) in order in the label
 * (lowercased), else null. Of every alignment the best scores: a word start 8, right after the previous match 5, any
 * other 1, less 1 per character skipped between two matches (at most 3 a gap; the leading gap is free).
 */
export function matchLabel(query: string, label: string): Match | null {
  const q = query.toLowerCase().replace(/\s+/g, "");
  if (q === "") return { score: 0, marks: [] };
  const l = label.toLowerCase();
  const n = l.length;
  const m = q.length;
  if (m > n) return null;
  const starts = Array.from({ length: n }, (_, j) => isWordStart(label, j));
  // best[i][j]: the best score with query[0..i] matched and query[i] at label[j]; from[i][j] the previous match.
  const best: number[][] = [];
  const from: number[][] = [];
  for (let i = 0; i < m; i++) {
    best.push(new Array<number>(n).fill(-Infinity));
    from.push(new Array<number>(n).fill(-1));
    for (let j = i; j < n; j++) {
      if (l[j] !== q[i]) continue;
      const own = starts[j] ? wordStartScore : otherScore;
      if (i === 0) {
        best[i]![j] = own;
        continue;
      }
      for (let k = i - 1; k < j; k++) {
        const previous = best[i - 1]![k]!;
        if (previous === -Infinity) continue;
        const adjacent = k === j - 1;
        const gain = adjacent ? Math.max(own, runScore) : own - Math.min(maxGapCost, j - k - 1);
        if (previous + gain > best[i]![j]!) {
          best[i]![j] = previous + gain;
          from[i]![j] = k;
        }
      }
    }
  }
  const last = best[m - 1]!;
  let end = -1;
  for (let j = 0; j < n; j++) if (last[j]! !== -Infinity && (end < 0 || last[j]! > last[end]!)) end = j;
  if (end < 0) return null;
  const marks: number[] = [];
  for (let i = m - 1, j = end; i >= 0; j = from[i]![j]!, i--) marks.push(j);
  return { score: last[end]!, marks: marks.reverse() };
}

export interface Ranked {
  command: Command;
  match: Match;
}

/** The most the palette lists at once. */
export const shownCommands = 50;

/** The commands matching `query`, best first (then the shorter label, then the list's order), at most `limit`. */
export function rank(commands: readonly Command[], query: string, limit: number = shownCommands): Ranked[] {
  const ranked: (Ranked & { index: number })[] = [];
  commands.forEach((command, index) => {
    const match = matchLabel(query, command.label);
    if (match !== null) ranked.push({ command, match, index });
  });
  // An empty query scores every command 0 and keeps the list's order.
  if (query.replace(/\s+/g, "") !== "")
    ranked.sort(
      (a, b) =>
        b.match.score - a.match.score || a.command.label.length - b.command.label.length || a.index - b.index,
    );
  return ranked.slice(0, limit).map(({ command, match }) => ({ command, match }));
}

// ----- The popover -----

/**
 * Where a box anchored at `point` sits: its top-left at the point, moved left or up to stay at least `margin` inside
 * the viewport, and never above or left of `margin`.
 */
export function anchoredAt(
  point: { x: number; y: number },
  box: { width: number; height: number },
  viewport: { width: number; height: number },
  margin = 8,
): { left: number; top: number } {
  return {
    left: Math.max(margin, Math.min(point.x, viewport.width - margin - box.width)),
    top: Math.max(margin, Math.min(point.y, viewport.height - margin - box.height)),
  };
}
