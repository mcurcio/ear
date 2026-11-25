// The ear.conf drawer's logic, pure (docs/editor.md#the-drawer): what its always-visible header says the file needs,
// in words, most urgent first; when it opens by itself (only on a refusal or a write failure the person has not been
// shown, never on a hand edit); and the pane's reducer over null, before ear has sent the first file.
import { dirty, paneFrom, paneReduce, shownDiagnostics, type PaneEvent, type PaneState } from "./pane";
import type { FormatKey, GraphFile, Refusal } from "./protocol";
import type { Tone } from "./shell";

/** The wire's keys named as ear.conf's graph block writes them (graph { rate ...; block_size ...; }). */
export const formatNames: Record<FormatKey, string> = { graphFrequency: "graph rate", blockSize: "block size" };

export type DrawerWordKey = "refused" | "write-failed" | "stale" | "unsaved" | "errors" | "restart";

export interface DrawerWord {
  key: DrawerWordKey;
  text: string;
  tone: Tone;
  title: string;
}

function place(line: number | undefined, column: number | undefined): string {
  return line === undefined ? "" : ` at ${line}:${column ?? 1}`;
}

/** What the file needs, most urgent first; a clean file gives []. */
export function drawerStatus(file: GraphFile, pane: PaneState): DrawerWord[] {
  const words: DrawerWord[] = [];
  if (file.refusal !== undefined) {
    const { message, line, column } = file.refusal;
    const title = `ear refused this file${place(line, column)}: ${message}`;
    words.push({ key: "refused", text: "refused", tone: "err", title });
  }
  if (file.writeFailed !== undefined) {
    words.push({
      key: "write-failed",
      text: "write failed",
      tone: "err",
      title: `ear could not write ear.conf: ${file.writeFailed}`,
    });
  }
  if (pane.stale) {
    const title = "ear.conf changed under the draft; reload";
    words.push({ key: "stale", text: "stale: reload", tone: "warn", title });
  } else if (dirty(pane)) {
    words.push({ key: "unsaved", text: "unsaved", tone: "warn", title: "the draft has changes ear has not been sent" });
  }
  const errors = shownDiagnostics(pane).filter((diagnostic) => diagnostic.severity === "error").length;
  if (errors > 0) {
    const text = errors === 1 ? "1 error" : `${errors} errors`;
    words.push({ key: "errors", text, tone: "err", title: `${text} in ear.conf, listed under the text` });
  }
  if (file.pendingRestart.length > 0) {
    const names = file.pendingRestart.map((key) => formatNames[key]).join(" and ");
    const title = `${names} change applies at restart`;
    words.push({ key: "restart", text: "applies at restart", tone: "muted", title });
  }
  return words;
}

/** The top bar's toggle chip: the words joined, in the first word's tone; undefined when the file needs nothing. */
export function drawerChip(words: readonly DrawerWord[]): { text: string; tone: Tone } | undefined {
  if (words.length === 0) return undefined;
  return { text: words.map((word) => word.text).join(" · "), tone: words[0]!.tone };
}

function sameRefusal(a: Refusal | undefined, b: Refusal | undefined): boolean {
  if (a === undefined || b === undefined) return a === b;
  return a.code === b.code && a.message === b.message && a.line === b.line && a.column === b.column;
}

/**
 * True when `next` brings a refusal or a write failure the person has not been shown: a refusal on the first file, a
 * refusal where there was none, on a new version or a different one; a write failure where there was none or a new
 * reason. Never for an edit alone (a new version without a refusal, dropped edits, pendingRestart, diagnostics).
 */
export function drawerOpens(previous: GraphFile | null, next: GraphFile): boolean {
  if (next.refusal !== undefined) {
    if (previous === null || previous.refusal === undefined) return true;
    if (previous.version !== next.version || !sameRefusal(previous.refusal, next.refusal)) return true;
  }
  if (next.writeFailed !== undefined && next.writeFailed !== previous?.writeFailed) return true;
  return false;
}

/** paneReduce over null: before the first file a "file" event makes the pane, and any other event leaves it null. */
export function drawerReduce(state: PaneState | null, event: PaneEvent): PaneState | null {
  if (state === null) return event.kind === "file" ? paneFrom(event.file) : null;
  return paneReduce(state, event);
}
