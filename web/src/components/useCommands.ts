// The palette's commands over the live state, and what running each does (docs/editor.md#the-palette): palette.ts
// builds them; here an action becomes a request to ear (lifecycle and removal, through the session's ask, so a
// refusal is a toast and a spinner shows in the inspector while it is on its way) or a change of the browser's own
// view. Nothing here draws a result: ear announces it.
import type { ThemeChoice } from "@mcurcio/patchbay/hooks";
import { useCallback, useMemo } from "react";
import type { Position } from "../layout";
import { commandsOf, type Command, type CommandAction, type PaletteScope } from "../palette";
import type { Graph, RouteEnds } from "../protocol";
import type { Selection } from "../selection";
import { useEditor } from "./hooks";
import type { DrawerModel } from "./useDrawer";

export interface CommandDeps {
  graph: Graph | null;
  selection: Selection;
  drawer: DrawerModel | null;
  minimap: boolean;
  theme: ThemeChoice;
  goTo(name: string): void;
  onAdd(type: string, place?: Position): void;
  onRemove(nodes: readonly string[], routes: readonly RouteEnds[]): void;
  fit(): void;
  tidy(): void;
  toggleMinimap(): void;
  setTheme(choice: ThemeChoice): void;
  onShortcuts(): void;
}

export function useCommands(
  deps: CommandDeps,
  scope: PaletteScope,
): { commands: Command[]; run: (action: CommandAction, place?: Position) => void } {
  const editor = useEditor();
  const { graph, selection, drawer, minimap, theme } = deps;
  const open = drawer?.open ?? false;
  const canSave = drawer?.canSave ?? false;
  const stale = drawer?.pane.stale ?? false;
  const hasDrawer = drawer !== null;

  const commands = useMemo(() => {
    if (graph === null) return [];
    const file = hasDrawer ? { open, canSave, stale } : null;
    return commandsOf({ graph, selection, drawer: file, minimap, theme }, scope);
  }, [graph, selection, hasDrawer, open, canSave, stale, minimap, theme, scope]);

  const run = useCallback(
    (action: CommandAction, place?: Position) => {
      switch (action.kind) {
        case "go":
          return deps.goTo(action.node);
        case "add":
          return deps.onAdd(action.type, place);
        case "lifecycle":
          return editor.send(action.method, { name: action.node });
        case "remove-node":
          return deps.onRemove([action.node], []);
        case "remove-route":
          return deps.onRemove([], [action.ends]);
        case "drawer":
          if (drawer === null) return;
          if (action.what === "open") drawer.setOpen(true);
          else if (action.what === "close") drawer.setOpen(false);
          else if (action.what === "save") {
            if (drawer.canSave) drawer.save();
          } else if (drawer.pane.stale) drawer.dispatch({ kind: "reload" });
          return;
        case "fit":
          return deps.fit();
        case "tidy":
          return deps.tidy();
        case "minimap":
          return deps.toggleMinimap();
        case "theme":
          return deps.setTheme(action.choice);
        case "shortcuts":
          return deps.onShortcuts();
      }
    },
    [deps, drawer, editor],
  );

  return { commands, run };
}
