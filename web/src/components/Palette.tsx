// The command palette (docs/editor.md#the-palette): a WAI-ARIA combobox over a listbox, in a modal dialog, centred over
// the canvas for Cmd/Ctrl+K, or at the pointer for quick add (docs/editor.md#adding-on-the-canvas). The commands and
// their ranking are palette.ts's; the matched letters are marked. Arrows, Page Up and Page Down move, Enter runs,
// Escape and Tab close, each kept from the editor's shortcuts; focus goes back where it was. Hand-written, no package.
import { useEffect, useLayoutEffect, useMemo, useRef, useState, type KeyboardEvent, type ReactNode } from "react";
import { keyLabel } from "../keys";
import { anchoredAt, rank, type Command } from "../palette";

/** The anchored popover's size: editor.css's .palette[data-anchored] width and max-height. */
const anchoredBox = { width: 320, height: 360 };
const pageStep = 8;

/** The label with its matched characters in <mark>. */
function marked(label: string, marks: readonly number[]): ReactNode[] {
  const parts: ReactNode[] = [];
  const set = new Set(marks);
  let run = "";
  let runMarked = false;
  const flush = (key: number) => {
    if (run === "") return;
    parts.push(runMarked ? <mark key={key}>{run}</mark> : run);
    run = "";
  };
  [...label].forEach((char, index) => {
    const isMarked = set.has(index);
    if (isMarked !== runMarked) {
      flush(index);
      runMarked = isMarked;
    }
    run += char;
  });
  flush(label.length);
  return parts;
}

export function Palette({
  commands,
  at,
  placeholder,
  mac,
  onRun,
  onClose,
}: {
  commands: readonly Command[];
  /** Screen point for quick add; absent: centred over the canvas. */
  at?: { x: number; y: number };
  placeholder: string;
  mac: boolean;
  /** The palette closes itself first. */
  onRun: (command: Command) => void;
  onClose: () => void;
}) {
  const [query, setQuery] = useState("");
  const [active, setActive] = useState(0);
  const list = useMemo(() => rank(commands, query), [commands, query]);
  const listRef = useRef<HTMLUListElement>(null);
  const opener = useRef<Element | null>(null);

  // Focus goes back to what had it when the palette opened.
  useLayoutEffect(() => {
    opener.current = document.activeElement;
    return () => {
      const back = opener.current;
      if (back instanceof HTMLElement && back.isConnected) back.focus();
    };
  }, []);

  useEffect(() => setActive(0), [query]);
  const current = list.length === 0 ? -1 : Math.min(active, list.length - 1);

  useEffect(() => {
    if (current < 0) return;
    listRef.current?.querySelector(`#palette-option-${current}`)?.scrollIntoView({ block: "nearest" });
  }, [current]);

  const run = (index: number) => {
    const chosen = list[index];
    if (chosen === undefined) return;
    onClose();
    onRun(chosen.command);
  };

  const move = (by: number, wrap: boolean) => {
    if (list.length === 0) return;
    const next = current + by;
    if (wrap) setActive((next + list.length) % list.length);
    else setActive(Math.max(0, Math.min(list.length - 1, next)));
  };

  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    switch (event.key) {
      case "ArrowDown":
        move(1, true);
        break;
      case "ArrowUp":
        move(-1, true);
        break;
      case "PageDown":
        move(pageStep, false);
        break;
      case "PageUp":
        move(-pageStep, false);
        break;
      case "Enter":
        run(current);
        break;
      case "Escape":
      case "Tab":
        onClose();
        break;
      default:
        return;
    }
    event.preventDefault();
    event.stopPropagation();
  };

  const style = at === undefined ? undefined : anchoredAt(at, anchoredBox, { width: innerWidth, height: innerHeight });

  return (
    <div className="palette-backdrop" onPointerDown={(event) => event.target === event.currentTarget && onClose()}>
      <div
        className="palette"
        role="dialog"
        aria-modal="true"
        aria-label="Command palette"
        data-anchored={at !== undefined || undefined}
        style={style}
      >
        <input
          className="palette-input nokey"
          role="combobox"
          aria-expanded="true"
          aria-controls="palette-list"
          aria-autocomplete="list"
          aria-activedescendant={current < 0 ? undefined : `palette-option-${current}`}
          aria-label={placeholder}
          placeholder={placeholder}
          autoFocus
          autoComplete="off"
          spellCheck={false}
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          onKeyDown={onKeyDown}
        />
        {list.length === 0 && (
          <p className="palette-empty" role="status">
            No command matches
          </p>
        )}
        <ul id="palette-list" ref={listRef} className="palette-list" role="listbox" aria-label="Commands">
          {list.map(({ command, match }, index) => (
            <li
              key={command.id}
              id={`palette-option-${index}`}
              className="palette-option"
              role="option"
              aria-selected={index === current}
              onPointerMove={() => index !== current && setActive(index)}
              // Keeps the focus in the input, so a click runs the command and not a blur.
              onPointerDown={(event) => event.preventDefault()}
              onClick={() => run(index)}
            >
              <span className="palette-label">{marked(command.label, match.marks)}</span>
              <span className="palette-meta">
                {command.hint !== undefined && <span className="palette-hint">{command.hint}</span>}
                <span className="palette-group">{command.group}</span>
              </span>
              {command.shortcut !== undefined ? <kbd>{keyLabel(command.shortcut, mac)}</kbd> : <span />}
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}
