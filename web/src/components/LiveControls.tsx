// The editor's live controls (docs/editor.md#live-numbers-faders-and-knobs): a fader (the form's, horizontal) and a
// knob (the box's, dragged vertically) for a live ramped decimal, each a gesture (gesture.ts) drawn over a visually
// hidden native range input that takes the focus, the keys and a screen reader's swipes; and the number field with
// steppers that sends a live number by node.set on Enter or a step. The rules are gesture.ts's, taper.ts's, live.ts's
// and form.ts's; this file only turns pointer, key and wheel events into them.
import { useEffect, useMemo, useRef, useState, useSyncExternalStore, type PointerEvent as ReactPointerEvent } from "react";
import { liveNumberPatch, steppedValue, type FieldView } from "../form";
import {
  heldByOther,
  inputScale,
  inputStep,
  inputValueOf,
  liveDisabledReason,
  rangeKey,
  shownNumber,
  valueOfInput,
} from "../live";
import type { GraphNode, JsonObject, SettingSchema } from "../protocol";
import { roundedValue, travelOf, valueAt, valueText } from "../taper";
import { useConnection, useEditor, useGraphFile } from "./hooks";

/** Why the live controls are disabled now (live.ts), or undefined. */
export function useLiveDisabled(): string | undefined {
  const connection = useConnection();
  const file = useGraphFile();
  return liveDisabledReason(connection, file?.file);
}

/** The window lost the focus or was hidden: every held control lets go (its gesture ends at the shown value). */
export function useReleaseOnBlur(): void {
  const { gestures } = useEditor();
  useEffect(() => {
    const blur = () => gestures.releaseAll();
    const hidden = () => {
      if (document.visibilityState === "hidden") gestures.releaseAll();
    };
    window.addEventListener("blur", blur);
    document.addEventListener("visibilitychange", hidden);
    return () => {
      window.removeEventListener("blur", blur);
      document.removeEventListener("visibilitychange", hidden);
    };
  }, [gestures]);
}

/** The whole travel of a knob's vertical drag, in px; with Shift, ten times finer. */
export const knobTravelPx = 200;
/** The wheel's travel per notch on a focused control. */
const wheelTravel = 0.01;

function defaultOf(row: SettingSchema): number {
  return typeof row.default === "number" ? row.default : (row.min ?? 0);
}

