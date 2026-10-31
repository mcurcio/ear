// The ear.conf pane's logic, pure (docs/design/knobly-config.md, Contracts: Editor). The draft is
// the person's text; the base is the file it was based on, whose version Save sends, so ear can
// refuse a Save over a file that changed meanwhile (file-changed). A new file while the draft has
// unsaved changes never overwrites them: the pane goes stale ("ear.conf changed; reload", VS
// Code's pattern) until the person reloads.
import type { Diagnostic, GraphFile, RpcErrorObject } from "./protocol";

export interface PaneState {
  draft: string;
  /** The file the draft was based on; Save sends its version. */
  base: GraphFile;
  /** The newest file ear announced; reload takes it. The same as base unless stale. */
  latest: GraphFile;
  /** ear.conf changed under a dirty draft. */
  stale: boolean;
  /** The diagnostics of the last Save ear refused as invalid-text. */
  errors: Diagnostic[];
}

export type PaneEvent =
  | { kind: "file"; file: GraphFile } // a new GraphFile arrived
  | { kind: "edit"; text: string }
  | { kind: "reload" } // take the newest text, drop the draft
  | { kind: "refused"; error: RpcErrorObject }; // graphFile.set's error

/** A pane showing `file`, with nothing typed yet. */
export function paneFrom(file: GraphFile): PaneState {
  return { draft: file.text, base: file, latest: file, stale: false, errors: [] };
}

/** True when the draft differs from the base's text. */
export function dirty(state: PaneState): boolean {
  return state.draft !== state.base.text;
}

function onFile(state: PaneState, file: GraphFile): PaneState {
  // The same text (a write's state, the running version): nothing to reconcile.
  if (file.text === state.base.text) return { ...state, base: file, latest: file, stale: false };
  // ear now holds what the draft says (this pane's own Save, or the same edit elsewhere).
  if (file.text === state.draft) return paneFrom(file);
  if (!dirty(state)) return paneFrom(file);
  return { ...state, latest: file, stale: true };
}

export function paneReduce(state: PaneState, event: PaneEvent): PaneState {
  switch (event.kind) {
    case "file":
      return onFile(state, event.file);
    case "edit":
      return { ...state, draft: event.text };
    case "reload":
      return paneFrom(state.latest);
    case "refused": {
      const data = event.error.data;
      if (data?.code === "file-changed") return { ...state, stale: true };
      if (data?.code === "invalid-text") return { ...state, errors: data.diagnostics ?? [] };
      return state; // a refusal the pane has no place for; the caller shows it
    }
  }
}

/** graphFile.set's params: the draft and the base version. */
export function saveRequest(state: PaneState): { text: string; version: string } {
  return { text: state.draft, version: state.base.version };
}

/** The diagnostics to list: those of the last refused Save first, else the base file's. */
export function shownDiagnostics(state: PaneState): Diagnostic[] {
  return state.errors.length > 0 ? state.errors : state.base.diagnostics;
}

/**
 * The text offset of line `line`, column `column` (both from 1, columns in UTF-16 code units, as
 * the wire gives them), clamped to the text; where a click on a diagnostic puts the caret.
 */
export function offsetOf(text: string, line: number, column: number): number {
  let offset = 0;
  for (let at = 1; at < line; at++) {
    const newline = text.indexOf("\n", offset);
    if (newline < 0) return text.length;
    offset = newline + 1;
  }
  const end = text.indexOf("\n", offset);
  const lineEnd = end < 0 ? text.length : end;
  return Math.min(offset + Math.max(0, column - 1), lineEnd);
}
