// The inspector's Node subject (docs/editor.md#the-inspector): the node's name and type, its state pill as the box
// draws it and its line (health.ts, docs/editor.md#a-nodes-health), the lifecycle bar (Start, Stop, Restart, Disable
// and Enable when ear has the lifecycle feature, each enabled by health.ts), its text in knobly (with the node-text
// feature, docs/editor.md#the-node-panels-text; else its settings as JSON), its counters, its companions in words and
// Delete. Every change is a request to ear; the canvas changes only when ear announces it.
import { useEffect, useState } from "react";
import { describeHealth, type ActionAvailability, type HealthView } from "../health";
import { lifecycleFeature, nodeTextFeature, type GraphNode } from "../protocol";
import { parseSettings } from "../settings";
import { CompanionList } from "./CompanionStrip";
import { CounterTable } from "./Counters";
import { useCompanionBadges, useEditor, useGraphFile, useNodeCounters } from "./hooks";
import { NodeTextBox } from "./NodeTextBox";
import { StatePill } from "./StatePill";

type LifecycleMethod = "node.start" | "node.stop" | "node.restart" | "node.disable" | "node.enable";

/** The lifecycle bar's buttons, in order, each enabled by its action in health.ts. */
const lifecycleButtons: { label: string; method: LifecycleMethod; action: keyof HealthView["actions"] }[] = [
  { label: "Start", method: "node.start", action: "start" },
  { label: "Stop", method: "node.stop", action: "stop" },
  { label: "Restart", method: "node.restart", action: "restart" },
  { label: "Disable", method: "node.disable", action: "disable" },
  { label: "Enable", method: "node.enable", action: "enable" },
];

function LifecycleButton({
  label,
  method,
  node,
  availability,
  onError,
}: {
  label: string;
  method: LifecycleMethod;
  node: string;
  availability: ActionAvailability;
  onError: (error: unknown) => void;
}) {
  const { client } = useEditor();
  return (
    <button
      type="button"
      disabled={!availability.enabled}
      title={availability.why}
      onClick={() => client.call(method, { name: node }).catch(onError)}
    >
      {label}
    </button>
  );
}

export function NodePanel({
  node,
  features,
  onRemove,
  onError,
}: {
  node: GraphNode;
  features: string[];
  onRemove: (name: string) => void;
  onError: (error: unknown) => void;
}) {
  const { client } = useEditor();
  const health = describeHealth(node);
  const companions = useCompanionBadges(node);
  const counters = useNodeCounters(node);
  const lifecycle = features.includes(lifecycleFeature);
  const file = useGraphFile();
  // A person edits knobly; JSON is the machine API: the JSON box is only for an ear without node-text.
  const knobly = features.includes(nodeTextFeature) && file !== null;
  const shown = JSON.stringify(node.settings, null, 2);
  const [settings, setSettings] = useState(shown);
  // A new node selected, or its settings changed on the server: show what the server has.
  useEffect(() => setSettings(shown), [node.name, shown]);

  const save = () => {
    const parsed = parseSettings(settings);
    if (typeof parsed === "string") return onError(new Error(parsed));
    client.call("node.update", { name: node.name, settings: parsed }).catch(onError);
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
          {lifecycleButtons.map(({ label, method, action }) => (
            <LifecycleButton
              key={method}
              label={label}
              method={method}
              node={node.name}
              availability={health.actions[action]}
              onError={onError}
            />
          ))}
        </div>
      )}
      {/* Slice 5's settings form and status list go here, under the lifecycle bar (docs/editor.md#the-inspector). */}
      {knobly ? (
        <NodeTextBox key={node.name} node={node.name} file={file.file} onError={onError} />
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
      <CounterTable heading="Counters since it was added" rows={counters.rows} />
      <CompanionList badges={companions} />
      <button type="button" className="inspector-delete" onClick={() => onRemove(node.name)}>
        Delete node
      </button>
    </section>
  );
}
