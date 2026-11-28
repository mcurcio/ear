// The command palette's logic, pure (docs/editor.md#the-palette): its commands from the graph, the selection, the
// features and the shell's state, so a command ear cannot take is never offered, and those commands as patchbay's
// CommandPalette takes them (it ranks, marks and places them). A command only names a request or a change of the
// browser's own view: ear judges every request and draws every result (docs/editor.md#its-rules).
import type { Command as PaletteItem } from "@mcurcio/patchbay/composites";
import { themeChoices, type ThemeChoice } from "@mcurcio/patchbay/hooks";
import { edgeId } from "./connect";
import { describeHealth, type HealthView } from "./health";
import { shortcutKeys, type ShortcutAction } from "./keys";
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

// ----- patchbay's palette -----

/**
 * The commands as patchbay's CommandPalette takes them, in the same order: each one's key drawn from keys.ts, its hint
 * as the quieter second line, and a press running `run` with its action (the palette asks to close first).
 */
export function paletteItems(commands: readonly Command[], run: (action: CommandAction) => void): PaletteItem[] {
  return commands.map((command) => {
    const item: PaletteItem = {
      id: command.id,
      label: command.label,
      group: command.group,
      run: () => run(command.action),
    };
    if (command.hint !== undefined) item.hint = command.hint;
    if (command.shortcut !== undefined) item.keys = shortcutKeys[command.shortcut];
    return item;
  });
}
