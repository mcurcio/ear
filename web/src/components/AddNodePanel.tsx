// The inspector's add-node form (docs/editor.md#adding-a-node), in its Graph subject: a node's statement in knobly with
// the node-text feature (AddNodeText), else a type list and JSON settings for an ear without it. Every change is a
// request to ear; the canvas changes only when ear announces it, and Add says Adding… while its request is on its way
// (docs/editor.md#requests-in-flight).
import { useEffect, useRef, useState } from "react";
import { nodeTextFeature } from "../protocol";
import { isAsking } from "../requests";
import { parseSettings } from "../settings";
import { AddNodeText } from "./AddNodeText";
import { useEditor, useGraphFile, useRequests } from "./hooks";
import type { AddIntent } from "./Inspector";
import { Spinner } from "./Spinner";

export function AddNodePanel({
  types,
  names,
  features,
  intent,
}: {
  types: string[];
  names: string[];
  features: string[];
  /** An Add command's type (docs/editor.md#the-palette): chosen, and the form focused. */
  intent: AddIntent | null;
}) {
  const file = useGraphFile();
  // A person edits knobly; JSON is the machine API: the JSON form is only for an ear without node-text.
  if (features.includes(nodeTextFeature) && file !== null)
    return <AddNodeText types={types} names={names} file={file.file} intent={intent} />;
  return <AddNodeJson types={types} intent={intent} />;
}

function AddNodeJson({ types, intent }: { types: string[]; intent: AddIntent | null }) {
  const editor = useEditor();
  const adding = isAsking(useRequests(), "node.add");
  const [name, setName] = useState("");
  const [type, setType] = useState(intent?.type ?? "");
  const nameInput = useRef<HTMLInputElement>(null);
  // The form has no template, so the name is left for the person to type.
  useEffect(() => {
    if (intent === null) return;
    setType(intent.type);
    nameInput.current?.focus();
  }, [intent?.nonce]);
  const [settings, setSettings] = useState("{}");
  const chosenType = types.includes(type) ? type : (types[0] ?? "");

  const add = () => {
    const parsed = parseSettings(settings);
    if (typeof parsed === "string") return editor.report({ method: "node.add", node: name }, new Error(parsed));
    // A refusal is a toast (ask's own); the name is kept to fix.
    editor
      .ask("node.add", { name, type: chosenType, settings: parsed })
      .then(() => setName(""))
      .catch(() => {});
  };

  return (
    <section className="inspector-section">
      <h2>Add a node</h2>
      <label>
        Name <input ref={nameInput} value={name} onChange={(event) => setName(event.target.value)} />
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
      <button
        type="button"
        disabled={adding || name === "" || chosenType === ""}
        aria-busy={adding || undefined}
        onClick={add}
      >
        {adding && <Spinner />}
        {adding ? "Adding…" : "Add"}
      </button>
    </section>
  );
}
