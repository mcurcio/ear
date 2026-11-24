// The ear.conf drawer's body (docs/editor.md#the-drawer): banners for what a person must know (the file changed under
// the draft, the graph refused the file, a write failed, editor edits were dropped, a setting applies only at
// restart), the file's text, editable, and its diagnostics (a click puts the caret there). The pane's state and Save
// are useDrawer's, so the drawer's header and the top bar read the same status; the logic is pane.ts and drawer.ts,
// and ear judges every text.
import { useRef } from "react";
import { formatNames } from "../drawer";
import type { FileNotice } from "../graphFile";
import { offsetOf, shownDiagnostics } from "../pane";
import { useEditor } from "./hooks";
import { KnoblyText } from "./KnoblyText";
import type { DrawerModel } from "./useDrawer";

function place(line: number, column: number): string {
  return `${line}:${column}`;
}

function noticeText(notice: FileNotice): string {
  const edits = notice.count === 1 ? "1 editor edit was" : `${notice.count} editor edits were`;
  return `${edits} not saved: ear.conf changed on disk`;
}

export function TextPane({ drawer }: { drawer: DrawerModel }) {
  const { file: store } = useEditor();
  const { state, pane, dispatch } = drawer;
  const { file, notices } = state;
  const text = useRef<HTMLTextAreaElement>(null);

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
    <div className="text-pane">
      {pane.stale && (
        <div className="banner banner-warn" role="status">
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
        <div className="banner banner-warn" role="status">
          {pending.join(" and ")} change applies at restart
        </div>
      )}
      <KnoblyText
        ref={text}
        rows={20}
        value={pane.draft}
        onEdit={(draft) => dispatch({ kind: "edit", text: draft })}
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
    </div>
  );
}
