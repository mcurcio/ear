// The add-node form in knobly (docs/editor.md#adding-a-node): a type list that writes a template, the statement in the
// pane's textarea style, its diagnostics under it (a click puts the caret there) and Add (node.addText). The logic is
// addText.ts; ear judges every text. Add says Adding… while its request is on its way
// (docs/editor.md#requests-in-flight).
import { Banner } from "@mcurcio/patchbay/composites";
import { Button, Field, Select } from "@mcurcio/patchbay/primitives";
import { useEffect, useReducer, useRef, useState } from "react";
import { addFrom, addReduce, addRequest, canAdd } from "../addText";
import { placedRefusal } from "../notices";
import { offsetOf } from "../pane";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { isAsking } from "../requests";
import { RpcError } from "../rpc";
import { useEditor, useRequests } from "./hooks";
import type { AddIntent } from "./Inspector";
import { KnoblyText } from "./KnoblyText";

export function AddNodeText({
  types,
  names,
  file,
  intent,
}: {
  types: string[];
  names: string[];
  file: GraphFile;
  /** An Add command's type (docs/editor.md#the-palette): chosen, so its template follows, and the text focused. */
  intent: AddIntent | null;
}) {
  const editor = useEditor();
  const [type, setType] = useState(intent?.type ?? "");
  const chosenType = types.includes(type) ? type : (types[0] ?? "");
  const [form, dispatch] = useReducer(addReduce, undefined, () => addFrom(chosenType, names));
  const adding = isAsking(useRequests(), "node.addText");
  const text = useRef<HTMLTextAreaElement>(null);
  const namesKey = names.join("\n");

  // A type chosen, or the nodes changed (a name taken): the template follows while it is untouched.
  useEffect(() => dispatch({ kind: "type", type: chosenType, names }), [chosenType, namesKey]);

  // An Add command: its type chosen (the template follows only while the text is untouched, so a typed draft is kept),
  // then, once that text is drawn, the caret at the end of the first line, where the name is.
  const [focusFor, setFocusFor] = useState<number | null>(null);
  useEffect(() => {
    if (intent === null) return;
    setType(intent.type);
    dispatch({ kind: "type", type: intent.type, names });
    setFocusFor(intent.nonce);
  }, [intent?.nonce]);
  useEffect(() => {
    const area = text.current;
    if (focusFor === null || area === null) return;
    area.focus();
    const end = area.value.indexOf("\n");
    const at = end < 0 ? area.value.length : end;
    area.setSelectionRange(at, at);
  }, [focusFor]);

  // The form shows file-changed and invalid-text itself (the diagnostics, Add again); any other is a toast
  // (docs/editor.md#toasts).
  const add = () => {
    editor
      .ask("node.addText", addRequest(form, file), { placed: placedRefusal })
      .then(() => dispatch({ kind: "added", type: chosenType, names }))
      .catch((error: unknown) => {
        if (!(error instanceof RpcError)) return;
        const refused = { code: error.code as RpcErrorCode, message: error.message, data: error.data };
        dispatch({ kind: "refused", error: refused });
      });
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
    <section className="inspector-section node-text">
      <h2>Add a node</h2>
      <Field label="Type">
        <Select font="code" value={chosenType} onChange={(event) => setType(event.target.value)}>
          {types.map((t) => (
            <option key={t}>{t}</option>
          ))}
        </Select>
      </Field>
      <label htmlFor="add-node-text">Its text in ear.conf</label>
      {form.raced && (
        <Banner tone="warn">ear.conf changed while adding; press Add again</Banner>
      )}
      <KnoblyText
        id="add-node-text"
        ref={text}
        rows={lines}
        value={form.text}
        onEdit={(typed) => dispatch({ kind: "edit", text: typed })}
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
      <Button busy={adding} busyLabel="Adding…" disabled={!adding && !canAdd(form)} onClick={add}>
        Add
      </Button>
    </section>
  );
}
