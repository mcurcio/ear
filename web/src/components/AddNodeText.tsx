// The add-node form in knobly (docs/editor.md#adding-a-node): a type list that writes a template, the statement in the
// pane's textarea style, its diagnostics under it (a click puts the caret there) and Add (node.addText). The logic is
// addText.ts; ear judges every text.
import { useEffect, useReducer, useRef, useState } from "react";
import { addFrom, addReduce, addRequest, canAdd } from "../addText";
import { offsetOf } from "../pane";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { RpcError } from "../rpc";
import { useEditor } from "./hooks";

export function AddNodeText({
  types,
  names,
  file,
  onError,
}: {
  types: string[];
  names: string[];
  file: GraphFile;
  onError: (error: unknown) => void;
}) {
  const { client } = useEditor();
  const [type, setType] = useState("");
  const chosenType = types.includes(type) ? type : (types[0] ?? "");
  const [form, dispatch] = useReducer(addReduce, undefined, () => addFrom(chosenType, names));
  const [adding, setAdding] = useState(false);
  const text = useRef<HTMLTextAreaElement>(null);
  const namesKey = names.join("\n");

  // A type chosen, or the nodes changed (a name taken): the template follows while it is untouched.
  useEffect(() => dispatch({ kind: "type", type: chosenType, names }), [chosenType, namesKey]);

  const add = () => {
    setAdding(true);
    client
      .call("node.addText", addRequest(form, file))
      .then(() => dispatch({ kind: "added", type: chosenType, names }))
      .catch((error: unknown) => {
        if (error instanceof RpcError) {
          const refused = { code: error.code as RpcErrorCode, message: error.message, data: error.data };
          dispatch({ kind: "refused", error: refused });
          // The form shows these itself (the diagnostics, Add again); any other goes to the error banner.
          const code = error.data?.code;
          if (code === "file-changed" || code === "invalid-text") return;
        }
        onError(error);
      })
      .finally(() => setAdding(false));
  };

  const goTo = (line: number, column: number) => {
    const area = text.current;
    if (area === null) return;
    const offset = offsetOf(area.value, line, column);
    area.focus();
    area.setSelectionRange(offset, offset);
  };

  const lines = Math.min(12, Math.max(3, form.text.split("\n").length + 1));

  return (
    <section className="panel-section node-text">
      <h2>Add a node</h2>
      <label>
        Type{" "}
        <select value={chosenType} onChange={(event) => setType(event.target.value)}>
          {types.map((t) => (
            <option key={t}>{t}</option>
          ))}
        </select>
      </label>
      <label htmlFor="add-node-text">Its text in ear.conf</label>
      {form.raced && (
        <div className="banner banner-connection" role="status">
          ear.conf changed while adding; press Add again
        </div>
      )}
      <textarea
        id="add-node-text"
        ref={text}
        className="text-pane-text"
        rows={lines}
        spellCheck={false}
        value={form.text}
        onChange={(event) => dispatch({ kind: "edit", text: event.target.value })}
      />
      {form.errors.length > 0 && (
        <ul className="diagnostics">
          {form.errors.map((diagnostic, index) => (
            <li key={index} className={`diagnostic diagnostic-${diagnostic.severity}`}>
              <button type="button" onClick={() => goTo(diagnostic.line, diagnostic.column)}>
                {diagnostic.line}:{diagnostic.column} {diagnostic.code} {diagnostic.message}
              </button>
            </li>
          ))}
        </ul>
      )}
      <button type="button" disabled={adding || !canAdd(form)} onClick={add}>
        Add
      </button>
    </section>
  );
}
