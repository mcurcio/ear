// The inspector's settings form (docs/editor.md#the-settings-form): one field per row of the node type's schema, each
// with its draft, its hints from the schema alone and ear's refusal placed under the key it names; Apply sends the
// drafts as one node.set, asking first when the dry run says the node restarts, reshapes or loses routes. The logic is
// form.ts; ear judges every value.
import { useEffect, useMemo, useState } from "react";
import { appliesAfter, ApplyFlow, edit, fieldsOf, patchOf, takeEar, type ApplyStep, type Draft, type FieldView } from "../form";
import { refusalParts } from "../notices";
import type { GraphNode, TypeSchema } from "../protocol";
import { RpcError } from "../rpc";
import { useEditor } from "./hooks";
import { Spinner } from "./Spinner";

function Field({
  field,
  node,
  onChange,
  onTakeEar,
}: {
  field: FieldView;
  node: GraphNode;
  onChange: (value: string | boolean) => void;
  onTakeEar: () => void;
}) {
  const id = `setting-${node.name}-${field.key}`;
  const disabled = field.disabledBy !== undefined;
  const words = [...field.hints];
  if (field.drafted && appliesAfter(node, field.key)) words.push("applies after this change");
  const control = (() => {
    switch (field.control) {
      case "flag":
        return (
          <input
            id={id}
            type="checkbox"
            checked={field.value === true}
            disabled={disabled}
            onChange={(event) => onChange(event.target.checked)}
          />
        );
      case "select":
        return (
          <select id={id} value={String(field.value)} disabled={disabled} onChange={(event) => onChange(event.target.value)}>
            {(field.options ?? []).map((option) => (
              <option key={option} value={option}>
                {option === "" ? `(${field.placeholder})` : option}
              </option>
            ))}
          </select>
        );
      case "number":
        return (
          <input
            id={id}
            type="number"
            inputMode="numeric"
            step={1}
            min={field.min}
            max={field.max}
            placeholder={field.placeholder}
            value={String(field.value)}
            disabled={disabled}
            onChange={(event) => onChange(event.target.value)}
          />
        );
      case "decimal":
        return (
          <input
            id={id}
            type="number"
            inputMode="decimal"
            step="any"
            min={field.min}
            max={field.max}
            placeholder={field.placeholder}
            value={String(field.value)}
            disabled={disabled}
            onChange={(event) => onChange(event.target.value)}
          />
        );
      default:
        return (
          <input
            id={id}
            type="text"
            spellCheck={false}
            placeholder={field.placeholder}
            value={String(field.value)}
            disabled={disabled}
            onChange={(event) => onChange(event.target.value)}
          />
        );
    }
  })();
  return (
    <div className={`settings-field${field.drafted ? " settings-field-drafted" : ""}`}>
      <label htmlFor={id}>
        {field.key}
        {field.unit !== undefined && <span className="settings-unit"> ({field.unit})</span>}
        {field.required && <span className="settings-required"> required</span>}
      </label>
      {control}
      {disabled && <p className="settings-hint">only without {field.disabledBy}</p>}
      {words.length > 0 && <p className="settings-hint">{words.join(" · ")}</p>}
      {field.help !== undefined && <p className="settings-hint">{field.help}</p>}
      {field.changedOnEar && (
        <p className="settings-hint settings-changed" role="status">
          changed on ear{" "}
          <button type="button" onClick={onTakeEar}>
            Take ear&apos;s value
          </button>
        </p>
      )}
      {field.problem !== undefined && <p className="settings-problem">{field.problem}</p>}
    </div>
  );
}

export function SettingsForm({ node, schema, dryRun }: { node: GraphNode; schema: TypeSchema; dryRun: boolean }) {
  const editor = useEditor();
  const [drafts, setDrafts] = useState<ReadonlyMap<string, Draft>>(new Map());
  const [step, setStep] = useState<ApplyStep>({ kind: "idle" });
  // Refusals are placed here, under their key or at the top, so only a lost answer is also a toast.
  const flow = useMemo(
    () =>
      new ApplyFlow(
        node.name,
        { set: (params) => editor.ask("node.set", params, { placed: (error) => error instanceof RpcError }) },
        dryRun,
        setStep,
      ),
    [editor, node.name, dryRun],
  );
  // Once ear applied the drafts, the fields show ear's values: the next Graph carries them.
  useEffect(() => {
    if (step.kind === "applied") setDrafts(new Map());
  }, [step]);

  const refusal = step.kind === "refused" && step.key !== undefined ? { key: step.key, detail: step.detail } : undefined;
  const fields = fieldsOf(schema, node.settings, drafts, refusal);
  const busy = step.kind === "checking" || step.kind === "sending";
  const made = patchOf(schema, node.settings, drafts);
  const canApply = drafts.size > 0 && "patch" in made && !busy && step.kind !== "confirm";

  const apply = () => {
    if ("patch" in made) void flow.apply(made.patch);
  };

  if (schema.settings.length === 0) return <p className="settings-none">{node.type} takes no settings.</p>;
  return (
    <form
      className="settings-form"
      aria-label={`${node.name}'s settings`}
      onSubmit={(event) => {
        event.preventDefault();
        if (canApply) apply();
      }}
    >
      {step.kind === "refused" && step.key === undefined && (
        <p className="settings-problem">
          {(() => {
            const parts = refusalParts(step.error);
            return `${parts.verb}: ${parts.code}${parts.detail !== "" ? ` ${parts.detail}` : ""}`;
          })()}
        </p>
      )}
      {fields.map((field) => (
        <Field
          key={field.key}
          field={field}
          node={node}
          onChange={(value) => setDrafts((now) => edit(schema, node.settings, now, field.key, value))}
          onTakeEar={() => setDrafts((now) => takeEar(now, field.key))}
        />
      ))}
      {step.kind === "confirm" ? (
        <div className="banner banner-warn settings-confirm" role="alertdialog" aria-label="Apply asks first">
          <p>{step.words}</p>
          <button type="button" onClick={() => void flow.confirm()}>
            Apply
          </button>{" "}
          <button type="button" onClick={() => flow.cancel()}>
            Cancel
          </button>
        </div>
      ) : (
        <div className="settings-actions">
          <button type="submit" disabled={!canApply} aria-busy={busy || undefined}>
            {busy && <Spinner />}
            {step.kind === "checking" ? "Checking…" : step.kind === "sending" ? "Applying…" : "Apply"}
          </button>{" "}
          <button type="button" disabled={drafts.size === 0 || busy} onClick={() => setDrafts(new Map())}>
            Discard
          </button>
        </div>
      )}
    </form>
  );
}
