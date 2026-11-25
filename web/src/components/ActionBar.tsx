// A node's type actions (docs/editor.md#actions): one button per action after the lifecycle bar, each enabled by
// actions.ts; a call with arguments opens a small form under the bar, drawn from its argument rows as the settings form
// draws a field. A button spins and is disabled while its own request is on its way; a refusal shows under the bar as
// well as in a toast (docs/editor.md#where-a-refusal-lands).
import { useState } from "react";
import { actionAvailability, actionLabel, argFields, argsOf } from "../actions";
import type { FieldValue } from "../form";
import { refusalParts, type RefusalParts } from "../notices";
import type { ActionSchema, GraphNode } from "../protocol";
import { useEditor } from "./hooks";
import { Spinner } from "./Spinner";

function ArgsForm({
  action,
  onSend,
  onCancel,
}: {
  action: ActionSchema;
  onSend: (drafts: ReadonlyMap<string, FieldValue>) => void;
  onCancel: () => void;
}) {
  const [drafts, setDrafts] = useState<ReadonlyMap<string, FieldValue>>(new Map());
  const fields = argFields(action, drafts);
  const set = (key: string, value: FieldValue) => setDrafts(new Map(drafts).set(key, value));
  return (
    <form
      className="action-args"
      aria-label={`${actionLabel(action)} arguments`}
      onSubmit={(event) => {
        event.preventDefault();
        onSend(drafts);
      }}
    >
      {fields.map((field) => (
        <label key={field.key}>
          {field.key}
          {field.unit !== undefined && ` (${field.unit})`}
          {field.control === "flag" ? (
            <input type="checkbox" checked={field.value === true} onChange={(e) => set(field.key, e.target.checked)} />
          ) : field.control === "select" ? (
            <select value={String(field.value)} onChange={(e) => set(field.key, e.target.value)}>
              {(field.options ?? []).map((option) => (
                <option key={option} value={option}>
                  {option}
                </option>
              ))}
            </select>
          ) : (
            <input
              type={field.control === "number" ? "number" : "text"}
              min={field.min}
              max={field.max}
              value={String(field.value)}
              required={field.required}
              aria-invalid={field.problem !== undefined || undefined}
              onChange={(e) => set(field.key, e.target.value)}
            />
          )}
          {field.problem !== undefined && <span className="field-problem">{field.problem}</span>}
          {field.help !== undefined && <span className="field-help">{field.help}</span>}
        </label>
      ))}
      <button type="submit" disabled={fields.some((field) => field.problem !== undefined)}>
        {actionLabel(action)}
      </button>
      <button type="button" onClick={onCancel}>
        Cancel
      </button>
    </form>
  );
}

export function ActionBar({ node, actions }: { node: GraphNode; actions: ActionSchema[] }) {
  const editor = useEditor();
  const [inFlight, setInFlight] = useState<ReadonlySet<string>>(new Set());
  const [open, setOpen] = useState<string | null>(null);
  const [refusal, setRefusal] = useState<{ label: string; parts: RefusalParts } | null>(null);
  if (actions.length === 0) return null;

  const invoke = (action: ActionSchema, drafts?: ReadonlyMap<string, FieldValue>) => {
    let args: Record<string, unknown> | undefined;
    if (drafts !== undefined) {
      const made = argsOf(action, drafts);
      if ("problems" in made) return;
      args = made.args;
    }
    setRefusal(null);
    setOpen(null);
    setInFlight((now) => new Set(now).add(action.name));
    const params = args === undefined ? { name: node.name, action: action.name } : { name: node.name, action: action.name, args };
    editor
      .ask("node.invoke", params as never)
      .catch((error: unknown) => setRefusal({ label: actionLabel(action), parts: refusalParts(error) }))
      .finally(() =>
        setInFlight((now) => {
          const next = new Set(now);
          next.delete(action.name);
          return next;
        }),
      );
  };

  return (
    <>
      <div className="action-buttons" role="group" aria-label="actions">
        {actions.map((action) => {
          const availability = actionAvailability(node, action);
          const busy = inFlight.has(action.name);
          return (
            <button
              key={action.name}
              type="button"
              disabled={!availability.enabled || busy}
              aria-busy={busy || undefined}
              aria-expanded={action.args.length > 0 ? open === action.name : undefined}
              title={availability.why ?? action.help}
              onClick={() => (action.args.length > 0 ? setOpen(open === action.name ? null : action.name) : invoke(action))}
            >
              {busy && <Spinner />}
              {actionLabel(action)}
            </button>
          );
        })}
      </div>
      {open !== null &&
        actions
          .filter((action) => action.name === open)
          .map((action) => (
            <ArgsForm
              key={action.name}
              action={action}
              onSend={(drafts) => invoke(action, drafts)}
              onCancel={() => setOpen(null)}
            />
          ))}
      {refusal !== null && (
        // No live role: the toast announces it.
        <p className="lifecycle-refusal">
          {refusal.label} {refusal.parts.verb}: <code>{refusal.parts.code}</code>
          {refusal.parts.detail !== "" && ` ${refusal.parts.detail}`}
        </p>
      )}
    </>
  );
}
