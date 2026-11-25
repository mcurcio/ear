// A resize handle (docs/editor.md#the-inspector, docs/editor.md#the-drawer): the inspector's left edge and the
// drawer's top edge, dragged with the pointer or moved by the arrow keys, Home and End while focused, as a WAI-ARIA
// separator. Hand-written, no package; sizes.ts holds the maths, editor.css the look.
import { useRef, type KeyboardEvent, type PointerEvent } from "react";
import { dragged, keyed, type Bounds, type GrowKey } from "../sizes";

export function Splitter({
  orientation,
  label,
  value,
  bounds,
  growKey,
  onChange,
}: {
  /** vertical: a vertical bar that resizes a width (the inspector); horizontal: one that resizes a height. */
  orientation: "vertical" | "horizontal";
  label: string;
  value: number;
  bounds: Bounds;
  growKey: GrowKey;
  onChange: (px: number) => void;
}) {
  const drag = useRef<{ start: number; at: number } | null>(null);
  const along = (event: PointerEvent) => (orientation === "vertical" ? event.clientX : event.clientY);

  const onPointerDown = (event: PointerEvent<HTMLDivElement>) => {
    if (event.button !== 0) return;
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    drag.current = { start: value, at: along(event) };
  };
  const onPointerMove = (event: PointerEvent<HTMLDivElement>) => {
    const started = drag.current;
    if (started === null) return;
    // Both splitters sit on the leading edge of what they resize: moving left or up grows it.
    onChange(dragged(started.start, along(event) - started.at, -1, bounds));
  };
  const onPointerEnd = (event: PointerEvent<HTMLDivElement>) => {
    if (drag.current === null) return;
    drag.current = null;
    const target = event.currentTarget;
    if (target.hasPointerCapture(event.pointerId)) target.releasePointerCapture(event.pointerId);
  };
  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    const next = keyed(value, event.key, growKey, bounds);
    if (next === undefined) return;
    event.preventDefault();
    onChange(next);
  };

  return (
    <div
      role="separator"
      tabIndex={0}
      aria-orientation={orientation}
      aria-label={label}
      aria-valuenow={value}
      aria-valuemin={bounds.min}
      aria-valuemax={bounds.max}
      className={`splitter splitter-${orientation}`}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerEnd}
      onPointerCancel={onPointerEnd}
      onKeyDown={onKeyDown}
    />
  );
}
