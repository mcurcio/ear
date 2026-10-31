// S6 criterion 3: the ear.conf pane's logic. A dirty draft plus a new file goes stale and keeps
// the draft; reload takes the newest text; invalid-text sets the errors; Save carries the base
// version; a click on a diagnostic finds its place in the text.
import { describe, expect, it } from "vitest";
import { dirty, offsetOf, paneFrom, paneReduce, saveRequest, shownDiagnostics, type PaneState } from "./pane";
import type { GraphFile, RpcErrorObject, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const messages = messagesOf(fixtures.graphFile.messages);

/** The fixture's files in order: the subscription's answer, then every graphFile.changed. */
const files: GraphFile[] = messages.flatMap((message: ServerMessage): GraphFile[] => {
  if ("result" in message && message.id === 1) return [message.result as unknown as GraphFile];
  if ("method" in message && message.method === "graphFile.changed") {
    const { cause: _cause, droppedEdits: _dropped, ...file } = message.params as GraphFile & {
      cause: string;
      droppedEdits?: number;
    };
    return [file];
  }
  return [];
});
const v1 = files[0]!;
const v2 = files[1]!;
const v3 = files[2]!; // a hand edit with @include: diagnostics

function refusal(error: ServerMessage): RpcErrorObject {
  if (!("error" in error)) throw new Error("not an error");
  return error.error;
}
const refusedAs = (code: string) =>
  refusal(messages.find((message) => "error" in message && message.error.data?.code === code)!);

const edited = (state: PaneState, text: string) => paneReduce(state, { kind: "edit", text });

describe("the ear.conf pane", () => {
  it("starts clean on the file's text; an edit makes it dirty", () => {
    const pane = paneFrom(v1);
    expect(pane.draft).toBe(v1.text);
    expect(dirty(pane)).toBe(false);
    expect(dirty(edited(pane, v1.text + "# mine\n"))).toBe(true);
  });

  it("takes a new file when the draft is clean", () => {
    const pane = paneReduce(paneFrom(v1), { kind: "file", file: v2 });
    expect(pane).toStrictEqual(paneFrom(v2));
  });

  it("goes stale on a new file under a dirty draft, keeping the draft and the old base", () => {
    const draft = v1.text + "# mine\n";
    const pane = paneReduce(edited(paneFrom(v1), draft), { kind: "file", file: v2 });
    expect(pane.stale).toBe(true);
    expect(pane.draft).toBe(draft);
    expect(pane.base).toBe(v1);
    expect(saveRequest(pane)).toStrictEqual({ text: draft, version: v1.version });
  });

  it("reload drops the draft and takes the newest text", () => {
    const stale = paneReduce(edited(paneFrom(v1), "mine"), { kind: "file", file: v2 });
    const reloaded = paneReduce(stale, { kind: "reload" });
    expect(reloaded).toStrictEqual(paneFrom(v2));
    expect(reloaded.draft).toBe(v2.text);
  });

  it("is not stale when ear's new file is the draft (its own Save) or the same text", () => {
    const saving = edited(paneFrom(v1), v2.text);
    expect(paneReduce(saving, { kind: "file", file: v2 })).toStrictEqual(paneFrom(v2));
    const dirtyPane = edited(paneFrom(v1), "mine");
    const sameText = { ...v1, writeFailed: "ear.conf: No space left on device" };
    const after = paneReduce(dirtyPane, { kind: "file", file: sameText });
    expect(after.stale).toBe(false);
    expect(after.draft).toBe("mine");
    expect(after.base).toBe(sameText);
  });

  it("saveRequest carries the draft and the base version", () => {
    const pane = edited(paneFrom(v2), "graph_frequency 48000;\n");
    expect(saveRequest(pane)).toStrictEqual({ text: "graph_frequency 48000;\n", version: v2.version });
  });

  it("file-changed makes it stale; invalid-text sets the errors, listed first", () => {
    const pane = edited(paneFrom(v3), "typo");
    expect(paneReduce(pane, { kind: "refused", error: refusedAs("file-changed") }).stale).toBe(true);
    const invalid = refusedAs("invalid-text");
    const refused = paneReduce(pane, { kind: "refused", error: invalid });
    expect(refused.errors).toStrictEqual(invalid.data!.diagnostics);
    expect(refused.errors.length).toBeGreaterThan(0);
    expect(shownDiagnostics(refused)).toStrictEqual(invalid.data!.diagnostics);
    expect(shownDiagnostics(pane)).toStrictEqual(v3.diagnostics);
    expect(paneReduce(refused, { kind: "reload" }).errors).toStrictEqual([]);
  });

  it("leaves other refusals to the caller", () => {
    const pane = edited(paneFrom(v1), "x");
    expect(paneReduce(pane, { kind: "refused", error: refusedAs("unknown-port") })).toBe(pane);
  });

  it("finds a diagnostic's place: lines and UTF-16 columns from 1, clamped to the line", () => {
    const text = "ab\n\u{1F3B5}x\nlast";
    expect(offsetOf(text, 1, 1)).toBe(0);
    expect(offsetOf(text, 1, 3)).toBe(2);
    expect(offsetOf(text, 2, 3)).toBe(5); // the note is two UTF-16 code units
    expect(offsetOf(text, 2, 99)).toBe(6);
    expect(offsetOf(text, 3, 2)).toBe(8);
    expect(offsetOf(text, 9, 1)).toBe(text.length);
  });
});
