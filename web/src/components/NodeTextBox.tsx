// The node panel's knobly box (docs/editor.md#the-node-panels-text): the node's own statement as ear.conf has it,
// editable, in the pane's textarea style with its diagnostics under it (a click puts the caret there) and Save
// (node.setText), which says Saving… while its request is on its way (docs/editor.md#requests-in-flight). The logic is
// nodeText.ts; ear judges every text.
import { useEffect, useReducer, useRef } from "react";
import { placedRefusal } from "../notices";
import { boxFrom, boxNote, boxReduce, canSave, saveRequest } from "../nodeText";
import { offsetOf } from "../pane";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { isAsking } from "../requests";
import { RpcError } from "../rpc";
import { useEditor, useRequests } from "./hooks";
import { KnoblyText } from "./KnoblyText";
import { Spinner } from "@mcurcio/patchbay/primitives";

export function NodeTextBox({ node, file }: { node: string; file: GraphFile }) {
  const editor = useEditor();
  const [box, dispatch] = useReducer(boxReduce, undefined, () => boxFrom(file, node));
  const saving = isAsking(useRequests(), "node.setText", node);
  const text = useRef<HTMLTextAreaElement>(null);

  useEffect(() => dispatch({ kind: "file", file }), [file]);

  // The box shows file-changed and invalid-text itself (stale, the diagnostics); any other is a toast
  // (docs/editor.md#where-a-refusal-lands).
  const save = () => {
    editor.ask("node.setText", saveRequest(box), { placed: placedRefusal }).catch((error: unknown) => {
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

  const note = boxNote(box);
  const lines = Math.min(12, Math.max(3, box.draft.split("\n").length + 1));

  return (
    <div className="node-text">
      <label htmlFor={`node-text-${node}`}>
        Its text in ear.conf{box.base.text.kind !== "own" && " (read-only)"}
      </label>
      {box.stale && (
        <div className="banner banner-warn" role="status">
          {node} changed in ear.conf; reload{" "}
          <button type="button" onClick={() => dispatch({ kind: "reload" })}>
            Reload
          </button>
        </div>
      )}
      {note !== "" && <div className="node-text-note">{note}</div>}
      <KnoblyText
        id={`node-text-${node}`}
        ref={text}
        rows={lines}
        readOnly={box.base.text.kind !== "own"}
        value={box.draft}
        onEdit={(draft) => dispatch({ kind: "edit", text: draft })}
      />
      {box.errors.length > 0 && (
        <ul className="diagnostics">
          {box.errors.map((diagnostic, index) => (
            <li key={index} className={`diagnostic diagnostic-${diagnostic.severity}`}>
              <button type="button" onClick={() => goTo(diagnostic.line, diagnostic.column)}>
                {diagnostic.line}:{diagnostic.column} {diagnostic.code} {diagnostic.message}
              </button>
            </li>
          ))}
        </ul>
      )}
      <button type="button" disabled={saving || !canSave(box)} aria-busy={saving || undefined} onClick={save}>
        {saving && <Spinner />}
        {saving ? "Saving…" : "Save"}
      </button>
    </div>
  );
}
