// The node panel's knobly box (docs/editor.md#the-node-panels-text): the node's own statement as ear.conf has it,
// editable, in patchbay's CodeEditor as the pane's is, with its diagnostics under it in patchbay's
// DiagnosticList (a row's press puts the caret there)
// and Save (node.setText), which says Saving… while its request is on its way
// (docs/editor.md#requests-in-flight). The logic is nodeText.ts; ear judges every text.
import { Banner, CodeEditor, DiagnosticList } from "@mcurcio/patchbay/composites";
import { Button } from "@mcurcio/patchbay/primitives";
import { useEffect, useReducer, useRef } from "react";
import { placedRefusal } from "../notices";
import { boxFrom, boxNote, boxReduce, canSave, saveRequest } from "../nodeText";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { isAsking } from "../requests";
import { RpcError } from "../rpc";
import { useEditor, useRequests } from "./hooks";

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


  const note = boxNote(box);
  const lines = Math.min(12, Math.max(3, box.draft.split("\n").length + 1));

  return (
    <div className="node-text">
      <label htmlFor={`node-text-${node}`}>
        Its text in ear.conf{box.base.text.kind !== "own" && " (read-only)"}
      </label>
      {box.stale && (
        <Banner
          tone="warn"
          actions={
            <Button size="sm" onClick={() => dispatch({ kind: "reload" })}>
              Reload
            </Button>
          }
        >
          {node} changed in ear.conf; reload
        </Banner>
      )}
      {note !== "" && <div className="node-text-note">{note}</div>}
      <CodeEditor
        id={`node-text-${node}`}
        ref={text}
        rows={lines}
        language="knobly"
        readOnly={box.base.text.kind !== "own"}
        value={box.draft}
        onChange={(draft) => dispatch({ kind: "edit", text: draft })}
        diagnostics={box.errors}
      />
      <DiagnosticList diagnostics={box.errors} editor={text} />
      <Button busy={saving} busyLabel="Saving…" disabled={!saving && !canSave(box)} onClick={save}>
        Save
      </Button>
    </div>
  );
}
