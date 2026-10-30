// The side panel: the engine facts (with the "not realtime" badge), the add-node form (type
// list, JSON settings) and the selected node's settings and delete button. Every change is a
// request to ear; the canvas changes only when ear announces it.
import { useEffect, useState } from "react";
import type { Graph, GraphNode, JsonObject } from "../protocol";
import { useEditor } from "./hooks";

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

export function NodePanel({ node, onError }: { node: GraphNode; onError: (error: unknown) => void }) {
  const { client } = useEditor();
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
      <div>
        {node.state}
        {node.reason !== undefined && ` (${node.reason.code}): ${node.reason.detail}`}
      </div>
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
