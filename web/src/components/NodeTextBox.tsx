// The node panel's knobly box (docs/editor.md#the-node-panels-text): the node's own statement as ear.conf has it,
// editable, in the pane's textarea style with its diagnostics under it (a click puts the caret there) and Save
// (node.setText). The logic is nodeText.ts; ear judges every text.
import { useEffect, useReducer, useRef, useState } from "react";
import { boxFrom, boxNote, boxReduce, canSave, saveRequest } from "../nodeText";
import { offsetOf } from "../pane";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { RpcError } from "../rpc";
import { useEditor } from "./hooks";

export function NodeTextBox({
  node,
  file,
  onError,
}: {
  node: string;
  file: GraphFile;
  onError: (error: unknown) => void;
}) {
  const { client } = useEditor();
  const [box, dispatch] = useReducer(boxReduce, undefined, () => boxFrom(file, node));
  const [saving, setSaving] = useState(false);
  const text = useRef<HTMLTextAreaElement>(null);

  useEffect(() => dispatch({ kind: "file", file }), [file]);

  const save = () => {
    setSaving(true);
    client
      .call("node.setText", saveRequest(box))
      .catch((error: unknown) => {
        if (error instanceof RpcError) {
          const refused = { code: error.code as RpcErrorCode, message: error.message, data: error.data };
          dispatch({ kind: "refused", error: refused });
          // The box shows these itself (stale, the diagnostics); any other goes to the error banner.
          const code = error.data?.code;
          if (code === "file-changed" || code === "invalid-text") return;
        }
        onError(error);
      })
      .finally(() => setSaving(false));
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
      <label htmlFor={`node-text-${node}`}>Its text in ear.conf</label>
      {box.stale && (
        <div className="banner banner-connection" role="status">
          {node} changed in ear.conf; reload{" "}
          <button type="button" onClick={() => dispatch({ kind: "reload" })}>
            Reload
          </button>
        </div>
      )}
      {note !== "" && <div className="node-text-note">{note}</div>}
      <textarea
        id={`node-text-${node}`}
        ref={text}
        className="text-pane-text"
        rows={lines}
        spellCheck={false}
        readOnly={box.base.text.kind !== "own"}
        value={box.draft}
        onChange={(event) => dispatch({ kind: "edit", text: event.target.value })}
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
      <button type="button" disabled={saving || !canSave(box)} onClick={save}>
        Save
      </button>
    </div>
  );
}
