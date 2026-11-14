// The add-node form's knobly text, pure (docs/editor.md#adding-a-node). A person edits knobly; JSON is the machine API:
// the form takes a whole `node <type> <name> { ... }` statement, starting from a template for the chosen type, and Add
// sends node.addText with the newest version of ear.conf the editor holds. The template is a string built here; the
// browser never parses knobly, and ear places its diagnostics in the text sent.
import type { Diagnostic, GraphFile, RpcErrorObject } from "./protocol";

/** `<type><n>` for the first n from 1 that no node holds, the type cut to a name's characters ([A-Za-z0-9_-]). */
export function freeName(type: string, names: readonly string[]): string {
  const base = type.replace(/[^A-Za-z0-9_-]/g, "") || "node";
  const taken = new Set(names);
  for (let n = 1; ; n++) if (!taken.has(`${base}${n}`)) return `${base}${n}`;
}

/** What choosing a type writes into the form: its statement with a free name and an empty block. */
export function templateFor(type: string, names: readonly string[]): string {
  return `node ${type} ${freeName(type, names)} {\n}`;
}

export interface AddState {
  text: string;
  /** The last template written: while the text is still it, choosing another type replaces it. */
  template: string;
  /** The diagnostics of the last Add ear refused as invalid-text, placed in the text. */
  errors: Diagnostic[];
  /** ear.conf changed while the Add was on its way (file-changed): press Add again. */
  raced: boolean;
}

export type AddEvent =
  | { kind: "type"; type: string; names: readonly string[] } // a type chosen, or the form shown for one
  | { kind: "edit"; text: string }
  | { kind: "refused"; error: RpcErrorObject }
  | { kind: "added"; type: string; names: readonly string[] }; // ear ran it: a fresh template

export function addFrom(type: string, names: readonly string[]): AddState {
  const template = templateFor(type, names);
  return { text: template, template, errors: [], raced: false };
}

export function addReduce(state: AddState, event: AddEvent): AddState {
  switch (event.kind) {
    case "type": {
      const template = templateFor(event.type, event.names);
      // Never overwrite what the person typed.
      return state.text === state.template ? { ...state, text: template, template } : { ...state, template };
    }
    case "edit":
      return { ...state, text: event.text, raced: false };
    case "refused": {
      const data = event.error.data;
      if (data?.code === "file-changed") return { ...state, raced: true };
      if (data?.code === "invalid-text") return { ...state, errors: data.diagnostics ?? [], raced: false };
      return state; // the caller shows it
    }
    case "added":
      return addFrom(event.type, event.names);
  }
}

/** Add is offered when there is a text. */
export function canAdd(state: AddState): boolean {
  return state.text.trim() !== "";
}

/** node.addText's params: the text, against the newest file the editor holds. */
export function addRequest(state: AddState, file: GraphFile): { text: string; version: string } {
  return { text: state.text, version: file.version };
}
