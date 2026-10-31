// The ear.conf pane (docs/design/knobly-config.md, Contracts: Editor): the file's text, editable,
// its diagnostics (a click puts the caret there), Save, and banners for what a person must know:
// the file changed under the draft, the graph refused the file, a write failed, editor edits were
// dropped, a setting applies only at restart. The logic is pane.ts; ear judges every text.
import { useEffect, useReducer, useRef, useState } from "react";
import type { FileNotice, GraphFileState } from "../graphFile";
import { dirty, offsetOf, paneFrom, paneReduce, saveRequest, shownDiagnostics } from "../pane";
import type { FormatKey, RpcErrorCode } from "../protocol";
import { RpcError } from "../rpc";
import { useEditor } from "./hooks";

const formatNames: { [K in FormatKey]: string } = { graphFrequency: "graph frequency", blockSize: "block size" };

function place(line: number, column: number): string {
  return `${line}:${column}`;
}

function noticeText(notice: FileNotice): string {
  const edits = notice.count === 1 ? "1 editor edit was" : `${notice.count} editor edits were`;
  return `${edits} not saved: ear.conf changed on disk`;
}

export function TextPane({ state, onError }: { state: GraphFileState; onError: (error: unknown) => void }) {
  const { client, file: store } = useEditor();
  const { file, notices } = state;
  const [pane, dispatch] = useReducer(paneReduce, file, paneFrom);
  const [saving, setSaving] = useState(false);
  const text = useRef<HTMLTextAreaElement>(null);

  useEffect(() => dispatch({ kind: "file", file }), [file]);

  const save = () => {
    setSaving(true);
    client
      .call("graphFile.set", saveRequest(pane))
      .catch((error: unknown) => {
        if (error instanceof RpcError) {
          const refused = { code: error.code as RpcErrorCode, message: error.message, data: error.data };
          dispatch({ kind: "refused", error: refused });
          // The pane shows these itself (stale, the diagnostics); any other goes to the error banner.
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

  const diagnostics = shownDiagnostics(pane);
  const pending = file.pendingRestart.map((key) => formatNames[key]);

  return (
    <section className="panel-section text-pane">
      <h2>ear.conf</h2>
      {pane.stale && (
        <div className="banner banner-connection" role="status">
          ear.conf changed; reload{" "}
          <button type="button" onClick={() => dispatch({ kind: "reload" })}>
            Reload
          </button>
        </div>
      )}
      {file.refusal !== undefined && (
        <div className="banner banner-error" role="alert">
          ear refused this file
          {file.refusal.line !== undefined && ` at ${place(file.refusal.line, file.refusal.column ?? 1)}`}:{" "}
          {file.refusal.message}. The graph keeps running{" "}
          {file.running !== undefined ? `version ${file.running}` : "nothing"}.
        </div>
      )}
      {file.writeFailed !== undefined && (
        <div className="banner banner-error" role="alert">
          ear could not write ear.conf: {file.writeFailed}
        </div>
      )}
      {notices.map((notice, index) => (
        <div key={index} className="banner banner-error" role="alert">
          {noticeText(notice)}{" "}
          <button type="button" onClick={() => store.dismiss(notice)}>
            Dismiss
          </button>
        </div>
      ))}
      {pending.length > 0 && (
        <div className="banner banner-connection" role="status">
          {pending.join(" and ")} change applies at restart
        </div>
      )}
      <textarea
        ref={text}
        className="text-pane-text"
        rows={20}
        spellCheck={false}
        value={pane.draft}
        onChange={(event) => dispatch({ kind: "edit", text: event.target.value })}
      />
      {diagnostics.length > 0 && (
        <ul className="diagnostics">
          {diagnostics.map((diagnostic, index) => (
            <li key={index} className={`diagnostic diagnostic-${diagnostic.severity}`}>
              <button type="button" onClick={() => goTo(diagnostic.line, diagnostic.column)}>
                {place(diagnostic.line, diagnostic.column)} {diagnostic.code} {diagnostic.message}
              </button>
            </li>
          ))}
        </ul>
      )}
      <button type="button" disabled={saving || pane.stale || !dirty(pane)} onClick={save}>
        Save
      </button>
    </section>
  );
}
