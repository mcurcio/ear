// The side panel: the engine facts (with the "not realtime" badge), the add-node form (type
// list, JSON settings) and the selected node's health, its companions in words, its Start, Stop, Restart, Disable and Enable
// (when ear has the lifecycle feature; each enabled by health.ts, docs/editor.md#a-nodes-health), its settings
// and delete button. Every change is a request to ear; the canvas changes only when ear announces it.
import { useEffect, useState } from "react";
import { describeHealth, type ActionAvailability, type HealthView } from "../health";
import { lifecycleFeature, type Graph, type GraphNode, type JsonObject } from "../protocol";
import { CompanionList } from "./CompanionStrip";
import { useCompanionBadges, useEditor } from "./hooks";

/** The newest protocol this editor knows (docs/control-port.md#protocol-and-features): a higher one shows a banner. */
const knownProtocol = 1;

/** Settings typed as JSON text: the object, or the reason it is not one. */
function parseSettings(text: string): JsonObject | string {
  if (text.trim() === "") return {};
  try {
    const value: unknown = JSON.parse(text);
    if (typeof value === "object" && value !== null && !Array.isArray(value)) return value as JsonObject;
    return "settings must be a JSON object";
  } catch (error) {
    return `settings are not JSON: ${(error as Error).message}`;
  }
}

export function EngineFacts({ graph }: { graph: Graph }) {
  return (
    <section className="panel-section">
      <div>
        {graph.graphFrequency} Hz, {graph.blockSize} frames
      </div>
      <div title="how ear's audio thread is scheduled">
        {graph.scheduling}{" "}
        {!graph.realtime && <span className="badge-not-realtime">not realtime</span>}
      </div>
      {(graph.protocol ?? 0) > knownProtocol && (
        <div className="banner banner-protocol">
          ear speaks protocol {graph.protocol}, newer than this editor's {knownProtocol}: some of it is not shown.
        </div>
      )}
    </section>
  );
}

export function AddNodePanel({ types, onError }: { types: string[]; onError: (error: unknown) => void }) {
  const { client } = useEditor();
  const [name, setName] = useState("");
  const [type, setType] = useState("");
  const [settings, setSettings] = useState("{}");
  const chosenType = types.includes(type) ? type : (types[0] ?? "");

  const add = () => {
    const parsed = parseSettings(settings);
    if (typeof parsed === "string") return onError(new Error(parsed));
    client
      .call("node.add", { name, type: chosenType, settings: parsed })
      .then(() => setName(""))
      .catch(onError);
  };

  return (
    <section className="panel-section">
      <h2>Add a node</h2>
      <label>
        Name <input value={name} onChange={(event) => setName(event.target.value)} />
      </label>
      <label>
        Type{" "}
        <select value={chosenType} onChange={(event) => setType(event.target.value)}>
          {types.map((t) => (
            <option key={t}>{t}</option>
          ))}
        </select>
      </label>
      <label>
        Settings (JSON)
        <textarea rows={3} value={settings} onChange={(event) => setSettings(event.target.value)} />
      </label>
      <button type="button" disabled={name === "" || chosenType === ""} onClick={add}>
        Add
      </button>
    </section>
  );
}

type LifecycleMethod = "node.start" | "node.stop" | "node.restart" | "node.disable" | "node.enable";

/** The node panel's lifecycle buttons, in order, each enabled by its action in health.ts. */
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
  onError,
}: {
  node: GraphNode;
  features: string[];
  onError: (error: unknown) => void;
}) {
  const { client } = useEditor();
  const health = describeHealth(node);
  const companions = useCompanionBadges(node);
  const lifecycle = features.includes(lifecycleFeature);
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
    <section className="panel-section">
      <h2>
        {node.name} <span className="ear-node-type">{node.type}</span>
      </h2>
      <div className={health.styleClass}>
        {health.word}
        {node.reason !== undefined && ` (${node.reason.code})`}
        {health.line !== "" && `: ${health.line}`}
      </div>
      {lifecycle && (
        <div className="lifecycle-buttons">
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
      <CompanionList badges={companions} />
      <label>
        Settings (JSON)
        <textarea rows={5} value={settings} onChange={(event) => setSettings(event.target.value)} />
      </label>
      <button type="button" disabled={settings === shown} onClick={save}>
        Save settings
      </button>{" "}
      <button type="button" onClick={() => client.call("node.remove", { name: node.name }).catch(onError)}>
        Delete node
      </button>
    </section>
  );
}
