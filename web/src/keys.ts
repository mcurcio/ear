// The editor's key bindings (docs/editor.md#shortcuts): which key does what, as patchbay's key strings (Mod is
// Command on Apple platforms and Ctrl elsewhere), which of them act while the focus is in a text field, and the
// shortcut sheet's lines. patchbay's useShortcuts reads the table and its keyLabel draws the keys; single keys and
// Escape never act in a text field there, and the three chords opt in, since they type nothing. React Flow's Delete and
// Backspace stay its own, filtered by the same rule.
import type { ShortcutLine } from "@mcurcio/patchbay/composites";
import type { Shortcut } from "@mcurcio/patchbay/hooks";

export type ShortcutAction =
  | "palette"
  | "drawer"
  | "save"
  | "fit"
  | "tidy"
  | "minimap"
  | "add"
  | "shortcuts"
  | "escape";

/** Each action's key, as useShortcuts and keyLabel read it. */
export const shortcutKeys: Record<ShortcutAction, string> = {
  palette: "Mod+K",
  drawer: "Mod+E",
  save: "Mod+S",
  fit: "F",
  tidy: "L",
  minimap: "M",
  add: "A",
  shortcuts: "?",
  escape: "Escape",
};

/** The chords, which act in a text field too. */
const chords: ReadonlySet<ShortcutAction> = new Set(["palette", "drawer", "save"]);

/** What still acts while the palette is open: its toggle (the palette handles its own Escape). */
const overPalette: ReadonlySet<ShortcutAction> = new Set(["palette"]);

/** The table's order; the first binding a press matches runs. */
const order: readonly ShortcutAction[] = [
  "palette",
  "drawer",
  "save",
  "fit",
  "tidy",
  "minimap",
  "add",
  "shortcuts",
  "escape",
];

/**
 * The editor's bindings for useShortcuts, each running `run` with its action. While the palette is open only its
 * toggle acts; useShortcuts is turned off whole while the shortcut sheet is open (patchbay's Dialog owns its keys).
 */
export function shortcutTable(
  run: (action: ShortcutAction, event: KeyboardEvent) => void,
  state: { palette: boolean },
): Shortcut[] {
  return order.map((action) => ({
    keys: shortcutKeys[action],
    run: (event: KeyboardEvent) => run(action, event),
    inTextFields: chords.has(action),
    enabled: !state.palette || overPalette.has(action),
  }));
}

/** The groups the shortcut sheet lists its lines under, in its order. */
export type ShortcutGroup = "Commands" | "View" | "Selection";

/** The shortcut sheet's lines, in its order; Delete and Backspace are React Flow's own keys. */
export const shortcutLines: readonly (ShortcutLine & { group: ShortcutGroup })[] = [
  { keys: shortcutKeys.palette, label: "Command palette", group: "Commands" },
  { keys: shortcutKeys.add, label: "Add a node", group: "Commands" },
  { keys: shortcutKeys.drawer, label: "Show or hide ear.conf", group: "Commands" },
  { keys: shortcutKeys.save, label: "Save ear.conf's draft", group: "Commands" },
  { keys: shortcutKeys.shortcuts, label: "These shortcuts", group: "Commands" },
  { keys: shortcutKeys.fit, label: "Fit the view", group: "View" },
  { keys: shortcutKeys.tidy, label: "Tidy the layout", group: "View" },
  { keys: shortcutKeys.minimap, label: "Show or hide the minimap", group: "View" },
  {
    keys: shortcutKeys.escape,
    label: "Close the palette or the inspector's sheet, or clear the selection",
    group: "Selection",
  },
  { keys: ["Delete", "Backspace"], label: "Remove the selection (asks ear)", group: "Selection" },
];
