// The editor's copy of ear.conf (docs/design/knobly-config.md, "Wire additions"): the file as ear
// last announced it, and the notices a person should see once (edits dropped by a hand edit, a
// failed write). The browser never parses knobly: ear is the one judge of the text.
import type { GraphFile, Notifications, ServerMessage } from "./protocol";

export type FileNotice = { kind: "dropped"; count: number } | { kind: "writeFailed"; detail: string };

export interface GraphFileState {
  file: GraphFile;
  notices: FileNotice[];
}

/** True when a result is a graph file (graphFile.get's and graphFile.subscribe's). */
function isGraphFile(value: unknown): value is GraphFile {
  if (typeof value !== "object" || value === null) return false;
  const file = value as GraphFile;
  return (
    typeof file.version === "string" &&
    typeof file.text === "string" &&
    Array.isArray(file.diagnostics) &&
    Array.isArray(file.pendingRestart)
  );
}

/**
 * A GraphFile result replaces the state; graphFile.changed replaces the file and adds a notice for
 * droppedEdits or a newly set writeFailed; other messages leave it unchanged. A change before the
 * first GraphFile result changes nothing (the subscription's answer comes first).
 */
export function reduceFile(state: GraphFileState | null, message: ServerMessage): GraphFileState | null {
  if ("result" in message) return isGraphFile(message.result) ? { file: message.result, notices: [] } : state;
  if ("error" in message || state === null || message.method !== "graphFile.changed") return state;
  const { cause: _cause, droppedEdits, ...file } = message.params as Notifications["graphFile.changed"];
  const notices = [...state.notices];
  if (droppedEdits !== undefined && droppedEdits > 0) notices.push({ kind: "dropped", count: droppedEdits });
  if (file.writeFailed !== undefined && file.writeFailed !== state.file.writeFailed) {
    notices.push({ kind: "writeFailed", detail: file.writeFailed });
  }
  return { file, notices };
}

/** The state without one notice (the person dismissed it). */
export function dismissNotice(state: GraphFileState, notice: FileNotice): GraphFileState {
  return { ...state, notices: state.notices.filter((known) => known !== notice) };
}

/**
 * The graph file and who is watching it, outside React (App reads it with useSyncExternalStore).
 * Null until graphFile.subscribe is answered with a file; an error answer leaves it null, so the
 * pane stays hidden.
 */
export class GraphFileStore {
  private state: GraphFileState | null = null;
  private readonly listeners = new Set<() => void>();

  get(): GraphFileState | null {
    return this.state;
  }

  apply(message: ServerMessage): void {
    this.set(reduceFile(this.state, message));
  }

  dismiss(notice: FileNotice): void {
    if (this.state !== null) this.set(dismissNotice(this.state, notice));
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(next: GraphFileState | null): void {
    if (next === this.state) return;
    this.state = next;
    for (const listener of [...this.listeners]) listener();
  }
}