/** A fader or a knob: one gesture control over a hidden native range input. */
function LiveRange({
  node,
  row,
  shape,
  disabledReason,
}: {
  node: GraphNode;
  row: SettingSchema;
  shape: "fader" | "knob";
  disabledReason: string | undefined;
}) {
  const { gestures } = useEditor();
  const control = useMemo(() => gestures.control(node.name, row.key, row), [gestures, node.name, row]);
  const view = useSyncExternalStore(
    (listener) => control.subscribe(listener),
    () => control.get(),
  );
  const held = view.state === "idle" ? heldByOther(node, row.key, view.id) : undefined;
  const reason = view.state !== "idle" ? undefined : (disabledReason ?? (held !== undefined ? "held by another editor" : undefined));
  const disabled = reason !== undefined;
  const value = shownNumber(node, row, view);
  const travel = travelOf(row, value);
  const words = valueText(row, value);
  const input = useRef<HTMLInputElement>(null);
  const surface = useRef<HTMLDivElement>(null);
  const drag = useRef<{ y: number; travel: number } | null>(null);

  // The wheel only on a focused control, and never the page's scroll then: a listener that is not passive.
  useEffect(() => {
    const element = surface.current;
    if (element === null) return undefined;
    const wheel = (event: WheelEvent) => {
      if (disabled || document.activeElement !== input.current || event.deltaY === 0) return;
      event.preventDefault();
      const now = control.get();
      const at = now.state !== "idle" && now.value !== undefined ? now.value : value;
      control.nudge(valueAt(row, travelOf(row, at) + (event.deltaY < 0 ? wheelTravel : -wheelTravel)));
    };
    element.addEventListener("wheel", wheel, { passive: false });
    return () => element.removeEventListener("wheel", wheel);
  }, [control, row, value, disabled]);

  const travelAtPointer = (event: ReactPointerEvent<HTMLDivElement>): number => {
    const box = event.currentTarget.getBoundingClientRect();
    return box.width > 0 ? (event.clientX - box.left) / box.width : 0;
  };

  const onPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (disabled || event.button !== 0) return;
    event.preventDefault();
    input.current?.focus({ preventScroll: true });
    if (event.altKey) {
      control.setTo(defaultOf(row));
      return;
    }
    event.currentTarget.setPointerCapture?.(event.pointerId);
    control.press();
    if (shape === "fader") control.move(valueAt(row, travelAtPointer(event)));
    else drag.current = { y: event.clientY, travel };
  };
  const onPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!event.currentTarget.hasPointerCapture?.(event.pointerId)) return;
    if (shape === "fader") {
      control.move(valueAt(row, travelAtPointer(event)));
      return;
    }
    const start = drag.current;
    if (start === null) return;
    const moved = start.travel + (start.y - event.clientY) / (event.shiftKey ? knobTravelPx * 10 : knobTravelPx);
    drag.current = { y: event.clientY, travel: Math.min(1, Math.max(0, moved)) };
    control.move(valueAt(row, drag.current.travel));
  };
  const letGo = () => {
    drag.current = null;
    control.release();
  };

  const label = `${node.name} ${row.key}`;
  const state = `live-${shape} live-state-${view.state}${view.pending ? " live-pending" : ""}${disabled ? " live-disabled" : ""}`;
  return (
    <div className={`live-range ${state}${shape === "knob" ? " nodrag nowheel nopan nokey" : " nokey"}`}>
      <input
        ref={input}
        type="range"
        className="visually-hidden"
        aria-label={label}
        aria-valuetext={words}
        min={0}
        max={inputScale}
        step={inputStep}
        value={inputValueOf(row, value)}
        disabled={disabled}
        onKeyDown={(event) => {
          const action = rangeKey(row, value, event.key, event.shiftKey, view.state !== "idle");
          if (action === undefined) return;
          event.preventDefault();
          if ("escape" in action) {
            event.stopPropagation();
            control.escape();
          } else control.nudge(action.nudge);
        }}
        // A screen reader's adjustment (a swipe) is no key the control saw: a gesture as the arrows' are.
        onChange={(event) => control.nudge(valueOfInput(row, event.target.value))}
        onBlur={() => control.release()}
      />
      <div
        ref={surface}
        className="live-surface"
        aria-hidden="true"
        title={reason !== undefined ? `${label}: ${reason}` : `${label} ${words}`}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={letGo}
        onPointerCancel={letGo}
        onLostPointerCapture={letGo}
        onDoubleClick={() => {
          if (!disabled) control.setTo(defaultOf(row));
        }}
      >
        {shape === "fader" ? (
          <div className="live-track">
            <div className="live-fill" style={{ width: `${travel * 100}%` }} />
            <div className="live-thumb" style={{ left: `${travel * 100}%` }} />
          </div>
        ) : (
          <svg className="live-dial" viewBox="0 0 40 40" width="40" height="40">
            <circle className="live-dial-face" cx="20" cy="20" r="15" />
            <line
              className="live-dial-pointer"
              x1="20"
              y1="20"
              x2="20"
              y2="7"
              transform={`rotate(${-135 + travel * 270} 20 20)`}
            />
          </svg>
        )}
      </div>
      {shape === "knob" && (
        <span className="live-knob-words">
          <span className="live-knob-key">{row.key}</span> {words}
        </span>
      )}
      {reason !== undefined && shape === "fader" && <p className="settings-hint">{reason}</p>}
      {view.lost !== undefined && (
        <p className="settings-hint live-lost" role="status">
          {view.lost}
        </p>
      )}
    </div>
  );
}

