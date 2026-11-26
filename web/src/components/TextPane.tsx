// The ear.conf drawer's body (docs/editor.md#the-drawer): patchbay's Banners for what a person must know (the file changed under
// the draft, the graph refused the file, a write failed, editor edits were dropped, a setting applies only at
// restart), the file's text, editable, in patchbay's CodeEditor (docs/editor.md#highlighting), its gutter marking each
// line a diagnostic names, and its diagnostics listed (a click puts the caret there). The pane's state and Save
// are useDrawer's, so the drawer's header and the top bar read the same status; the logic is pane.ts and drawer.ts,
// and ear judges every text.
import { Banner, CodeEditor } from "@mcurcio/patchbay/composites";
import { Button } from "@mcurcio/patchbay/primitives";
import { useRef } from "react";
import { formatNames } from "../drawer";
import type { FileNotice } from "../graphFile";
import { offsetOf, shownDiagnostics } from "../pane";
import { useEditor } from "./hooks";
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

  // Marked in the gutter and read with the text, and listed under it, where a click puts the caret.
  const diagnostics = shownDiagnostics(pane);
  const pending = file.pendingRestart.map((key) => formatNames[key]);

  return (
    <div className="text-pane">
      {pane.stale && (
        <Banner
          tone="warn"
          actions={
            <Button size="sm" onClick={() => dispatch({ kind: "reload" })}>
              Reload
            </Button>
          }
        >
          ear.conf changed; reload
        </Banner>
      )}
      {file.refusal !== undefined && (
        <Banner tone="err">
          ear refused this file
          {file.refusal.line !== undefined && ` at ${place(file.refusal.line, file.refusal.column ?? 1)}`}:{" "}
          {file.refusal.message}. The graph keeps running{" "}
          {file.running !== undefined ? `version ${file.running}` : "nothing"}.
        </Banner>
      )}
      {file.writeFailed !== undefined && <Banner tone="err">ear could not write ear.conf: {file.writeFailed}</Banner>}
      {notices.map((notice, index) => (
        <Banner key={index} tone="err" onDismiss={() => store.dismiss(notice)}>
          {noticeText(notice)}
        </Banner>
      ))}
      {pending.length > 0 && <Banner tone="warn">{pending.join(" and ")} change applies at restart</Banner>}
      <CodeEditor
        ref={text}
        className="text-pane-code"
        aria-label="ear.conf"
        language="knobly"
        value={pane.draft}
        onChange={(draft) => dispatch({ kind: "edit", text: draft })}
        diagnostics={diagnostics}
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
