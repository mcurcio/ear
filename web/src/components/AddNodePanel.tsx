// The inspector's add-node form (docs/editor.md#adding-a-node), in its Graph subject: a node's statement in knobly with
// the node-text feature (AddNodeText), else a type list and JSON settings for an ear without it. Every change is a
// request to ear; the canvas changes only when ear announces it.
import { useState } from "react";
import { nodeTextFeature } from "../protocol";
import { parseSettings } from "../settings";
import { AddNodeText } from "./AddNodeText";
import { useEditor, useGraphFile } from "./hooks";

export function AddNodePanel({
  types,
  names,
  features,
  onError,
}: {
  types: string[];
  names: string[];
  features: string[];
  onError: (error: unknown) => void;
}) {
  const file = useGraphFile();
  // A person edits knobly; JSON is the machine API: the JSON form is only for an ear without node-text.
  if (features.includes(nodeTextFeature) && file !== null)
    return <AddNodeText types={types} names={names} file={file.file} onError={onError} />;
  return <AddNodeJson types={types} onError={onError} />;
}

function AddNodeJson({ types, onError }: { types: string[]; onError: (error: unknown) => void }) {
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
    <section className="inspector-section">
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
