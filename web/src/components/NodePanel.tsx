// The inspector's Node subject (docs/editor.md#the-inspector): the node's name and type, its state pill as the box
// draws it and its line (health.ts, docs/editor.md#a-nodes-health), the lifecycle bar (Start, Stop, Restart, Disable
// and Enable when ear has the lifecycle feature, each enabled by health.ts), its type's actions after it (with the
// actions feature and the type's schema held: ActionBar, docs/editor.md#actions), its settings form (with the types and
// node.set features and the type's schema held, docs/editor.md#the-settings-form) or, by the Form/Text toggle, its text
// in knobly (with the node-text feature, docs/editor.md#the-node-panels-text; else its settings as JSON), its counters,
// its companions in words and Delete. Every change is a request to ear; the canvas changes only when ear announces it. A lifecycle button spins
// while its own request is on its way, and a refusal shows under the bar as well as in a toast
// (docs/editor.md#where-a-refusal-lands).
import { useEffect, useState } from "react";
import { describeHealth, type ActionAvailability, type HealthView } from "../health";
import { refusalParts, type RefusalParts } from "../notices";
import {
  actionsFeature,
  dryRunFeature,
  gesturesFeature,
  lifecycleFeature,
  nodeSetFeature,
  nodeTextFeature,
  type GraphNode,
  type TypeSchema,
} from "../protocol";
import { lifecycleAsked, lifecycleLabels, type LifecycleMethod } from "../requests";
import { parseSettings } from "../settings";
import { ActionBar } from "./ActionBar";
import { CompanionList } from "./CompanionStrip";
import { NodeActivity, NodeDiagnostics, NodeStatus } from "./NodeActivity";
import { CounterTable } from "./Counters";
import { useCompanionBadges, useEditor, useGraphFile, useNodeCounters, useRequests } from "./hooks";
import { NodeTextBox } from "./NodeTextBox";
import { SettingsForm } from "./SettingsForm";
import { Spinner } from "./Spinner";
import { StatePill } from "./StatePill";

/** The lifecycle bar's buttons, in order, each enabled by its action in health.ts. */
const lifecycleButtons: { method: LifecycleMethod; action: keyof HealthView["actions"] }[] = [
  { method: "node.start", action: "start" },
  { method: "node.stop", action: "stop" },
  { method: "node.restart", action: "restart" },
  { method: "node.disable", action: "disable" },
  { method: "node.enable", action: "enable" },
];

function LifecycleButton({
  label,
  availability,
  busy,
  onClick,
}: {
  label: string;
  availability: ActionAvailability;
  /** Its own request is on its way. */
  busy: boolean;
  onClick: () => void;
}) {
  return (
    <button
      type="button"
      disabled={!availability.enabled || busy}
      aria-busy={busy || undefined}
      title={availability.why}
      onClick={onClick}
    >
      {busy && <Spinner />}
      {label}
    </button>
  );
}

export function NodePanel({
  node,
  features,
  schema,
  onRemove,
}: {
  node: GraphNode;
  features: string[];
  /** The node's type's schema, when ear has the types feature and the editor holds the set the Graph names. */
  schema?: TypeSchema;
  onRemove: (name: string) => void;
}) {
  const editor = useEditor();
  const asked = lifecycleAsked(useRequests(), node.name);
  // The panel is keyed by the node's name, so another node starts with no refusal shown.
  const [refusal, setRefusal] = useState<{ label: string; parts: RefusalParts } | null>(null);
  const health = describeHealth(node);
  const companions = useCompanionBadges(node);
  const counters = useNodeCounters(node);
  const lifecycle = features.includes(lifecycleFeature);
  const file = useGraphFile();
  // A person edits knobly; JSON is the machine API: the JSON box is only for an ear without node-text.
  const knobly = features.includes(nodeTextFeature) && file !== null;
  // The form needs node.set; the toggle shows the node's text instead (docs/editor.md#the-settings-form).
  const formable = schema !== undefined && features.includes(nodeSetFeature);
  const [asText, setAsText] = useState(false);
  const shown = JSON.stringify(node.settings, null, 2);
  const [settings, setSettings] = useState(shown);
  // A new node selected, or its settings changed on the server: show what the server has.
  useEffect(() => setSettings(shown), [node.name, shown]);

  const save = () => {
    const parsed = parseSettings(settings);
    if (typeof parsed === "string") return editor.report({ method: "node.update", node: node.name }, new Error(parsed));
    editor.send("node.update", { name: node.name, settings: parsed });
  };

  const ask = (method: LifecycleMethod) => {
    setRefusal(null);
    // The toast comes from ask itself; the line under the bar keeps the refusal beside the button that asked.
    editor
      .ask(method, { name: node.name })
      .catch((error: unknown) => setRefusal({ label: lifecycleLabels[method], parts: refusalParts(error) }));
  };

  return (
    <section className="inspector-section">
      {/* The health's style class sets --state, so the pill takes the box's colours. */}
      <header className={`inspector-node ${health.styleClass} phase-${node.phase}`}>
        <h2>
          {node.name} <span className="ear-node-type">{node.type}</span>
        </h2>
        <StatePill node={node} health={health} />
        {health.line !== "" && <p className="inspector-line">{health.line}</p>}
      </header>
      {lifecycle && (
        <div className="lifecycle-buttons" role="group" aria-label="lifecycle">
          {lifecycleButtons.map(({ method, action }) => (
            <LifecycleButton
              key={method}
              label={lifecycleLabels[method]}
              availability={health.actions[action]}
              busy={asked.has(method)}
              onClick={() => ask(method)}
            />
          ))}
        </div>
      )}
      {lifecycle && refusal !== null && (
        // No live role: the toast announces it.
        <p className="lifecycle-refusal">
          {refusal.label} {refusal.parts.verb}: <code>{refusal.parts.code}</code>
          {refusal.parts.detail !== "" && ` ${refusal.parts.detail}`}
        </p>
      )}
      {features.includes(actionsFeature) && schema?.actions !== undefined && (
        <ActionBar node={node} actions={schema.actions} />
      )}
      {formable && (
        <div className="settings-toggle" role="group" aria-label="settings as">
          <button type="button" aria-pressed={!asText} onClick={() => setAsText(false)}>
            Form
          </button>
          <button type="button" aria-pressed={asText} onClick={() => setAsText(true)}>
            {knobly ? "Text" : "JSON"}
          </button>
        </div>
      )}
      {formable && !asText && schema !== undefined ? (
        <SettingsForm
          key={node.name}
          node={node}
          schema={schema}
          dryRun={features.includes(dryRunFeature)}
          gestures={features.includes(gesturesFeature)}
        />
      ) : knobly ? (
        <NodeTextBox key={node.name} node={node.name} file={file.file} />
      ) : (
        <>
          <label>
            Settings (JSON)
            <textarea rows={5} value={settings} onChange={(event) => setSettings(event.target.value)} />
          </label>
          <button type="button" disabled={settings === shown} onClick={save}>
            Save settings
          </button>
        </>
      )}
      <NodeStatus node={node} schema={schema} />
      <CounterTable heading="Counters since it was added" rows={counters.rows} />
      <CompanionList badges={companions} />
      <NodeDiagnostics node={node} />
      <NodeActivity node={node.name} />
      <button type="button" className="inspector-delete" onClick={() => onRemove(node.name)}>
        Delete node
      </button>
    </section>
  );
}
