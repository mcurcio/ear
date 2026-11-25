// The sheet `?` opens (docs/editor.md#shortcuts): every shortcut and what it does, from keys.ts's rows, in a modal
// dialog. Escape, Close or a pointer outside closes it, and focus goes back where it was.
import { useLayoutEffect, useRef } from "react";
import { rowKeys, shortcutRows } from "../keys";

export function ShortcutSheet({ mac, onClose }: { mac: boolean; onClose: () => void }) {
  const close = useRef<HTMLButtonElement>(null);
  useLayoutEffect(() => {
    const opener = document.activeElement;
    close.current?.focus();
    return () => {
      if (opener instanceof HTMLElement && opener.isConnected) opener.focus();
    };
  }, []);

  return (
    <div className="palette-backdrop" onPointerDown={(event) => event.target === event.currentTarget && onClose()}>
      <div
        className="shortcut-sheet"
        role="dialog"
        aria-modal="true"
        aria-labelledby="shortcut-sheet-title"
        onKeyDown={(event) => {
          if (event.key !== "Escape") return;
          event.preventDefault();
          event.stopPropagation();
          onClose();
        }}
      >
        <h2 id="shortcut-sheet-title">Keyboard shortcuts</h2>
        <dl className="shortcut-list">
          {shortcutRows.map(({ action, does }) => (
            <div key={action} className="shortcut-row">
              <dt>
                <kbd>{rowKeys(action, mac)}</kbd>
              </dt>
              <dd>{does}</dd>
            </div>
          ))}
        </dl>
        <p className="shortcut-note">Single keys act only when the focus is not in a text field.</p>
        <button type="button" ref={close} onClick={onClose}>
          Close
        </button>
      </div>
    </div>
  );
}
