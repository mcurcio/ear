// The editor's one keydown listener (docs/editor.md#shortcuts): each key turned into a shortcut by keys.ts, by the
// platform and by whether the focus is in a text field, and handed on with the browser's own action prevented (so
// Cmd/Ctrl+S never opens the Save page). React Flow's Delete and Backspace stay its own: it skips inputs, selects,
// textareas, contenteditable and anything inside .nokey by the same rule, so both coexist with focus in the text.
import { useEffect, useRef } from "react";
import { isTextTarget, shortcutOf, type ShortcutAction } from "../keys";

export function useShortcuts(mac: boolean, onShortcut: (action: ShortcutAction, event: KeyboardEvent) => void): void {
  // Kept in a ref, so a new handler each render does not subscribe again.
  const handler = useRef(onShortcut);
  handler.current = onShortcut;

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.defaultPrevented || event.isComposing) return;
      const target = event.target instanceof Element ? event.target : document.activeElement;
      const action = shortcutOf(event, mac, isTextTarget(target as HTMLElement | null));
      if (action === undefined) return;
      event.preventDefault();
      handler.current(action, event);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [mac]);
}
