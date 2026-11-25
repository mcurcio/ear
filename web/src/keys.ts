// The editor's shortcuts, pure (docs/editor.md#shortcuts): which shortcut a key event is, by platform (Mod is Command
// on a Mac and Ctrl elsewhere) and by whether the focus is in a text field, and the shortcut sheet's rows. Single keys
// and Escape act only outside a text field, where every key is the text's; the three chords act everywhere, since
// they type nothing. React Flow's Delete and Backspace stay its own, filtered by the same rule.

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

/** What shortcutOf reads of a KeyboardEvent. */
export interface KeyLike {
  key: string;
  metaKey: boolean;
  ctrlKey: boolean;
  altKey: boolean;
  shiftKey: boolean;
}

/** What isTextTarget reads of an element. */
export interface TargetLike {
  tagName?: string;
  type?: string;
  isContentEditable?: boolean;
  readOnly?: boolean;
}

/** The input types a person types text into; a checkbox, button or range is not one. */
const textInputTypes = new Set(["", "text", "search", "url", "tel", "email", "password", "number"]);

/** An input of a text kind (no type counts as text), a textarea, a select, or contenteditable. */
export function isTextTarget(target: TargetLike | null): boolean {
  if (target === null) return false;
  if (target.isContentEditable === true) return true;
  const tag = (target.tagName ?? "").toUpperCase();
  if (tag === "TEXTAREA" || tag === "SELECT") return true;
  if (tag === "INPUT") return textInputTypes.has((target.type ?? "").toLowerCase());
  return false;
}

/** Whether a platform string (navigator's) is Apple's, where Mod is Command. */
export function isMac(platform: string): boolean {
  return /mac|iphone|ipad|ipod/i.test(platform);
}

const chords: Record<string, ShortcutAction> = { k: "palette", e: "drawer", s: "save" };
const singles: Record<string, ShortcutAction> = { f: "fit", l: "tidy", m: "minimap", a: "add", "?": "shortcuts" };

/**
 * The shortcut a key is, or undefined. Mod+K, Mod+E and Mod+S (no Alt) act in a text field too; Escape and the single
 * keys (with no modifier but Shift, which `?` needs on most layouts) only outside one.
 */
export function shortcutOf(event: KeyLike, mac: boolean, inText: boolean): ShortcutAction | undefined {
  const key = event.key.toLowerCase();
  const mod = mac ? event.metaKey && !event.ctrlKey : event.ctrlKey && !event.metaKey;
  if (mod && !event.altKey) return chords[key];
  if (event.metaKey || event.ctrlKey || event.altKey || inText) return undefined;
  if (event.key === "Escape") return "escape";
  return singles[key];
}

const letters: Record<Exclude<ShortcutAction, "palette" | "drawer" | "save">, string> = {
  fit: "F",
  tidy: "L",
  minimap: "M",
  add: "A",
  shortcuts: "?",
  escape: "Esc",
};

/** A shortcut's key as the platform writes it: "⌘K" or "Ctrl+K", "F", "?", "Esc". */
export function keyLabel(action: ShortcutAction, mac: boolean): string {
  switch (action) {
    case "palette":
    case "drawer":
    case "save": {
      const letter = { palette: "K", drawer: "E", save: "S" }[action];
      return mac ? `⌘${letter}` : `Ctrl+${letter}`;
    }
    default:
      return letters[action];
  }
}

/** The shortcut sheet's rows, in its order; "delete" is React Flow's own key. */
export const shortcutRows: readonly { action: ShortcutAction | "delete"; does: string }[] = [
  { action: "palette", does: "Command palette" },
  { action: "add", does: "Add a node" },
  { action: "drawer", does: "Show or hide ear.conf" },
  { action: "save", does: "Save ear.conf's draft" },
  { action: "fit", does: "Fit the view" },
  { action: "tidy", does: "Tidy the layout" },
  { action: "minimap", does: "Show or hide the minimap" },
  { action: "shortcuts", does: "These shortcuts" },
  { action: "escape", does: "Close the palette or this sheet, or clear the selection" },
  { action: "delete", does: "Remove the selection (asks ear)" },
];

/** A row's keys: keyLabel's, or "Delete / Backspace". */
export function rowKeys(action: ShortcutAction | "delete", mac: boolean): string {
  return action === "delete" ? "Delete / Backspace" : keyLabel(action, mac);
}