/** The form's fader for a live ramped decimal (feature gestures), with its number field beside it. */
export function Fader({
  node,
  field,
  disabledReason,
  onCommit,
}: {
  node: GraphNode;
  field: FieldView;
  disabledReason: string | undefined;
  onCommit: (key: string, patch: JsonObject) => void;
}) {
  const { gestures } = useEditor();
  const row = field.row;
  const control = useMemo(
    () => (row !== undefined ? gestures.control(node.name, row.key, row) : undefined),
    [gestures, node.name, row],
  );
  const view = useSyncExternalStore(
    (listener) => control?.subscribe(listener) ?? (() => {}),
    () => control?.get(),
  );
  if (row === undefined) return null;
  // While the person holds it, the field beside shows the hand's value, at the row's places.
  const holding = view !== undefined && view.state !== "idle" && view.value !== undefined;
  const showing = holding ? String(roundedValue(row, view.value!)) : undefined;
  return (
    <div className="live-fader-row">
      <LiveRange node={node} row={row} shape="fader" disabledReason={disabledReason} />
      <LiveNumber
        node={node}
        field={field}
        disabledReason={disabledReason}
        onCommit={onCommit}
        steppers={false}
        showing={showing}
      />
    </div>
  );
}

/** The box's knob for a live ramped decimal not in a family (feature gestures). */
export function Knob({ node, row, disabledReason }: { node: GraphNode; row: SettingSchema; disabledReason: string | undefined }) {
  return <LiveRange node={node} row={row} shape="knob" disabledReason={disabledReason} />;
}

/**
 * A live number's field: what the person types stays theirs until Enter sends it by node.set (no draft, no Apply);
 * the steppers send a step at once. It shows ear's value otherwise.
 */
export function LiveNumber({
  node,
  field,
  disabledReason,
  onCommit,
  steppers = true,
  showing,
}: {
  node: GraphNode;
  field: FieldView;
  disabledReason: string | undefined;
  onCommit: (key: string, patch: JsonObject) => void;
  steppers?: boolean;
  /** The value a held fader beside it shows, in place of ear's. */
  showing?: string;
}) {
  const { schemaOf } = useSchemaOf();
  const [typed, setTyped] = useState<string | undefined>(undefined);
  const [problem, setProblem] = useState<string | undefined>(undefined);
  const id = `setting-${node.name}-${field.key}`;
  const disabled = disabledReason !== undefined || field.disabledBy !== undefined;
  const shown = typed ?? showing ?? String(field.value);
  const send = (text: string) => {
    const schema = schemaOf(node);
    if (schema === undefined) return;
    const made = liveNumberPatch(schema, field.key, text);
    if (made === undefined) return;
    if ("problem" in made) return setProblem(made.problem);
    setProblem(undefined);
    setTyped(undefined);
    onCommit(field.key, made.patch);
  };
  return (
    <span className="live-number">
      {steppers && (
        <button
          type="button"
          aria-label={`${field.key} down a step`}
          disabled={disabled}
          onClick={() => send(steppedValue(field, String(field.value), -1))}
        >
          −
        </button>
      )}
      <input
        id={id}
        type="number"
        inputMode="decimal"
        step={field.step ?? "any"}
        min={field.min}
        max={field.max}
        placeholder={field.placeholder}
        value={shown}
        disabled={disabled}
        aria-describedby={problem !== undefined ? `${id}-problem` : undefined}
        onChange={(event) => setTyped(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            send(shown);
          } else if (event.key === "Escape" && typed !== undefined) {
            event.preventDefault();
            event.stopPropagation();
            setTyped(undefined);
            setProblem(undefined);
          }
        }}
        onBlur={() => {
          // An unsent number goes back to ear's: only Enter sends.
          setTyped(undefined);
          setProblem(undefined);
        }}
      />
      {steppers && (
        <button
          type="button"
          aria-label={`${field.key} up a step`}
          disabled={disabled}
          onClick={() => send(steppedValue(field, String(field.value), 1))}
        >
          +
        </button>
      )}
      {problem !== undefined && (
        <span id={`${id}-problem`} className="settings-problem">
          {problem}
        </span>
      )}
    </span>
  );
}

/** The schema of a node's type, as the editor holds it. */
function useSchemaOf() {
  const { types, graph } = useEditor();
  return { schemaOf: (node: GraphNode) => types.schemaOf(graph.get(), node.type) };
}
