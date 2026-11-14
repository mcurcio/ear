// The node panel's knobly box, pure (docs/editor.md#the-node-panels-text). A person edits knobly; JSON is the machine
// API: the box holds the node's own statement as ear.conf has it, cut from the GraphFile's text where its nodeTexts
// say (feature node-text), and Save sends node.setText with the version it was cut from. The browser never parses
// knobly: ear judges every text and places its diagnostics in the text sent. A draft is never overwritten: a change
// elsewhere in the file moves the draft onto the new version; a change to this node's own text under a dirty draft
// makes the box stale until the person reloads, as the pane does.
import { offsetOf } from "./pane";
import type { Diagnostic, GraphFile, NodeText, RpcErrorObject } from "./protocol";

/** What the file says of one node's text. */
export type NodeTextIn =
  | { kind: "own"; text: string }
  /** Written in a snippet: shown, never replaced from the box. */
  | { kind: "shared"; text: string; at: { line: number; column: number } }
  /** The file has errors (no nodeTexts), or names no such node. */
  | { kind: "none" };

/** The node's entry in the file's nodeTexts, read out of its text. */
export function nodeTextIn(file: GraphFile, name: string): NodeTextIn {
  const entry: NodeText | undefined = file.nodeTexts?.find((node) => node.name === name);
  if (entry === undefined) return { kind: "none" };
  const from = offsetOf(file.text, entry.start.line, entry.start.column);
  const to = offsetOf(file.text, entry.end.line, entry.end.column);
  // A textarea holds "\n" only, so a CRLF file's text is cut as it would come back: unchanged, it is not dirty. ear
  // writes the file's own ending into what Save sends.
  const text = file.text.slice(from, to).replaceAll("\r\n", "\n");
  return entry.shared === true ? { kind: "shared", text, at: entry.start } : { kind: "own", text };
}

export interface BoxState {
  node: string;
  /** The person's text. */
  draft: string;
  /** The node's own text and the version it was cut from; Save sends this version. */
  base: { version: string; text: NodeTextIn };
  /** The newest file ear announced; reload takes it. */
  latest: GraphFile;
  /** This node's text changed under a dirty draft, or ear answered file-changed. */
  stale: boolean;
  /** The diagnostics of the last Save ear refused as invalid-text, placed in the draft. */
  errors: Diagnostic[];
}

export type BoxEvent =
  | { kind: "file"; file: GraphFile }
  | { kind: "edit"; text: string }
  | { kind: "reload" }
  | { kind: "refused"; error: RpcErrorObject };

function textOf(found: NodeTextIn): string {
  return found.kind === "none" ? "" : found.text;
}

/** A box showing `node`'s text in `file`, nothing typed. */
export function boxFrom(file: GraphFile, node: string): BoxState {
  const text = nodeTextIn(file, node);
  return { node, draft: textOf(text), base: { version: file.version, text }, latest: file, stale: false, errors: [] };
}

/** The draft differs from the text it was cut from. */
export function dirty(state: BoxState): boolean {
  return state.draft !== textOf(state.base.text);
}

/** Save is offered: the node has its own text, the draft changed, and nothing newer is waiting to be read. */
export function canSave(state: BoxState): boolean {
  return state.base.text.kind === "own" && dirty(state) && !state.stale;
}

function same(a: NodeTextIn, b: NodeTextIn): boolean {
  return a.kind === b.kind && textOf(a) === textOf(b);
}

function onFile(state: BoxState, file: GraphFile): BoxState {
  const text = nodeTextIn(file, state.node);
  // The change was elsewhere (another node, a route, a comment): the draft moves onto the new version as it is.
  if (same(text, state.base.text))
    return { ...state, base: { version: file.version, text }, latest: file, stale: false };
  // ear now holds what the draft says (this box's own Save, or the same edit made elsewhere), or nothing was typed.
  if ((text.kind === "own" && text.text === state.draft) || !dirty(state)) return boxFrom(file, state.node);
  return { ...state, latest: file, stale: true };
}

export function boxReduce(state: BoxState, event: BoxEvent): BoxState {
  switch (event.kind) {
    case "file":
      return onFile(state, event.file);
    case "edit":
      return { ...state, draft: event.text };
    case "reload":
      return boxFrom(state.latest, state.node);
    case "refused": {
      const data = event.error.data;
      if (data?.code === "file-changed") return { ...state, stale: true };
      if (data?.code === "invalid-text") return { ...state, errors: data.diagnostics ?? [] };
      return state; // the caller shows it
    }
  }
}

/** node.setText's params: the draft, against the version it was cut from. */
export function saveRequest(state: BoxState): { name: string; text: string; version: string } {
  return { name: state.node, text: state.draft, version: state.base.version };
}

/** Why the box cannot save, in words; "" when it can be edited. */
export function boxNote(state: BoxState): string {
  const text = state.base.text;
  if (text.kind === "shared")
    return (
      `${state.node} is written in a snippet at ${text.at.line}:${text.at.column} of ear.conf, ` +
      "with every node it makes: edit it in ear.conf below"
    );
  if (text.kind === "none") return "ear.conf has errors: fix them in ear.conf below, then edit this node here";
  return "";
}
